//
// xemu User Interface - Game library
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Standard headers must come before common.hh: QEMU's Windows compatibility
// layer #defines close(), which would otherwise break std::filebuf.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fpng.h>
#include <nlohmann/json.hpp>

#include "common.hh"
#include "game-library.hh"
#include "game-library-scan.hh"
#include "actions.hh"
#include "misc.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "IconsFontAwesome6.h"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"

namespace fs = std::filesystem;

extern float g_main_menu_height;
using json = nlohmann::json;

GameLibraryWindow game_library_window;

namespace {

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct LibraryEntry {
    // From the scan
    std::string path; // UTF-8
    std::string file_stem;
    uint64_t file_size = 0;
    int64_t mtime = 0;
    bool valid = false;
    uint32_t title_id = 0;
    std::string title;
    uint32_t region = 0;
    bool has_icon = false;

    // Display texture (main thread only)
    GLuint tex = 0;
    int tex_w = 0, tex_h = 0;
    bool tex_attempted = false;

    const std::string &DisplayName() const
    {
        return title.empty() ? file_stem : title;
    }
};

struct GameStats {
    bool favorite = false;
    double play_seconds = 0;
    int64_t last_played = 0; // unix time
};

struct GameOverrides {
    bool enabled = false;
    int surface_scale = 0; // 0 = use global
    int filtering = -1;    // -1 = use global
    float fsr_sharpness = -1; // < 0 = use global
};

std::vector<LibraryEntry> g_entries;
std::map<std::string, GameStats> g_stats;        // by image path
std::map<uint32_t, GameOverrides> g_overrides;   // by title ID
bool g_loaded = false;
bool g_dirty = false;
uint64_t g_last_save_tick = 0;
std::string g_scanned_dir; // signature of the folders last scanned

// Game folders: the primary one is general.games_dir in xemu.toml, extra
// folders live in library.json. Each can include its subfolders.
struct LibraryFolder {
    std::string path;
    bool recursive = true;
};
std::vector<LibraryFolder> g_extra_folders;
bool g_primary_recursive = true;

std::vector<LibraryFolder> AllFolders()
{
    std::vector<LibraryFolder> out;
    const char *dir = g_config.general.games_dir;
    if (dir && dir[0]) {
        out.push_back({ dir, g_primary_recursive });
    }
    for (auto &f : g_extra_folders) {
        bool dup = f.path.empty();
        for (auto &o : out) {
            dup |= (o.path == f.path);
        }
        if (!dup) {
            out.push_back(f);
        }
    }
    return out;
}

std::string FolderSignature()
{
    std::string sig;
    for (auto &f : AllFolders()) {
        sig += f.path + (f.recursive ? "|r\n" : "|n\n");
    }
    return sig;
}

// Background scan
std::mutex g_scan_mutex;
std::vector<LibraryEntry> g_scan_queue;
std::set<std::string> g_scan_found;
bool g_scan_finished = false;
std::thread g_scan_thread;
std::atomic<bool> g_scan_running{ false };
std::atomic<bool> g_scan_cancel{ false };
std::atomic<int> g_scan_done{ 0 };
std::atomic<int> g_scan_total{ 0 };

// Loaded disc tracking
std::string g_last_dvd_path;
uint64_t g_last_tick = 0;

// Per-game override state
struct AppliedOverrides {
    bool active = false;
    GameOverrides ov;
    int base_scale = 1;
    int base_filtering = 0;
    float base_fsr = 0.8f;
} g_applied;

struct SavedConfig {
    int scale, filtering;
    float fsr;
} g_saved_config;

// UI state
std::string g_search;
bool g_favorites_only = false;
std::string g_selected_path;
std::string g_settings_path; // game whose settings popup is open
GameOverrides g_settings_edit;
bool g_open_settings_popup = false;
bool g_focus_requested = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fs::path BasePath()
{
    return fs::u8path(xemu_settings_get_base_path());
}

fs::path LibraryFile()
{
    return BasePath() / "library.json";
}

fs::path IconCacheDir()
{
    return BasePath() / "library-cache";
}

std::string TitleIdHex(uint32_t id)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", id);
    return buf;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool ReadWholeFile(const fs::path &p, std::vector<uint8_t> &out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len <= 0 || len > 64 * 1024 * 1024) {
        return false;
    }
    out.resize((size_t)len);
    f.seekg(0);
    f.read((char *)out.data(), len);
    return (bool)f;
}

std::string FormatPlayTime(double seconds)
{
    if (seconds < 60) {
        return seconds > 0 ? "< 1m" : "Never played";
    }
    int mins = (int)(seconds / 60);
    if (mins < 60) {
        return string_format("%dm", mins);
    }
    return string_format("%dh %dm", mins / 60, mins % 60);
}

std::string FormatLastPlayed(int64_t t)
{
    if (t <= 0) {
        return "Never";
    }
    int64_t days = ((int64_t)time(NULL) - t) / 86400;
    if (days <= 0) {
        return "Today";
    }
    if (days == 1) {
        return "Yesterday";
    }
    if (days < 30) {
        return string_format("%d days ago", (int)days);
    }
    time_t tt = (time_t)t;
    struct tm *tm = localtime(&tt);
    char buf[32];
    if (!tm || !strftime(buf, sizeof(buf), "%Y-%m-%d", tm)) {
        return "Long ago";
    }
    return buf;
}

