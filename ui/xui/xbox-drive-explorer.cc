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
#include <map>
#include <memory>

#include "common.hh"
#include "osk.hh"
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
bool g_open_saves = false; // open the Game saves window

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
        *err = string_format(tr("Can't read \"%s\""), src.u8string().c_str());
        return false;
    }
    if (size > 0xFFFFFFFFull) {
        *err = string_format(tr("\"%s\" is larger than 4 GB, the Xbox's file size limit"),
                             name.c_str());
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
                j.err = tr("Couldn't save the disc image");
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
        if (j.i > 0) {
            // Remembered so loading an older save state asks first
            GameLibraryNoteDriveChange(j.title);
        }
    }
    if (!j.err.empty()) {
        SetStatus(j.err, true);
    } else if (j.cancel) {
        SetStatus(tr("Cancelled"));
    } else {
        SetStatus(j.title + " - " + tr("done.") +
                  (j.writes ? std::string(" ") + tr("The Xbox restarted to load the changes.")
                            : std::string()));
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
            j.err = string_format(tr("Can't create \"%s\""), s.host.c_str());
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
                j.err = string_format(tr("Can't read \"%s\""), s.host.c_str());
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
            j.err = string_format(tr("Couldn't copy %s into the disc image"),
                                  s.xpath.c_str());
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
                j.err = string_format(tr("Can't write \"%s\""), s.host.c_str());
                return false;
            }
            j.off = 0;
        }
        size_t want = (size_t)std::min<uint64_t>(j.buf.size(), s.size - j.off);
        int64_t n = want ? g_drive.ReadAt(j.vol, s.xpath, j.off, j.buf.data(), want)
                         : 0;
        if (n < 0 || (want && n == 0) ||
            (n > 0 && fwrite(j.buf.data(), 1, (size_t)n, j.f) != (size_t)n)) {
            j.err = string_format(tr("Couldn't copy %s"), s.xpath.c_str());
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
        SetStatus(string_format(tr("Not enough space on %c: (%s needed, %s free)"), g_vol,
                                FormatSize(job->total).c_str(),
                                FormatSize(v->free).c_str()),
                  true);
        return;
    }
    job->title = string_format(tr("Copying files to %c: (%d)"), g_vol, job->files);
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
    job->title = string_format(tr("Copying \"%s\" to your PC"), fi->name.c_str());
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
    job->title = string_format(tr("Deleting \"%s\""), fi->name.c_str());
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
    if (ok) {
        GameLibraryNoteDriveChange(title);
    }
    SetStatus(ok ? title + " - " + tr("done. The Xbox restarted to load the change.")
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
                ImGui::SetTooltip(tr("%s free of %s"), FormatSize(v.free).c_str(),
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
        std::string fr = string_format(tr("%s free"), FormatSize(v->free).c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x -
                        ImGui::CalcTextSize(fr.c_str()).x);
        ImGui::TextDisabled("%s", fr.c_str());
    }

    ImGui::BeginDisabled(busy);
    const XboxFileInfo *sel = Selected();
    if (ImGui::Button(tr(ICON_FA_ARROW_UP " Up")) && g_path != "/") {
        Navigate(g_vol, XboxParent(g_path));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_ROTATE " Refresh"))) {
        Reopen();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_PLUS " New folder"))) {
        g_name_buf[0] = 0;
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-newfolder");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FILE_IMPORT " Add files"))) {
        ShowOpenFilesDialog(nullptr, [](const std::vector<std::string> &p) {
            AddHostPaths(p);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN " Add folder"))) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { AddHostPaths({ p }); });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(tr(ICON_FA_FILE_EXPORT " Copy to PC"))) {
        ShowOpenFolderDialog(nullptr, [](const char *p) { ExportSelected(p); });
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_PEN " Rename")) && sel) {
        snprintf(g_name_buf, sizeof(g_name_buf), "%s", sel->name.c_str());
        g_focus_name = true;
        ImGui::OpenPopup("##xdrive-rename");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_TRASH " Delete"))) {
        ImGui::OpenPopup("##xdrive-delete");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK " Game saves"))) {
        g_open_saves = true;
    }
    ImGui::SetItemTooltip("%s", tr("Back up the Xbox's game saves to your PC, or put "
                                   "a backup back"));
    ImGui::EndDisabled();
}

