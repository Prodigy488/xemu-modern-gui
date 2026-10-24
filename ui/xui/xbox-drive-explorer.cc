//
// xemu User Interface - Xbox hard drive explorer
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xbox-drive-explorer.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>

#include "common.hh"
#include "game-library.hh"
#include "IconsFontAwesome6.h"
#include "misc.hh"
#include "viewport-manager.hh"
#include "xbox-drive-qemu.hh"
#include "xiso.hh"

namespace fs = std::filesystem;

namespace
{

XboxDrive g_drive;
std::string g_open_err;
uint64_t g_open_tried = 0; // ticks of the last failed open
char g_vol = 'E';
std::string g_path = "/";
std::vector<XboxFileInfo> g_list;
std::string g_list_err;
bool g_list_dirty = true;
int g_sel = -1;
std::string g_status;
bool g_status_error = false;
bool g_window_open = false;

void SetStatus(const std::string &msg, bool error = false)
{
    g_status = msg;
    g_status_error = error;
}

std::string FormatSize(uint64_t n)
{
    char b[32];
    if (n < 1024) {
        snprintf(b, sizeof(b), "%llu B", (unsigned long long)n);
    } else if (n < (1ull << 20)) {
        snprintf(b, sizeof(b), "%.1f KB", n / 1024.0);
    } else if (n < (1ull << 30)) {
        snprintf(b, sizeof(b), "%.1f MB", n / 1048576.0);
    } else {
        snprintf(b, sizeof(b), "%.2f GB", n / 1073741824.0);
    }
    return b;
}

std::string FormatTime(time_t t)
{
    if (t <= 0) {
        return "";
    }
    char b[32];
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
    return b;
}

bool EnsureOpen()
{
    if (g_drive.IsOpen()) {
        return true;
    }
    uint64_t now = SDL_GetTicks();
    if (g_open_tried && now - g_open_tried < 2000) {
        return false;
    }
    g_open_err.clear();
    if (!g_drive.Open(XboxDiskOpenEmulated(&g_open_err), &g_open_err)) {
        g_open_tried = now;
        return false;
    }
    g_open_tried = 0;
    if (!g_drive.Volume(g_vol)) {
        g_vol = g_drive.Volumes()[0].letter;
        g_path = "/";
    }
    g_list_dirty = true;
    return true;
}

// Re-read the disk (the running Xbox may have changed it)
void Reopen()
{
    g_drive.Close();
    g_open_tried = 0;
    EnsureOpen();
    g_list_dirty = true;
}

void LoadList()
{
    g_list_dirty = false;
    g_sel = -1;
    g_list_err.clear();
    if (!g_drive.List(g_vol, g_path, &g_list, &g_list_err)) {
        // The folder may be gone: fall back to the root
        if (g_path != "/") {
            g_path = "/";
            g_drive.List(g_vol, g_path, &g_list, &g_list_err);
        }
    }
    std::sort(g_list.begin(), g_list.end(),
              [](const XboxFileInfo &a, const XboxFileInfo &b) {
                  if (a.dir != b.dir) {
                      return a.dir;
                  }
                  std::string x = a.name, y = b.name;
                  std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                  std::transform(y.begin(), y.end(), y.begin(), ::tolower);
                  return x < y;
              });
}

void Navigate(char vol, const std::string &path)
{
    g_vol = vol;
    g_path = path.empty() ? "/" : path;
    if (XboxIsRunning()) {
        Reopen(); // see the Xbox's latest changes
    }
    g_list_dirty = true;
}

// ---- Jobs: copy in, copy out, delete ---------------------------------------

struct Step {
    enum Kind { XMkDir, XFile, HMkDir, HFile, XDelFile, XDelDir, IsoFile } kind;
    std::string host;  // host path (UTF-8)
    std::string xpath; // path on the Xbox volume
    uint64_t size = 0;
    uint64_t dest = 0; // IsoFile: byte offset in the disc image
};

struct Job {
    std::string title;
    bool writes = false; // changes the Xbox drive
    char vol = 'E';
    std::vector<Step> steps;
    size_t i = 0;
    uint64_t done = 0, total = 0;
    FILE *f = nullptr;
    uint64_t off = 0;
    bool cancel = false;
    std::string err;
    std::vector<uint8_t> buf;
    int files = 0;
    // Disc image jobs
    FILE *iso = nullptr;
    std::string iso_part, iso_final, iso_sig;
};

FILE *OpenHost(const std::string &path, const char *mode)
{
#ifdef _WIN32
    std::wstring m(mode, mode + strlen(mode));
    return _wfopen(fs::u8path(path).wstring().c_str(), m.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool SeekHost(FILE *f, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(f, (__int64)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

std::unique_ptr<Job> g_job;
bool g_paused_for_job = false;

// Confirmation before a job that writes to the Xbox drive
std::unique_ptr<Job> g_pending_write;

bool HostToXboxSteps(const fs::path &src, const std::string &xdir, Job *job,
                     std::string *err)
{
    std::string name = src.filename().u8string();
    std::string why;
    if (!XboxValidName(name, &why)) {
        *err = "\"" + name + "\": " + why;
        return false;
    }
    std::string xpath = XboxJoin(xdir, name);
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        job->steps.push_back({ Step::XMkDir, src.u8string(), xpath, 0 });
        std::vector<fs::path> children;
        for (auto &e : fs::directory_iterator(src, ec)) {
            children.push_back(e.path());
        }
        std::sort(children.begin(), children.end());
        for (auto &c : children) {
            if (!HostToXboxSteps(c, xpath, job, err)) {
                return false;
            }
        }
        return true;
    }
    uint64_t size = fs::file_size(src, ec);
    if (ec) {
        *err = "Can't read \"" + src.u8string() + "\"";
        return false;
    }
    if (size > 0xFFFFFFFFull) {
        *err = "\"" + name + "\" is larger than 4 GB, the Xbox's file size limit";
        return false;
    }
    job->steps.push_back({ Step::XFile, src.u8string(), xpath, size });
    job->total += size;
    job->files++;
    return true;
}

bool XboxToHostSteps(char vol, const std::string &xpath, const XboxFileInfo &fi,
                     const fs::path &hdir, Job *job, std::string *err)
{
    fs::path hpath = hdir / fs::u8path(fi.name);
    if (!fi.dir) {
        job->steps.push_back({ Step::HFile, hpath.u8string(), xpath, fi.size });
        job->total += fi.size;
        job->files++;
        return true;
    }
    job->steps.push_back({ Step::HMkDir, hpath.u8string(), xpath, 0 });
    std::vector<XboxFileInfo> list;
    if (!g_drive.List(vol, xpath, &list, err)) {
        return false;
    }
    for (auto &c : list) {
        if (!XboxToHostSteps(vol, XboxJoin(xpath, c.name), c, hpath, job, err)) {
            return false;
        }
    }
    return true;
}

bool DeleteSteps(char vol, const std::string &xpath, const XboxFileInfo &fi,
                 Job *job, std::string *err)
{
    if (fi.dir) {
        std::vector<XboxFileInfo> list;
        if (!g_drive.List(vol, xpath, &list, err)) {
            return false;
        }
        for (auto &c : list) {
            if (!DeleteSteps(vol, XboxJoin(xpath, c.name), c, job, err)) {
                return false;
            }
        }
        job->steps.push_back({ Step::XDelDir, "", xpath, 0 });
    } else {
        job->steps.push_back({ Step::XDelFile, "", xpath, 0 });
        job->files++;
    }
    job->total += 1;
    return true;
}

void StartJob(std::unique_ptr<Job> job)
{
    if (job->writes) {
        // The Xbox caches its file system: pause it, start from a fresh view
        // of the disk, and restart it when done
        g_paused_for_job = XboxIsRunning();
        XboxPause();
        Reopen();
        if (!g_drive.IsOpen()) {
            SetStatus(g_open_err, true);
            return;
        }
    }
    job->buf.resize(1 << 20);
    g_job = std::move(job);
    SetStatus(g_job->title + "...");
}

void AskThenStart(std::unique_ptr<Job> job)
{
    if (job->steps.empty()) {
        return;
    }
    if (job->writes) {
        // Confirmed in a dialog first (opened by DrawPopups)
        g_pending_write = std::move(job);
        return;
    }
    StartJob(std::move(job));
}

void FinishJob()
{
    Job &j = *g_job;
    if (j.f) {
        fclose(j.f);
        j.f = nullptr;
    }
    if (j.cancel && j.i < j.steps.size()) {
        // Don't leave a half-copied file behind
        Step &s = j.steps[j.i];
        std::string e;
        if (s.kind == Step::XFile && j.off > 0) {
            g_drive.RemoveFile(j.vol, s.xpath, &e);
        } else if (s.kind == Step::HFile) {
            std::error_code ec;
            fs::remove(fs::u8path(s.host), ec);
        }
    }
    if (j.iso) {
        bool ok = fclose(j.iso) == 0 && j.err.empty() && !j.cancel;
        j.iso = nullptr;
        std::error_code ec;
        if (ok) {
            fs::remove(fs::u8path(j.iso_final), ec);
            fs::rename(fs::u8path(j.iso_part), fs::u8path(j.iso_final), ec);
            ok = !ec;
            if (!ok) {
                j.err = "Couldn't save the disc image";
            }
        }
        if (ok) {
            FILE *sf = OpenHost(j.iso_final + ".sig", "wb");
            if (sf) {
                fwrite(j.iso_sig.data(), 1, j.iso_sig.size(), sf);
                fclose(sf);
            }
        } else {
            fs::remove(fs::u8path(j.iso_part), ec);
        }
    }
    if (j.writes) {
        g_drive.Flush();
        g_drive.UpdateFreeSpace(j.vol);
        XboxRestart();
        g_paused_for_job = false;
    }
    if (!j.err.empty()) {
        SetStatus(j.err, true);
    } else if (j.cancel) {
        SetStatus("Cancelled");
    } else {
        SetStatus(j.title + " - done." +
                  (j.writes ? " The Xbox restarted to load the changes." : ""));
    }
    g_job.reset();
    g_list_dirty = true;
}

// Do one piece of work; false when the job has ended
bool RunStep(Job &j)
{
    if (j.cancel || !j.err.empty() || j.i >= j.steps.size()) {
        return false;
    }
    Step &s = j.steps[j.i];
    std::string err;
    switch (s.kind) {
    case Step::XMkDir:
        if (!g_drive.MakeDir(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.i++;
        return true;
    case Step::HMkDir: {
        std::error_code ec;
        fs::create_directories(fs::u8path(s.host), ec);
        if (ec) {
            j.err = "Can't create \"" + s.host + "\"";
            return false;
        }
        j.i++;
        return true;
    }
    case Step::XDelFile:
        if (!g_drive.RemoveFile(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.done++;
        j.i++;
        return true;
    case Step::XDelDir:
        if (!g_drive.DeleteEmptyDir(j.vol, s.xpath, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.done++;
        j.i++;
        return true;
    case Step::XFile: {
        if (!j.f) {
            j.f = fopen(s.host.c_str(), "rb");
#ifdef _WIN32
            if (!j.f) {
                j.f = _wfopen(fs::u8path(s.host).wstring().c_str(), L"rb");
            }
#endif
            if (!j.f) {
                j.err = "Can't read \"" + s.host + "\"";
                return false;
            }
            if (!g_drive.NewFile(j.vol, s.xpath, &err)) {
                j.err = s.xpath + ": " + err;
                return false;
            }
            j.off = 0;
        }
        size_t n = fread(j.buf.data(), 1, j.buf.size(), j.f);
        if (n > 0 &&
            !g_drive.WriteAt(j.vol, s.xpath, j.off, j.buf.data(), n, &err)) {
            j.err = s.xpath + ": " + err;
            return false;
        }
        j.off += n;
        j.done += n;
        if (n < j.buf.size()) {
            fclose(j.f);
            j.f = nullptr;
            j.off = 0;
            j.i++;
        }
        return true;
    }
    case Step::IsoFile: {
        size_t want = (size_t)std::min<uint64_t>(j.buf.size(), s.size - j.off);
        int64_t n = want ? g_drive.ReadAt(j.vol, s.xpath, j.off, j.buf.data(), want)
                         : 0;
        if (n < 0 || (want && n == 0) || !SeekHost(j.iso, s.dest + j.off) ||
            (n > 0 && fwrite(j.buf.data(), 1, (size_t)n, j.iso) != (size_t)n)) {
            j.err = "Couldn't copy " + s.xpath + " into the disc image";
            return false;
        }
        j.off += n;
        j.done += n;
        if (j.off >= s.size) {
            j.off = 0;
            j.i++;
        }
        return true;
    }
    case Step::HFile: {
        if (!j.f) {
#ifdef _WIN32
            j.f = _wfopen(fs::u8path(s.host).wstring().c_str(), L"wb");
#else
            j.f = fopen(s.host.c_str(), "wb");
#endif
            if (!j.f) {
                j.err = "Can't write \"" + s.host + "\"";
                return false;
            }
            j.off = 0;
        }
        size_t want = (size_t)std::min<uint64_t>(j.buf.size(), s.size - j.off);
        int64_t n = want ? g_drive.ReadAt(j.vol, s.xpath, j.off, j.buf.data(), want)
                         : 0;
        if (n < 0 || (want && n == 0) ||
            (n > 0 && fwrite(j.buf.data(), 1, (size_t)n, j.f) != (size_t)n)) {
            j.err = "Couldn't copy " + s.xpath;
            return false;
        }
        j.off += n;
        j.done += n;
        if (j.off >= s.size) {
            fclose(j.f);
            j.f = nullptr;
            j.off = 0;
            j.i++;
        }
        return true;
    }
    }
    return false;
}

// ---- Actions ---------------------------------------------------------------

const XboxFileInfo *Selected()
{
    if (g_sel < 0 || g_sel >= (int)g_list.size()) {
        return nullptr;
    }
    return &g_list[g_sel];
}

void AddHostPaths(const std::vector<std::string> &paths)
{
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = g_vol;
    std::string err;
    for (auto &p : paths) {
        if (!HostToXboxSteps(fs::u8path(p), g_path, job.get(), &err)) {
            SetStatus(err, true);
            return;
        }
    }
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && v->free_known && job->total > v->free) {
        SetStatus("Not enough space on " + std::string(1, g_vol) + ": (" +
                      FormatSize(job->total) + " needed, " +
                      FormatSize(v->free) + " free)",
                  true);
        return;
    }
    job->title = string_format("Copying %d file%s to %c:", job->files,
                               job->files == 1 ? "" : "s", g_vol);
    AskThenStart(std::move(job));
}

void ExportSelected(const std::string &host_dir)
{
    const XboxFileInfo *fi = Selected();
    if (!fi) {
        return;
    }
    auto job = std::make_unique<Job>();
    job->vol = g_vol;
    std::string err;
    if (!XboxToHostSteps(g_vol, XboxJoin(g_path, fi->name), *fi,
                         fs::u8path(host_dir), job.get(), &err)) {
        SetStatus(err, true);
        return;
    }
    job->title = "Copying \"" + fi->name + "\" to your PC";
    AskThenStart(std::move(job));
}

void DeleteSelected()
{
    const XboxFileInfo *fi = Selected();
    if (!fi) {
        return;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = g_vol;
    std::string err;
    if (!DeleteSteps(g_vol, XboxJoin(g_path, fi->name), *fi, job.get(), &err)) {
        SetStatus(err, true);
        return;
    }
    job->title = "Deleting \"" + fi->name + "\"";
    // Already confirmed by the delete dialog
    StartJob(std::move(job));
}

// Single quick changes (new folder, rename) run as a one-step write job so
// they get the same pause/restart handling
void SimpleWrite(const std::string &title, std::function<bool(std::string *)> op)
{
    g_paused_for_job = XboxIsRunning();
    XboxPause();
    Reopen();
    std::string err;
    bool ok = g_drive.IsOpen() && op(&err);
    g_drive.Flush();
    XboxRestart();
    g_paused_for_job = false;
    SetStatus(ok ? title + " - done. The Xbox restarted to load the change."
                 : (err.empty() ? g_open_err : err),
              !ok);
    g_list_dirty = true;
}

// Esc or B in a dialog: cancel it without also closing the dashboard
bool CancelPressed()
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        GameLibraryConsumeBack();
        return true;
    }
    return false;
}

char g_name_buf[64];
bool g_focus_name = false;

// ---- Drawing ---------------------------------------------------------------

void DrawVolumes()
{
    const float s = g_viewport_mgr.m_scale;
    for (auto &v : g_drive.Volumes()) {
        ImGui::PushID(v.letter);
        bool cur = v.letter == g_vol;
        std::string label =
            string_format(ICON_FA_HARD_DRIVE "  %c:  %s", v.letter, v.label);
        ImVec2 size(ImGui::CalcTextSize(label.c_str()).x + 28 * s, 0);
        if (cur) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label.c_str(), size) && !g_job) {
            Navigate(v.letter, "/");
        }
        if (cur) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered()) {
            if (v.free_known) {
                ImGui::SetTooltip("%s free of %s", FormatSize(v.free).c_str(),
                                  FormatSize(v.size).c_str());
            } else {
                ImGui::SetTooltip("%s", FormatSize(v.size).c_str());
            }
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::NewLine();
}

void DrawToolbar()
{
    const float s = g_viewport_mgr.m_scale;
    bool busy = g_job != nullptr;
    // Breadcrumbs: E: > Apps > Foo
    if (ImGui::SmallButton(string_format("%c:", g_vol).c_str()) && !busy) {
        Navigate(g_vol, "/");
    }
    std::string acc;
    size_t start = 1;
    while (start < g_path.size()) {
        size_t end = g_path.find('/', start);
        if (end == std::string::npos) {
            end = g_path.size();
        }
        std::string part = g_path.substr(start, end - start);
        acc += "/" + part;
        ImGui::SameLine(0, 4 * s);
        ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
        ImGui::SameLine(0, 4 * s);
        ImGui::PushID(acc.c_str());
        if (ImGui::SmallButton(part.c_str()) && !busy) {
            Navigate(g_vol, acc);
        }
        ImGui::PopID();
        start = end + 1;
    }
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && v->free_known) {
        std::string fr = FormatSize(v->free) + " free";
        ImGui::SameLine(ImGui::GetContentRegionMax().x -
                        ImGui::CalcTextSize(fr.c_str()).x);
        ImGui::TextDisabled("%s", fr.c_str());
    }

    ImGui::BeginDisabled(busy);
    const XboxFileInfo *sel = Selected();
    if (ImGui::Button(ICON_FA_ARROW_UP " Up") && g_path != "/") {
        Navigate(g_vol, XboxParent(g_path));
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE " Refresh")) {
        Reopen();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_PLUS " New folder")) {
        g_name_buf[0] = 0;
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-newfolder");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT " Add files")) {
        ShowOpenFilesDialog(nullptr, [](const std::vector<std::string> &p) {
            AddHostPaths(p);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Add folder")) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { AddHostPaths({ p }); });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Copy to PC")) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { ExportSelected(p); });
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PEN " Rename") && sel) {
        snprintf(g_name_buf, sizeof(g_name_buf), "%s", sel->name.c_str());
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-rename");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Delete")) {
        ImGui::OpenPopup("##xdrive-delete");
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void DrawList(float height)
{
    const float s = g_viewport_mgr.m_scale;
    if (g_list_dirty && !g_job) {
        LoadList();
    }
    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##xdrive-files", 3, flags, ImVec2(0, height))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 110 * s);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 160 * s);
    ImGui::TableHeadersRow();
    if (!g_list_err.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g_list_err.c_str());
    } else if (g_list.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("This folder is empty");
    }
    std::string open_dir;
    for (int i = 0; i < (int)g_list.size(); i++) {
        const XboxFileInfo &fi = g_list[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        std::string label = std::string(fi.dir ? ICON_FA_FOLDER : ICON_FA_FILE) +
                            "  " + fi.name;
        bool clicked = ImGui::Selectable(label.c_str(), g_sel == i,
                                         ImGuiSelectableFlags_SpanAllColumns |
                                             ImGuiSelectableFlags_AllowDoubleClick);
        if (clicked) {
            g_sel = i;
            bool mouse = ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (fi.dir && !g_job &&
                (!mouse || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                open_dir = XboxJoin(g_path, fi.name);
            }
        }
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
            g_sel = i;
        }
        ImGui::TableSetColumnIndex(1);
        if (!fi.dir) {
            ImGui::TextDisabled("%s", FormatSize(fi.size).c_str());
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("%s", FormatTime(fi.modified).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (!open_dir.empty()) {
        Navigate(g_vol, open_dir);
    }
}

void DrawFooter()
{
    const float s = g_viewport_mgr.m_scale;
    if (g_job) {
        Job &j = *g_job;
        float frac = j.total ? (float)((double)j.done / (double)j.total) : 0.0f;
        std::string cur =
            j.i < j.steps.size() ? j.steps[j.i].xpath : std::string();
        ImGui::ProgressBar(frac, ImVec2(-120 * s, 0), j.title.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
            j.cancel = true;
        }
        ImGui::TextDisabled("%s", cur.c_str());
        return;
    }
    if (!g_status.empty()) {
        if (g_status_error) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), ICON_FA_TRIANGLE_EXCLAMATION
                               "  %s", g_status.c_str());
        } else {
            ImGui::TextDisabled("%s", g_status.c_str());
        }
    } else if (XboxIsRunning()) {
        ImGui::TextDisabled("Copying to the Xbox pauses it and restarts it when done.");
    } else {
        ImGui::TextDisabled("Double-click a folder to open it.");
    }
}

bool NamePopup(const char *id, const char *title, const char *button,
               std::string *out)
{
    bool done = false;
    if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(title);
        if (g_focus_name) {
            ImGui::SetKeyboardFocusHere();
            g_focus_name = false;
        }
        bool enter = ImGui::InputText("##name", g_name_buf, sizeof(g_name_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        std::string why;
        bool valid = XboxValidName(g_name_buf, &why);
        if (!valid && g_name_buf[0]) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", why.c_str());
        }
        ImGui::TextDisabled("The Xbox pauses and restarts to make the change.");
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(button) || (enter && valid)) {
            *out = g_name_buf;
            done = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return done;
}

void DrawPopups()
{
    std::string name;
    if (NamePopup("##xdrive-newfolder", "New folder name:", "Create", &name)) {
        std::string path = XboxJoin(g_path, name);
        char vol = g_vol;
        SimpleWrite("New folder", [vol, path](std::string *err) {
            return g_drive.MakeDir(vol, path, err);
        });
    }
    if (NamePopup("##xdrive-rename", "New name:", "Rename", &name)) {
        const XboxFileInfo *fi = Selected();
        if (fi && fi->name != name) {
            std::string from = XboxJoin(g_path, fi->name);
            std::string to = XboxJoin(g_path, name);
            char vol = g_vol;
            SimpleWrite("Rename", [vol, from, to](std::string *err) {
                return g_drive.Rename(vol, from, to, err);
            });
        }
    }
    if (ImGui::BeginPopupModal("##xdrive-delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const XboxFileInfo *fi = Selected();
        ImGui::Text("Delete \"%s\"%s from %c:?", fi ? fi->name.c_str() : "",
                    fi && fi->dir ? " and everything in it" : "", g_vol);
        ImGui::TextDisabled("This can't be undone. The Xbox pauses and "
                            "restarts to make the change.");
        if (ImGui::Button(ICON_FA_TRASH " Delete")) {
            ImGui::CloseCurrentPopup();
            DeleteSelected();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (g_pending_write && !ImGui::IsPopupOpen("##xdrive-confirm-write")) {
        ImGui::OpenPopup("##xdrive-confirm-write");
    }
    if (ImGui::BeginPopupModal("##xdrive-confirm-write", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", g_pending_write ? g_pending_write->title.c_str() : "");
        if (g_pending_write) {
            ImGui::TextDisabled("%s", FormatSize(g_pending_write->total).c_str());
        }
        ImGui::Spacing();
        ImGui::TextWrapped("The Xbox pauses while the files are written and "
                           "restarts when they're done. Progress in a running "
                           "game that hasn't been saved will be lost.");
        if (ImGui::Button("Copy to the Xbox")) {
            ImGui::CloseCurrentPopup();
            if (g_pending_write) {
                StartJob(std::move(g_pending_write));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || CancelPressed()) {
            g_pending_write.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void XboxExplorerUpdate(void)
{
    if (!g_job) {
        return;
    }
    // About 12 ms of work per frame (the step limit only matters if the
    // clock is coarse)
    uint64_t start = SDL_GetTicks();
    for (int n = 0; n < 256 && SDL_GetTicks() - start < 12; n++) {
        if (!RunStep(*g_job)) {
            FinishJob();
            return;
        }
    }
}

bool XboxExplorerBusy(void)
{
    return g_job != nullptr;
}

bool XboxExplorerInstallFolder(const std::string &host_dir, char vol,
                               const std::string &parent, const std::string &name,
                               const std::string &title, std::string *err)
{
    if (g_job) {
        *err = "Another copy to the Xbox is still running";
        return false;
    }
    std::string why;
    if (!XboxValidName(name, &why)) {
        *err = "\"" + name + "\": " + why;
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? "Can't open the Xbox hard drive" : g_open_err;
        return false;
    }
    if (!g_drive.Volume(vol)) {
        *err = std::string("This hard disk image has no ") + vol + ": drive";
        return false;
    }
    if (!XboxDiskEmulatedWritable()) {
        *err = "The hard disk image is read-only";
        return false;
    }
    // Plan everything before pausing, so a problem leaves the Xbox running
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = vol;
    job->title = title;
    // The Xbox ignores case but libfatx doesn't: reuse "apps" if that's
    // what's there rather than making a second "Apps"
    auto same_case = [vol](const std::string &dir, const std::string &leaf) {
        std::vector<XboxFileInfo> list;
        std::string e, want = leaf;
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        if (g_drive.List(vol, dir, &list, &e)) {
            for (auto &f : list) {
                std::string n = f.name;
                std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                if (n == want) {
                    return f.name;
                }
            }
        }
        return leaf;
    };
    std::string parent_path = XboxParent(parent);
    std::string resolved_parent =
        parent == "/" ? parent
                      : XboxJoin(parent_path,
                                 same_case(parent_path,
                                           parent.substr(parent.find_last_of('/') + 1)));
    std::string resolved_name = same_case(resolved_parent, name);
    std::string target = XboxJoin(resolved_parent, resolved_name);
    XboxFileInfo fi;
    if (!g_drive.Stat(vol, resolved_parent, &fi)) {
        job->steps.push_back({ Step::XMkDir, "", resolved_parent, 0 });
    } else if (!fi.dir) {
        *err = parent + " on " + std::string(1, vol) + ": is a file";
        return false;
    } else if (g_drive.Stat(vol, target, &fi)) {
        // An update: the old version goes first
        fi.name = resolved_name;
        if (!DeleteSteps(vol, target, fi, job.get(), err)) {
            return false;
        }
    }
    job->steps.push_back({ Step::XMkDir, host_dir, target, 0 });
    std::error_code ec;
    std::vector<fs::path> children;
    for (auto &e : fs::directory_iterator(fs::u8path(host_dir), ec)) {
        children.push_back(e.path());
    }
    std::sort(children.begin(), children.end());
    if (children.empty()) {
        *err = "There's nothing to copy";
        return false;
    }
    for (auto &c : children) {
        if (!HostToXboxSteps(c, target, job.get(), err)) {
            return false;
        }
    }
    const XboxVolume *v = g_drive.Volume(vol);
    if (v && !v->free_known) {
        g_drive.UpdateFreeSpace(vol);
        v = g_drive.Volume(vol);
    }
    if (v && v->free_known && job->total > v->free) {
        *err = "Not enough space on " + std::string(1, vol) + ": (" +
               FormatSize(job->total) + " needed, " + FormatSize(v->free) +
               " free)";
        return false;
    }
    // Planned against the disk as it was; StartJob re-reads it after pausing
    StartJob(std::move(job));
    if (!g_job) {
        *err = g_status;
        return false;
    }
    // Show where it went (not Navigate: that would re-read the disk mid-job)
    g_vol = vol;
    g_path = resolved_parent;
    g_list_dirty = true;
    return true;
}

namespace
{
// Folder tree on the Xbox drive, for a disc image; *sig summarizes it
bool XisoTree(char vol, const std::string &path, XisoNode *node, uint64_t *sig,
              int depth, std::string *err)
{
    if (depth > 32) {
        *err = "Folders nested too deeply";
        return false;
    }
    std::vector<XboxFileInfo> list;
    if (!g_drive.List(vol, path, &list, err)) {
        return false;
    }
    for (auto &fi : list) {
        XisoNode k;
        k.name = fi.name;
        k.dir = fi.dir;
        k.size = fi.dir ? 0 : fi.size;
        k.source = XboxJoin(path, fi.name);
        std::string key = k.source + "|" + std::to_string(k.size) + "|" +
                          std::to_string((long long)fi.modified) + "\n";
        for (unsigned char c : key) {
            *sig = (*sig ^ c) * 1099511628211ull;
        }
        if (fi.dir && !XisoTree(vol, k.source, &k, sig, depth + 1, err)) {
            return false;
        }
        node->kids.push_back(std::move(k));
    }
    return true;
}

std::string ReadSmallHostFile(const std::string &path)
{
    std::string out;
    FILE *f = OpenHost(path, "rb");
    if (f) {
        char b[256];
        size_t n = fread(b, 1, sizeof(b), f);
        out.assign(b, n);
        fclose(f);
    }
    return out;
}

std::string Utf16Le(const uint8_t *p, size_t max_chars)
{
    std::string out;
    for (size_t i = 0; i < max_chars; i++) {
        unsigned c = p[i * 2] | (p[i * 2 + 1] << 8);
        if (!c) {
            break;
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0xD800 || c > 0xDFFF) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Title name and ID from an XBE's certificate
void ReadXbeTitle(char vol, const std::string &xbe, XboxHomebrewApp *app)
{
    uint8_t hdr[0x180];
    if (g_drive.ReadAt(vol, xbe, 0, hdr, sizeof(hdr)) != (int64_t)sizeof(hdr) ||
        memcmp(hdr, "XBEH", 4) != 0) {
        return;
    }
    auto le32 = [](const uint8_t *p) {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
               (uint32_t)p[3] << 24;
    };
    uint32_t base = le32(hdr + 0x104), cert = le32(hdr + 0x118);
    if (cert < base || cert - base > 0x100000) {
        return;
    }
    uint8_t c[0x5C];
    if (g_drive.ReadAt(vol, xbe, cert - base, c, sizeof(c)) != (int64_t)sizeof(c)) {
        return;
    }
    app->title_id = le32(c + 8);
    std::string t = Utf16Le(c + 0x0C, 40);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) {
        t.pop_back();
    }
    if (!t.empty()) {
        app->title = t;
    }
}
} // namespace

bool XboxExplorerMakeXiso(char vol, const std::string &dir,
                          const std::string &iso_path, bool *ready,
                          std::string *err)
{
    *ready = false;
    if (g_job) {
        *err = "The Xbox hard drive is busy with another copy";
        return false;
    }
    if (XboxIsRunning() && g_drive.IsOpen()) {
        Reopen(); // the running Xbox may have changed files
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? "Can't open the Xbox hard drive" : g_open_err;
        return false;
    }
    XisoNode root;
    root.dir = true;
    uint64_t sig = 1469598103934665603ull;
    if (!XisoTree(vol, dir, &root, &sig, 0, err)) {
        return false;
    }
    char sigs[32];
    snprintf(sigs, sizeof(sigs), "%016llx", (unsigned long long)sig);
    std::error_code ec;
    if (fs::exists(fs::u8path(iso_path), ec) &&
        ReadSmallHostFile(iso_path + ".sig") == sigs) {
        *ready = true;
        return true;
    }
    XisoPlan plan;
    if (!XisoLayout(root, &plan, err)) {
        return false;
    }
    auto job = std::make_unique<Job>();
    job->vol = vol;
    job->title = "Preparing " + dir.substr(dir.find_last_of('/') + 1);
    job->iso_final = iso_path;
    job->iso_part = iso_path + ".part";
    job->iso_sig = sigs;
    fs::create_directories(fs::u8path(iso_path).parent_path(), ec);
    job->iso = OpenHost(job->iso_part, "wb+");
    if (!job->iso) {
        *err = "Can't write \"" + job->iso_part + "\"";
        return false;
    }
    bool ok = true;
    for (auto &b : plan.blobs) {
        ok = ok && SeekHost(job->iso, b.first) &&
             fwrite(b.second.data(), 1, b.second.size(), job->iso) == b.second.size();
    }
    // Full size up front (the end of the image is padding)
    ok = ok && SeekHost(job->iso, plan.image_size - 1) && fputc(0, job->iso) != EOF;
    if (!ok) {
        fclose(job->iso);
        fs::remove(fs::u8path(job->iso_part), ec);
        *err = "Couldn't write the disc image (is the disk full?)";
        return false;
    }
    for (const XisoNode *f : plan.files) {
        Step st{ Step::IsoFile, "", f->source, f->size };
        st.dest = (uint64_t)f->sector * kXisoSectorSize;
        job->steps.push_back(st);
        job->total += f->size;
        job->files++;
    }
    if (job->steps.empty()) {
        // Nothing but empty files: done already
        Step st{ Step::IsoFile, "", "", 0 };
        job->steps.push_back(st);
    }
    StartJob(std::move(job));
    return g_job != nullptr;
}

bool XboxExplorerFindHomebrew(std::vector<XboxHomebrewApp> *out, std::string *err)
{
    out->clear();
    if (g_job) {
        *err = "The Xbox hard drive is busy";
        return false;
    }
    if (XboxIsRunning() && g_drive.IsOpen()) {
        Reopen();
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? "Can't open the Xbox hard drive" : g_open_err;
        return false;
    }
    static const char *const kSections[] = { "Games", "Apps", "Emulators" };
    for (char vol : { 'E', 'F' }) {
        if (!g_drive.Volume(vol)) {
            continue;
        }
        std::vector<XboxFileInfo> top;
        std::string e;
        if (!g_drive.List(vol, "/", &top, &e)) {
            continue;
        }
        for (auto &t : top) {
            const char *section = nullptr;
            for (const char *s : kSections) {
                std::string a = t.name, b = s;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (t.dir && a == b) {
                    section = s;
                }
            }
            if (!section) {
                continue;
            }
            std::vector<XboxFileInfo> apps;
            if (!g_drive.List(vol, "/" + t.name, &apps, &e)) {
                continue;
            }
            for (auto &a : apps) {
                if (!a.dir) {
                    continue;
                }
                XboxHomebrewApp app;
                app.vol = vol;
                app.dir = "/" + t.name + "/" + a.name;
                app.folder = a.name;
                app.section = section;
                app.title = a.name;
                // default.xbe, whatever its case
                std::vector<XboxFileInfo> files;
                std::string xbe;
                if (g_drive.List(vol, app.dir, &files, &e)) {
                    for (auto &f : files) {
                        std::string n = f.name;
                        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                        if (!f.dir && n == "default.xbe") {
                            xbe = XboxJoin(app.dir, f.name);
                            app.xbe_size = f.size;
                        }
                    }
                }
                if (xbe.empty()) {
                    continue;
                }
                ReadXbeTitle(vol, xbe, &app);
                out->push_back(app);
            }
        }
    }
    std::sort(out->begin(), out->end(),
              [](const XboxHomebrewApp &a, const XboxHomebrewApp &b) {
                  std::string x = a.title, y = b.title;
                  std::transform(x.begin(), x.end(), x.begin(), ::tolower);
                  std::transform(y.begin(), y.end(), y.begin(), ::tolower);
                  return x < y;
              });
    return true;
}

std::string XboxExplorerStatus(bool *is_error)
{
    if (is_error) {
        *is_error = g_status_error;
    }
    return g_status;
}

float XboxExplorerProgress(void)
{
    if (!g_job || !g_job->total) {
        return 0;
    }
    return std::min(1.0f, (float)((double)g_job->done / g_job->total));
}

void XboxExplorerDraw(void)
{
    const float s = g_viewport_mgr.m_scale;
    ImGui::PushID("xbox-drive");
    if (!EnsureOpen()) {
        ImGui::Dummy(ImVec2(0, 20 * s));
        ImGui::TextUnformatted(ICON_FA_HARD_DRIVE "  Xbox hard drive");
        ImGui::TextDisabled("%s", g_open_err.empty() ? "Opening..." : g_open_err.c_str());
        if (ImGui::Button("Try again")) {
            g_open_tried = 0;
        }
        ImGui::PopID();
        return;
    }
    // Free space of the current drive, counted once per opening
    const XboxVolume *v = g_drive.Volume(g_vol);
    if (v && !v->free_known && !g_job && ImGui::GetFrameCount() > 1) {
        g_drive.UpdateFreeSpace(g_vol);
    }
    DrawVolumes();
    DrawToolbar();
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    DrawList(ImGui::GetContentRegionAvail().y - footer);
    DrawFooter();
    DrawPopups();
    ImGui::PopID();
}

void XboxExplorerOpenWindow(void)
{
    g_window_open = true;
    ImGui::SetWindowFocus("Xbox Hard Drive");
}

void XboxExplorerWindowDraw(void)
{
    if (!g_window_open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(820 * s, 560 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Xbox Hard Drive", &g_window_open)) {
        XboxExplorerDraw();
    }
    ImGui::End();
}