LibraryEntry *FindEntry(const std::string &path)
{
    for (auto &e : g_entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

void FreeTexture(LibraryEntry &e)
{
    if (e.tex) {
        glDeleteTextures(1, &e.tex);
        e.tex = 0;
    }
    e.tex_attempted = false;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void LoadLibrary()
{
    std::vector<uint8_t> data;
    if (!ReadWholeFile(LibraryFile(), data)) {
        return;
    }
    try {
        json j = json::parse(data.begin(), data.end());
        json games = j.value("games", json::array());
        json stats = j.value("stats", json::object());
        json overrides = j.value("overrides", json::object());
        g_primary_recursive = j.value("primary_recursive", true);
        for (auto &f : j.value("folders", json::array())) {
            LibraryFolder lf;
            lf.path = f.value("path", "");
            lf.recursive = f.value("recursive", true);
            if (!lf.path.empty()) {
                g_extra_folders.push_back(lf);
            }
        }
        for (auto &g : games) {
            LibraryEntry e;
            e.path = g.value("path", "");
            if (e.path.empty()) {
                continue;
            }
            e.file_stem = fs::u8path(e.path).stem().u8string();
            e.file_size = g.value("size", (uint64_t)0);
            e.mtime = g.value("mtime", (int64_t)0);
            e.valid = g.value("valid", false);
            e.title_id = g.value("title_id", (uint32_t)0);
            e.title = g.value("title", "");
            e.region = g.value("region", (uint32_t)0);
            e.has_icon = g.value("has_icon", false);
            g_entries.push_back(std::move(e));
        }
        for (auto &[path, s] : stats.items()) {
            GameStats st;
            st.favorite = s.value("favorite", false);
            st.play_seconds = s.value("play_seconds", 0.0);
            st.last_played = s.value("last_played", (int64_t)0);
            g_stats[path] = st;
        }
        for (auto &[id, o] : overrides.items()) {
            GameOverrides ov;
            ov.enabled = o.value("enabled", false);
            ov.surface_scale = o.value("surface_scale", 0);
            ov.filtering = o.value("filtering", -1);
            ov.fsr_sharpness = o.value("fsr_sharpness", -1.0f);
            g_overrides[(uint32_t)strtoul(id.c_str(), NULL, 16)] = ov;
        }
    } catch (const std::exception &ex) {
        fprintf(stderr, "Game library: failed to read %s: %s\n",
                LibraryFile().u8string().c_str(), ex.what());
    }
}

void SaveLibrary()
{
    json j;
    j["version"] = 1;
    json games = json::array();
    for (auto &e : g_entries) {
        games.push_back({ { "path", e.path },
                          { "size", e.file_size },
                          { "mtime", e.mtime },
                          { "valid", e.valid },
                          { "title_id", e.title_id },
                          { "title", e.title },
                          { "region", e.region },
                          { "has_icon", e.has_icon } });
    }
    j["games"] = games;
    json stats = json::object();
    for (auto &[path, s] : g_stats) {
        if (!s.favorite && s.play_seconds <= 0 && s.last_played <= 0) {
            continue;
        }
        stats[path] = { { "favorite", s.favorite },
                        { "play_seconds", s.play_seconds },
                        { "last_played", s.last_played } };
    }
    j["stats"] = stats;
    json ovs = json::object();
    for (auto &[id, o] : g_overrides) {
        ovs[TitleIdHex(id)] = { { "enabled", o.enabled },
                                { "surface_scale", o.surface_scale },
                                { "filtering", o.filtering },
                                { "fsr_sharpness", o.fsr_sharpness } };
    }
    j["overrides"] = ovs;
    j["primary_recursive"] = g_primary_recursive;
    json folders = json::array();
    for (auto &f : g_extra_folders) {
        folders.push_back({ { "path", f.path }, { "recursive", f.recursive } });
    }
    j["folders"] = folders;

    std::error_code ec;
    fs::create_directories(BasePath(), ec);
    fs::path tmp = LibraryFile();
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return;
        }
        f << j.dump(1, '\t', false, json::error_handler_t::replace);
    }
    fs::rename(tmp, LibraryFile(), ec);
    if (ec) {
        // Windows can refuse to rename over an existing file
        fs::remove(LibraryFile(), ec);
        fs::rename(tmp, LibraryFile(), ec);
    }
    g_dirty = false;
}

// ---------------------------------------------------------------------------
// Scanning (worker thread)
// ---------------------------------------------------------------------------

int64_t FileMtime(const fs::path &p)
{
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : (int64_t)t.time_since_epoch().count();
}

void ScanWorker(std::vector<LibraryFolder> folders,
                std::map<std::string, LibraryEntry> cache)
{
    std::vector<fs::path> files;
    std::set<std::string> seen;
    auto consider = [&](const fs::directory_entry &de) {
        std::error_code fec;
        if (!de.is_regular_file(fec)) {
            return;
        }
        std::string ext = Lower(de.path().extension().u8string());
        if ((ext == ".iso" || ext == ".xiso") &&
            seen.insert(de.path().u8string()).second) {
            files.push_back(de.path());
        }
    };
    std::error_code ec;
    for (auto &folder : folders) {
        fs::path root = fs::u8path(folder.path);
        if (folder.recursive) {
            fs::recursive_directory_iterator it(
                root, fs::directory_options::skip_permission_denied, ec);
            for (; !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (g_scan_cancel) {
                    break;
                }
                if (it.depth() >= 32) {
                    it.disable_recursion_pending(); // guard against loops
                }
                consider(*it);
            }
        } else {
            fs::directory_iterator it(
                root, fs::directory_options::skip_permission_denied, ec);
            for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
                consider(*it);
            }
        }
        ec.clear();
    }
    g_scan_total = (int)files.size();

    fs::create_directories(IconCacheDir(), ec);
    std::set<std::string> found;
    for (auto &p : files) {
        if (g_scan_cancel) {
            break;
        }
        LibraryEntry e;
        e.path = p.u8string();
        e.file_stem = p.stem().u8string();
        e.file_size = fs::file_size(p, ec);
        e.mtime = FileMtime(p);
        found.insert(e.path);

        auto c = cache.find(e.path);
        if (c != cache.end() && c->second.file_size == e.file_size &&
            c->second.mtime == e.mtime) {
            e = c->second;
        } else {
            DiscTitleInfo info;
            if (ReadDiscTitleInfo(e.path, info)) {
                e.valid = true;
                e.title_id = info.title_id;
                e.title = info.title_name;
                e.region = info.region;
                if (!info.icon_rgba.empty()) {
                    std::vector<uint8_t> png;
                    if (fpng::fpng_encode_image_to_memory(
                            info.icon_rgba.data(), info.icon_w, info.icon_h,
                            4, png)) {
                        std::ofstream f(IconCacheDir() /
                                            (TitleIdHex(e.title_id) + ".png"),
                                        std::ios::binary | std::ios::trunc);
                        f.write((const char *)png.data(), png.size());
                        e.has_icon = (bool)f;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_scan_mutex);
            g_scan_queue.push_back(std::move(e));
        }
        g_scan_done++;
    }

    std::lock_guard<std::mutex> lock(g_scan_mutex);
    if (!g_scan_cancel) {
        g_scan_found = std::move(found);
        g_scan_finished = true;
    }
    g_scan_running = false;
}

void StartScan()
{
    if (g_scan_running) {
        return;
    }
    if (g_scan_thread.joinable()) {
        g_scan_thread.join();
    }
    std::vector<LibraryFolder> folders = AllFolders();
    g_scanned_dir = FolderSignature();
    if (folders.empty()) {
        for (auto &e : g_entries) {
            FreeTexture(e);
        }
        g_entries.clear();
        SaveLibrary();
        return;
    }
    std::map<std::string, LibraryEntry> cache;
    for (auto &e : g_entries) {
        LibraryEntry c = e;
        c.tex = 0;
        cache[e.path] = c;
    }
    g_scan_cancel = false;
    g_scan_done = 0;
    g_scan_total = 0;
    g_scan_running = true;
    g_scan_thread = std::thread(ScanWorker, std::move(folders), std::move(cache));
}

void MergeScanResults()
{
    std::vector<LibraryEntry> queue;
    std::set<std::string> found;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(g_scan_mutex);
        queue.swap(g_scan_queue);
        if (g_scan_finished) {
            finished = true;
            found.swap(g_scan_found);
            g_scan_finished = false;
        }
    }
    for (auto &r : queue) {
        LibraryEntry *e = FindEntry(r.path);
        if (e) {
            bool changed = e->title_id != r.title_id || e->mtime != r.mtime;
            GLuint tex = e->tex;
            int tw = e->tex_w, th = e->tex_h;
            bool attempted = e->tex_attempted;
            *e = r;
            if (changed) {
                if (tex) {
                    glDeleteTextures(1, &tex);
                }
            } else {
                e->tex = tex;
                e->tex_w = tw;
                e->tex_h = th;
                e->tex_attempted = attempted;
            }
        } else {
            g_entries.push_back(r);
        }
        g_dirty = true;
    }
    if (finished) {
        for (auto &e : g_entries) {
            if (!found.count(e.path)) {
                FreeTexture(e);
            }
        }
        g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                       [&](const LibraryEntry &e) {
                                           return !found.count(e.path);
                                       }),
                        g_entries.end());
        SaveLibrary();
    }
}

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