// "4D530003" -> "Project Gotham Racing" for folders named by title ID
std::string TitleFolderName(const XboxFileInfo &fi)
{
    if (!fi.dir || fi.name.size() != 8) {
        return "";
    }
    for (char c : fi.name) {
        if (!isxdigit((unsigned char)c)) {
            return "";
        }
    }
    static std::map<std::string, std::string> cache;
    auto it = cache.find(fi.name);
    if (it != cache.end()) {
        return it->second;
    }
    std::string name =
        GameLibraryTitleName((uint32_t)strtoul(fi.name.c_str(), nullptr, 16));
    cache[fi.name] = name;
    return name;
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
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 110 * s);
    ImGui::TableSetupColumn(tr("Modified"), ImGuiTableColumnFlags_WidthFixed, 160 * s);
    ImGui::TableHeadersRow();
    if (!g_list_err.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g_list_err.c_str());
    } else if (g_list.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(tr("This folder is empty"));
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
        // Save and cache folders are named by title ID: show the game
        std::string game = TitleFolderName(fi);
        if (!game.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("  %s", game.c_str());
        }
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
        if (ImGui::Button(tr("Cancel"), ImVec2(-1, 0))) {
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
        ImGui::TextDisabled(tr("Copying to the Xbox pauses it and restarts it when done."));
    } else {
        ImGui::TextDisabled(tr("Double-click a folder to open it."));
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
        OskTextBox();
        std::string why;
        bool valid = XboxValidName(g_name_buf, &why);
        if (!valid && g_name_buf[0]) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", why.c_str());
        }
        ImGui::TextDisabled(tr("The Xbox pauses and restarts to make the change."));
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(button) || (enter && valid)) {
            *out = g_name_buf;
            done = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return done;
}

// ---- Game saves window ----

struct SaveBackup {
    std::string name, path;
    uint64_t bytes = 0;
    int files = 0;
};
std::vector<SaveBackup> g_backups;
std::string g_restore_path; // asking to restore this one

void ListBackups()
{
    g_backups.clear();
    std::error_code ec;
    fs::path dir = fs::u8path(GameLibrarySaveBackupDir());
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) {
            continue;
        }
        SaveBackup b;
        b.name = e.path().filename().u8string();
        b.path = e.path().u8string();
        for (auto &f : fs::recursive_directory_iterator(e.path(), ec)) {
            if (f.is_regular_file(ec)) {
                b.bytes += f.file_size(ec);
                b.files++;
            }
        }
        g_backups.push_back(b);
    }
    // Named by date and time, so newest first is reverse order
    std::sort(g_backups.begin(), g_backups.end(),
              [](const SaveBackup &a, const SaveBackup &b) { return a.name > b.name; });
}

void OpenHostFolder(const std::string &path)
{
    std::string url = "file:///" + fs::u8path(path).generic_u8string();
    SDL_OpenURL(url.c_str());
}

