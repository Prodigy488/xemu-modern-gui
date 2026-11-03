//
// xemu User Interface - custom soundtracks for the Xbox dashboard
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "soundtracks.hh"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common.hh"
#include "IconsFontAwesome6.h"
#include "misc.hh"
#include "osk.hh"
#include "soundtrack-convert.hh"
#include "soundtrack-db.hh"
#include "viewport-manager.hh"
#include "xbox-drive-explorer.hh"
#include "xbox-drive-qemu.hh"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"

namespace fs = std::filesystem;

namespace
{
const char *const kMusicDir = "/TDATA/fffe0000/music";
const char *const kDbPath = "/TDATA/fffe0000/music/ST.DB";

struct Item {
    std::string path;
    char title[96] = {};
};

// One run: converting the songs, then copying them to the Xbox
struct Work {
    std::vector<std::string> paths, titles;
    std::vector<int> state; // 0 waiting, 1 converting, 2 done, 3 failed
    std::vector<std::string> errors;
    std::vector<uint32_t> ms;
    fs::path dir;
    int32_t target = -1;
    std::string new_name;
    bool extra_copies = true;
};

bool g_show = false;
std::vector<Item> g_items;
char g_new_name[96] = "My Soundtrack";
int g_target = -1; // -1: a new soundtrack, else the soundtrack's id
bool g_extra_copies = true;
std::vector<StSoundtrackInfo> g_existing;
std::string g_existing_err;
bool g_existing_dirty = true;

enum class Phase { Idle, Converting, Writing };
Phase g_phase = Phase::Idle;
std::unique_ptr<Work> g_work;
std::thread g_thread;
std::mutex g_mx;
std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_finished{ false };
std::atomic<int> g_current{ 0 };

std::string g_status;
bool g_status_err = false;
std::string g_pending_ok; // the message to show when the copy to the Xbox succeeds

fs::path TempDir()
{
    return fs::u8path(xemu_settings_get_base_path()) / "soundtrack-temp";
}

void SetStatus(const std::string &m, bool err = false)
{
    g_status = m;
    g_status_err = err;
}

bool IsAudioName(const fs::path &p)
{
    std::string e = p.extension().u8string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    static const char *const kExt[] = { ".mp3", ".wma", ".wav", ".flac", ".ogg", ".oga",
                                        ".m4a", ".aac", ".opus", ".mp2", ".aif", ".aiff" };
    for (const char *x : kExt) {
        if (e == x) {
            return true;
        }
    }
    return false;
}

void AddPath(const std::string &path)
{
    for (const Item &i : g_items) {
        if (i.path == path) {
            return;
        }
    }
    Item it;
    it.path = path;
    std::string t = StClipName(SoundtrackTitleOf(path), kStNameChars);
    snprintf(it.title, sizeof(it.title), "%s", t.c_str());
    g_items.push_back(it);
}

void AddFolder(const std::string &folder)
{
    std::vector<std::string> found;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(fs::u8path(folder), ec)) {
        if (e.is_regular_file(ec) && IsAudioName(e.path())) {
            found.push_back(e.path().u8string());
        }
    }
    std::sort(found.begin(), found.end());
    for (auto &p : found) {
        AddPath(p);
    }
    if (found.empty()) {
        SetStatus("No songs found in that folder", true);
    }
}

void RefreshExisting()
{
    g_existing_dirty = false;
    g_existing.clear();
    g_existing_err.clear();
    std::vector<uint8_t> db;
    bool exists = false;
    if (!XboxExplorerReadFile('E', kDbPath, &db, &exists, &g_existing_err)) {
        return;
    }
    if (!exists) {
        return; // none yet
    }
    if (!StList(db, &g_existing, &g_existing_err)) {
        g_existing.clear();
    }
    bool found = g_target < 0;
    for (auto &s : g_existing) {
        found |= s.id == g_target;
    }
    if (!found) {
        g_target = -1;
    }
}