GLuint UploadTexture(const uint8_t *rgba, int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

// Cover art lookup order: covers/<TITLEID>, covers/<file name>,
// <file name> next to the image, then the icon embedded in the disc.
std::vector<fs::path> ArtworkCandidates(const LibraryEntry &e)
{
    std::vector<fs::path> out;
    const char *exts[] = { ".png", ".jpg", ".jpeg" };
    fs::path img = fs::u8path(e.path);
    for (auto &folder : AllFolders()) {
        fs::path covers = fs::u8path(folder.path) / "covers";
        for (const char *ext : exts) {
            if (e.valid) {
                out.push_back(covers / (TitleIdHex(e.title_id) + ext));
                out.push_back(covers / (FormatTitleId(e.title_id) + ext));
            }
            out.push_back(covers / (img.stem().u8string() + ext));
        }
    }
    for (const char *ext : exts) {
        out.push_back(img.parent_path() / (img.stem().u8string() + ext));
    }
    if (e.has_icon) {
        out.push_back(IconCacheDir() / (TitleIdHex(e.title_id) + ".png"));
    }
    return out;
}

void EnsureTexture(LibraryEntry &e, int &budget)
{
    if (e.tex_attempted || budget <= 0) {
        return;
    }
    budget--;
    e.tex_attempted = true;
    std::error_code ec;
    for (auto &p : ArtworkCandidates(e)) {
        if (!fs::is_regular_file(p, ec)) {
            continue;
        }
        std::vector<uint8_t> data;
        if (!ReadWholeFile(p, data)) {
            continue;
        }
        int w, h, n;
        stbi_set_flip_vertically_on_load(false);
        uint8_t *rgba = stbi_load_from_memory(data.data(), (int)data.size(),
                                              &w, &h, &n, 4);
        if (!rgba) {
            continue;
        }
        e.tex = UploadTexture(rgba, w, h);
        e.tex_w = w;
        e.tex_h = h;
        stbi_image_free(rgba);
        return;
    }
}

// ---------------------------------------------------------------------------
// Per-game settings
// ---------------------------------------------------------------------------

uint32_t TitleIdForPath(const std::string &path, bool *ok)
{
    LibraryEntry *e = FindEntry(path);
    if (e && e->valid) {
        *ok = true;
        return e->title_id;
    }
    DiscTitleInfo info;
    *ok = ReadDiscTitleInfo(path, info);
    return info.title_id;
}

void RestoreBaseline()
{
    if (!g_applied.active) {
        return;
    }
    const GameOverrides &ov = g_applied.ov;
    // Only undo values the user has not changed while the game was running
    if (ov.surface_scale > 0 &&
        g_config.display.quality.surface_scale == ov.surface_scale &&
        g_applied.base_scale != ov.surface_scale) {
        nv2a_set_surface_scale_factor(g_applied.base_scale);
    }
    if (ov.filtering >= 0 && g_config.display.filtering == ov.filtering) {
        g_config.display.filtering = g_applied.base_filtering;
    }
    if (ov.fsr_sharpness >= 0 &&
        g_config.display.fsr_sharpness == ov.fsr_sharpness) {
        g_config.display.fsr_sharpness = g_applied.base_fsr;
    }
    g_applied.active = false;
}

void ApplyOverridesFor(const std::string &path)
{
    RestoreBaseline();
    if (path.empty()) {
        return;
    }
    bool ok;
    uint32_t id = TitleIdForPath(path, &ok);
    auto it = g_overrides.find(id);
    if (!ok || it == g_overrides.end() || !it->second.enabled) {
        return;
    }
    const GameOverrides &ov = it->second;
    g_applied.active = true;
    g_applied.ov = ov;
    g_applied.base_scale = g_config.display.quality.surface_scale;
    g_applied.base_filtering = g_config.display.filtering;
    g_applied.base_fsr = g_config.display.fsr_sharpness;

    if (ov.surface_scale > 0 &&
        ov.surface_scale != g_config.display.quality.surface_scale) {
        nv2a_set_surface_scale_factor(ov.surface_scale);
    }
    if (ov.filtering >= 0) {
        g_config.display.filtering = ov.filtering;
    }
    if (ov.fsr_sharpness >= 0) {
        g_config.display.fsr_sharpness = ov.fsr_sharpness;
    }
    xemu_queue_notification("Per-game settings applied");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

const ImU32 kAccent = IM_COL32(0x62, 0xCA, 0x13, 0xFF); // xemu green

bool IsCurrentDisc(const LibraryEntry &e)
{
    const char *p = g_config.sys.files.dvd_path;
    return p && e.path == p;
}

void SortedFiltered(std::vector<LibraryEntry *> &out)
{
    out.clear();
    std::string needle = Lower(g_search);
    for (auto &e : g_entries) {
        if (g_favorites_only && !g_stats[e.path].favorite) {
            continue;
        }
        if (!needle.empty() &&
            Lower(e.DisplayName()).find(needle) == std::string::npos &&
            Lower(e.file_stem).find(needle) == std::string::npos &&
            (!e.valid ||
             Lower(FormatTitleId(e.title_id)).find(needle) == std::string::npos)) {
            continue;
        }
        out.push_back(&e);
    }
    int sort = g_config.general.library.sort;
    std::stable_sort(out.begin(), out.end(), [&](LibraryEntry *a, LibraryEntry *b) {
        const GameStats &sa = g_stats[a->path], &sb = g_stats[b->path];
        if (sa.favorite != sb.favorite) {
            return sa.favorite; // favorites first
        }
        if (sort == CONFIG_GENERAL_LIBRARY_SORT_LAST_PLAYED &&
            sa.last_played != sb.last_played) {
            return sa.last_played > sb.last_played;
        }
        if (sort == CONFIG_GENERAL_LIBRARY_SORT_PLAY_TIME &&
            sa.play_seconds != sb.play_seconds) {
            return sa.play_seconds > sb.play_seconds;
        }
        return Lower(a->DisplayName()) < Lower(b->DisplayName());
    });
}

void OpenSettingsFor(LibraryEntry &e)
{
    g_settings_path = e.path;
    g_settings_edit = g_overrides.count(e.title_id) ? g_overrides[e.title_id]
                                                    : GameOverrides();
    g_open_settings_popup = true;
}

void DrawContextMenu(LibraryEntry &e)
{
    if (!ImGui::BeginPopupContextItem("##ctx")) {
        return;
    }
    GameStats &st = g_stats[e.path];
    ImGui::TextDisabled("%s", e.DisplayName().c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_PLAY "  Play")) {
        game_library_window.Launch(e.path);
    }
    if (ImGui::MenuItem(st.favorite ? ICON_FA_STAR "  Remove from favorites"
                                    : ICON_FA_STAR "  Add to favorites")) {
        st.favorite = !st.favorite;
        g_dirty = true;
    }
    if (ImGui::MenuItem(ICON_FA_GEAR "  Game settings...", NULL, false,
                        e.valid)) {
        OpenSettingsFor(e);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy title ID", NULL, false, e.valid)) {
        ImGui::SetClipboardText(FormatTitleId(e.title_id).c_str());
    }
    if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show file location")) {
        std::string url =
            "file:///" + fs::u8path(e.path).parent_path().generic_u8string();
        SDL_OpenURL(url.c_str());
    }
    ImGui::EndPopup();
}