void DrawSavesPopup()
{
    const float s = g_viewport_mgr.m_scale;
    if (g_open_saves) {
        g_open_saves = false;
        ListBackups();
        ImGui::OpenPopup(trid("Game saves"));
    }
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(trid("Game saves"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::PushTextWrapPos(540 * s);
    ImGui::TextUnformatted(tr("Copies the Xbox's game saves (E:\\UDATA and E:\\TDATA) to "
                              "a folder on your PC, so you can get them back if a save "
                              "or the hard disk image goes wrong."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * s));
    bool busy = XboxExplorerBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr(ICON_FA_FLOPPY_DISK "  Back up now"))) {
        char stamp[32];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", tm ? tm : gmtime(&now));
        std::string err;
        std::string dest = (fs::u8path(GameLibrarySaveBackupDir()) / stamp).u8string();
        if (!XboxExplorerBackupSaves(dest, &err)) {
            SetStatus(err, true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN "  Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(fs::u8path(GameLibrarySaveBackupDir()), ec);
        OpenHostFolder(GameLibrarySaveBackupDir());
    }
    // Progress and the result of the last backup or restore
    bool status_err = false;
    std::string status = XboxExplorerStatus(&status_err);
    if (busy) {
        ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(-FLT_MIN, 0), status.c_str());
    } else if (!status.empty()) {
        ImGui::TextColored(status_err ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(0.6f, 0.9f, 0.5f, 1),
                           "%s", status.c_str());
    }
    static bool was_busy = false;
    if (was_busy && !busy) {
        ListBackups(); // a backup just finished
    }
    was_busy = busy;

    ImGui::Separator();
    if (g_backups.empty()) {
        ImGui::TextDisabled(tr("No backups yet."));
    } else {
        ImGui::TextDisabled(tr("Backups (newest first)"));
        float rows = ImGui::GetFrameHeightWithSpacing() * g_backups.size();
        ImGui::BeginChild("##backups", ImVec2(0, std::min(260 * s, rows + 4 * s)),
                          ImGuiChildFlags_NavFlattened);
        for (auto &b : g_backups) {
            ImGui::PushID(b.path.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(tr("%d files, %s"), b.files, FormatSize(b.bytes).c_str());
            float bw = ImGui::CalcTextSize(tr("Restore")).x + ImGui::CalcTextSize(tr("Show")).x +
                       ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Restore"))) {
                g_restore_path = b.path;
                ImGui::OpenPopup("##restore-saves");
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Show"))) {
                OpenHostFolder(b.path);
            }
            if (ImGui::BeginPopupModal("##restore-saves", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text(tr("Put back the game saves from %s?"),
                            fs::u8path(g_restore_path).filename().u8string().c_str());
                ImGui::PushTextWrapPos(460 * s);
                ImGui::TextDisabled("%s", tr("The saves on the Xbox are replaced by the "
                                             "backup: anything saved since then is lost. "
                                             "Back up first if you're not sure. The Xbox "
                                             "pauses and restarts, so save your game "
                                             "first."));
                ImGui::PopTextWrapPos();
                if (ImGui::Button(tr("Restore"))) {
                    std::string err;
                    if (!XboxExplorerRestoreSaves(g_restore_path, &err)) {
                        SetStatus(err, true);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(tr("Cancel")) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Close")) ||
        (!ImGui::IsPopupOpen("##restore-saves", ImGuiPopupFlags_AnyPopupId) &&
         ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawPopups()
{
    DrawSavesPopup();
    std::string name;
    if (NamePopup("##xdrive-newfolder", tr("New folder name:"), tr("Create"), &name)) {
        std::string path = XboxJoin(g_path, name);
        char vol = g_vol;
        SimpleWrite(tr("New folder"), [vol, path](std::string *err) {
            return g_drive.MakeDir(vol, path, err);
        });
    }
    if (NamePopup("##xdrive-rename", tr("New name:"), tr("Rename"), &name)) {
        const XboxFileInfo *fi = Selected();
        if (fi && fi->name != name) {
            std::string from = XboxJoin(g_path, fi->name);
            std::string to = XboxJoin(g_path, name);
            char vol = g_vol;
            SimpleWrite(tr("Rename"), [vol, from, to](std::string *err) {
                return g_drive.Rename(vol, from, to, err);
            });
        }
    }
    if (ImGui::BeginPopupModal("##xdrive-delete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const XboxFileInfo *fi = Selected();
        ImGui::Text(tr("Delete \"%s\"%s from %c:?"), fi ? fi->name.c_str() : "",
                    fi && fi->dir ? tr(" and everything in it") : "", g_vol);
        ImGui::TextDisabled(tr("This can't be undone. The Xbox pauses and "
                            "restarts to make the change."));
        if (ImGui::Button(tr(ICON_FA_TRASH " Delete"))) {
            ImGui::CloseCurrentPopup();
            DeleteSelected();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
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
        ImGui::TextWrapped(tr("The Xbox pauses while the files are written and "
                           "restarts when they're done. Progress in a running "
                           "game that hasn't been saved will be lost."));
        if (ImGui::Button(tr("Copy to the Xbox"))) {
            ImGui::CloseCurrentPopup();
            if (g_pending_write) {
                StartJob(std::move(g_pending_write));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || CancelPressed()) {
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

void XboxExplorerShowFolder(char vol, const std::string &path)
{
    if (g_job) {
        return;
    }
    Navigate(vol, path);
}

bool XboxExplorerInstallFolder(const std::string &host_dir, char vol,
                               const std::string &parent, const std::string &name,
                               const std::string &title, std::string *err)
{
    if (g_job) {
        *err = tr("Another copy to the Xbox is still running");
        return false;
    }
    std::string why;
    if (!XboxValidName(name, &why)) {
        *err = "\"" + name + "\": " + why;
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    if (!g_drive.Volume(vol)) {
        *err = string_format(tr("This hard disk image has no %c: drive"), vol);
        return false;
    }
    if (!XboxDiskEmulatedWritable()) {
        *err = tr("The hard disk image is read-only");
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
        *err = string_format(tr("%s on %c: is a file"), parent.c_str(), vol);
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
        *err = tr("There's nothing to copy");
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
        *err = string_format(tr("Not enough space on %c: (%s needed, %s free)"), vol,
                             FormatSize(job->total).c_str(), FormatSize(v->free).c_str());
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
        *err = tr("Folders nested too deeply");
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

// Title name, ID and title image from an XBE
void ReadXbeTitle(char vol, const std::string &xbe, XboxHomebrewApp *app)
{
    auto le32 = [](const uint8_t *p) {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
               (uint32_t)p[3] << 24;
    };
    uint8_t h0[0x180];
    if (g_drive.ReadAt(vol, xbe, 0, h0, sizeof(h0)) != (int64_t)sizeof(h0) ||
        memcmp(h0, "XBEH", 4) != 0) {
        return;
    }
    uint32_t base = le32(h0 + 0x104), hsize = le32(h0 + 0x108);
    if (hsize < sizeof(h0) || hsize > 4 * 1024 * 1024 || hsize > app->xbe_size) {
        // Odd header size: the certificate is still near the start
        hsize = (uint32_t)std::min<uint64_t>(app->xbe_size, 0x10000);
    }
    std::vector<uint8_t> hdr(hsize);
    if (g_drive.ReadAt(vol, xbe, 0, hdr.data(), hsize) != (int64_t)hsize) {
        return;
    }
    uint32_t cert = le32(&hdr[0x118]);
    if (cert < base || (uint64_t)cert - base + 0x5C > hsize) {
        return;
    }
    const uint8_t *c = &hdr[cert - base];
    app->title_id = le32(c + 8);
    std::string t = Utf16Le(c + 0x0C, 40);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) {
        t.pop_back();
    }
    if (!t.empty()) {
        app->title = t;
    }
    // $$XTIMAGE section: the title image dashboards show
    uint32_t n = le32(&hdr[0x11C]), secs = le32(&hdr[0x120]);
    if (secs < base || n > 1024 || (uint64_t)secs - base + (uint64_t)n * 56 > hsize) {
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = &hdr[secs - base + i * 56];
        uint32_t raw = le32(s + 12), size = le32(s + 16), name = le32(s + 20);
        const char want[] = "$$XTIMAGE";
        if (name < base || (uint64_t)name - base + sizeof(want) > hsize ||
            memcmp(&hdr[name - base], want, sizeof(want)) != 0) {
            continue;
        }
        if (size && size < 4 * 1024 * 1024 && (uint64_t)raw + size <= app->xbe_size) {
            app->title_image.resize(size);
            if (g_drive.ReadAt(vol, xbe, raw, app->title_image.data(), size) !=
                (int64_t)size) {
                app->title_image.clear();
            }
        }
        break;
    }
}
} // namespace

// ---- Game save backups -------------------------------------------------------

namespace
{
const char *const kSaveFolders[] = { "UDATA", "TDATA" };

bool SameNoCase(const std::string &a, const char *b)
{
    size_t n = strlen(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

// The name a folder has on E: ("UDATA", "udata"...), or ""
std::string SaveFolderOnXbox(const std::vector<XboxFileInfo> &root, const char *want)
{
    for (auto &f : root) {
        if (f.dir && SameNoCase(f.name, want)) {
            return f.name;
        }
    }
    return "";
}
} // namespace

bool XboxExplorerBackupSaves(const std::string &host_dir, std::string *err)
{
    if (g_job) {
        *err = tr("Another copy is still running");
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    std::vector<XboxFileInfo> root;
    if (!g_drive.List('E', "/", &root, err)) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(host_dir), ec);
    if (ec) {
        *err = string_format(tr("Can't make the folder %s"), host_dir.c_str());
        return false;
    }
    auto job = std::make_unique<Job>();
    job->vol = 'E';
    for (const char *want : kSaveFolders) {
        std::string name = SaveFolderOnXbox(root, want);
        if (name.empty()) {
            continue;
        }
        XboxFileInfo fi;
        for (auto &f : root) {
            if (f.name == name) {
                fi = f;
            }
        }
        if (!XboxToHostSteps('E', XboxJoin("/", name), fi, fs::u8path(host_dir), job.get(),
                             err)) {
            return false;
        }
    }
    if (job->steps.empty()) {
        fs::remove(fs::u8path(host_dir), ec);
        *err = tr("There are no game saves on this Xbox hard drive yet");
        return false;
    }
    job->title = tr("Backing up game saves");
    StartJob(std::move(job));
    return true;
}

bool XboxExplorerRestoreSaves(const std::string &host_dir, std::string *err)
{
    if (g_job) {
        *err = tr("Another copy is still running");
        return false;
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    if (!XboxDiskEmulatedWritable()) {
        *err = tr("The hard disk image is read-only");
        return false;
    }
    std::vector<XboxFileInfo> root;
    if (!g_drive.List('E', "/", &root, err)) {
        return false;
    }
    // A backup with no files in it would only wipe the saves: refuse
    std::error_code ec;
    int files = 0;
    for (auto &e : fs::recursive_directory_iterator(fs::u8path(host_dir), ec)) {
        files += e.is_regular_file(ec) ? 1 : 0;
    }
    if (!files) {
        *err = tr("This backup has no game saves in it");
        return false;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = 'E';
    for (const char *want : kSaveFolders) {
        fs::path src;
        for (auto &e : fs::directory_iterator(fs::u8path(host_dir), ec)) {
            if (e.is_directory(ec) &&
                SameNoCase(e.path().filename().u8string(), want)) {
                src = e.path();
            }
        }
        if (src.empty()) {
            continue;
        }
        // The folder on the Xbox goes first, then the backup's copy
        std::string name = SaveFolderOnXbox(root, want);
        if (!name.empty()) {
            XboxFileInfo fi;
            for (auto &f : root) {
                if (f.name == name) {
                    fi = f;
                }
            }
            if (!DeleteSteps('E', XboxJoin("/", name), fi, job.get(), err)) {
                return false;
            }
        }
        if (!HostToXboxSteps(src, "/", job.get(), err)) {
            return false;
        }
    }
    if (job->steps.empty()) {
        *err = tr("This backup has no game saves in it");
        return false;
    }
    job->title = tr("Restoring game saves");
    StartJob(std::move(job));
    if (!g_job) {
        *err = g_status;
        return false;
    }
    return true;
}

bool XboxExplorerMakeXiso(char vol, const std::string &dir,
                          const std::string &iso_path, bool *ready,
                          std::string *err)
{
    *ready = false;
    if (g_job) {
        *err = tr("The Xbox hard drive is busy with another copy");
        return false;
    }
    if (XboxIsRunning() && g_drive.IsOpen()) {
        Reopen(); // the running Xbox may have changed files
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
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
    job->title = string_format(tr("Preparing %s"),
                               dir.substr(dir.find_last_of('/') + 1).c_str());
    job->iso_final = iso_path;
    job->iso_part = iso_path + ".part";
    job->iso_sig = sigs;
    fs::create_directories(fs::u8path(iso_path).parent_path(), ec);
    job->iso = OpenHost(job->iso_part, "wb+");
    if (!job->iso) {
        *err = string_format(tr("Can't write \"%s\""), job->iso_part.c_str());
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
        *err = tr("Couldn't write the disc image (is the disk full?)");
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

bool XboxExplorerDeleteFolder(char vol, const std::string &dir,
                              const std::string &title, std::string *err)
{
    if (g_job) {
        *err = tr("The Xbox hard drive is busy with another copy");
        return false;
    }
    if (XboxIsRunning() && g_drive.IsOpen()) {
        Reopen();
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
        return false;
    }
    if (!XboxDiskEmulatedWritable()) {
        *err = tr("The hard disk image is read-only");
        return false;
    }
    XboxFileInfo fi;
    if (dir.empty() || dir == "/" || !g_drive.Stat(vol, dir, &fi) || !fi.dir) {
        *err = tr("It's no longer on the drive");
        return false;
    }
    auto job = std::make_unique<Job>();
    job->writes = true;
    job->vol = vol;
    job->title = string_format(tr("Deleting %s"), title.c_str());
    if (!DeleteSteps(vol, dir, fi, job.get(), err)) {
        return false;
    }
    StartJob(std::move(job));
    if (!g_job) {
        *err = g_status;
        return false;
    }
    return true;
}

bool XboxExplorerFindHomebrew(std::vector<XboxHomebrewApp> *out, std::string *err)
{
    out->clear();
    if (g_job) {
        *err = tr("The Xbox hard drive is busy");
        return false;
    }
    if (XboxIsRunning() && g_drive.IsOpen()) {
        Reopen();
    }
    if (!EnsureOpen()) {
        *err = g_open_err.empty() ? std::string(tr("Can't open the Xbox hard drive")) : g_open_err;
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
                app.installed = (int64_t)a.modified;
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
        ImGui::TextUnformatted(tr(ICON_FA_HARD_DRIVE "  Xbox hard drive"));
        ImGui::TextDisabled("%s", g_open_err.empty() ? tr("Opening...") : g_open_err.c_str());
        if (ImGui::Button(tr("Try again"))) {
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
    ImGui::SetWindowFocus(trid("Xbox Hard Drive"));
}

void XboxExplorerWindowDraw(void)
{
    if (!g_window_open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(820 * s, 560 * s), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(trid("Xbox Hard Drive"), &g_window_open)) {
        XboxExplorerDraw();
    }
    ImGui::End();
}
