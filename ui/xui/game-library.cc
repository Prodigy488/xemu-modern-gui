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
#include <condition_variable>
#include <deque>
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
#include "game-patches.hh"
#include "game-preview.hh"
#include "insignia-titles.hh"
#include "game-shortcut.hh"
#include "actions.hh"
#include "misc.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "IconsFontAwesome6.h"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"
#include "../xemu-snapshots.h"
#include "qemu/http.h"
#include "snapshot-manager.hh"

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
    uint32_t version = 0;
    uint32_t disc_number = 0;
    bool has_icon = false;

    // Display texture (main thread only)
    GLuint tex = 0;
    int tex_w = 0, tex_h = 0;
    bool tex_attempted = false;
    uint32_t tex_gen = 0; // bumped on reload; stale decodes are dropped

    const std::string &DisplayName() const
    {
        return title.empty() ? file_stem : title;
    }
};

bool IsInsigniaTitle(uint32_t title_id)
{
    return std::binary_search(std::begin(kInsigniaTitleIds),
                              std::end(kInsigniaTitleIds), title_id);
}

bool SupportsOnline(const LibraryEntry &e)
{
    return e.valid && IsInsigniaTitle(e.title_id);
}

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

// Game patches: files in <base>/patches/<TITLEID>/, applied in list order
struct PatchItem {
    std::string file; // file name inside the patch folder
    bool enabled = true;
};
std::map<uint32_t, std::vector<PatchItem>> g_patches;
std::map<std::string, std::string> g_overlay_to_original; // overlay -> image
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