void DrawPlaceholder(ImDrawList *dl, ImVec2 a, ImVec2 b, const LibraryEntry &e,
                     float rounding)
{
    // Deterministic per-title tint so placeholders are easy to tell apart
    uint32_t h = e.valid ? e.title_id * 2654435761u
                         : (uint32_t)std::hash<std::string>()(e.path);
    ImVec4 c1 = ImColor::HSV((h % 360) / 360.0f, 0.45f, 0.38f);
    ImVec4 c2 = ImColor::HSV((h % 360) / 360.0f, 0.55f, 0.18f);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(c2), rounding);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rounding),
                                ImVec2(b.x, b.y - rounding),
                                ImGui::GetColorU32(c1), ImGui::GetColorU32(c1),
                                ImGui::GetColorU32(c2), ImGui::GetColorU32(c2));
    const char *icon = ICON_FA_COMPACT_DISC;
    float size = (b.x - a.x) * 0.35f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, icon);
    dl->AddText(ImGui::GetFont(), size,
                ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2),
                IM_COL32(255, 255, 255, 140), icon);
}

void DrawArtwork(ImDrawList *dl, ImVec2 a, ImVec2 b, const LibraryEntry &e,
                 float rounding)
{
    if (!e.tex) {
        DrawPlaceholder(dl, a, b, e, rounding);
        return;
    }
    // Fit the image inside the box, keeping its aspect ratio
    dl->AddRectFilled(a, b, IM_COL32(16, 16, 16, 255), rounding);
    float bw = b.x - a.x, bh = b.y - a.y;
    float s = std::min(bw / e.tex_w, bh / e.tex_h);
    float w = e.tex_w * s, h = e.tex_h * s;
    ImVec2 ia((a.x + b.x - w) / 2, (a.y + b.y - h) / 2);
    dl->AddImageRounded((ImTextureID)(intptr_t)e.tex, ia,
                        ImVec2(ia.x + w, ia.y + h), ImVec2(0, 0), ImVec2(1, 1),
                        IM_COL32_WHITE, rounding);
}

bool ActivatedByNav()
{
    return ImGui::IsItemFocused() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false));
}