std::string FormatLength(uint32_t ms)
{
    uint32_t s = ms / 1000;
    char b[32];
    if (s >= 3600) {
        snprintf(b, sizeof(b), "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(b, sizeof(b), "%u:%02u", s / 60, s % 60);
    }
    return b;
}

void CleanTemp()
{
    std::error_code ec;
    fs::remove_all(TempDir(), ec);
}

void Convert(Work *w)
{
    for (size_t i = 0; i < w->paths.size(); i++) {
        if (g_cancel) {
            break;
        }
        g_current = (int)i;
        {
            std::lock_guard<std::mutex> g(g_mx);
            w->state[i] = 1;
        }
        std::string out = (w->dir / (std::to_string(i) + ".wma")).u8string();
        std::string err;
        uint32_t ms = 0;
        bool ok = SoundtrackConvert(w->paths[i], out, &err, &g_cancel);
        if (ok && (!SoundtrackWmaLengthMs(out, &ms) || ms == 0)) {
            ok = false;
            err = "The converted song has no length";
        }
        std::lock_guard<std::mutex> g(g_mx);
        w->state[i] = ok ? 2 : 3;
        w->errors[i] = err;
        w->ms[i] = ms;
    }
    g_finished = true;
}

void Start()
{
    std::string how;
    if (!SoundtrackCanConvert(&how)) {
        SetStatus("No converter found. Put ffmpeg next to xemu, or choose it with "
                  "\"Choose ffmpeg...\".",
                  true);
        return;
    }
    // Check the Xbox's drive now, so a problem shows before the conversion
    RefreshExisting();
    if (!g_existing_err.empty()) {
        SetStatus(g_existing_err, true);
        return;
    }
    CleanTemp();
    std::error_code ec;
    fs::create_directories(TempDir(), ec);
    if (ec) {
        SetStatus("Couldn't make a folder for the converted songs", true);
        return;
    }
    auto w = std::make_unique<Work>();
    w->dir = TempDir();
    w->target = g_target;
    w->new_name = g_new_name;
    w->extra_copies = g_extra_copies;
    for (const Item &i : g_items) {
        w->paths.push_back(i.path);
        std::string t = StClipName(i.title, kStNameChars);
        w->titles.push_back(t.empty() ? StClipName(fs::u8path(i.path).stem().u8string(), kStNameChars) : t);
    }
    w->state.assign(w->paths.size(), 0);
    w->errors.assign(w->paths.size(), "");
    w->ms.assign(w->paths.size(), 0);
    g_work = std::move(w);
    g_cancel = false;
    g_finished = false;
    g_current = 0;
    g_phase = Phase::Converting;
    SetStatus("");
    g_thread = std::thread(Convert, g_work.get());
}

void Fail(const std::string &msg)
{
    SetStatus(msg, true);
    g_phase = Phase::Idle;
    g_work.reset();
    CleanTemp();
}

// All songs are converted: put them and the new index on the Xbox
void Finish()
{
    if (g_thread.joinable()) {
        g_thread.join();
    }
    Work &w = *g_work;
    if (g_cancel) {
        Fail("Cancelled");
        return;
    }
    std::vector<size_t> ok;
    std::string first_error;
    for (size_t i = 0; i < w.paths.size(); i++) {
        if (w.state[i] == 2) {
            ok.push_back(i);
        } else if (first_error.empty()) {
            first_error = fs::u8path(w.paths[i]).filename().u8string() + ": " + w.errors[i];
        }
    }
    if (ok.empty()) {
        Fail("No song could be converted. " + first_error);
        return;
    }
    // Read the Xbox's index again: what's on it now decides the song numbers
    std::vector<uint8_t> db;
    bool exists = false;
    std::string err;
    if (!XboxExplorerReadFile('E', kDbPath, &db, &exists, &err)) {
        Fail(err);
        return;
    }
    if (exists) {
        std::vector<StSoundtrackInfo> l;
        if (!StList(db, &l, &err)) {
            Fail(err + ". Nothing was changed.");
            return;
        }
    } else {
        db.clear();
    }
    std::vector<StNewSong> songs;
    for (size_t i : ok) {
        songs.push_back({ w.titles[i], w.ms[i] });
    }
    std::vector<StPlaced> placed;
    if (!StAdd(&db, w.target, w.new_name, songs, &placed, &err)) {
        Fail(err);
        return;
    }
    fs::path dbfile = w.dir / "ST.DB";
    {
        FILE *f = fopen(dbfile.u8string().c_str(), "wb");
#ifdef _WIN32
        if (!f) {
            f = _wfopen(dbfile.wstring().c_str(), L"wb");
        }
#endif
        if (!f || fwrite(db.data(), 1, db.size(), f) != db.size()) {
            if (f) {
                fclose(f);
            }
            Fail("Couldn't write the soundtrack index");
            return;
        }
        fclose(f);
    }
    std::vector<XboxPutFile> files;
    char buf[96];
    for (size_t k = 0; k < ok.size(); k++) {
        std::string host = (w.dir / (std::to_string(ok[k]) + ".wma")).u8string();
        snprintf(buf, sizeof(buf), "%s/%04u/%08x.wma", kMusicDir, placed[k].group_id,
                 placed[k].song_id);
        files.push_back({ host, buf });
        if (w.extra_copies && placed[k].group_id != 0) {
            snprintf(buf, sizeof(buf), "%s/0000/%08x.wma", kMusicDir, placed[k].song_id);
            files.push_back({ host, buf });
        }
    }
    files.push_back({ dbfile.u8string(), kDbPath }); // last: the index only points at songs that are there
    if (!XboxExplorerPutFiles('E', files, "Adding custom soundtrack songs", &err)) {
        Fail(err);
        return;
    }
    std::string name = w.target < 0 ? w.new_name : std::string();
    for (auto &s : g_existing) {
        if (s.id == w.target) {
            name = s.name;
        }
    }
    if (ok.size() == 1) {
        g_pending_ok = string_format("Added 1 song to \"%s\".", name.c_str());
    } else {
        g_pending_ok = string_format("Added %d songs to \"%s\".", (int)ok.size(), name.c_str());
    }
    if (ok.size() < w.paths.size()) {
        g_pending_ok += string_format(" %d couldn't be converted (%s)",
                                      (int)(w.paths.size() - ok.size()), first_error.c_str());
    }
    g_phase = Phase::Writing;
    SetStatus("Copying to the Xbox...");
}

void DrawContent()
{
    const float s = g_viewport_mgr.m_scale;
    bool busy = g_phase != Phase::Idle;

    ImGui::TextWrapped("%s",
                       "Add your own music to the Xbox dashboard. Songs are converted to WMA "
                       "(the format the Xbox plays) and added to the hard drive as a soundtrack. "
                       "Find them in the dashboard under Music > Soundtracks, or pick them in games "
                       "that allow custom soundtracks.");
    ImGui::Spacing();

    // Converter
    std::string how;
    if (SoundtrackCanConvert(&how)) {
        ImGui::TextDisabled("Converting with %s", how.c_str());
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%s",
                           "No converter found. Put ffmpeg.exe next to xemu, or choose it.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose ffmpeg...")) {
        ShowOpenFileDialog(nullptr, 0, nullptr, [](const char *p) {
            SoundtrackSetFfmpeg(p);
        });
    }
    ImGui::Separator();

    // Where the songs go
    if (g_existing_dirty && !busy) {
        RefreshExisting();
    }
    ImGui::Text("Add to");
    ImGui::SameLine();
    std::string cur = "A new soundtrack";
    for (auto &st : g_existing) {
        if (st.id == g_target) {
            cur = st.name + " (" + std::to_string(st.songs) + " songs)";
        }
    }
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(320 * s);
    if (ImGui::BeginCombo("##target", cur.c_str())) {
        if (ImGui::Selectable("A new soundtrack", g_target < 0)) {
            g_target = -1;
        }
        for (auto &st : g_existing) {
            std::string label = st.name + " (" + std::to_string(st.songs) + " songs, " +
                                FormatLength(st.total_ms) + ")";
            ImGui::PushID(st.id);
            if (ImGui::Selectable(label.c_str(), g_target == st.id)) {
                g_target = st.id;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE " Refresh")) {
        g_existing_dirty = true;
    }
    if (g_target < 0) {
        ImGui::Text("Name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(320 * s);
        ImGui::InputText("##stname", g_new_name, sizeof(g_new_name));
        OskTextBox();
    }
    ImGui::EndDisabled();
    if (!g_existing_err.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", g_existing_err.c_str());
    }
    ImGui::Separator();

    // The songs
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(ICON_FA_PLUS " Add songs...")) {
        ShowOpenFilesDialog(nullptr, [](const std::vector<std::string> &paths) {
            for (auto &p : paths) {
                AddPath(p);
            }
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Add a folder...")) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { AddFolder(p); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear list")) {
        g_items.clear();
    }
    ImGui::EndDisabled();

    float list_h = std::max(120.0f * s, ImGui::GetContentRegionAvail().y - 150 * s);
    if (ImGui::BeginChild("##songs", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
        if (g_items.empty()) {
            ImGui::TextDisabled("Add mp3, flac, ogg, m4a, wav or wma files. Song names can be "
                                "edited here; the Xbox shows up to %d characters.",
                                kStNameChars);
        }
        int remove = -1, up = -1;
        for (size_t i = 0; i < g_items.size(); i++) {
            ImGui::PushID((int)i);
            int st = 0;
            std::string err;
            if (busy && g_work && i < g_work->state.size()) {
                std::lock_guard<std::mutex> g(g_mx);
                st = g_work->state[i];
                err = g_work->errors[i];
            }
            ImGui::BeginDisabled(busy);
            ImGui::Text("%2d", (int)i + 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(160.0f * s, ImGui::GetContentRegionAvail().x - 150 * s));
            ImGui::InputText("##t", g_items[i].title, sizeof(g_items[i].title));
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_ARROW_UP) && i > 0) {
                up = (int)i;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK)) {
                remove = (int)i;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (st == 1) {
                ImGui::TextDisabled("converting...");
            } else if (st == 2) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), ICON_FA_CHECK);
            } else if (st == 3) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), ICON_FA_TRIANGLE_EXCLAMATION);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", err.c_str());
                }
            } else {
                ImGui::TextUnformatted(""); // ends the row
            }
            ImGui::PopID();
        }
        if (remove >= 0) {
            g_items.erase(g_items.begin() + remove);
        }
        if (up > 0) {
            std::swap(g_items[up], g_items[up - 1]);
        }
    }
    ImGui::EndChild();

    // Go
    if (g_phase == Phase::Converting && g_work) {
        int total = (int)g_work->paths.size();
        int done = 0;
        {
            std::lock_guard<std::mutex> g(g_mx);
            for (int st : g_work->state) {
                done += st >= 2;
            }
        }
        char label[64];
        snprintf(label, sizeof(label), "Converting %d of %d", std::min(done + 1, total), total);
        ImGui::ProgressBar(total ? (float)done / total : 0, ImVec2(-1, 0), label);
        if (ImGui::Button("Cancel")) {
            g_cancel = true;
        }
    } else if (g_phase == Phase::Writing) {
        ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(-1, 0), "Copying to the Xbox");
    } else {
        bool name_ok = g_target >= 0 || !StClipName(g_new_name, kStNameChars).empty();
        ImGui::BeginDisabled(g_items.empty() || !name_ok || XboxExplorerBusy());
        if (ImGui::Button(ICON_FA_VOLUME_HIGH "  Convert and add to the Xbox")) {
            ImGui::OpenPopup(trid("Add to the Xbox?##st"));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("Also keep a copy in folder 0000", &g_extra_copies);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                              "Some dashboards look for every song in the first folder. This "
                              "uses more disk space for soundtracks of more than 6 songs.");
        }
    }
    if (!g_status.empty()) {
        if (g_status_err) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", g_status.c_str());
        } else {
            ImGui::TextWrapped("%s", g_status.c_str());
        }
    }

    if (ImGui::BeginPopupModal(trid("Add to the Xbox?##st"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",
                           "The songs are converted first (the Xbox keeps running). Then the "
                           "Xbox pauses while they are copied and restarts when done. Progress "
                           "in a running game that hasn't been saved will be lost.");
        if (ImGui::Button("Continue")) {
            ImGui::CloseCurrentPopup();
            Start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} // namespace

void SoundtracksDrawContent(void)
{
    DrawContent();
}

void SoundtracksOpenWindow(void)
{
    g_show = true;
    g_existing_dirty = true;
}

void SoundtracksUpdate(void)
{
    if (g_phase == Phase::Converting && g_finished) {
        g_finished = false;
        Finish();
    } else if (g_phase == Phase::Writing && !XboxExplorerBusy()) {
        bool err = false;
        std::string st = XboxExplorerStatus(&err);
        g_phase = Phase::Idle;
        g_work.reset();
        CleanTemp();
        if (err) {
            SetStatus(st, true);
        } else {
            SetStatus(g_pending_ok + " The Xbox restarted; the songs are under Music in the "
                                     "dashboard.");
            g_items.clear();
            xemu_queue_notification(g_pending_ok.c_str());
        }
        g_existing_dirty = true;
    }
}

void SoundtracksWindowDraw(void)
{
    if (!g_show) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(720 * s, 640 * s), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin(trid("Custom Soundtracks"), &open)) {
        DrawContent();
    }
    ImGui::End();
    if (!open && g_phase == Phase::Idle) {
        g_show = false;
    }
}