// Individually added disc images, and games removed (hidden) from the
// library by normalized path. Neither touches the files themselves.
std::vector<std::string> g_extra_files;
std::set<std::string> g_hidden;

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
    for (auto &f : g_extra_files) {
        sig += "file:" + f + "\n";
    }
    for (auto &h : g_hidden) {
        sig += "hide:" + h + "\n";
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

// Cover downloads from xemu's game database (github.com/xemu-project/xdb)
std::thread g_cover_thread;
std::atomic<bool> g_covers_running{ false };
std::atomic<int> g_covers_done{ 0 };
std::atomic<int> g_covers_total{ 0 };
std::atomic<int> g_covers_found{ 0 };
std::mutex g_cover_mutex;
std::vector<uint32_t> g_covers_fetched;  // finished downloads, for reload
std::vector<uint32_t> g_covers_404;      // worker results, merged on main
std::set<uint32_t> g_covers_missing;     // not in the database (persisted)
bool g_covers_finished = false;

// Preview video player. Heap-allocated and never destroyed: its GL texture
// must not be released after the window is gone. GameLibraryShutdown()
// stops its thread.
PreviewPlayer *g_preview = new PreviewPlayer();

// Never start or stop the player while a frame is being built: the current
// frame's draw list may still reference the video texture, and deleting it
// before ImGui renders crashes in the GPU driver. UI code only queues a
// request; Update() applies it before the next frame begins.
bool g_preview_stop_requested = false;
std::string g_preview_open_requested;

struct PendingPreviewImport {
    bool active = false;
    std::string source;
    uint32_t title_id = 0;
    std::string game_path;
} g_pending_import;

void FinishPreviewImport();

void RequestPreviewStop()
{
    g_preview_stop_requested = true;
    g_preview_open_requested.clear();
}

void RequestPreviewOpen(const std::string &path)
{
    g_preview_stop_requested = false;
    g_preview_open_requested = path;
}

bool PreviewStopping()
{
    return g_preview_stop_requested;
}

void ApplyPreviewRequests()
{
    if (g_preview_stop_requested) {
        g_preview->Close();
        g_preview_stop_requested = false;
    }
    if (g_pending_import.active) {
        g_preview->Close();
        FinishPreviewImport();
    }
    if (!g_preview_open_requested.empty()) {
        g_preview->Open(g_preview_open_requested);
        g_preview_open_requested.clear();
    }
}

// Resume points: a snapshot named "Resume <TITLE ID>" saved when leaving a
// game. Saving needs the main loop lock and the GL context, so actions that
// leave a game are queued and run from Update().
enum class LeaveAction { None, Quit, Eject, Launch };
LeaveAction g_leave_action = LeaveAction::None;
std::string g_leave_launch_path;

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
bool g_online_only = false;
bool g_open_insignia_popup = false;

// Insignia website status: checked on demand (when its panel is shown), at
// most once every 5 minutes, so xemu never polls their site in the
// background. This is the website, not the game servers.
enum class InsigniaStatus { Unknown, Checking, Up, Maintenance, Down };
std::atomic<int> g_insignia_status{ (int)InsigniaStatus::Unknown };
std::atomic<uint64_t> g_insignia_checked_at{ 0 };
std::thread g_insignia_thread;
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

std::string PathKey(const fs::path &p);

// PathKey() asks the filesystem; memoize it for paths checked every frame
std::map<std::string, std::string> g_pathkey_cache;

std::string PathKeyCached(const std::string &path)
{
    auto it = g_pathkey_cache.find(path);
    if (it == g_pathkey_cache.end()) {
        it = g_pathkey_cache.emplace(path, PathKey(fs::u8path(path))).first;
    }
    return it->second;
}

// Normalized form of a path for duplicate checks: resolves "..", links and
// trailing slashes; case-insensitive on Windows.
std::string PathKey(const fs::path &p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (ec) {
        c = p.lexically_normal();
    }
    std::string k = c.generic_u8string();
#ifdef _WIN32
    k = Lower(k);
#endif
    while (k.size() > 1 && k.back() == '/') {
        k.pop_back();
    }
    return k;
}

bool PathWithin(const std::string &child_key, const std::string &parent_key)
{
    return child_key == parent_key ||
           (child_key.size() > parent_key.size() &&
            child_key.compare(0, parent_key.size(), parent_key) == 0 &&
            (parent_key.back() == '/' || child_key[parent_key.size()] == '/'));
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

// Textures are never deleted while a frame is being built: the frame's draw
// list may still reference them, and ImGui renders it later. Deletions are
// queued and done at the start of the next Update(), before the next frame.
std::vector<GLuint> g_textures_to_delete;

void DeleteTextureLater(GLuint tex)
{
    if (tex) {
        g_textures_to_delete.push_back(tex);
    }
}

void FlushDeletedTextures()
{
    if (!g_textures_to_delete.empty()) {
        glDeleteTextures((GLsizei)g_textures_to_delete.size(),
                         g_textures_to_delete.data());
        g_textures_to_delete.clear();
    }
}

void FreeTexture(LibraryEntry &e)
{
    DeleteTextureLater(e.tex);
    e.tex = 0;
    e.tex_attempted = false;
    e.tex_gen++;
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
        for (auto &f : j.value("files", json::array())) {
            if (f.is_string()) {
                g_extra_files.push_back(f.get<std::string>());
            }
        }
        for (auto &m : j.value("covers_missing", json::array())) {
            if (m.is_string()) {
                g_covers_missing.insert(
                    (uint32_t)strtoul(m.get<std::string>().c_str(), NULL, 16));
            }
        }
        for (auto &h : j.value("hidden", json::array())) {
            if (h.is_string()) {
                g_hidden.insert(h.get<std::string>());
            }
        }
        json patch_lists = j.value("patches", json::object());
        json overlay_map = j.value("overlays", json::object());
        for (auto &[id, list] : patch_lists.items()) {
            std::vector<PatchItem> items;
            for (auto &it : list) {
                PatchItem pi;
                pi.file = it.value("file", "");
                pi.enabled = it.value("enabled", true);
                if (!pi.file.empty()) {
                    items.push_back(pi);
                }
            }
            g_patches[(uint32_t)strtoul(id.c_str(), NULL, 16)] = items;
        }
        for (auto &[overlay, original] : overlay_map.items()) {
            g_overlay_to_original[overlay] = original.get<std::string>();
        }
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
            e.version = g.value("version", (uint32_t)0);
            e.disc_number = g.value("disc", (uint32_t)0);
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
                          { "version", e.version },
                          { "disc", e.disc_number },
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
    j["files"] = g_extra_files;
    j["hidden"] = g_hidden;
    json missing = json::array();
    for (uint32_t id : g_covers_missing) {
        missing.push_back(TitleIdHex(id));
    }
    j["covers_missing"] = missing;
    json patches = json::object();
    for (auto &[id, items] : g_patches) {
        json list = json::array();
        for (auto &pi : items) {
            list.push_back({ { "file", pi.file }, { "enabled", pi.enabled } });
        }
        patches[TitleIdHex(id)] = list;
    }
    j["patches"] = patches;
    j["overlays"] = g_overlay_to_original;
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
                std::vector<std::string> extra_files,
                std::set<std::string> hidden,
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
        if (ext != ".iso" && ext != ".xiso") {
            return;
        }
        std::string key = PathKey(de.path());
        if (!hidden.count(key) && seen.insert(key).second) {
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
    for (auto &f : extra_files) {
        std::error_code fec;
        fs::directory_entry de(fs::u8path(f), fec);
        if (!fec) {
            consider(de);
        }
    }
    g_scan_total = (int)files.size();

    fs::create_directories(IconCacheDir(), ec);
    std::set<std::string> found;
    std::set<std::string> discs; // identical copies of a disc are listed once
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
                e.version = info.version;
                e.disc_number = info.disc_number;
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
        if (e.valid) {
            std::string disc_key = string_format(
                "%08X/%08X/%08X/%u/%llu", e.title_id, e.region, e.version,
                e.disc_number, (unsigned long long)e.file_size);
            if (!discs.insert(disc_key).second) {
                found.erase(e.path); // duplicate of a disc already listed
                g_scan_done++;
                continue;
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
    if (folders.empty() && g_extra_files.empty()) {
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
    g_scan_thread = std::thread(ScanWorker, std::move(folders), g_extra_files,
                                g_hidden, std::move(cache));
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
            uint32_t gen = e->tex_gen;
            *e = r;
            e->tex_gen = gen + (changed ? 1 : 0);
            if (changed) {
                DeleteTextureLater(tex);
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
    if (e.valid) {
        out.push_back(IconCacheDir() / "covers" / (TitleIdHex(e.title_id) + ".jpg"));
    }
    if (e.has_icon) {
        out.push_back(IconCacheDir() / (TitleIdHex(e.title_id) + ".png"));
    }
    return out;
}

// Artwork is decoded on a background thread: the library is drawn while
// xemu holds its main loop lock, so decoding large covers there would stall
// emulation (visible as a slow boot animation). The main thread only
// uploads finished, already-downscaled images, a couple per frame.
struct ImageJob {
    std::string key; // entry path, or "shot:<path>" for a screenshot
    std::vector<fs::path> candidates;
    int max_h = 600;
    uint32_t gen = 0;
};
struct ImageResult {
    std::string key;
    uint32_t gen = 0;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};
std::thread g_img_thread;
std::mutex g_img_mutex;
std::condition_variable g_img_cv;
std::deque<ImageJob> g_img_jobs;
std::vector<ImageResult> g_img_results;
bool g_img_quit = false;

// Box-filter downscale so covers stay small in memory and quick to upload
void Downscale(std::vector<uint8_t> &rgba, int &w, int &h, int max_h)
{
    if (h <= max_h) {
        return;
    }
    int nh = max_h, nw = std::max(1, (int)((int64_t)w * max_h / h));
    std::vector<uint8_t> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((int64_t)y * h / nh), y1 = std::max(y0 + 1, (int)((int64_t)(y + 1) * h / nh));
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((int64_t)x * w / nw), x1 = std::max(x0 + 1, (int)((int64_t)(x + 1) * w / nw));
            uint32_t acc[4] = { 0, 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *row = &rgba[((size_t)yy * w + x0) * 4];
                for (int xx = x0; xx < x1; xx++, row += 4) {
                    acc[0] += row[0];
                    acc[1] += row[1];
                    acc[2] += row[2];
                    acc[3] += row[3];
                    n++;
                }
            }
            uint8_t *d = &out[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 4; c++) {
                d[c] = (uint8_t)(acc[c] / n);
            }
        }
    }
    rgba.swap(out);
    w = nw;
    h = nh;
}

void ImageWorker()
{
    stbi_set_flip_vertically_on_load_thread(0);
    for (;;) {
        ImageJob job;
        {
            std::unique_lock<std::mutex> lock(g_img_mutex);
            g_img_cv.wait(lock, [] { return g_img_quit || !g_img_jobs.empty(); });
            if (g_img_quit) {
                return;
            }
            job = std::move(g_img_jobs.front());
            g_img_jobs.pop_front();
        }
        ImageResult res;
        res.key = job.key;
        res.gen = job.gen;
        std::error_code ec;
        for (auto &p : job.candidates) {
            if (!fs::is_regular_file(p, ec)) {
                continue;
            }
            std::vector<uint8_t> data;
            if (!ReadWholeFile(p, data)) {
                continue;
            }
            int w, h, n;
            uint8_t *rgba = stbi_load_from_memory(data.data(), (int)data.size(),
                                                  &w, &h, &n, 4);
            if (!rgba) {
                continue;
            }
            res.rgba.assign(rgba, rgba + (size_t)w * h * 4);
            stbi_image_free(rgba);
            res.w = w;
            res.h = h;
            Downscale(res.rgba, res.w, res.h, job.max_h);
            break;
        }
        std::lock_guard<std::mutex> lock(g_img_mutex);
        g_img_results.push_back(std::move(res));
    }
}

void StopImageWorker()
{
    {
        std::lock_guard<std::mutex> lock(g_img_mutex);
        g_img_quit = true;
        g_img_jobs.clear();
    }
    g_img_cv.notify_all();
    if (g_img_thread.joinable()) {
        g_img_thread.join();
    }
}

void EnsureTexture(LibraryEntry &e, int &budget)
{
    (void)budget;
    if (e.tex_attempted) {
        return;
    }
    e.tex_attempted = true;
    ImageJob job{ e.path, ArtworkCandidates(e), 600, e.tex_gen };
    {
        std::lock_guard<std::mutex> lock(g_img_mutex);
        if (g_img_quit) {
            return;
        }
        g_img_jobs.push_back(std::move(job));
    }
    if (!g_img_thread.joinable()) {
        g_img_thread = std::thread(ImageWorker);
    }
    g_img_cv.notify_one();
}

// Screenshot thumbnails, by file path
struct ShotThumb {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool requested = false;
};
std::map<std::string, ShotThumb> g_shot_thumbs;

void StoreScreenshotThumb(const std::string &path, const ImageResult &r)
{
    auto it = g_shot_thumbs.find(path);
    if (it == g_shot_thumbs.end() || r.rgba.empty()) {
        return;
    }
    DeleteTextureLater(it->second.tex);
    it->second.tex = UploadTexture(r.rgba.data(), r.w, r.h);
    it->second.w = r.w;
    it->second.h = r.h;
}

void FreeScreenshotThumbs()
{
    for (auto &kv : g_shot_thumbs) {
        DeleteTextureLater(kv.second.tex);
    }
    g_shot_thumbs.clear();
}

void RequestScreenshotThumb(const std::string &path)
{
    ShotThumb &t = g_shot_thumbs[path];
    if (t.requested) {
        return;
    }
    t.requested = true;
    ImageJob job{ "shot:" + path, { fs::u8path(path) }, 120 };
    {
        std::lock_guard<std::mutex> lock(g_img_mutex);
        if (g_img_quit) {
            return;
        }
        g_img_jobs.push_back(std::move(job));
    }
    if (!g_img_thread.joinable()) {
        g_img_thread = std::thread(ImageWorker);
    }
    g_img_cv.notify_one();
}

// Main thread: upload up to `max_uploads` finished images
void UploadDecodedImages(int max_uploads)
{
    std::vector<ImageResult> ready;
    {
        std::lock_guard<std::mutex> lock(g_img_mutex);
        int n = std::min<int>(max_uploads, (int)g_img_results.size());
        ready.assign(std::make_move_iterator(g_img_results.begin()),
                     std::make_move_iterator(g_img_results.begin() + n));
        g_img_results.erase(g_img_results.begin(), g_img_results.begin() + n);
    }
    for (auto &r : ready) {
        if (r.key.compare(0, 5, "shot:") == 0) {
            StoreScreenshotThumb(r.key.substr(5), r);
            continue;
        }
        LibraryEntry *e = FindEntry(r.key);
        if (!e || r.rgba.empty() || !e->tex_attempted || r.gen != e->tex_gen) {
            continue; // removed, nothing found, or reloaded since requested
        }
        DeleteTextureLater(e->tex);
        e->tex = UploadTexture(r.rgba.data(), r.w, r.h);
        e->tex_w = r.w;
        e->tex_h = r.h;
    }
}

// ---------------------------------------------------------------------------
// Resume points
// ---------------------------------------------------------------------------

std::string ResumeSnapshotName(uint32_t title_id)
{
    return "Resume " + FormatTitleId(title_id);
}

// Index into g_snapshot_mgr of this game's resume point, or -1
int FindResumeSnapshot(const LibraryEntry &e)
{
    if (!e.valid) {
        return -1;
    }
    std::string name = ResumeSnapshotName(e.title_id);
    for (int i = 0; i < g_snapshot_mgr.m_snapshots_len; i++) {
        if (name == g_snapshot_mgr.m_snapshots[i].name) {
            return i;
        }
    }
    return -1;
}

std::string OriginalDiscPath(const std::string &path);

// The library game currently in the drive, if any
LibraryEntry *CurrentGame()
{
    const char *dvd = g_config.sys.files.dvd_path;
    if (!dvd || !dvd[0]) {
        return nullptr;
    }
    LibraryEntry *e = FindEntry(OriginalDiscPath(dvd));
    return (e && e->valid) ? e : nullptr;
}

bool ShouldSaveResumePoint()
{
    return g_config.general.library.auto_resume && CurrentGame() != nullptr;
}

void SaveResumePoint()
{
    LibraryEntry *e = CurrentGame();
    if (!e) {
        return;
    }
    std::string name = ResumeSnapshotName(e->title_id);
    Error *err = NULL;
    xemu_snapshots_save(name.c_str(), &err);
    if (err) {
        std::string msg = std::string("Could not save a resume point: ") +
                          error_get_pretty(err);
        xemu_queue_error_message(msg.c_str());
        error_free(err);
    }
}

void LaunchNow(const std::string &path);

// Run a queued action that leaves the current game (from Update())
void RunLeaveAction()
{
    LeaveAction action = g_leave_action;
    g_leave_action = LeaveAction::None;
    if (action == LeaveAction::None) {
        return;
    }
    SaveResumePoint();
    switch (action) {
    case LeaveAction::Quit:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        break;
    case LeaveAction::Eject:
        ActionEjectDiscNow();
        break;
    case LeaveAction::Launch:
        LaunchNow(g_leave_launch_path);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Per-game settings
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Game patches
// ---------------------------------------------------------------------------

fs::path PatchDir(uint32_t title_id)
{
    return BasePath() / "patches" / TitleIdHex(title_id);
}

fs::path OverlayDir()
{
    return BasePath() / "patched-discs";
}

// The disc image behind a loaded path (overlays map back to their game)
std::string OriginalDiscPath(const std::string &path)
{
    auto it = g_overlay_to_original.find(path);
    return it == g_overlay_to_original.end() ? path : it->second;
}

bool IsPatchFile(const fs::path &p)
{
    std::string ext = Lower(p.extension().u8string());
    return ext == ".xbe" || ext == ".ips" || ext == ".bps";
}

// Match the saved list with the files actually in the patch folder: keep the
// user's order and on/off state, append new files (on), drop removed ones.
void SyncPatches(uint32_t title_id)
{
    std::set<std::string> present;
    std::vector<std::string> sorted;
    std::error_code ec;
    for (fs::directory_iterator it(PatchDir(title_id), ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && IsPatchFile(it->path())) {
            std::string name = it->path().filename().u8string();
            present.insert(name);
            sorted.push_back(name);
        }
    }
    std::sort(sorted.begin(), sorted.end());
    std::vector<PatchItem> &items = g_patches[title_id];
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const PatchItem &pi) {
                                   return !present.count(pi.file);
                               }),
                items.end());
    bool changed = items.size() != before;
    for (auto &name : sorted) {
        bool known = false;
        for (auto &pi : items) {
            known |= (pi.file == name);
        }
        if (!known) {
            items.push_back({ name, true });
            changed = true;
        }
    }
    if (items.empty()) {
        g_patches.erase(title_id);
    }
    if (changed) {
        g_dirty = true;
    }
}

std::vector<std::string> EnabledPatchPaths(uint32_t title_id)
{
    std::vector<std::string> out;
    auto it = g_patches.find(title_id);
    if (it == g_patches.end()) {
        return out;
    }
    for (auto &pi : it->second) {
        if (pi.enabled) {
            out.push_back((PatchDir(title_id) / fs::u8path(pi.file)).u8string());
        }
    }
    return out;
}

int EnabledPatchCount(uint32_t title_id)
{
    return (int)EnabledPatchPaths(title_id).size();
}

// Overlay file name depends on the image and the exact patch files, so any
// change produces a fresh overlay.
std::string OverlayPathFor(const LibraryEntry &e,
                           const std::vector<std::string> &paths)
{
    std::error_code ec;
    std::string sig = e.path + "|" + std::to_string(e.file_size) + "|" +
                      std::to_string(e.mtime);
    for (auto &p : paths) {
        fs::path fp = fs::u8path(p);
        sig += "|" + p + "|" + std::to_string(fs::file_size(fp, ec)) + "|" +
               std::to_string(fs::last_write_time(fp, ec).time_since_epoch().count());
    }
    char hash[32];
    snprintf(hash, sizeof(hash), "%016llx",
             (unsigned long long)std::hash<std::string>()(sig));
    return (OverlayDir() / (TitleIdHex(e.title_id) + "-" + hash + ".qcow2"))
        .u8string();
}

// Image to load for this game: the disc itself, or a patched overlay.
bool PrepareLaunchImage(LibraryEntry &e, std::string &load_path,
                        std::string &error)
{
    load_path = e.path;
    if (!e.valid) {
        return true;
    }
    SyncPatches(e.title_id);
    std::vector<std::string> paths = EnabledPatchPaths(e.title_id);
    if (paths.empty()) {
        return true;
    }
    std::string overlay = OverlayPathFor(e, paths);
    std::error_code ec;
    if (!fs::exists(fs::u8path(overlay), ec) &&
        !BuildPatchedOverlay(e.path, paths, overlay, error)) {
        return false;
    }
    // Remove this game's older overlays (ignore files still in use)
    std::string prefix = TitleIdHex(e.title_id) + "-";
    for (fs::directory_iterator it(OverlayDir(), ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string name = it->path().filename().u8string();
        std::string full = it->path().u8string();
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            PathKey(it->path()) != PathKey(fs::u8path(overlay))) {
            std::error_code rec;
            if (fs::remove(it->path(), rec)) {
                g_overlay_to_original.erase(full);
            }
        }
    }
    g_overlay_to_original[overlay] = e.path;
    SaveLibrary();
    load_path = overlay;
    return true;
}

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
    return p && p[0] && e.path == OriginalDiscPath(p);
}

void SortedFiltered(std::vector<LibraryEntry *> &out)
{
    out.clear();
    std::string needle = Lower(g_search);
    for (auto &e : g_entries) {
        if (g_favorites_only && !g_stats[e.path].favorite) {
            continue;
        }
        if (g_online_only && !SupportsOnline(e)) {
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

std::string g_patches_path;           // game whose Patches window is open
bool g_open_patches_popup = false;
std::string g_patch_check_msg;
bool g_patch_check_ok = false;
std::string g_patch_confirm_remove;

void OpenPatchesFor(LibraryEntry &e)
{
    g_patches_path = e.path;
    g_patch_check_msg.clear();
    g_patch_confirm_remove.clear();
    SyncPatches(e.title_id);
    g_open_patches_popup = true;
}

void AddPatchFile(uint32_t title_id)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Game patches (*.xbe, *.ips, *.bps)", "xbe;ips;bps" },
        { "All Files", "*" },
    };
    ShowOpenFileDialog(filters, 2, NULL, [title_id](const char *path) {
        fs::path src = fs::u8path(path);
        if (!IsPatchFile(src)) {
            xemu_queue_error_message(
                "Unsupported patch file. Use .xbe, .ips or .bps files.");
            return;
        }
        std::error_code ec;
        fs::path dir = PatchDir(title_id);
        fs::create_directories(dir, ec);
        fs::path dst = dir / src.filename();
        for (int n = 2; fs::exists(dst, ec) && n < 100; n++) {
            dst = dir / fs::u8path(src.stem().u8string() + " (" +
                                   std::to_string(n) + ")" +
                                   src.extension().u8string());
        }
        if (!fs::copy_file(src, dst, ec)) {
            xemu_queue_error_message("Could not copy the patch file.");
            return;
        }
        SyncPatches(title_id);
        g_patch_check_msg.clear();
        SaveLibrary();
    });
}

const char *PatchTypeLabel(const std::string &file)
{
    std::string ext = Lower(fs::u8path(file).extension().u8string());
    if (ext == ".xbe") {
        return "Custom XBE";
    }
    if (ext == ".ips") {
        return "IPS patch";
    }
    return "BPS patch";
}

void DrawPatchesPopup()
{
    if (g_open_patches_popup) {
        ImGui::OpenPopup("Game Patches");
        g_open_patches_popup = false;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(620 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Game Patches", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    LibraryEntry *e = FindEntry(g_patches_path);
    if (!e || !e->valid) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    uint32_t id = e->title_id;
    ImGui::TextUnformatted(e->DisplayName().c_str());
    ImGui::TextDisabled("%s  -  patch folder: patches\\%s",
                        FormatTitleId(id).c_str(), TitleIdHex(id).c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600 * s);
    ImGui::TextDisabled("Enabled patches are applied, top to bottom, when you "
                        "play this game from the library. Your disc image is "
                        "never modified. A custom .xbe replaces the game's "
                        "default.xbe; .ips and .bps patches modify it.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    std::vector<PatchItem> &items = g_patches[id];
    if (items.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted("No patches yet. Add a .xbe, .ips or .bps file.");
        ImGui::Dummy(ImVec2(0, 4 * s));
    } else if (ImGui::BeginTable("##patches", 4,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(600 * s, 0))) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int move_from = -1, move_to = -1;
        std::string remove;
        for (size_t i = 0; i < items.size(); i++) {
            PatchItem &pi = items[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &pi.enabled)) {
                g_dirty = true;
                g_patch_check_msg.clear();
            }
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(pi.file.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", PatchTypeLabel(pi.file));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                move_from = (int)i;
                move_to = (int)i - 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 2 * s);
            ImGui::BeginDisabled(i + 1 == items.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                move_from = (int)i;
                move_to = (int)i + 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (g_patch_confirm_remove == pi.file) {
                if (ImGui::SmallButton("Delete file?")) {
                    remove = pi.file;
                }
            } else if (ImGui::SmallButton(ICON_FA_XMARK)) {
                g_patch_confirm_remove = pi.file;
            }
            ImGui::SetItemTooltip("Remove this patch from the patch folder");
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (move_from >= 0) {
            std::swap(items[move_from], items[move_to]);
            g_dirty = true;
            g_patch_check_msg.clear();
        }
        if (!remove.empty()) {
            std::error_code ec;
            fs::remove(PatchDir(id) / fs::u8path(remove), ec);
            g_patch_confirm_remove.clear();
            SyncPatches(id);
            g_patch_check_msg.clear();
            SaveLibrary();
        }
    }

    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Add patch...")) {
        AddPatchFile(id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open patch folder")) {
        std::error_code ec;
        fs::create_directories(PatchDir(id), ec);
        SDL_OpenURL(("file:///" + PatchDir(id).generic_u8string()).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        SyncPatches(id);
        g_patch_check_msg.clear();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(EnabledPatchCount(id) == 0);
    if (ImGui::Button(ICON_FA_CHECK "  Check patches")) {
        std::string load, error;
        g_patch_check_ok = PrepareLaunchImage(*e, load, error);
        g_patch_check_msg =
            g_patch_check_ok
                ? string_format("Ready: %d patch%s will be applied on Play.",
                                EnabledPatchCount(id),
                                EnabledPatchCount(id) == 1 ? "" : "es")
                : error;
    }
    ImGui::EndDisabled();
    if (!g_patch_check_msg.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 600 * s);
        ImGui::TextColored(g_patch_check_ok ? ImVec4(0.45f, 0.86f, 0.16f, 1)
                                            : ImVec4(1.0f, 0.45f, 0.4f, 1),
                           "%s", g_patch_check_msg.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    if (ImGui::Button("Done", ImVec2(120 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        SaveLibrary();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

fs::path PreviewDir();
std::string FindPreview(const LibraryEntry &e);
void ImportPreview(const LibraryEntry &e);
void RemoveImportedPreview(uint32_t title_id);
void RemoveFromLibrary(const std::string &path);

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
    if (ImGui::MenuItem("Patches...", NULL, false, e.valid)) {
        OpenPatchesFor(e);
    }
    if (PreviewPlayer::Supported()) {
        if (ImGui::MenuItem(ICON_FA_PLAY "  Add preview video...", NULL, false,
                            e.valid)) {
            ImportPreview(e);
        }
        std::error_code pec;
        bool imported = false;
        for (int i = 0; e.valid && kPreviewExtensions[i]; i++) {
            imported |= fs::exists(
                PreviewDir() / (TitleIdHex(e.title_id) + kPreviewExtensions[i]), pec);
        }
        if (imported && ImGui::MenuItem("Remove preview video")) {
            RequestPreviewStop();
            RemoveImportedPreview(e.title_id);
        }
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
    if (GameShortcutsSupported() &&
        ImGui::MenuItem("Create desktop shortcut")) {
        std::string error;
        if (CreateGameDesktopShortcut(e.DisplayName(), e.path, error)) {
            xemu_queue_notification("Shortcut created on your desktop");
        } else {
            xemu_queue_error_message(error.c_str());
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_XMARK "  Remove from library")) {
        std::string path = e.path; // e may go away once removed
        ImGui::EndPopup();
        RemoveFromLibrary(path);
        return;
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
    // NoPadWithHalfSpacing: by default a selectable extends half the item
    // spacing past each side, which pushed the first column out of view and
    // clipped the start of titles.
    bool clicked = ImGui::Selectable(
        "##tile", selected,
        ImGuiSelectableFlags_AllowDoubleClick |
            ImGuiSelectableFlags_NoPadWithHalfSpacing,
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
        if (SupportsOnline(e)) {
            const char *badge = "ONLINE";
            float fs = font_h * 0.75f;
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, badge);
            ImVec2 p(a.x + 6 * s, a.y + 6 * s);
            dl->AddRectFilled(ImVec2(p.x - 3 * s, p.y - 2 * s),
                              ImVec2(p.x + ts.x + 3 * s, p.y + ts.y + 2 * s),
                              IM_COL32(16, 124, 16, 230), 3 * s);
            dl->AddText(ImGui::GetFont(), fs, p, IM_COL32(255, 255, 255, 255), badge);
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
    // Leave room for the vertical scrollbar even before it appears, so the
    // layout doesn't jump or spill under it
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    if (!win->ScrollbarY) {
        avail -= ImGui::GetStyle().ScrollbarSize;
    }
    avail = std::max(avail, tile_w);
    int cols = std::max(1, (int)((avail + spacing) / (tile_w + spacing)));
    // Stretch tiles to use the full width (whole pixels, so text stays crisp)
    tile_w = std::floor((avail - spacing * (cols - 1)) / cols);

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

fs::path DownloadedCoverPath(uint32_t title_id)
{
    return IconCacheDir() / "covers" / (TitleIdHex(title_id) + ".jpg");
}

// xdb keeps titles/<publisher>/<number>/cover_front.jpg, e.g. MS-004 ->
// titles/MS/004. Only titles with a standard two-letter publisher code.
std::string CoverUrl(uint32_t title_id)
{
    std::string id = FormatTitleId(title_id);
    if (id.size() != 6 || id[2] != '-') {
        return "";
    }
    return "https://raw.githubusercontent.com/xemu-project/xdb/main/titles/" +
           id.substr(0, 2) + "/" + id.substr(3) + "/cover_front.jpg";
}

void CoverWorker(std::vector<uint32_t> ids)
{
    std::error_code ec;
    fs::create_directories(IconCacheDir() / "covers", ec);
    for (uint32_t id : ids) {
        if (g_scan_cancel) {
            break;
        }
        std::string url = CoverUrl(id);
        GByteArray *data = g_byte_array_new();
        int code = url.empty() ? 404 : http_get(url.c_str(), data, NULL, NULL);
        bool is_jpeg = data->len > 4 && data->data[0] == 0xFF && data->data[1] == 0xD8;
        if (code == 200 && is_jpeg) {
            fs::path out = DownloadedCoverPath(id);
            fs::path tmp = out;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write((const char *)data->data, data->len);
            }
            fs::rename(tmp, out, ec);
            g_covers_found++;
            std::lock_guard<std::mutex> lock(g_cover_mutex);
            g_covers_fetched.push_back(id);
        } else if (code == 404) {
            std::lock_guard<std::mutex> lock(g_cover_mutex);
            g_covers_404.push_back(id); // don't ask again
        }
        g_byte_array_unref(data);
        g_covers_done++;
    }
    std::lock_guard<std::mutex> lock(g_cover_mutex);
    g_covers_finished = true;
    g_covers_running = false;
}

void StartCoverDownload()
{
    if (g_covers_running) {
        return;
    }
    if (g_cover_thread.joinable()) {
        g_cover_thread.join();
    }
    // Games without a cover of their own or a previous download
    std::vector<uint32_t> ids;
    std::set<uint32_t> seen;
    std::error_code ec;
    for (auto &e : g_entries) {
        if (!e.valid || seen.count(e.title_id) ||
            g_covers_missing.count(e.title_id) || CoverUrl(e.title_id).empty() ||
            fs::exists(DownloadedCoverPath(e.title_id), ec)) {
            continue;
        }
        bool own_cover = false;
        for (auto &p : ArtworkCandidates(e)) {
            if (p.filename().u8string().find(".png") != std::string::npos &&
                p.parent_path() == IconCacheDir()) {
                continue; // the embedded disc icon doesn't count
            }
            if (p != DownloadedCoverPath(e.title_id) && fs::exists(p, ec)) {
                own_cover = true;
                break;
            }
        }
        if (!own_cover) {
            seen.insert(e.title_id);
            ids.push_back(e.title_id);
        }
    }
    if (ids.empty()) {
        xemu_queue_notification("All your games already have covers");
        return;
    }
    Error *err = NULL;
    if (!ensure_libcurl_initialized(&err)) {
        xemu_queue_error_message(err ? error_get_pretty(err)
                                     : "Could not start downloads");
        if (err) {
            error_free(err);
        }
        return;
    }
    g_covers_done = 0;
    g_covers_found = 0;
    g_covers_total = (int)ids.size();
    g_covers_running = true;
    g_cover_thread = std::thread(CoverWorker, std::move(ids));
}

// Main thread: show newly downloaded covers and report when done
void MergeCoverResults()
{
    std::vector<uint32_t> fetched, missing;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(g_cover_mutex);
        fetched.swap(g_covers_fetched);
        missing.swap(g_covers_404);
        finished = g_covers_finished;
        g_covers_finished = false;
    }
    g_covers_missing.insert(missing.begin(), missing.end());
    for (uint32_t id : fetched) {
        for (auto &e : g_entries) {
            if (e.valid && e.title_id == id) {
                FreeTexture(e); // reload with the new cover
            }
        }
    }
    if (finished) {
        std::string msg = string_format(
            "Downloaded %d cover%s (%d not in the database)", (int)g_covers_found,
            g_covers_found == 1 ? "" : "s",
            (int)g_covers_total - (int)g_covers_found);
        xemu_queue_notification(msg.c_str());
        g_dirty = true;
    }
}

void AddGameFile()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disc Image Files (*.iso, *.xiso)", "iso;xiso" },
        { "All Files", "*" },
    };
    ShowOpenFileDialog(filters, 2, g_config.general.games_dir,
                       [](const char *path) {
        std::string key = PathKey(fs::u8path(path));
        if (g_hidden.erase(key)) {
            xemu_queue_notification("Game restored to your library");
        } else {
            for (auto &e : g_entries) {
                if (PathKey(fs::u8path(e.path)) == key) {
                    g_selected_path = e.path;
                    xemu_queue_notification("That game is already in your library");
                    return;
                }
            }
            for (auto &f : g_extra_files) {
                if (PathKey(fs::u8path(f)) == key) {
                    return;
                }
            }
            g_extra_files.push_back(path);
            xemu_queue_notification("Game added to your library");
        }
        g_selected_path = path;
        SaveLibrary();
    });
}

// Take a game out of the library. Files added one by one are dropped from
// the list; games found in a folder are hidden. Nothing is deleted.
void RemoveFromLibrary(const std::string &path)
{
    std::string key = PathKey(fs::u8path(path));
    size_t before = g_extra_files.size();
    g_extra_files.erase(
        std::remove_if(g_extra_files.begin(), g_extra_files.end(),
                       [&](const std::string &f) {
                           return PathKey(fs::u8path(f)) == key;
                       }),
        g_extra_files.end());
    // Hide it as well in case a folder also contains it
    bool in_folder = false;
    for (auto &folder : AllFolders()) {
        in_folder |= PathWithin(key, PathKey(fs::u8path(folder.path)));
    }
    if (in_folder || g_extra_files.size() == before) {
        g_hidden.insert(key);
    }
    if (g_selected_path == path) {
        g_selected_path.clear();
    }
    SaveLibrary();
    xemu_queue_notification("Removed from your library (the file was not deleted)");
}

void AddGameFolder()
{
    ShowOpenFolderDialog(g_config.general.games_dir, [](const char *path) {
        const char *primary = g_config.general.games_dir;
        if (!primary || !primary[0]) {
            xemu_settings_set_string(&g_config.general.games_dir, path);
            xemu_settings_save(); // remember the folder right away
        } else {
            std::string key = PathKey(fs::u8path(path));
            for (auto &f : AllFolders()) {
                std::string fkey = PathKey(fs::u8path(f.path));
                if (key == fkey || (f.recursive && PathWithin(key, fkey))) {
                    xemu_queue_notification(
                        "That folder is already in your game library");
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

void CheckInsigniaStatus(bool force)
{
    uint64_t now = SDL_GetTicks();
    int st = g_insignia_status;
    if (st == (int)InsigniaStatus::Checking ||
        (!force && st != (int)InsigniaStatus::Unknown &&
         now - g_insignia_checked_at < 5 * 60 * 1000)) {
        return;
    }
    if (g_insignia_thread.joinable()) {
        g_insignia_thread.join();
    }
    g_insignia_status = (int)InsigniaStatus::Checking;
    g_insignia_thread = std::thread([] {
        Error *err = NULL;
        InsigniaStatus result = InsigniaStatus::Down;
        if (ensure_libcurl_initialized(&err)) {
            GByteArray *body = g_byte_array_new();
            int code = http_get("https://insignia.live/games", body, NULL, NULL);
            std::string text((const char *)body->data, body->len);
            g_byte_array_unref(body);
            if (code >= 200 && code < 400) {
                result = Lower(text).find("maintenance") != std::string::npos &&
                                 Lower(text).find("unavailable") != std::string::npos
                             ? InsigniaStatus::Maintenance
                             : InsigniaStatus::Up;
            } else if (code == 503) {
                result = InsigniaStatus::Maintenance;
            }
        } else if (err) {
            error_free(err);
        }
        g_insignia_checked_at = SDL_GetTicks();
        g_insignia_status = (int)result;
    });
}

// Colored dot + label; returns the label
const char *DrawInsigniaStatusDot()
{
    const char *label = "Website status unknown";
    ImU32 col = IM_COL32(140, 140, 140, 255);
    switch ((InsigniaStatus)(int)g_insignia_status) {
    case InsigniaStatus::Checking:
        label = "Checking website...";
        break;
    case InsigniaStatus::Up:
        label = "Website online";
        col = IM_COL32(70, 200, 70, 255);
        break;
    case InsigniaStatus::Maintenance:
        label = "Website under maintenance";
        col = IM_COL32(235, 180, 40, 255);
        break;
    case InsigniaStatus::Down:
        label = "Website unreachable";
        col = IM_COL32(230, 70, 60, 255);
        break;
    default:
        break;
    }
    float h = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + h * 0.35f, p.y + h * 0.55f), h * 0.3f, col);
    ImGui::Dummy(ImVec2(h * 0.8f, h));
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    return label;
}

void DrawInsigniaPopup()
{
    if (g_open_insignia_popup) {
        ImGui::OpenPopup("Insignia");
        g_open_insignia_popup = false;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(560 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Insignia", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    int owned = 0;
    for (auto &e : g_entries) {
        owned += SupportsOnline(e) ? 1 : 0;
    }
    float wrap = ImGui::GetCursorPosX() + 540 * s;
    ImGui::PushTextWrapPos(wrap);
    ImGui::TextUnformatted("Insignia is a free, fan-run revival of the original "
                           "Xbox Live service.");
    CheckInsigniaStatus(false);
    DrawInsigniaStatusDot();
    ImGui::SameLine();
    ImGui::BeginDisabled(g_insignia_status == (int)InsigniaStatus::Checking);
    if (ImGui::SmallButton("Check again")) {
        CheckInsigniaStatus(true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Shows whether insignia.live is reachable. Online play "
                          "can keep working even while the website is down.");
    ImGui::TextDisabled("%d of your games support it (Insignia's list from %s, "
                        "%d games). They're marked ONLINE in the library.",
                        owned, kInsigniaListDate,
                        (int)(sizeof(kInsigniaTitleIds) / sizeof(kInsigniaTitleIds[0])));
    ImGui::Separator();
    ImGui::TextUnformatted("To play online in xemu:");
    ImGui::BulletText("In xemu: Settings > Network, turn on Enable and choose "
                      "the NAT backend.");
    ImGui::BulletText("Follow Insignia's Connect guide: set the DNS address it "
                      "gives you in the Xbox dashboard's network settings, then "
                      "register with the Insignia Setup Assistant and create "
                      "your account.");
    ImGui::BulletText("Run the dashboard's network test, then start a "
                      "supported game and sign in.");
    ImGui::TextDisabled("Insignia is an independent service, not part of xemu. "
                        "For setup help, use Insignia's Discord.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button("Connect guide")) {
        SDL_OpenURL("https://insignia.live/connect");
    }
    ImGui::SameLine();
    if (ImGui::Button("Who's playing")) {
        SDL_OpenURL("https://insignia.live/games");
    }
    ImGui::SetItemTooltip("Insignia's games page: players online and active "
                          "sessions per game, in your browser");
    ImGui::SameLine();
    if (ImGui::Button(g_online_only ? "Show all games" : "Show my online games")) {
        g_online_only = !g_online_only;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(90 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
        ImGui::TextUnformatted("No game folders yet.");
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
    std::string drop_file, unhide;
    if (!g_extra_files.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * s));
        ImGui::TextDisabled("Games added individually");
        for (auto &f : g_extra_files) {
            ImGui::PushID(f.c_str());
            if (ImGui::SmallButton(ICON_FA_XMARK)) {
                drop_file = f;
            }
            ImGui::SetItemTooltip("Remove from library (the file is not deleted)");
            ImGui::SameLine();
            ImGui::TextUnformatted(f.c_str());
            ImGui::PopID();
        }
    }
    if (!g_hidden.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * s));
        ImGui::TextDisabled("Hidden games (removed from the library)");
        for (auto &h : g_hidden) {
            ImGui::PushID(h.c_str());
            if (ImGui::SmallButton("Restore")) {
                unhide = h;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(h.c_str());
            ImGui::PopID();
        }
    }
    if (!drop_file.empty()) {
        RemoveFromLibrary(drop_file);
    }
    if (!unhide.empty()) {
        g_hidden.erase(unhide);
        SaveLibrary();
    }

    ImGui::Dummy(ImVec2(0, 4 * s));
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Add folder...")) {
        AddGameFolder();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add game...")) {
        AddGameFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Done", ImVec2(100 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Preview videos
// ---------------------------------------------------------------------------

fs::path PreviewDir()
{
    return BasePath() / "previews";
}

// <base>/previews/<TITLEID>.<ext>, then <image name>.<ext> next to the image
std::string FindPreview(const LibraryEntry &e)
{
    std::error_code ec;
    fs::path img = fs::u8path(e.path);
    for (int i = 0; kPreviewExtensions[i]; i++) {
        if (e.valid) {
            fs::path p = PreviewDir() / (TitleIdHex(e.title_id) + kPreviewExtensions[i]);
            if (fs::is_regular_file(p, ec)) {
                return p.u8string();
            }
        }
        fs::path p = img.parent_path() / (img.stem().u8string() + kPreviewExtensions[i]);
        if (fs::is_regular_file(p, ec)) {
            return p.u8string();
        }
    }
    return "";
}

// FindPreview touches the disk, so remember the answer per game; cleared
// when previews are added or removed and on rescan.
std::map<std::string, std::string> g_preview_cache;

std::string FindPreviewCached(const LibraryEntry &e)
{
    // Forget answers after a few seconds so newly added videos show up
    static uint64_t cleared_at = 0;
    uint64_t now = SDL_GetTicks();
    if (now - cleared_at > 3000) {
        g_preview_cache.clear();
        cleared_at = now;
    }
    auto it = g_preview_cache.find(e.path);
    if (it == g_preview_cache.end()) {
        it = g_preview_cache.emplace(e.path, FindPreview(e)).first;
    }
    return it->second;
}

void RemoveImportedPreview(uint32_t title_id)
{
    g_preview_cache.clear();
    std::error_code ec;
    for (int i = 0; kPreviewExtensions[i]; i++) {
        fs::remove(PreviewDir() / (TitleIdHex(title_id) + kPreviewExtensions[i]), ec);
    }
}

void FinishPreviewImport()
{
    PendingPreviewImport imp = g_pending_import;
    g_pending_import = PendingPreviewImport();
    fs::path src = fs::u8path(imp.source);
    std::string ext = Lower(src.extension().u8string());
    bool ok_ext = false;
    for (int i = 0; kPreviewExtensions[i]; i++) {
        ok_ext |= (ext == kPreviewExtensions[i]);
    }
    if (!ok_ext) {
        xemu_queue_error_message(
            "Unsupported video type. Use MP4, WMV, AVI, MOV or MKV.");
        return;
    }
    std::error_code ec;
    fs::create_directories(PreviewDir(), ec);
    RemoveImportedPreview(imp.title_id);
    fs::path dst = PreviewDir() / (TitleIdHex(imp.title_id) + ext);
    if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
        xemu_queue_error_message("Could not copy the video.");
        return;
    }
    g_preview_cache.clear();
    g_selected_path = imp.game_path;
    g_preview_open_requested = dst.u8string();
    xemu_queue_notification("Preview video added");
}

void ImportPreview(const LibraryEntry &e)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Videos (*.mp4, *.m4v, *.wmv, *.avi, *.mov, *.mkv)",
          "mp4;m4v;wmv;avi;mov;mkv" },
        { "All Files", "*" },
    };
    uint32_t id = e.title_id;
    std::string game_path = e.path;
    ShowOpenFileDialog(filters, 2, NULL, [id, game_path](const char *path) {
        // Applied by ApplyPreviewRequests() at the start of the next frame,
        // after stopping the player (which may be reading the old file).
        g_pending_import = { true, path, id, game_path };
    });
}

// xemu snapshots taken while playing this game, newest first
void DrawSaveStates(LibraryEntry &e, float w)
{
    const float s = g_viewport_mgr.m_scale;
    g_snapshot_mgr.Refresh(); // cached by xemu; cheap unless snapshots changed

    std::string key = PathKeyCached(e.path);
    std::string title = Lower(e.title);
    std::vector<int> mine;
    for (int i = 0; i < g_snapshot_mgr.m_snapshots_len; i++) {
        const XemuSnapshotData &d = g_snapshot_mgr.m_extra_data[i];
        bool same_disc =
            d.disc_path && d.disc_path[0] &&
            PathKeyCached(OriginalDiscPath(d.disc_path)) == key;
        bool same_title = !title.empty() && d.xbe_title_name &&
                          Lower(d.xbe_title_name) == title;
        if (same_disc || same_title) {
            mine.push_back(i);
        }
    }
    if (mine.empty()) {
        return;
    }
    std::sort(mine.begin(), mine.end(), [](int a, int b) {
        return g_snapshot_mgr.m_snapshots[a].date_sec >
               g_snapshot_mgr.m_snapshots[b].date_sec;
    });

    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::Separator();
    ImGui::TextDisabled("Save states (%d)", (int)mine.size());
    const float thumb_h = 42 * s, thumb_w = thumb_h * 4 / 3;
    int shown = 0;
    std::string load_name; // loaded after the loop: loading refreshes the list
    for (int i : mine) {
        if (shown++ >= 6) {
            ImGui::TextDisabled("More in Settings > Snapshots");
            break;
        }
        const QEMUSnapshotInfo &info = g_snapshot_mgr.m_snapshots[i];
        const XemuSnapshotData &d = g_snapshot_mgr.m_extra_data[i];
        ImGui::PushID(info.name);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::Selectable("##snap", false, 0, ImVec2(w, thumb_h));
        ImGui::SetItemTooltip("Load this save state");
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (d.gl_thumbnail) {
            dl->AddImageRounded((ImTextureID)(intptr_t)d.gl_thumbnail, p,
                                ImVec2(p.x + thumb_w, p.y + thumb_h),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                                4 * s);
        } else {
            dl->AddRectFilled(p, ImVec2(p.x + thumb_w, p.y + thumb_h),
                              IM_COL32(40, 40, 44, 255), 4 * s);
        }
        time_t t = (time_t)info.date_sec;
        char date[48] = "";
        struct tm *tm = localtime(&t);
        if (tm) {
            strftime(date, sizeof(date), "%Y-%m-%d %H:%M", tm);
        }
        float tx = p.x + thumb_w + 8 * s;
        ImVec4 clip(tx, p.y, p.x + w, p.y + thumb_h);
        float fh = ImGui::GetFontSize();
        dl->AddText(ImGui::GetFont(), fh, ImVec2(tx, p.y + 2 * s),
                    ImGui::GetColorU32(ImGuiCol_Text), info.name, NULL, 0, &clip);
        dl->AddText(ImGui::GetFont(), fh * 0.9f, ImVec2(tx, p.y + fh + 6 * s),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), date, NULL, 0,
                    &clip);
        if (clicked) {
            load_name = info.name;
        }
        ImGui::PopID();
    }
    if (!load_name.empty()) {
        ActionLoadSnapshotChecked(load_name.c_str());
        game_library_window.is_open = false;
    }
}

// This game's screenshots (xemu-<TITLEID>-*.png in the screenshot folder),
// newest first. The folder listing is cached for a few seconds.
std::vector<std::string> ScreenshotsFor(uint32_t title_id)
{
    static std::map<uint32_t, std::vector<std::string>> cache;
    static uint64_t cached_at = 0;
    uint64_t now = SDL_GetTicks();
    if (now - cached_at > 3000) {
        cache.clear();
        cached_at = now;
    }
    auto it = cache.find(title_id);
    if (it != cache.end()) {
        return it->second;
    }
    std::vector<std::string> out;
    const char *dir = g_config.general.screenshot_dir;
    std::string prefix = "xemu-" + TitleIdHex(title_id) + "-";
    std::error_code ec;
    for (fs::directory_iterator d(fs::u8path(dir && dir[0] ? dir : "."), ec), end;
         !ec && d != end; d.increment(ec)) {
        std::string name = d->path().filename().u8string();
        if (name.compare(0, prefix.size(), prefix) == 0 &&
            Lower(d->path().extension().u8string()) == ".png") {
            out.push_back(d->path().u8string());
        }
    }
    std::sort(out.rbegin(), out.rend()); // names carry the date
    cache[title_id] = out;
    return out;
}

void DrawScreenshots(const LibraryEntry &e, float w)
{
    if (!e.valid) {
        return;
    }
    std::vector<std::string> shots = ScreenshotsFor(e.title_id);
    if (shots.empty()) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::Separator();
    ImGui::TextDisabled("Screenshots (%d)", (int)shots.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder")) {
        const char *dir = g_config.general.screenshot_dir;
        std::string p = fs::absolute(fs::u8path(dir && dir[0] ? dir : ".")).generic_u8string();
        SDL_OpenURL(("file:///" + p).c_str());
    }
    const int per_row = 3;
    float gap = 4 * s;
    float tw = (w - gap * (per_row - 1)) / per_row, th = tw * 3 / 4;
    int shown = 0;
    for (auto &path : shots) {
        if (shown == 6) {
            break;
        }
        if (shown % per_row) {
            ImGui::SameLine(0, gap);
        }
        shown++;
        RequestScreenshotThumb(path);
        ImGui::PushID(path.c_str());
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##shot", ImVec2(tw, th))) {
            SDL_OpenURL(("file:///" + fs::u8path(path).generic_u8string()).c_str());
        }
        ImGui::SetItemTooltip("%s", fs::u8path(path).filename().u8string().c_str());
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ShotThumb &t = g_shot_thumbs[path];
        dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + th), IM_COL32(30, 30, 34, 255), 3 * s);
        if (t.tex && t.w > 0) {
            float sc = std::min(tw / t.w, th / t.h);
            ImVec2 a(p.x + (tw - t.w * sc) / 2, p.y + (th - t.h * sc) / 2);
            dl->AddImageRounded((ImTextureID)(intptr_t)t.tex, a,
                                ImVec2(a.x + t.w * sc, a.y + t.h * sc), ImVec2(0, 0),
                                ImVec2(1, 1), IM_COL32_WHITE, 3 * s);
        }
        if (ImGui::IsItemHovered()) {
            dl->AddRect(p, ImVec2(p.x + tw, p.y + th), kAccent, 3 * s, 0, 2 * s);
        }
        ImGui::PopID();
    }
}

// Right-hand panel with the selected game's art and details
void DrawDetailsPanel(LibraryEntry &e, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    GameStats &st = g_stats[e.path];
    float w = ImGui::GetContentRegionAvail().x;
    EnsureTexture(e, tex_budget);

    std::string preview = FindPreviewCached(e);
    bool playing = g_preview->IsOpen() && !PreviewStopping() &&
                   !preview.empty() && g_preview->Path() == preview;
    if (g_preview->IsOpen() && !playing && !PreviewStopping()) {
        RequestPreviewStop(); // selection changed
    }

    ImVec2 a = ImGui::GetCursorScreenPos();
    float art = w;
    int vw = 0, vh = 0;
    GLuint vtex = playing ? g_preview->Update(&vw, &vh) : 0;
    if (vtex && vw > 0) {
        art = std::min(w * vh / (float)vw, w * 1.5f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + art), IM_COL32(0, 0, 0, 255), 8 * s);
        float sc = std::min(w / vw, art / vh);
        ImVec2 ia(a.x + (w - vw * sc) / 2, a.y + (art - vh * sc) / 2);
        dl->AddImageRounded((ImTextureID)(intptr_t)vtex, ia,
                            ImVec2(ia.x + vw * sc, ia.y + vh * sc), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, 8 * s);
    } else {
        if (e.tex && e.tex_w > 0) {
            art = std::min(w * e.tex_h / (float)e.tex_w, w * 1.5f);
        }
        DrawArtwork(ImGui::GetWindowDrawList(), a, ImVec2(a.x + w, a.y + art), e,
                    8 * s);
    }
    ImGui::Dummy(ImVec2(w, art));
    if (!preview.empty() && PreviewPlayer::Supported()) {
        if (ImGui::SmallButton(playing ? ICON_FA_XMARK "  Stop preview"
                                       : ICON_FA_PLAY "  Watch preview")) {
            if (playing) {
                RequestPreviewStop();
            } else {
                RequestPreviewOpen(preview);
            }
        }
        std::string perr = playing ? g_preview->Error() : std::string();
        if (!perr.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1), "%s", perr.c_str());
            ImGui::PopTextWrapPos();
        }
    }
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

    g_snapshot_mgr.Refresh(); // cached by xemu
    int resume = FindResumeSnapshot(e);
    if (resume >= 0) {
        const QEMUSnapshotInfo &info = g_snapshot_mgr.m_snapshots[resume];
        time_t t = (time_t)info.date_sec;
        char when[48] = "";
        struct tm *tm = localtime(&t);
        if (tm) {
            strftime(when, sizeof(when), "%b %d, %H:%M", tm);
        }
        std::string label = std::string(ICON_FA_PLAY "  Resume  ") + when;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.78f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.5f, 0.88f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.36f, 0.68f, 1));
        if (ImGui::Button(label.c_str(), ImVec2(w, 40 * s))) {
            std::string name = info.name; // the list may refresh on load
            RequestPreviewStop();
            ActionLoadSnapshotChecked(name.c_str());
            game_library_window.is_open = false;
        }
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Continue from where you left off");
    }
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
    int npatch = e.valid ? EnabledPatchCount(e.title_id) : 0;
    std::string patch_label =
        npatch ? string_format("Patches (%d on)", npatch) : std::string("Patches");
    if (ImGui::Button(patch_label.c_str(), ImVec2(w, 0))) {
        OpenPatchesFor(e);
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
    if (SupportsOnline(e)) {
        row("Online", "Supported on Insignia");
        if (ImGui::SmallButton("How to play online...")) {
            g_open_insignia_popup = true;
        }
    }
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

    DrawSaveStates(e, w);
    DrawScreenshots(e, w);
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

static void GameLibraryShutdown(void);
static std::vector<RecentGame> GetRecentGamesUncached(int max_count);

void GameLibraryWindow::Open()
{
    is_open = true;
    g_focus_requested = true;
}

void GameLibraryWindow::OpenInsignia()
{
    Open();
    g_open_insignia_popup = true;
}

void GameLibraryWindow::DownloadCovers()
{
    Open();
    StartCoverDownload();
}

void GameLibraryWindow::AddGame()
{
    Open();
    AddGameFile();
}

void GameLibraryWindow::OpenFolderManager()
{
    Open();
    g_open_folders_popup = true;
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
    g_preview_cache.clear();
    g_pathkey_cache.clear();
    FreeScreenshotThumbs();
    StartScan();
}

namespace {

// Load and boot a game (no resume-point handling)
void LaunchNow(const std::string &path)
{
    LibraryEntry *e = FindEntry(path);
    std::string name = e ? e->DisplayName() : fs::u8path(path).stem().u8string();
    RequestPreviewStop();
    std::string load_path = path;
    if (e) {
        std::string error;
        if (!PrepareLaunchImage(*e, load_path, error)) {
            std::string msg = "Could not apply patches for " + name + ": " + error;
            xemu_queue_error_message(msg.c_str());
            return;
        }
    }
    ActionLoadDiscFile(load_path.c_str());
    const char *loaded = g_config.sys.files.dvd_path;
    if (!loaded || load_path != loaded) {
        return; // the load failed and xemu already showed why
    }
    if (load_path != path) {
        name += " (patched)";
    }
    // Restart the console so it boots the disc, like powering on a real Xbox
    // with the game in the tray. Swapping the disc alone leaves many
    // dashboards and BIOSes sitting at the menu until a manual reset.
    ActionReset();
    if (!runstate_is_running()) {
        vm_start();
    }
    std::string msg = "Starting " + name;
    xemu_queue_notification(msg.c_str());
    game_library_window.is_open = false;
}

} // namespace


void GameLibraryWindow::Launch(const std::string &path)
{
    LibraryEntry *cur = CurrentGame();
    if (ShouldSaveResumePoint() && cur && cur->path != path) {
        // Leaving a running game: save its resume point first (in Update)
        g_leave_action = LeaveAction::Launch;
        g_leave_launch_path = path;
        is_open = false;
        xemu_queue_notification("Saving a resume point...");
        return;
    }
    LaunchNow(path);
}

std::vector<RecentGame> GameLibraryWindow::GetRecentGames(int max_count)
{
    // Called every frame by the library and the menu; checking that each
    // file still exists touches the disk, so refresh at most every 2 s.
    static std::vector<RecentGame> cached;
    static int cached_count = -1;
    static uint64_t cached_at = 0;
    uint64_t now = SDL_GetTicks();
    if (cached_count == max_count && now - cached_at < 2000) {
        return cached;
    }
    cached_count = max_count;
    cached_at = now;
    cached = GetRecentGamesUncached(max_count);
    return cached;
}

static std::vector<RecentGame> GetRecentGamesUncached(int max_count)
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
    ApplyPreviewRequests();
    FlushDeletedTextures();
    RunLeaveAction();
    MergeScanResults();
    MergeCoverResults();
    UploadDecodedImages(2);

    // Track disc changes: last played + per-game settings
    const char *dvd = g_config.sys.files.dvd_path;
    std::string cur = (dvd && dvd[0]) ? OriginalDiscPath(dvd) : "";
    if (cur != g_last_dvd_path) {
        if (cur.empty() && !g_last_dvd_path.empty()) {
            Open(); // disc ejected: back to the library to pick the next game
        }
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
        if (g_preview->IsOpen() && !PreviewStopping()) {
            RequestPreviewStop();
        }
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
    // Slightly see-through so the boot animation or game shows behind it
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 0.80f));
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
    if (folders.empty() && g_extra_files.empty()) {
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
        ImGui::SetCursorPosX(x0 + (w - 240 * s) / 2);
        if (ImGui::Button("Add a single game...", ImVec2(240 * s, 0))) {
            AddGameFile();
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
    ImGui::SameLine(0, 2 * s);
    if (toggle("Online", g_online_only,
               "Show only games playable online with Insignia")) {
        g_online_only = !g_online_only;
    }
    ImGui::SameLine(0, 2 * s);
    if (ImGui::Button("Insignia")) {
        g_open_insignia_popup = true;
    }
    {
        int st = g_insignia_status;
        if (st == (int)InsigniaStatus::Up || st == (int)InsigniaStatus::Maintenance ||
            st == (int)InsigniaStatus::Down) {
            ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
            ImU32 col = st == (int)InsigniaStatus::Up ? IM_COL32(70, 200, 70, 255)
                        : st == (int)InsigniaStatus::Maintenance
                            ? IM_COL32(235, 180, 40, 255)
                            : IM_COL32(230, 70, 60, 255);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(mx.x - 4 * s, mn.y + 4 * s), 3.5f * s, col);
        }
    }
    ImGui::SetItemTooltip("Play original Xbox Live games online again");
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
    ImGui::BeginDisabled(g_covers_running);
    if (ImGui::Button(g_covers_running
                          ? string_format("Covers %d/%d", (int)g_covers_done,
                                          (int)g_covers_total).c_str()
                          : "Get covers")) {
        StartCoverDownload();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Download box art for games without a cover, from "
                          "xemu's game database (xemu-project/xdb on GitHub). "
                          "Your own covers always take priority.");
    ImGui::SameLine(0, 2 * s);
    if (ImGui::Button("+ Add game")) {
        AddGameFile();
    }
    ImGui::SetItemTooltip("Add a single disc image to the library");
    ImGui::SameLine(0, 2 * s);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Folders")) {
        g_open_folders_popup = true;
    }
    ImGui::SetItemTooltip("%d game folder%s", (int)folders.size(),
                          folders.size() == 1 ? "" : "s");
    ImGui::SameLine(0, 2 * s);
    ImGui::BeginDisabled(g_scan_running);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Scan folders")) {
        game_library_window.Rescan();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Look for new or removed games and covers");
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

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##content",
                      ImVec2(show_panel ? avail_w - panel_w - gap : 0, 0),
                      ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleColor();
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
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 0.88f));
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
    DrawPatchesPopup();
    DrawInsigniaPopup();

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

bool xemu_hud_intercept_quit(void)
{
    if (g_leave_action == LeaveAction::Quit) {
        return true; // already saving; quit follows
    }
    if (!ShouldSaveResumePoint()) {
        return false;
    }
    g_leave_action = LeaveAction::Quit;
    xemu_queue_notification("Saving a resume point...");
    return true;
}

bool GameLibraryInterceptEject(void)
{
    if (!ShouldSaveResumePoint()) {
        return false;
    }
    g_leave_action = LeaveAction::Eject;
    xemu_queue_notification("Saving a resume point...");
    return true;
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
    if (g_cover_thread.joinable()) {
        g_cover_thread.join();
    }
    g_preview->Close(false);
    StopImageWorker();
    if (g_insignia_thread.joinable()) {
        g_insignia_thread.join();
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