void DrawTile(LibraryEntry &e, float tile_w, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    const float rounding = 6 * s;
    const float font_h = ImGui::GetFontSize();
    const float art_h = tile_w;
    const float tile_h = art_h + 8 * s + font_h * 2 + 4 * s + font_h;
    GameStats &st = g_stats[e.path];

    ImGui::PushID(e.path.c_str());
    bool selected = g_selected_path == e.path;
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 14));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 24));
    bool clicked = ImGui::Selectable("##tile", selected,
                                     ImGuiSelectableFlags_AllowDoubleClick,
                                     ImVec2(tile_w, tile_h));
    ImGui::PopStyleColor(3);
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        g_selected_path = e.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            game_library_window.Launch(e.path);
        }
    }
    if (ActivatedByNav()) {
        game_library_window.Launch(e.path);
    }
    if (ImGui::IsItemFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
        st.favorite = !st.favorite;
        g_dirty = true;
    }
    if (hovered) {
        ImGui::SetItemTooltip("%s\n%s%s%s\nPlay time: %s\nLast played: %s",
                              e.DisplayName().c_str(),
                              e.valid ? FormatTitleId(e.title_id).c_str() : "",
                              e.valid ? "  -  " : "",
                              e.valid ? FormatRegion(e.region).c_str()
                                      : "Unrecognized image",
                              FormatPlayTime(st.play_seconds).c_str(),
                              FormatLastPlayed(st.last_played).c_str());
    }
    DrawContextMenu(e);

    ImVec2 a = ImGui::GetItemRectMin();
    if (ImGui::IsItemVisible()) {
        EnsureTexture(e, tex_budget);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 art_b(a.x + tile_w, a.y + art_h);
        DrawArtwork(dl, a, art_b, e, rounding);

        bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (hovered || selected || focused) {
            dl->AddRect(a, art_b,
                        (selected || focused) ? kAccent
                                              : IM_COL32(255, 255, 255, 90),
                        rounding, 0, 2.5f * s);
        }
        if (st.favorite) {
            ImVec2 p(art_b.x - font_h - 6 * s, a.y + 6 * s);
            dl->AddCircleFilled(ImVec2(p.x + font_h / 2, p.y + font_h / 2),
                                font_h * 0.75f, IM_COL32(0, 0, 0, 150));
            dl->AddText(p, IM_COL32(255, 205, 60, 255), ICON_FA_STAR);
        }
        if (IsCurrentDisc(e)) {
            const char *badge = ICON_FA_PLAY " Playing";
            ImVec2 ts = ImGui::CalcTextSize(badge);
            ImVec2 p(a.x + 6 * s, art_b.y - ts.y - 10 * s);
            dl->AddRectFilled(ImVec2(p.x - 4 * s, p.y - 2 * s),
                              ImVec2(p.x + ts.x + 4 * s, p.y + ts.y + 2 * s),
                              kAccent, 4 * s);
            dl->AddText(p, IM_COL32(0, 0, 0, 255), badge);
        }

        // Title (up to two lines) and subtitle
        ImVec4 clip(a.x, a.y, a.x + tile_w, art_b.y + 8 * s + font_h * 2);
        dl->AddText(ImGui::GetFont(), font_h, ImVec2(a.x + 2 * s, art_b.y + 8 * s),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    e.DisplayName().c_str(), NULL, tile_w - 4 * s, &clip);
        std::string sub = e.valid ? FormatTitleId(e.title_id) + "  " +
                                        FormatRegion(e.region)
                                  : "Unrecognized image";
        ImVec4 clip2(a.x, a.y, a.x + tile_w, a.y + tile_h);
        dl->AddText(ImGui::GetFont(), font_h * 0.85f,
                    ImVec2(a.x + 2 * s, art_b.y + 12 * s + font_h * 2),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str(),
                    NULL, 0, &clip2);
    }
    ImGui::PopID();
}

void DrawGrid(std::vector<LibraryEntry *> &items, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    float tile_w = (110 + 130 * g_config.general.library.tile_size) * s;
    float spacing = 14 * s;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((avail + spacing) / (tile_w + spacing)));
    // Stretch tiles to use the full width
    tile_w = (avail - spacing * (cols - 1)) / cols;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    for (size_t i = 0; i < items.size(); i++) {
        if (i % cols) {
            ImGui::SameLine();
        }
        DrawTile(*items[i], tile_w, tex_budget);
    }
    ImGui::PopStyleVar();
}

void DrawList(std::vector<LibraryEntry *> &items, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    float icon = ImGui::GetFontSize() * 1.8f;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##games", 6, flags)) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, icon + 8 * s);
    ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 4.0f);
    ImGui::TableSetupColumn("Title ID", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Play time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Last played", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableHeadersRow();

    for (LibraryEntry *ep : items) {
        LibraryEntry &e = *ep;
        GameStats &st = g_stats[e.path];
        ImGui::PushID(e.path.c_str());
        ImGui::TableNextRow(0, icon + 6 * s);
        ImGui::TableNextColumn();
        bool selected = g_selected_path == e.path;
        bool clicked = ImGui::Selectable(
            "##row", selected,
            ImGuiSelectableFlags_SpanAllColumns |
                ImGuiSelectableFlags_AllowDoubleClick,
            ImVec2(0, icon + 4 * s));
        if (clicked) {
            g_selected_path = e.path;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                game_library_window.Launch(e.path);
            }
        }
        if (ActivatedByNav()) {
            game_library_window.Launch(e.path);
        }
        DrawContextMenu(e);
        ImVec2 a = ImGui::GetItemRectMin();
        if (ImGui::IsItemVisible()) {
            EnsureTexture(e, tex_budget);
            DrawArtwork(ImGui::GetWindowDrawList(), ImVec2(a.x + 2 * s, a.y + 2 * s),
                        ImVec2(a.x + 2 * s + icon, a.y + 2 * s + icon), e, 3 * s);
        }
        float text_y = (icon + 4 * s - ImGui::GetFontSize()) / 2;
        auto cell = [&](const std::string &text, bool dim) {
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_y);
            if (dim) {
                ImGui::TextDisabled("%s", text.c_str());
            } else {
                ImGui::TextUnformatted(text.c_str());
            }
        };
        cell((st.favorite ? ICON_FA_STAR " " : "") + e.DisplayName(), false);
        cell(e.valid ? FormatTitleId(e.title_id) : "-", true);
        cell(e.valid ? FormatRegion(e.region) : "Unrecognized", true);
        cell(FormatPlayTime(st.play_seconds), true);
        cell(IsCurrentDisc(e) ? "Playing now" : FormatLastPlayed(st.last_played),
             !IsCurrentDisc(e));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void DrawRecentRow(int &tex_budget)
{
    auto recent = game_library_window.GetRecentGames(8);
    if (recent.empty()) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::TextDisabled(ICON_FA_CLOCK "  Recently played");
    float tile_w = 92 * s;
    float spacing = 10 * s;
    float avail = ImGui::GetContentRegionAvail().x;
    int max_items = std::max(1, (int)((avail + spacing) / (tile_w + spacing)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    int n = 0;
    for (auto &r : recent) {
        LibraryEntry *e = FindEntry(r.path);
        if (!e || n >= max_items) {
            continue;
        }
        if (n++) {
            ImGui::SameLine();
        }
        ImGui::PushID("recent");
        DrawTile(*e, tile_w, tex_budget);
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4 * s));
}

void DrawSettingsPopup()
{
    if (g_open_settings_popup) {
        ImGui::OpenPopup("Game Settings");
        g_open_settings_popup = false;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(460 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Game Settings", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    LibraryEntry *e = FindEntry(g_settings_path);
    if (!e) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted(e->DisplayName().c_str());
    ImGui::TextDisabled("%s  -  %s", FormatTitleId(e->title_id).c_str(),
                        FormatRegion(e->region).c_str());
    ImGui::Separator();
    ImGui::Checkbox("Use custom settings for this game", &g_settings_edit.enabled);
    ImGui::BeginDisabled(!g_settings_edit.enabled);

    int scale = g_settings_edit.surface_scale;
    if (ImGui::Combo("Resolution scale", &scale,
                     "Use global\0" "1x\0" "2x\0" "3x\0" "4x\0" "5x\0" "6x\0"
                     "7x\0" "8x\0" "9x\0" "10x\0")) {
        g_settings_edit.surface_scale = scale;
    }
    int filter = g_settings_edit.filtering + 1;
    if (ImGui::Combo("Upscaling filter", &filter,
                     "Use global\0" "Linear\0" "Nearest\0"
                     "AMD FidelityFX FSR 1.0\0" "Bicubic (Catmull-Rom)\0"
                     "Lanczos\0" "Snapdragon GSR 1\0")) {
        g_settings_edit.filtering = filter - 1;
    }
    bool custom_sharp = g_settings_edit.fsr_sharpness >= 0;
    if (ImGui::Checkbox("Custom sharpening", &custom_sharp)) {
        g_settings_edit.fsr_sharpness =
            custom_sharp ? g_config.display.fsr_sharpness : -1.0f;
    }
    if (custom_sharp) {
        ImGui::SliderFloat("Sharpness", &g_settings_edit.fsr_sharpness, 0.0f,
                           1.0f, "%.2f");
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Applied whenever this game's disc is loaded.");
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(120 * s, 0))) {
        g_overrides[e->title_id] = g_settings_edit;
        g_dirty = true;
        SaveLibrary();
        if (IsCurrentDisc(*e)) {
            ApplyOverridesFor(e->path);
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void AddGameFolder()
{
    ShowOpenFolderDialog(g_config.general.games_dir, [](const char *path) {
        const char *primary = g_config.general.games_dir;
        if (!primary || !primary[0]) {
            xemu_settings_set_string(&g_config.general.games_dir, path);
            xemu_settings_save(); // remember the folder right away
        } else {
            for (auto &f : AllFolders()) {
                if (f.path == path) {
                    return;
                }
            }
            g_extra_folders.push_back({ path, true });
        }
        SaveLibrary();
    });
}

void RemoveGameFolder(const std::string &path)
{
    const char *primary = g_config.general.games_dir;
    if (primary && path == primary) {
        // Promote the next folder so xemu's own games directory stays set
        std::string next;
        bool next_recursive = true;
        if (!g_extra_folders.empty()) {
            next = g_extra_folders.front().path;
            next_recursive = g_extra_folders.front().recursive;
            g_extra_folders.erase(g_extra_folders.begin());
        }
        xemu_settings_set_string(&g_config.general.games_dir, next.c_str());
        g_primary_recursive = next_recursive;
        xemu_settings_save();
    } else {
        g_extra_folders.erase(
            std::remove_if(g_extra_folders.begin(), g_extra_folders.end(),
                           [&](const LibraryFolder &f) { return f.path == path; }),
            g_extra_folders.end());
    }
    SaveLibrary();
}

bool g_open_folders_popup = false;

void DrawFoldersPopup()
{
    if (g_open_folders_popup) {
        ImGui::OpenPopup("Game Folders");
        g_open_folders_popup = false;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(640 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Game Folders", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextDisabled("Every .iso / .xiso in these folders appears in your library.");
    ImGui::Dummy(ImVec2(0, 4 * s));
    auto folders = AllFolders();
    std::string remove;
    if (folders.empty()) {
        ImGui::TextUnformatted("No folders yet.");
    } else if (ImGui::BeginTable("##folders", 3,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(600 * s, 0))) {
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Include subfolders", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        const char *primary = g_config.general.games_dir;
        for (auto &f : folders) {
            ImGui::PushID(f.path.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.path.c_str());
            ImGui::TableNextColumn();
            bool rec = f.recursive;
            if (ImGui::Checkbox("##rec", &rec)) {
                if (primary && f.path == primary) {
                    g_primary_recursive = rec;
                } else {
                    for (auto &e : g_extra_folders) {
                        if (e.path == f.path) {
                            e.recursive = rec;
                        }
                    }
                }
                SaveLibrary();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_FA_XMARK " Remove")) {
                remove = f.path;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!remove.empty()) {
        RemoveGameFolder(remove);
    }
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Add folder...")) {
        AddGameFolder();
    }
    ImGui::SameLine();
    if (ImGui::Button("Done", ImVec2(100 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Right-hand panel with the selected game's art and details
void DrawDetailsPanel(LibraryEntry &e, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    GameStats &st = g_stats[e.path];
    float w = ImGui::GetContentRegionAvail().x;
    EnsureTexture(e, tex_budget);

    ImVec2 a = ImGui::GetCursorScreenPos();
    float art = w;
    if (e.tex && e.tex_w > 0) {
        art = std::min(w * e.tex_h / (float)e.tex_w, w * 1.5f);
    }
    DrawArtwork(ImGui::GetWindowDrawList(), a, ImVec2(a.x + w, a.y + art), e,
                8 * s);
    ImGui::Dummy(ImVec2(w, art));
    ImGui::Dummy(ImVec2(0, 6 * s));

    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextUnformatted(e.DisplayName().c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (e.valid) {
        ImGui::TextDisabled("%s  -  %s", FormatTitleId(e.title_id).c_str(),
                            FormatRegion(e.region).c_str());
    } else {
        ImGui::TextDisabled("Unrecognized disc image");
    }
    ImGui::Dummy(ImVec2(0, 8 * s));

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.86f, 0.16f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.32f, 0.70f, 0.06f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 1));
    if (ImGui::Button(IsCurrentDisc(e) ? ICON_FA_PLAY "  Playing - restart"
                                       : ICON_FA_PLAY "  Play",
                      ImVec2(w, 40 * s))) {
        game_library_window.Launch(e.path);
    }
    ImGui::PopStyleColor(4);
    float half = (w - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button(st.favorite ? ICON_FA_STAR "  Favorited" : ICON_FA_STAR "  Favorite",
                      ImVec2(half, 0))) {
        st.favorite = !st.favorite;
        g_dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!e.valid);
    if (ImGui::Button(ICON_FA_GEAR "  Settings", ImVec2(half, 0))) {
        OpenSettingsFor(e);
    }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 8 * s));

    auto row = [&](const char *label, const std::string &value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(w * 0.42f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w * 0.58f);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    };
    row("Play time", FormatPlayTime(st.play_seconds));
    row("Last played", IsCurrentDisc(e) ? "Playing now" : FormatLastPlayed(st.last_played));
    if (e.valid && g_overrides.count(e.title_id) && g_overrides[e.title_id].enabled) {
        row("Settings", "Custom for this game");
    }
    row("File", fs::u8path(e.path).filename().u8string());
    row("Size", string_format("%.2f GB", e.file_size / 1e9));
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "  Show file location")) {
        std::string url =
            "file:///" + fs::u8path(e.path).parent_path().generic_u8string();
        SDL_OpenURL(url.c_str());
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

static void GameLibraryShutdown(void);

void GameLibraryWindow::Open()
{
    is_open = true;
    g_focus_requested = true;
}

void GameLibraryWindow::ToggleOpen()
{
    is_open = !is_open;
}

void GameLibraryWindow::Rescan()
{
    if (g_scan_running) {
        return;
    }
    // Reload artwork too, so newly added covers show up
    for (auto &e : g_entries) {
        FreeTexture(e);
    }
    StartScan();
}

void GameLibraryWindow::Launch(const std::string &path)
{
    LibraryEntry *e = FindEntry(path);
    std::string name = e ? e->DisplayName() : fs::u8path(path).stem().u8string();
    ActionLoadDiscFile(path.c_str());
    if (!runstate_is_running()) {
        vm_start();
    }
    std::string msg = "Loading " + name;
    xemu_queue_notification(msg.c_str());
    is_open = false;
}

std::vector<RecentGame> GameLibraryWindow::GetRecentGames(int max_count)
{
    std::vector<std::pair<int64_t, std::string>> order;
    for (auto &[path, st] : g_stats) {
        if (st.last_played > 0) {
            order.push_back({ st.last_played, path });
        }
    }
    std::sort(order.rbegin(), order.rend());
    std::vector<RecentGame> out;
    std::error_code ec;
    for (auto &[t, path] : order) {
        if ((int)out.size() >= max_count) {
            break;
        }
        if (!fs::exists(fs::u8path(path), ec)) {
            continue;
        }
        LibraryEntry *e = FindEntry(path);
        out.push_back({ path, e ? e->DisplayName()
                                : fs::u8path(path).stem().u8string() });
    }
    return out;
}

void GameLibraryWindow::Update()
{
    if (!g_loaded) {
        g_loaded = true;
        atexit(GameLibraryShutdown);
        LoadLibrary();
        g_last_tick = SDL_GetTicks();
        g_last_save_tick = g_last_tick;
    }

    // Rescan when the set of game folders changes (or on first use)
    if (!g_scan_running && FolderSignature() != g_scanned_dir) {
        StartScan();
    }
    MergeScanResults();

    // Track disc changes: last played + per-game settings
    const char *dvd = g_config.sys.files.dvd_path;
    std::string cur = dvd ? dvd : "";
    if (cur != g_last_dvd_path) {
        g_last_dvd_path = cur;
        if (!cur.empty()) {
            g_stats[cur].last_played = (int64_t)time(NULL);
            g_dirty = true;
        }
        ApplyOverridesFor(cur);
    }

    // Accumulate play time while the machine runs with a disc loaded
    uint64_t now = SDL_GetTicks();
    uint64_t dt = now - g_last_tick;
    g_last_tick = now;
    if (!cur.empty() && runstate_is_running() && dt < 1000) {
        g_stats[cur].play_seconds += dt / 1000.0;
        if (now - g_last_save_tick > 60000) {
            g_dirty = true; // persist play time about once a minute
        }
    }
    if (g_dirty && now - g_last_save_tick > 5000) {
        SaveLibrary();
        g_last_save_tick = now;
    }
}

void GameLibraryWindow::Draw()
{
    if (!is_open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;

    // Full-window home screen below the menu bar, like modern emulators
    ImGuiViewport *vp = ImGui::GetMainViewport();
    float top = g_main_menu_height;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - top));
    if (g_focus_requested) {
        ImGui::SetNextWindowFocus();
        g_focus_requested = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * s, 14 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.08f, 1.0f));
    bool visible = ImGui::Begin(
        "###Game Library", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!visible) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Header: title, game count, close
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kAccent), ICON_FA_GAMEPAD);
    ImGui::SameLine();
    ImGui::PushFont(g_font_mgr.m_menu_font_medium);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5 * s);
    ImGui::TextUnformatted("Game Library");
    ImGui::PopFont();
    ImGui::SameLine();
    if (g_scan_running) {
        ImGui::TextDisabled("Scanning... %d / %d", (int)g_scan_done,
                            (int)g_scan_total);
    } else {
        ImGui::TextDisabled("%d games", (int)g_entries.size());
    }
    const char *dvd = g_config.sys.files.dvd_path;
    const char *close_label = (dvd && dvd[0]) ? ICON_FA_XMARK "  Back to game"
                                              : ICON_FA_XMARK "  Close";
    float close_w = ImGui::CalcTextSize(close_label).x +
                    ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
    if (ImGui::Button(close_label)) {
        is_open = false;
    }
    ImGui::SetItemTooltip("Esc or B");
    ImGui::Dummy(ImVec2(0, 2 * s));

    auto folders = AllFolders();
    if (folders.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.3f);
        const char *msg = "Add the folders where you keep your Xbox disc "
                          "images (.iso / .xiso). Subfolders are included.";
        float w = std::min(avail.x, 440 * s);
        float x0 = ImGui::GetCursorPosX() + (avail.x - w) / 2;
        ImGui::SetCursorPosX(x0);
        ImGui::PushTextWrapPos(x0 + w);
        ImGui::TextWrapped("%s", msg);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 10 * s));
        ImGui::SetCursorPosX(x0 + (w - 240 * s) / 2);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Add a games folder",
                          ImVec2(240 * s, 36 * s))) {
            AddGameFolder();
        }
        DrawFoldersPopup();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // Toolbar
    float search_w = std::max(180 * s, ImGui::GetContentRegionAvail().x * 0.28f);
    ImGui::SetNextItemWidth(search_w);
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Search by name or title ID",
                             &g_search);
    ImGui::SameLine();
    bool grid = g_config.general.library.view == CONFIG_GENERAL_LIBRARY_VIEW_GRID;
    auto toggle = [&](const char *label, bool active, const char *tip) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        bool pressed = ImGui::Button(label);
        if (active) {
            ImGui::PopStyleColor();
        }
        ImGui::SetItemTooltip("%s", tip);
        return pressed;
    };
    if (toggle(ICON_FA_TABLE_CELLS, grid, "Grid view")) {
        g_config.general.library.view = CONFIG_GENERAL_LIBRARY_VIEW_GRID;
    }
    ImGui::SameLine(0, 2 * s);
    if (toggle(ICON_FA_LIST, !grid, "List view")) {
        g_config.general.library.view = CONFIG_GENERAL_LIBRARY_VIEW_LIST;
    }
    ImGui::SameLine();
    if (toggle(ICON_FA_STAR, g_favorites_only, "Show favorites only")) {
        g_favorites_only = !g_favorites_only;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * s);
    ImGui::Combo("##sort", &g_config.general.library.sort,
                 "Sort: Title\0Sort: Last played\0Sort: Play time\0");
    if (grid) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * s);
        ImGui::SliderFloat("##size", &g_config.general.library.tile_size, 0.0f,
                           1.0f, "");
        ImGui::SetItemTooltip("Tile size");
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Folders")) {
        g_open_folders_popup = true;
    }
    ImGui::SetItemTooltip("%d game folder%s", (int)folders.size(),
                          folders.size() == 1 ? "" : "s");
    ImGui::SameLine(0, 2 * s);
    ImGui::BeginDisabled(g_scan_running);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE)) {
        game_library_window.Rescan();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Rescan folders and covers");
    ImGui::Dummy(ImVec2(0, 4 * s));

    std::vector<LibraryEntry *> items;
    SortedFiltered(items);
    LibraryEntry *selected = FindEntry(g_selected_path);
    if (!selected && !items.empty()) {
        selected = items[0];
        g_selected_path = selected->path;
    }

    int tex_budget = 6; // decode at most a few images per frame
    float avail_w = ImGui::GetContentRegionAvail().x;
    bool show_panel = selected && avail_w > 720 * s;
    float panel_w = std::min(340 * s, avail_w * 0.3f);
    float gap = 16 * s;

    ImGui::BeginChild("##content",
                      ImVec2(show_panel ? avail_w - panel_w - gap : 0, 0),
                      ImGuiChildFlags_NavFlattened);
    if (items.empty()) {
        ImGui::Dummy(ImVec2(0, 20 * s));
        ImGui::TextDisabled(g_entries.empty() && !g_scan_running
                                ? "No disc images found in your game folders."
                                : "No games match.");
    }
    if (grid) {
        if (g_search.empty() && !g_favorites_only) {
            DrawRecentRow(tex_budget);
        }
        DrawGrid(items, tex_budget);
    } else if (!items.empty()) {
        DrawList(items, tex_budget);
    }
    ImGui::EndChild();

    if (show_panel) {
        ImGui::SameLine(0, gap);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 14 * s));
        ImGui::BeginChild("##details", ImVec2(0, 0),
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened);
        DrawDetailsPanel(*selected, tex_budget);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    DrawSettingsPopup();
    DrawFoldersPopup();

    // Esc / B closes the library when nothing else wants the key
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        is_open = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameLibraryBeforeSettingsSave(void)
{
    // Write the user's global values, not a game's overrides
    g_saved_config = { g_config.display.quality.surface_scale,
                       g_config.display.filtering,
                       g_config.display.fsr_sharpness };
    if (g_applied.active) {
        const GameOverrides &ov = g_applied.ov;
        if (ov.surface_scale > 0 &&
            g_config.display.quality.surface_scale == ov.surface_scale) {
            g_config.display.quality.surface_scale = g_applied.base_scale;
        }
        if (ov.filtering >= 0 && g_config.display.filtering == ov.filtering) {
            g_config.display.filtering = g_applied.base_filtering;
        }
        if (ov.fsr_sharpness >= 0 &&
            g_config.display.fsr_sharpness == ov.fsr_sharpness) {
            g_config.display.fsr_sharpness = g_applied.base_fsr;
        }
    }
}

// Runs at exit (registered after the settings-save handler, so it runs first)
static void GameLibraryShutdown(void)
{
    g_scan_cancel = true;
    if (g_scan_thread.joinable()) {
        g_scan_thread.join();
    }
    if (g_loaded) {
        SaveLibrary();
    }
}

void GameLibraryAfterSettingsSave(void)
{
    g_config.display.quality.surface_scale = g_saved_config.scale;
    g_config.display.filtering = g_saved_config.filtering;
    g_config.display.fsr_sharpness = g_saved_config.fsr;
}
