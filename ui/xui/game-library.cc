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
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fpng.h>
#include <nlohmann/json.hpp>

#include "common.hh"
#include "game-library.hh"
#include "game-library-scan.hh"
#include "rtx-video.hh"
#include "xbox-drive-explorer.hh"
#include "perf-capture.hh"
#include "shader-cache.hh"
#include "xbox-drive.hh"
#include "game-patches.hh"
#include "game-compat.hh"
#include "game-store.hh"
#include "game-preview.hh"
#include "insignia-titles.hh"
#include "jmp-index.hh"
#include "game-shortcut.hh"
#include "discord-presence.hh"
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
extern "C" {
#include "hw/xbox/eeprom_video.h"
}

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
    std::vector<int> toggled; // .jmp options flipped from their default
};
// Repository paths of online patches already downloaded (or skipped
// because they don't fit the user's copy), so bulk downloads skip them
std::set<std::string> g_jmp_fetched;
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

// Which BIOS/boot ROM each resume point was saved with (by title ID).
// A snapshot restores RAM, including the running kernel, but not the
// flash ROM, so resuming under a different BIOS can crash the guest.
std::map<uint32_t, std::string> g_resume_machine;
std::string g_resume_warn_name;      // snapshot awaiting confirmation
std::string g_resume_warn_saved_with;
bool g_open_resume_warning = false;

std::string MachineSignature()
{
    const char *flash = g_config.sys.files.flashrom_path;
    const char *boot = g_config.sys.files.bootrom_path;
    return std::string(flash ? flash : "") + "|" + (boot ? boot : "");
}

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
int g_settings_shader_count = 0;  // its cached shader files, when opened
bool g_settings_shader_confirm = false;
std::string g_settings_shader_msg;
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
        json resume_machine = j.value("resume_machine", json::object());
        for (auto &[id, sig] : resume_machine.items()) {
            if (sig.is_string()) {
                g_resume_machine[(uint32_t)strtoul(id.c_str(), NULL, 16)] =
                    sig.get<std::string>();
            }
        }
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
                json toggled = it.value("toggled", json::array());
                for (auto &t : toggled) {
                    if (t.is_number_integer()) {
                        pi.toggled.push_back(t.get<int>());
                    }
                }
                if (!pi.file.empty()) {
                    items.push_back(pi);
                }
            }
            g_patches[(uint32_t)strtoul(id.c_str(), NULL, 16)] = items;
        }
        json fetched = j.value("patches_fetched", json::array());
        for (auto &f : fetched) {
            if (f.is_string()) {
                g_jmp_fetched.insert(f.get<std::string>());
            }
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
    json resume_machine = json::object();
    for (auto &[id, sig] : g_resume_machine) {
        resume_machine[TitleIdHex(id)] = sig;
    }
    j["resume_machine"] = resume_machine;
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
            json item = { { "file", pi.file }, { "enabled", pi.enabled } };
            if (!pi.toggled.empty()) {
                item["toggled"] = pi.toggled;
            }
            list.push_back(item);
        }
        patches[TitleIdHex(id)] = list;
    }
    j["patches"] = patches;
    j["patches_fetched"] = g_jmp_fetched;
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
// Optional 3D box art (e.g. covers\MS-004-3d.png, transparent PNGs as used by
// other frontends), by entry path. Loaded separately from the flat cover so
// the Library keeps flat covers.
struct Box3DArt {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool requested = false;
};
std::map<std::string, Box3DArt> g_box3d;

std::vector<fs::path> Box3DCandidates(const LibraryEntry &e)
{
    std::vector<fs::path> out;
    fs::path img = fs::u8path(e.path);
    std::vector<std::string> names;
    if (e.valid) {
        names.push_back(TitleIdHex(e.title_id));
        names.push_back(FormatTitleId(e.title_id));
    }
    names.push_back(img.stem().u8string());
    for (auto &folder : AllFolders()) {
        fs::path covers = fs::u8path(folder.path) / "covers";
        for (auto &n : names) {
            out.push_back(covers / (n + "-3d.png"));
            out.push_back(covers / "3d" / (n + ".png"));
        }
    }
    out.push_back(img.parent_path() / (img.stem().u8string() + "-3d.png"));
    return out;
}

const Box3DArt *Get3DArt(const LibraryEntry &e)
{
    Box3DArt &b = g_box3d[e.path];
    if (!b.requested) {
        b.requested = true;
        ImageJob job{ "box3d:" + e.path, Box3DCandidates(e), 700 };
        {
            std::lock_guard<std::mutex> lock(g_img_mutex);
            if (g_img_quit) {
                return &b;
            }
            g_img_jobs.push_back(std::move(job));
        }
        if (!g_img_thread.joinable()) {
            g_img_thread = std::thread(ImageWorker);
        }
        g_img_cv.notify_one();
    }
    return &b;
}

// Homebrew store covers and screenshots ("c:<id>", "s:<id>")
struct StoreImage {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool requested = false;
};
std::map<std::string, StoreImage> g_store_images;

void FreeStoreImages()
{
    for (auto &kv : g_store_images) {
        DeleteTextureLater(kv.second.tex);
    }
    g_store_images.clear();
}

void FreeBox3DArt()
{
    for (auto &kv : g_box3d) {
        DeleteTextureLater(kv.second.tex);
    }
    g_box3d.clear();
}

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
        if (r.key.compare(0, 6, "store:") == 0) {
            auto it = g_store_images.find(r.key.substr(6));
            if (it != g_store_images.end() && !r.rgba.empty()) {
                DeleteTextureLater(it->second.tex);
                it->second.tex = UploadTexture(r.rgba.data(), r.w, r.h);
                it->second.w = r.w;
                it->second.h = r.h;
            }
            continue;
        }
        if (r.key.compare(0, 6, "box3d:") == 0) {
            auto it = g_box3d.find(r.key.substr(6));
            if (it != g_box3d.end() && !r.rgba.empty()) {
                DeleteTextureLater(it->second.tex);
                it->second.tex = UploadTexture(r.rgba.data(), r.w, r.h);
                it->second.w = r.w;
                it->second.h = r.h;
            }
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
        return;
    }
    g_resume_machine[e->title_id] = MachineSignature();
    SaveLibrary();
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
    return ext == ".xbe" || ext == ".ips" || ext == ".bps" || ext == ".jmp";
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

std::vector<PatchSpec> EnabledPatches(uint32_t title_id)
{
    std::vector<PatchSpec> out;
    auto it = g_patches.find(title_id);
    if (it == g_patches.end()) {
        return out;
    }
    for (auto &pi : it->second) {
        if (pi.enabled) {
            out.push_back({ (PatchDir(title_id) / fs::u8path(pi.file)).u8string(),
                            pi.toggled });
        }
    }
    return out;
}

int EnabledPatchCount(uint32_t title_id)
{
    auto it = g_patches.find(title_id);
    int n = 0;
    if (it != g_patches.end()) {
        for (auto &pi : it->second) {
            n += pi.enabled ? 1 : 0;
        }
    }
    return n;
}

// Overlay file name depends on the image and the exact patch files, so any
// change produces a fresh overlay.
std::string OverlayPathFor(const LibraryEntry &e,
                           const std::vector<PatchSpec> &patches)
{
    std::error_code ec;
    std::string sig = e.path + "|" + std::to_string(e.file_size) + "|" +
                      std::to_string(e.mtime);
    for (auto &spec : patches) {
        fs::path fp = fs::u8path(spec.path);
        sig += "|" + spec.path + "|" + std::to_string(fs::file_size(fp, ec)) +
               "|" +
               std::to_string(fs::last_write_time(fp, ec).time_since_epoch().count());
        for (int t : spec.toggled) {
            sig += "," + std::to_string(t);
        }
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
    std::vector<PatchSpec> patches = EnabledPatches(e.title_id);
    if (patches.empty()) {
        return true;
    }
    std::string overlay = OverlayPathFor(e, patches);
    std::error_code ec;
    if (!fs::exists(fs::u8path(overlay), ec) &&
        !BuildPatchedOverlay(e.path, patches, overlay, error)) {
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

// ---------------------------------------------------------------------------
// Themes: Neon (dark, cyan/magenta, cut corners) and Green (rounded, xemu
// green). The classic library window always uses Green.
// ---------------------------------------------------------------------------

struct LibTheme {
    bool neon;
    ImU32 accent;  // selection, focus, highlights
    ImU32 accent2; // secondary highlights (Resume, ONLINE)
    ImU32 text, dim, panel, border, online;
    ImU32 ok, warn, bad; // status colors
    ImVec4 primary, on_primary, resume, glass, white;
};

const LibTheme kThemeGreen = {
    false,
    IM_COL32(0x62, 0xCA, 0x13, 0xFF), // xemu green
    IM_COL32(46, 127, 224, 255),
    IM_COL32(232, 239, 230, 255),
    IM_COL32(150, 165, 147, 255),
    IM_COL32(255, 255, 255, 12),
    IM_COL32(255, 255, 255, 22),
    IM_COL32(16, 124, 16, 255),
    IM_COL32(98, 202, 19, 255),
    IM_COL32(235, 180, 40, 255),
    IM_COL32(230, 70, 60, 255),
    ImVec4(0.384f, 0.792f, 0.075f, 1),
    ImVec4(0.03f, 0.06f, 0.04f, 1),
    ImVec4(0.18f, 0.50f, 0.88f, 1),
    ImVec4(1, 1, 1, 0.10f),
    ImVec4(0.95f, 0.97f, 0.94f, 1),
};

const LibTheme kThemeNeon = {
    true,
    IM_COL32(0, 229, 255, 255),   // electric cyan
    IM_COL32(255, 46, 136, 255),  // hot magenta
    IM_COL32(228, 232, 245, 255),
    IM_COL32(118, 128, 162, 255),
    IM_COL32(14, 13, 26, 165),
    IM_COL32(0, 229, 255, 46),
    IM_COL32(255, 46, 136, 255),
    IM_COL32(57, 255, 136, 255),  // status OK: neon green, never the accent
    IM_COL32(255, 196, 0, 255),
    IM_COL32(255, 59, 59, 255),
    ImVec4(0.0f, 0.898f, 1.0f, 1),
    ImVec4(0.02f, 0.02f, 0.05f, 1),
    ImVec4(1.0f, 0.18f, 0.53f, 0.10f), // Resume: outlined magenta
    ImVec4(0.0f, 0.898f, 1.0f, 0.06f),
    ImVec4(0.90f, 0.93f, 1.0f, 1),
};

// Library style: the tabbed dashboard, or the classic single library window
bool ClassicStyle()
{
    return g_config.general.library.style == CONFIG_GENERAL_LIBRARY_STYLE_CLASSIC;
}

const LibTheme &T()
{
    return (ClassicStyle() ||
            g_config.general.library.theme == CONFIG_GENERAL_LIBRARY_THEME_GREEN)
               ? kThemeGreen
               : kThemeNeon;
}

ImU32 Alpha(ImU32 c, int a)
{
    return (c & 0x00FFFFFF) | ((ImU32)a << 24);
}

// Neon cuts the top-left and bottom-right corners; Green rounds them
int ChamferPoints(ImVec2 a, ImVec2 b, float r, ImVec2 *pts)
{
    float c = std::min(r * 0.9f, std::min(b.x - a.x, b.y - a.y) * 0.3f);
    pts[0] = ImVec2(a.x + c, a.y);
    pts[1] = ImVec2(b.x, a.y);
    pts[2] = ImVec2(b.x, b.y - c);
    pts[3] = ImVec2(b.x - c, b.y);
    pts[4] = ImVec2(a.x, b.y);
    pts[5] = ImVec2(a.x, a.y + c);
    return 6;
}

void ShapeFill(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float r)
{
    if (!T().neon) {
        dl->AddRectFilled(a, b, col, r);
        return;
    }
    ImVec2 pts[6];
    dl->AddConvexPolyFilled(pts, ChamferPoints(a, b, r, pts), col);
}

void ShapeStroke(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float r, float th)
{
    if (!T().neon) {
        dl->AddRect(a, b, col, r, 0, th);
        return;
    }
    ImVec2 pts[6];
    dl->AddPolyline(pts, ChamferPoints(a, b, r, pts), col, ImDrawFlags_Closed, th);
}

// An image filling a shape: the box [a, b] shows the texture region uv0-uv1
void ShapeImage(ImDrawList *dl, ImTextureID tex, ImVec2 a, ImVec2 b, ImVec2 uv0,
                ImVec2 uv1, ImU32 col, float r)
{
    if (!T().neon) {
        dl->AddImageRounded(tex, a, b, uv0, uv1, col, r);
        return;
    }
    ImVec2 pts[6];
    int n = ChamferPoints(a, b, r, pts);
    dl->PushTextureID(tex);
    dl->PrimReserve((n - 2) * 3, n);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < n; i++) {
        ImVec2 t((pts[i].x - a.x) / (b.x - a.x), (pts[i].y - a.y) / (b.y - a.y));
        dl->PrimWriteVtx(pts[i],
                         ImVec2(uv0.x + t.x * (uv1.x - uv0.x),
                                uv0.y + t.y * (uv1.y - uv0.y)),
                         col);
    }
    for (int i = 1; i + 1 < n; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + i + 1));
    }
    dl->PopTextureID();
}

ImFont *FontOr(ImFont *f)
{
    return f ? f : ImGui::GetFont();
}

// Neon's small HUD labels use xemu's pixel font
ImFont *HudFont()
{
    return T().neon ? FontOr(g_font_mgr.m_fixed_width_font) : ImGui::GetFont();
}

std::string Upper(const std::string &t)
{
    std::string u;
    for (char c : t) {
        u += (char)toupper((unsigned char)c);
    }
    return u;
}

// Animation state (all driven by ImGui::GetTime)
struct DashAnim {
    double tab_t = -10;      // when the current tab (or the dashboard) appeared
    double hero_t = -10;     // when the Home hero switched games
    std::string hero_path;
    float hl_x = -1, hl_w = 0; // animated tab highlight
} g_anim;

bool Animations()
{
    return g_config.display.ui.use_animations;
}

// 0 -> 1 over `dur` seconds since `since`, easing out
float EaseSince(double since, float dur)
{
    if (!Animations()) {
        return 1.0f;
    }
    float k = (float)((ImGui::GetTime() - since) / dur);
    k = std::clamp(k, 0.0f, 1.0f);
    return 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
}

// Neon: thin HUD brackets on two opposite corners
void Brackets(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float len, float th)
{
    dl->AddLine(ImVec2(b.x - len, a.y - th * 2), ImVec2(b.x + th * 2, a.y - th * 2), col, th);
    dl->AddLine(ImVec2(b.x + th * 2, a.y - th * 2), ImVec2(b.x + th * 2, a.y + len), col, th);
    dl->AddLine(ImVec2(a.x - th * 2, b.y - len), ImVec2(a.x - th * 2, b.y + th * 2), col, th);
    dl->AddLine(ImVec2(a.x - th * 2, b.y + th * 2), ImVec2(a.x + len, b.y + th * 2), col, th);
}

bool IsCurrentDisc(const LibraryEntry &e)
{
    const char *p = g_config.sys.files.dvd_path;
    return p && p[0] && e.path == OriginalDiscPath(p);
}

CompatStatus CompatFor(const LibraryEntry &e); // compatibility, below

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
        if (sort == CONFIG_GENERAL_LIBRARY_SORT_COMPATIBILITY) {
            int ca = (int)CompatFor(*a), cb = (int)CompatFor(*b);
            if (ca != cb) {
                return ca > cb; // best first
            }
        }
        return Lower(a->DisplayName()) < Lower(b->DisplayName());
    });
}

void OpenSettingsFor(LibraryEntry &e)
{
    g_settings_path = e.path;
    g_settings_edit = g_overrides.count(e.title_id) ? g_overrides[e.title_id]
                                                    : GameOverrides();
    g_settings_shader_count = e.valid ? ShaderCacheCountFor(e.title_id) : 0;
    g_settings_shader_confirm = false;
    g_settings_shader_msg.clear();
    g_open_settings_popup = true;
}

// ---------------------------------------------------------------------------
// Online patches (Jay's Magic Patches collection on GitHub)
// ---------------------------------------------------------------------------

const char kJmpRawBase[] =
    "https://raw.githubusercontent.com/JayYardley/Xbox-Magic-Patches-by-Jay/";
const char kJmpTreeUrl[] = "https://api.github.com/repos/JayYardley/"
                           "Xbox-Magic-Patches-by-Jay/git/trees/main?recursive=1";

// Patches found by "Check for new patches", added to the built-in index:
// repository path -> title ID (0 = not a usable patch). Main thread only.
std::map<std::string, uint32_t> g_jmp_extra;
bool g_jmp_extra_loaded = false;

fs::path JmpExtraIndexFile()
{
    return IconCacheDir() / "patch-index.json";
}

void LoadJmpExtraIndex()
{
    if (g_jmp_extra_loaded) {
        return;
    }
    g_jmp_extra_loaded = true;
    std::ifstream f(JmpExtraIndexFile());
    if (!f) {
        return;
    }
    try {
        json j = json::parse(f);
        json paths = j.value("paths", json::object());
        for (auto &[path, id] : paths.items()) {
            if (id.is_string()) {
                g_jmp_extra[path] =
                    (uint32_t)strtoul(id.get<std::string>().c_str(), NULL, 16);
            }
        }
    } catch (const std::exception &) {
    }
}

void SaveJmpExtraIndex()
{
    json paths = json::object();
    for (auto &[path, id] : g_jmp_extra) {
        paths[path] = TitleIdHex(id);
    }
    json j = { { "version", 1 }, { "paths", paths } };
    std::error_code ec;
    fs::create_directories(IconCacheDir(), ec);
    std::ofstream f(JmpExtraIndexFile(), std::ios::trunc);
    f << j.dump(1);
}

bool InBuiltinIndex(const std::string &path)
{
    for (auto &ent : kJmpIndex) {
        if (path == ent.path) {
            return true;
        }
    }
    return false;
}

// Repository paths of the online patches for a game
std::vector<std::string> OnlinePatchPaths(uint32_t title_id)
{
    std::vector<std::string> out;
    const JmpIndexEntry *begin = kJmpIndex;
    const JmpIndexEntry *end = kJmpIndex + sizeof(kJmpIndex) / sizeof(kJmpIndex[0]);
    const JmpIndexEntry *it = std::lower_bound(
        begin, end, title_id,
        [](const JmpIndexEntry &a, uint32_t id) { return a.title_id < id; });
    for (; it != end && it->title_id == title_id; ++it) {
        out.push_back(it->path);
    }
    LoadJmpExtraIndex();
    for (auto &[path, id] : g_jmp_extra) {
        if (id == title_id) {
            out.push_back(path);
        }
    }
    return out;
}

int OnlinePatchCount(uint32_t title_id)
{
    return title_id ? (int)OnlinePatchPaths(title_id).size() : 0;
}

std::string UrlEncodePath(const std::string &path)
{
    std::string out;
    for (unsigned char c : path) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == '/') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

bool HttpGetString(const std::string &url, std::string &body)
{
    GByteArray *data = g_byte_array_new();
    int code = http_get(url.c_str(), data, NULL, NULL);
    body.assign((const char *)data->data, data->len);
    g_byte_array_unref(data);
    return code == 200;
}

// Download one patch: the latest version first, then the version the
// built-in index was made from (in case the file has since moved)
bool DownloadJmp(const std::string &path, std::string &text)
{
    if (HttpGetString(kJmpRawBase + std::string("main/") + UrlEncodePath(path),
                      text)) {
        return true;
    }
    return InBuiltinIndex(path) &&
           HttpGetString(kJmpRawBase + std::string(kJmpRepoCommit) + "/" +
                             UrlEncodePath(path),
                         text);
}

struct PatchDownloadJob {
    uint32_t title_id;
    std::string disc_path;
    std::vector<std::string> repo_paths;
};

struct PatchDownloadResult {
    uint32_t title_id;
    std::string repo_path;
    std::string text; // empty: skipped (doesn't fit, or download failed)
    bool fits = false;
    bool failed = false;
};

std::thread g_patchdl_thread;
std::mutex g_patchdl_mutex;
std::atomic<bool> g_patchdl_running{ false };
std::atomic<int> g_patchdl_done{ 0 };
int g_patchdl_total = 0;
bool g_patchdl_finished = false;  // under the mutex
bool g_patchdl_quiet = false;     // no summary when nothing was found
std::vector<PatchDownloadResult> g_patchdl_results;   // under the mutex
std::map<std::string, uint32_t> g_patchdl_new_index;  // under the mutex
bool g_patchdl_index_failed = false;                  // under the mutex

void PatchDownloadWorker(std::vector<PatchDownloadJob> jobs, bool check_new,
                         std::set<std::string> known_paths)
{
    if (check_new) {
        // List the repository and read the title IDs of new .jmp files
        std::string body;
        std::map<std::string, uint32_t> found;
        bool ok = HttpGetString(kJmpTreeUrl, body);
        if (ok) {
            try {
                json j = json::parse(body);
                for (auto &ent : j.value("tree", json::array())) {
                    std::string p = ent.value("path", "");
                    std::string ext = Lower(fs::u8path(p).extension().u8string());
                    if (ent.value("type", "") != "blob" || ext != ".jmp" ||
                        known_paths.count(p)) {
                        continue;
                    }
                    if (g_scan_cancel) {
                        break;
                    }
                    std::string text, err;
                    JmpPatch jmp;
                    found[p] = DownloadJmp(p, text) && ParseJmp(text, jmp, err) &&
                                       !jmp.other_target
                                   ? jmp.title_id
                                   : 0;
                }
            } catch (const std::exception &) {
                ok = false;
            }
        }
        std::lock_guard<std::mutex> lock(g_patchdl_mutex);
        g_patchdl_new_index = std::move(found);
        g_patchdl_index_failed = !ok;
    }

    for (auto &job : jobs) {
        if (g_scan_cancel) {
            break;
        }
        std::vector<uint8_t> xbe;
        std::string err;
        bool have_xbe = ReadDefaultXbe(job.disc_path, xbe, err);
        for (auto &path : job.repo_paths) {
            PatchDownloadResult r;
            r.title_id = job.title_id;
            r.repo_path = path;
            std::string text;
            JmpPatch jmp;
            if (!DownloadJmp(path, text) || !ParseJmp(text, jmp, err)) {
                r.failed = true;
            } else {
                JmpFit fit = have_xbe ? CheckJmp(jmp, xbe, job.title_id)
                                      : JmpFit::Unverified;
                r.fits = fit == JmpFit::Works || fit == JmpFit::Unverified;
                if (r.fits) {
                    r.text = std::move(text);
                }
            }
            std::lock_guard<std::mutex> lock(g_patchdl_mutex);
            g_patchdl_results.push_back(std::move(r));
        }
        g_patchdl_done++;
    }
    std::lock_guard<std::mutex> lock(g_patchdl_mutex);
    g_patchdl_finished = true;
    g_patchdl_running = false;
}

bool EnsureDownloadsReady()
{
    Error *err = NULL;
    if (!ensure_libcurl_initialized(&err)) {
        xemu_queue_error_message(err ? error_get_pretty(err)
                                     : "Could not start downloads");
        if (err) {
            error_free(err);
        }
        return false;
    }
    return true;
}

// Download patches for one game (title_id != 0, also re-fetches) or for the
// whole library. check_new also looks for patches added since this build.
void StartPatchDownload(uint32_t only_title, bool check_new)
{
    if (g_patchdl_running) {
        xemu_queue_notification("Patch download already in progress");
        return;
    }
    if (g_patchdl_thread.joinable()) {
        g_patchdl_thread.join();
    }
    std::vector<PatchDownloadJob> jobs;
    std::set<uint32_t> seen;
    for (auto &e : g_entries) {
        if (!e.valid || seen.count(e.title_id) ||
            (only_title && e.title_id != only_title)) {
            continue;
        }
        PatchDownloadJob job{ e.title_id, e.path, {} };
        for (auto &p : OnlinePatchPaths(e.title_id)) {
            if (only_title || !g_jmp_fetched.count(p)) {
                job.repo_paths.push_back(p);
            }
        }
        seen.insert(e.title_id);
        if (!job.repo_paths.empty()) {
            jobs.push_back(std::move(job));
        }
    }
    if (jobs.empty() && !check_new) {
        xemu_queue_notification(only_title
                                    ? "No online patches for this game"
                                    : "No new online patches for your games");
        return;
    }
    if (!EnsureDownloadsReady()) {
        return;
    }
    std::set<std::string> known;
    if (check_new) {
        LoadJmpExtraIndex();
        for (auto &ent : kJmpIndex) {
            known.insert(ent.path);
        }
        for (auto &[path, id] : g_jmp_extra) {
            known.insert(path);
        }
    }
    g_patchdl_done = 0;
    g_patchdl_total = (int)jobs.size();
    g_patchdl_quiet = false;
    g_patchdl_running = true;
    g_patchdl_thread =
        std::thread(PatchDownloadWorker, std::move(jobs), check_new, std::move(known));
}

// Write a downloaded patch into the game's patch folder. Returns the file
// name, or "" if an identical file is already there.
std::string StoreDownloadedPatch(uint32_t title_id, const std::string &repo_path,
                                 const std::string &text)
{
    fs::path dir = PatchDir(title_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path base = fs::u8path(repo_path).filename();
    for (int n = 1; n < 100; n++) {
        fs::path name = n == 1 ? base
                               : fs::u8path(base.stem().u8string() + " (" +
                                            std::to_string(n) + ")" +
                                            base.extension().u8string());
        fs::path dst = dir / name;
        if (fs::exists(dst, ec)) {
            std::ifstream f(dst, std::ios::binary);
            std::string have((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            if (have == text) {
                return "";
            }
            continue;
        }
        std::ofstream f(dst, std::ios::binary | std::ios::trunc);
        f.write(text.data(), (std::streamsize)text.size());
        return f ? name.u8string() : "";
    }
    return "";
}

std::string g_patches_path; // game whose Patches window is open (see below)
void RefreshPatchInfo();

// Main thread: store downloaded patches (switched off) and report
void MergePatchResults()
{
    std::vector<PatchDownloadResult> results;
    std::map<std::string, uint32_t> new_index;
    bool finished = false, index_failed = false;
    {
        std::lock_guard<std::mutex> lock(g_patchdl_mutex);
        results.swap(g_patchdl_results);
        finished = g_patchdl_finished;
        g_patchdl_finished = false;
        if (finished) {
            new_index.swap(g_patchdl_new_index);
            index_failed = g_patchdl_index_failed;
            g_patchdl_index_failed = false;
        }
    }
    static int added = 0, skipped = 0, failed = 0;
    static std::set<uint32_t> games;
    std::set<uint32_t> touched;
    for (auto &r : results) {
        if (r.failed) {
            failed++;
            continue;
        }
        g_jmp_fetched.insert(r.repo_path);
        g_dirty = true;
        if (!r.fits) {
            skipped++;
            continue;
        }
        std::string name = StoreDownloadedPatch(r.title_id, r.repo_path, r.text);
        if (name.empty()) {
            continue;
        }
        SyncPatches(r.title_id);
        for (auto &pi : g_patches[r.title_id]) {
            if (pi.file == name) {
                pi.enabled = false; // the user picks which ones to use
            }
        }
        added++;
        games.insert(r.title_id);
        touched.insert(r.title_id);
    }
    if (!touched.empty()) {
        LibraryEntry *e = FindEntry(g_patches_path);
        if (e && touched.count(e->title_id)) {
            RefreshPatchInfo();
        }
    }
    if (!finished) {
        return;
    }
    int new_count = 0;
    if (!new_index.empty()) {
        for (auto &[path, id] : new_index) {
            g_jmp_extra[path] = id;
            new_count += id ? 1 : 0;
        }
        SaveJmpExtraIndex();
    }
    std::string msg;
    if (index_failed) {
        msg = "Could not check GitHub for new patches (try again later). ";
    } else if (!new_index.empty() || g_patchdl_total == 0) {
        msg = new_count ? string_format("Found %d new patch%s online. ", new_count,
                                        new_count == 1 ? "" : "es")
                        : std::string("No new patches online. ");
    }
    if (added) {
        msg += string_format("Downloaded %d patch%s for %d game%s, switched off "
                             "until you enable them in Patches.",
                             added, added == 1 ? "" : "es", (int)games.size(),
                             games.size() == 1 ? "" : "s");
    } else if (g_patchdl_total) {
        msg += "No new patches that fit your copies.";
    }
    if (skipped) {
        msg += string_format(" %d made for other versions skipped.", skipped);
    }
    if (failed) {
        msg += string_format(" %d could not be downloaded.", failed);
    }
    xemu_queue_notification(msg.c_str());
    SaveLibrary();
    added = skipped = failed = 0;
    games.clear();
    // Patches for the user's games among the new finds
    if (new_count && !index_failed) {
        bool any = false;
        for (auto &e : g_entries) {
            for (auto &[path, id] : new_index) {
                any |= e.valid && id && id == e.title_id;
            }
        }
        if (any) {
            StartPatchDownload(0, false);
        }
    }
}

bool g_open_patches_popup = false;
std::string g_patch_check_msg;
bool g_patch_check_ok = false;
std::string g_patch_confirm_remove;

// Parsed .jmp files of the game in the Patches window, and how they fit
struct JmpInfo {
    bool ok = false;
    std::string error;
    JmpPatch jmp;
    JmpFit fit = JmpFit::Unverified;
};
std::map<std::string, JmpInfo> g_jmp_info; // by file name
bool g_jmp_have_xbe = false;

void RefreshPatchInfo()
{
    g_jmp_info.clear();
    LibraryEntry *e = FindEntry(g_patches_path);
    if (!e || !e->valid) {
        return;
    }
    std::vector<uint8_t> xbe;
    std::string err;
    g_jmp_have_xbe = ReadDefaultXbe(e->path, xbe, err);
    for (auto &pi : g_patches[e->title_id]) {
        if (Lower(fs::u8path(pi.file).extension().u8string()) != ".jmp") {
            continue;
        }
        JmpInfo &info = g_jmp_info[pi.file];
        std::ifstream f(PatchDir(e->title_id) / fs::u8path(pi.file),
                        std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        info.ok = ParseJmp(text, info.jmp, info.error);
        if (info.ok && g_jmp_have_xbe) {
            info.fit = CheckJmp(info.jmp, xbe, e->title_id);
        }
    }
}

void OpenPatchesFor(LibraryEntry &e)
{
    g_patches_path = e.path;
    g_patch_check_msg.clear();
    g_patch_confirm_remove.clear();
    SyncPatches(e.title_id);
    RefreshPatchInfo();
    g_open_patches_popup = true;
}

void AddPatchFile(uint32_t title_id)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Game patches (*.xbe, *.ips, *.bps, *.jmp)", "xbe;ips;bps;jmp" },
        { "All Files", "*" },
    };
    ShowOpenFileDialog(filters, 2, NULL, [title_id](const char *path) {
        fs::path src = fs::u8path(path);
        if (!IsPatchFile(src)) {
            xemu_queue_error_message(
                "Unsupported patch file. Use .xbe, .ips, .bps or .jmp files.");
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
        RefreshPatchInfo();
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
    if (ext == ".jmp") {
        return "Magic Patch";
    }
    return "BPS patch";
}

ImVec4 JmpFitColor(JmpFit fit)
{
    switch (fit) {
    case JmpFit::Works:
        return ImVec4(0.45f, 0.86f, 0.16f, 1);
    case JmpFit::Unverified:
        return ImVec4(0.75f, 0.75f, 0.75f, 1);
    default:
        return ImVec4(1.0f, 0.45f, 0.4f, 1);
    }
}

// Under a .jmp patch: how it fits, its notes and its options
void DrawJmpDetails(PatchItem &pi, const JmpInfo &info)
{
    const float s = g_viewport_mgr.m_scale;
    if (!info.ok) {
        ImGui::TextColored(JmpFitColor(JmpFit::OtherGame), "Can't read: %s",
                           info.error.c_str());
        return;
    }
    const JmpPatch &jmp = info.jmp;
    if (g_jmp_have_xbe) {
        ImGui::TextColored(JmpFitColor(info.fit), "%s", JmpFitLabel(info.fit));
        if (info.fit == JmpFit::Unverified) {
            ImGui::SetItemTooltip("The patch has no checksum, so xemu can't "
                                  "confirm it was made for your version of "
                                  "the game. Try it; if the game misbehaves, "
                                  "switch it off.");
        }
        if (!jmp.author.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("by %s", jmp.author.c_str());
        }
    } else if (!jmp.author.empty()) {
        ImGui::TextDisabled("by %s", jmp.author.c_str());
    }
    if (!jmp.notes.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO);
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(400 * s);
            ImGui::TextUnformatted(jmp.notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    std::vector<bool> on = JmpOptionStates(jmp, pi.toggled);
    int n_on = 0, n_usable = 0;
    for (size_t k = 0; k < jmp.options.size(); k++) {
        if (!jmp.options[k].other_xbe) {
            n_usable++;
            n_on += on[k] ? 1 : 0;
        }
    }
    if (jmp.options.size() <= 1) {
        return;
    }
    std::string label = string_format("Options (%d of %d on)###opts", n_on, n_usable);
    if (!ImGui::TreeNode(label.c_str())) {
        return;
    }
    for (size_t k = 0; k < jmp.options.size(); k++) {
        const JmpOption &o = jmp.options[k];
        ImGui::PushID((int)k);
        bool v = on[k] && !o.other_xbe;
        ImGui::BeginDisabled(o.other_xbe);
        if (ImGui::Checkbox(o.name.c_str(), &v)) {
            auto t = std::find(pi.toggled.begin(), pi.toggled.end(), (int)k);
            if (t == pi.toggled.end()) {
                pi.toggled.push_back((int)k);
            } else {
                pi.toggled.erase(t);
            }
            std::sort(pi.toggled.begin(), pi.toggled.end());
            g_dirty = true;
            g_patch_check_msg.clear();
        }
        ImGui::EndDisabled();
        if (o.other_xbe) {
            ImGui::SetItemTooltip("Changes another program on the disc; only "
                                  "default.xbe can be patched");
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
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
                        "default.xbe; .ips, .bps and .jmp patches modify it.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    std::vector<PatchItem> &items = g_patches[id];
    if (items.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(OnlinePatchCount(id)
                                   ? "No patches yet. Download some below, or "
                                     "add a .xbe, .ips, .bps or .jmp file."
                                   : "No patches yet. Add a .xbe, .ips, .bps "
                                     "or .jmp file.");
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
            auto info_it = g_jmp_info.find(pi.file);
            if (info_it != g_jmp_info.end()) {
                DrawJmpDetails(pi, info_it->second);
            }
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
        RefreshPatchInfo();
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
    int online = OnlinePatchCount(id);
    ImGui::BeginDisabled(g_patchdl_running || online == 0);
    std::string dl_label =
        g_patchdl_running
            ? std::string(ICON_FA_DOWNLOAD "  Downloading...")
            : online ? string_format(ICON_FA_DOWNLOAD "  Download patches (%d online)",
                                     online)
                     : std::string(ICON_FA_DOWNLOAD "  No online patches for this game");
    if (ImGui::Button(dl_label.c_str())) {
        StartPatchDownload(id, false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Downloads this game's patches from Jay's Magic Patches collection on "
        "GitHub (widescreen, 60 FPS, 480p/720p and more). Only patches that "
        "fit your copy of the game are kept, switched off until you enable "
        "them.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_patchdl_running);
    if (ImGui::Button("Check for new patches")) {
        StartPatchDownload(0, true);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Look for patches added to the collection since "
                          "this version of xemu (list from %s)",
                          kJmpIndexDate);
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
    if (ImGui::MenuItem("Clear shader cache...", NULL, false, e.valid)) {
        OpenSettingsFor(e); // the option lives in the game's settings
        g_settings_shader_confirm = true;
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
    if (T().neon) {
        // Dark violet slab with a thin hue line
        ShapeFill(dl, a, b, IM_COL32(16, 15, 30, 255), rounding);
        ImVec4 line = ImColor::HSV((h % 360) / 360.0f, 0.7f, 0.9f);
        dl->AddRectFilled(ImVec2(a.x + rounding, b.y - 6 * g_viewport_mgr.m_scale),
                          ImVec2(b.x - rounding * 2, b.y - 4 * g_viewport_mgr.m_scale),
                          ImGui::GetColorU32(line));
    } else {
        dl->AddRectFilled(a, b, ImGui::GetColorU32(c2), rounding);
        dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rounding),
                                    ImVec2(b.x, b.y - rounding),
                                    ImGui::GetColorU32(c1), ImGui::GetColorU32(c1),
                                    ImGui::GetColorU32(c2), ImGui::GetColorU32(c2));
    }
    const char *icon = ICON_FA_COMPACT_DISC;
    float size = (b.x - a.x) * 0.35f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, icon);
    dl->AddText(ImGui::GetFont(), size,
                ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2),
                IM_COL32(255, 255, 255, 140), icon);
}

void DrawCover3D(ImDrawList *dl, ImVec2 a, ImVec2 b, const LibraryEntry &e,
                 float angle_deg);
extern bool g_draw_3d;

void DrawArtwork(ImDrawList *dl, ImVec2 a, ImVec2 b, const LibraryEntry &e,
                 float rounding)
{
    if (g_draw_3d) {
        DrawCover3D(dl, a, b, e, 24.0f);
        return;
    }
    if (!e.tex) {
        DrawPlaceholder(dl, a, b, e, rounding);
        return;
    }
    // Fit the image inside the box, keeping its aspect ratio
    ShapeFill(dl, a, b, T().neon ? IM_COL32(12, 12, 22, 255) : IM_COL32(16, 16, 16, 255),
              rounding);
    float bw = b.x - a.x, bh = b.y - a.y;
    float s = std::min(bw / e.tex_w, bh / e.tex_h);
    float w = e.tex_w * s, h = e.tex_h * s;
    ImVec2 ia((a.x + b.x - w) / 2, (a.y + b.y - h) / 2);
    ShapeImage(dl, (ImTextureID)(intptr_t)e.tex, ia, ImVec2(ia.x + w, ia.y + h),
               ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
}

// A textured quad with a color per corner (for fades and shading)
void TexQuad(ImDrawList *dl, ImTextureID tex, const ImVec2 p[4], const ImVec2 uv[4],
             const ImU32 col[4])
{
    dl->PushTextureID(tex);
    dl->PrimReserve(6, 4);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 4; i++) {
        dl->PrimWriteVtx(p[i], uv[i], col[i]);
    }
    const ImDrawIdx q[6] = { base, (ImDrawIdx)(base + 1), (ImDrawIdx)(base + 2),
                             base, (ImDrawIdx)(base + 2), (ImDrawIdx)(base + 3) };
    for (ImDrawIdx v : q) {
        dl->PrimWriteIdx(v);
    }
    dl->PopTextureID();
}

bool g_draw_3d = false; // Home: draw covers as 3D boxes

bool Home3DCovers()
{
    return g_config.general.library.home_covers == CONFIG_GENERAL_LIBRARY_HOME_COVERS_BOX3D;
}

// A cover as a 3D game case standing on a glossy floor: the flat cover is
// projected onto the front of a box turned slightly, with a spine on the
// left and a fading reflection below. A user's own 3D box image is shown
// as-is (with its reflection) instead. Fits inside [a, b].
void DrawCover3D(ImDrawList *dl, ImVec2 a, ImVec2 b, const LibraryEntry &e, float angle_deg)
{
    const float s = g_viewport_mgr.m_scale;
    float W = b.x - a.x, H = b.y - a.y;
    float floor_h = H * 0.16f;            // room for the reflection
    float stand_y = b.y - floor_h;        // where the box stands
    dl->PushClipRect(a, b, true);

    // Soft shadow on the floor
    dl->AddRectFilledMultiColor(ImVec2(a.x + W * 0.12f, stand_y - 2 * s),
                                ImVec2(b.x - W * 0.12f, stand_y + floor_h * 0.35f),
                                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));

    const Box3DArt *art = Get3DArt(e);
    if (art->tex && art->w > 0) {
        // User's 3D image: fit, stand it on the floor, reflect it
        float sc_ = std::min(W / art->w, (H - floor_h) / art->h);
        float w = art->w * sc_, h = art->h * sc_;
        ImVec2 p0((a.x + b.x - w) / 2, stand_y - h);
        ImTextureID tex = (ImTextureID)(intptr_t)art->tex;
        dl->AddImage(tex, p0, ImVec2(p0.x + w, stand_y));
        float rh = std::min(floor_h, h * 0.35f);
        ImVec2 rp[4] = { ImVec2(p0.x, stand_y), ImVec2(p0.x + w, stand_y),
                         ImVec2(p0.x + w, stand_y + rh), ImVec2(p0.x, stand_y + rh) };
        ImVec2 ruv[4] = { ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 1 - rh / h),
                          ImVec2(0, 1 - rh / h) };
        ImU32 rc[4] = { IM_COL32(255, 255, 255, 70), IM_COL32(255, 255, 255, 70),
                        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0) };
        TexQuad(dl, tex, rp, ruv, rc);
        dl->PopClipRect();
        return;
    }
    if (!e.tex || e.tex_w <= 0) {
        float bh = H - floor_h, bw = std::min(W * 0.8f, bh * 0.7f);
        DrawPlaceholder(dl, ImVec2((a.x + b.x - bw) / 2, stand_y - bh),
                        ImVec2((a.x + b.x + bw) / 2, stand_y), e, 4 * s);
        dl->PopClipRect();
        return;
    }

    // Box in 3D: front face at z = 0, depth going back; turned around Y
    float aspect = (float)e.tex_w / e.tex_h;
    float bh = 1.0f, bw = aspect, bd = bw * 0.13f;
    // Negative: the left (spine) edge turns toward the viewer
    float th = -angle_deg * 3.14159265f / 180.0f;
    float cam = 4.2f;
    auto proj = [&](float x, float y, float z) {
        float X = x * cosf(th) + z * sinf(th);
        float Z = -x * sinf(th) + z * cosf(th);
        float k = cam / (cam + Z);
        return ImVec2(X * k, y * k);
    };
    // Corners (y down): front face and the back of the left (spine) side
    ImVec2 fl_t = proj(-bw / 2, -bh / 2, 0), fr_t = proj(bw / 2, -bh / 2, 0);
    ImVec2 fl_b = proj(-bw / 2, bh / 2, 0), fr_b = proj(bw / 2, bh / 2, 0);
    ImVec2 bl_t = proj(-bw / 2, -bh / 2, bd), bl_b = proj(-bw / 2, bh / 2, bd);
    float minx = std::min({ fl_t.x, fl_b.x, bl_t.x, bl_b.x });
    float maxx = std::max(fr_t.x, fr_b.x);
    float miny = std::min({ fl_t.y, fr_t.y, bl_t.y });
    float maxy = std::max({ fl_b.y, fr_b.y, bl_b.y });
    float k = std::min(W * 0.92f / (maxx - minx), (H - floor_h) * 0.98f / (maxy - miny));
    float ox = (a.x + b.x) / 2 - (minx + maxx) / 2 * k;
    float oy = stand_y - maxy * k;
    auto S = [&](ImVec2 p) { return ImVec2(ox + p.x * k, oy + p.y * k); };
    ImTextureID tex = (ImTextureID)(intptr_t)e.tex;

    // Spine (left side): the cover's left edge, darkened
    bool spine = bl_t.x < fl_t.x;
    if (spine) {
        ImVec2 p[4] = { S(bl_t), S(fl_t), S(fl_b), S(bl_b) };
        ImVec2 uv[4] = { ImVec2(0.0f, 0), ImVec2(0.035f, 0), ImVec2(0.035f, 1),
                         ImVec2(0.0f, 1) };
        ImU32 c[4] = { IM_COL32(90, 90, 100, 255), IM_COL32(140, 140, 150, 255),
                       IM_COL32(140, 140, 150, 255), IM_COL32(90, 90, 100, 255) };
        TexQuad(dl, tex, p, uv, c);
    }
    // Front face in strips (keeps the texture straight under perspective)
    const int n = 14;
    for (int i = 0; i < n; i++) {
        float u0 = (float)i / n, u1 = (float)(i + 1) / n;
        float x0 = -bw / 2 + bw * u0, x1 = -bw / 2 + bw * u1;
        ImVec2 p[4] = { S(proj(x0, -bh / 2, 0)), S(proj(x1, -bh / 2, 0)),
                        S(proj(x1, bh / 2, 0)), S(proj(x0, bh / 2, 0)) };
        ImVec2 uv[4] = { ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1) };
        // Light falls off slightly toward the far edge
        int sh0 = 255 - (int)(40 * u0), sh1 = 255 - (int)(40 * u1);
        ImU32 col[4] = { IM_COL32(sh0, sh0, sh0, 255), IM_COL32(sh1, sh1, sh1, 255),
                         IM_COL32(sh1, sh1, sh1, 255), IM_COL32(sh0, sh0, sh0, 255) };
        TexQuad(dl, tex, p, uv, col);
        // Reflection: the bottom of the cover, flipped and fading out
        float rv = 0.28f;
        ImVec2 r0 = S(proj(x0, bh / 2, 0)), r1 = S(proj(x1, bh / 2, 0));
        float rh = std::min(floor_h, (S(fl_b).y - S(fl_t).y) * rv);
        ImVec2 rp[4] = { r0, r1, ImVec2(r1.x, r1.y + rh), ImVec2(r0.x, r0.y + rh) };
        ImVec2 ruv[4] = { ImVec2(u0, 1), ImVec2(u1, 1), ImVec2(u1, 1 - rv),
                          ImVec2(u0, 1 - rv) };
        ImU32 rc[4] = { IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 60),
                        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0) };
        TexQuad(dl, tex, rp, ruv, rc);
    }
    // Case edges: a highlight down the front-left edge, a dark outline
    ImVec2 fq[4] = { S(fl_t), S(fr_t), S(fr_b), S(fl_b) };
    dl->AddPolyline(fq, 4, IM_COL32(0, 0, 0, 120), ImDrawFlags_Closed, 1.0f * s);
    dl->AddLine(S(fl_t), S(fl_b), IM_COL32(255, 255, 255, 90), 1.5f * s);
    // Sheen across the case
    ImVec2 gl[4] = { S(fl_t), S(proj(-bw / 2 + bw * 0.45f, -bh / 2, 0)),
                     S(proj(-bw / 2 + bw * 0.15f, bh / 2, 0)), S(fl_b) };
    dl->AddConvexPolyFilled(gl, 4, IM_COL32(255, 255, 255, 14));
    dl->PopClipRect();
}

// ---- Compatibility ratings (xemu.app) ----------------------------------------

// The list is cached in library-cache/compat.json and refreshed once a day in
// the background
std::map<uint32_t, CompatStatus> g_compat;
std::map<uint32_t, std::string> g_compat_names; // title ID -> name
std::thread g_compat_thread;
std::atomic<bool> g_compat_downloaded{ false };
bool g_compat_started = false;

fs::path CompatCachePath()
{
    return IconCacheDir() / "compat.json";
}

void LoadCompatCache()
{
    std::ifstream f(CompatCachePath(), std::ios::binary);
    if (!f) {
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)), {});
    std::map<uint32_t, CompatStatus> list;
    if (ParseCompatJson(text, &list)) {
        g_compat = std::move(list);
    }
    std::map<uint32_t, std::string> names;
    if (ParseCompatNames(text, &names)) {
        g_compat_names = std::move(names);
    }
}

void StartCompatRefresh()
{
    if (g_compat_started) {
        return;
    }
    g_compat_started = true;
    LoadCompatCache();
    std::error_code ec;
    fs::path cache = CompatCachePath();
    if (fs::exists(cache, ec)) {
        auto age = fs::file_time_type::clock::now() - fs::last_write_time(cache, ec);
        if (!ec && age < std::chrono::hours(24) && !g_compat.empty()) {
            return;
        }
    }
    g_compat_thread = std::thread([cache] {
        Error *err = NULL;
        if (!ensure_libcurl_initialized(&err)) {
            if (err) {
                error_free(err);
            }
            return;
        }
        GByteArray *body = g_byte_array_new();
        int code = http_get(kCompatJsonUrl, body, NULL, NULL);
        std::string text((const char *)body->data, body->len);
        g_byte_array_unref(body);
        std::map<uint32_t, CompatStatus> check;
        if (code != 200 || !ParseCompatJson(text, &check) || check.size() < 50) {
            return; // keep the cached list
        }
        std::error_code ec2;
        fs::create_directories(cache.parent_path(), ec2);
        fs::path tmp = cache;
        tmp += ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            o.write(text.data(), (std::streamsize)text.size());
            if (!o) {
                return;
            }
        }
        fs::rename(tmp, cache, ec2);
        if (!ec2) {
            g_compat_downloaded = true;
        }
    });
}

CompatStatus CompatFor(const LibraryEntry &e)
{
    if (!e.valid || g_compat.empty()) {
        return CompatStatus::None;
    }
    return CompatLookup(g_compat, e.title_id);
}

ImU32 CompatColor(CompatStatus c)
{
    switch (c) {
    case CompatStatus::Perfect: return IM_COL32(60, 205, 95, 255);
    case CompatStatus::Playable: return IM_COL32(45, 165, 235, 255);
    case CompatStatus::Starts: return IM_COL32(240, 190, 40, 255);
    case CompatStatus::Intro: return IM_COL32(240, 125, 40, 255);
    case CompatStatus::Broken: return IM_COL32(225, 65, 60, 255);
    default: return IM_COL32(130, 130, 136, 255);
    }
}

// Details row: coloured dot, rating, and a link to the game's page
void DrawCompatRow(const LibraryEntry &e, float label_x)
{
    CompatStatus c = CompatFor(e);
    if (c == CompatStatus::None && g_compat.empty()) {
        return; // list not loaded (yet)
    }
    ImGui::TextDisabled("Compatibility");
    ImGui::SameLine(label_x);
    float h = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.3f, p.y + h * 0.55f),
                                                h * 0.28f, CompatColor(c), 16);
    ImGui::Dummy(ImVec2(h * 0.75f, h));
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(CompatColor(c)), "%s",
                       CompatName(c));
    ImGui::SetItemTooltip("%s\nRated by xemu users on xemu.app", CompatDescription(c));
    if (e.valid && ImGui::SmallButton("Compatibility report...")) {
        SDL_OpenURL(CompatTitleUrl(e.title_id).c_str());
    }
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
        CompatStatus cs = CompatFor(e);
        ImGui::SetItemTooltip("%s\n%s%s%s\nPlay time: %s\nLast played: %s%s%s",
                              e.DisplayName().c_str(),
                              e.valid ? FormatTitleId(e.title_id).c_str() : "",
                              e.valid ? "  -  " : "",
                              e.valid ? FormatRegion(e.region).c_str()
                                      : "Unrecognized image",
                              FormatPlayTime(st.play_seconds).c_str(),
                              FormatLastPlayed(st.last_played).c_str(),
                              cs != CompatStatus::None ? "\nCompatibility: " : "",
                              cs != CompatStatus::None ? CompatName(cs) : "");
    }
    DrawContextMenu(e);

    ImVec2 a = ImGui::GetItemRectMin();
    if (ImGui::IsItemVisible()) {
        EnsureTexture(e, tex_budget);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 art_b(a.x + tile_w, a.y + art_h);
        DrawArtwork(dl, a, art_b, e, rounding);

        bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (g_draw_3d) {
            if (hovered || selected || focused) {
                bool on = selected || focused;
                float pulse = Animations() && on
                                  ? 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.2f)
                                  : 0.5f;
                ImU32 col = on ? T().accent : IM_COL32(255, 255, 255, 90);
                float fy = art_b.y - art_h * 0.16f;
                dl->AddRectFilledMultiColor(ImVec2(a.x, fy), ImVec2(art_b.x, art_b.y),
                                            Alpha(col, (int)(30 + 40 * pulse)),
                                            Alpha(col, (int)(30 + 40 * pulse)),
                                            Alpha(col, 0), Alpha(col, 0));
                dl->AddLine(ImVec2(a.x + tile_w * 0.1f, fy), ImVec2(art_b.x - tile_w * 0.1f, fy),
                            col, 2.0f * s);
            }
        } else if (hovered || selected || focused) {
            ImU32 col = (selected || focused) ? T().accent : IM_COL32(255, 255, 255, 90);
            if (T().neon && (selected || focused)) {
                // Soft glow that breathes while selected
                float pulse = Animations()
                                  ? 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.2f)
                                  : 0.5f;
                ShapeStroke(dl, ImVec2(a.x - 3 * s, a.y - 3 * s),
                            ImVec2(art_b.x + 3 * s, art_b.y + 3 * s),
                            Alpha(T().accent, (int)(35 + 55 * pulse)), rounding + 3 * s,
                            5.0f * s);
            }
            ShapeStroke(dl, a, art_b, col, rounding, 2.5f * s);
        }
        if (SupportsOnline(e)) {
            const char *badge = "ONLINE";
            ImFont *bf = ImGui::GetFont();
            float fs = font_h * 0.75f;
            ImVec2 ts = bf->CalcTextSizeA(fs, FLT_MAX, 0, badge);
            ImVec2 p(a.x + 6 * s, a.y + 6 * s);
            ImVec2 b0(p.x - 3 * s, p.y - 2 * s), b1(p.x + ts.x + 3 * s, p.y + ts.y + 2 * s);
            if (T().neon) {
                ShapeFill(dl, b0, b1, IM_COL32(10, 6, 16, 220), 4 * s);
                ShapeStroke(dl, b0, b1, T().online, 4 * s, 1.0f * s);
                dl->AddText(bf, fs, p, T().online, badge);
            } else {
                dl->AddRectFilled(b0, b1, IM_COL32(16, 124, 16, 230), 3 * s);
                dl->AddText(bf, fs, p, IM_COL32(255, 255, 255, 255), badge);
            }
        }
        CompatStatus tcs = CompatFor(e);
        if (tcs != CompatStatus::None && tcs != CompatStatus::Unknown) {
            // Compatibility dot in the bottom-left corner of the art
            ImVec2 c(a.x + 11 * s, art_b.y - 11 * s);
            dl->AddCircleFilled(c, 6.5f * s, IM_COL32(0, 0, 0, 170), 16);
            dl->AddCircleFilled(c, 4.5f * s, CompatColor(tcs), 16);
        }
        if (e.valid && (EnabledPatchCount(e.title_id) || OnlinePatchCount(e.title_id))) {
            // Wrench: green when patches are on, grey when some are online
            bool on = EnabledPatchCount(e.title_id) > 0;
            ImVec2 c(art_b.x - font_h * 0.5f - 8 * s, art_b.y - font_h * 0.5f - 8 * s);
            dl->AddCircleFilled(c, font_h * 0.75f, IM_COL32(0, 0, 0, 150));
            ImVec2 ts = ImGui::CalcTextSize(ICON_FA_WRENCH);
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2),
                        on ? T().accent : IM_COL32(220, 220, 220, 230), ICON_FA_WRENCH);
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
                              T().accent, 4 * s);
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
                     "Lanczos\0" "Snapdragon GSR 1\0"
                     "NVIDIA Image Scaling (NIS)\0" "NVIDIA RTX Video (AI)\0")) {
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
    // Shader cache: try clearing it if the game hangs or draws wrong after
    // a crash
    ImGui::TextUnformatted("Shader cache");
    if (!g_settings_shader_msg.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440 * s);
        ImGui::TextDisabled("%s", g_settings_shader_msg.c_str());
        ImGui::PopTextWrapPos();
    } else if (!g_settings_shader_confirm) {
        ImGui::TextDisabled(g_settings_shader_count
                                ? "%d shader files saved for this game."
                                : "No shader files recorded for this game yet "
                                  "(recording started with this version).",
                            g_settings_shader_count);
        if (ImGui::Button("Clear this game's shader cache...")) {
            g_settings_shader_confirm = true;
        }
        ImGui::SetItemTooltip("If the game hangs or draws wrong after a crash, "
                              "a damaged cached shader may be the cause");
    } else {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440 * s);
        ImGui::TextWrapped(
            g_settings_shader_count
                ? "Delete this game's saved shaders and the Vulkan pipeline "
                  "cache (shared by all games)? They're rebuilt as you play, "
                  "with some stutter the first time."
                : "No shader files are recorded for this game, so only the "
                  "Vulkan pipeline cache (shared by all games) is cleared. To "
                  "be sure, use \"Clear all shader caches\" in the "
                  "dashboard's Settings tab.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Clear")) {
            ShaderCacheClearFor(e->title_id, &g_settings_shader_msg);
            g_settings_shader_confirm = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
            g_settings_shader_confirm = false;
        }
    }
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

void DrawResumeWarningPopup()
{
    if (g_open_resume_warning) {
        ImGui::OpenPopup("Different BIOS");
        g_open_resume_warning = false;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(520 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Different BIOS", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    auto file = [](const std::string &sig, bool flash) {
        size_t bar = sig.find('|');
        std::string p = flash ? sig.substr(0, bar) : sig.substr(bar + 1);
        return p.empty() ? std::string("(none)")
                         : fs::u8path(p).filename().u8string();
    };
    std::string now = MachineSignature();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 500 * s);
    ImGui::TextUnformatted("This resume point was saved with a different BIOS "
                           "or boot ROM. Resuming restores the game exactly as "
                           "it was, so with a different BIOS the game or xemu "
                           "may crash.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::TextDisabled("Saved with:  %s  /  %s",
                        file(g_resume_warn_saved_with, true).c_str(),
                        file(g_resume_warn_saved_with, false).c_str());
    ImGui::TextDisabled("Current:     %s  /  %s", file(now, true).c_str(),
                        file(now, false).c_str());
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::TextUnformatted("Switch back to that BIOS in Settings > System, "
                           "or start the game normally with Play.");
    ImGui::Separator();
    if (ImGui::Button("Cancel", ImVec2(120 * s, 0)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load anyway", ImVec2(120 * s, 0))) {
        std::string name = g_resume_warn_name;
        ImGui::CloseCurrentPopup();
        RequestPreviewStop();
        ActionLoadSnapshotChecked(name.c_str());
        game_library_window.is_open = false;
    }
    ImGui::EndPopup();
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
// Indices into g_snapshot_mgr of this game's save states, newest first
std::vector<int> SnapshotsFor(const LibraryEntry &e)
{
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
    std::sort(mine.begin(), mine.end(), [](int a, int b) {
        return g_snapshot_mgr.m_snapshots[a].date_sec >
               g_snapshot_mgr.m_snapshots[b].date_sec;
    });
    return mine;
}

// Open the folder containing path, with the file selected where possible
void RevealInFolder(const std::string &path)
{
    if (!RevealFileInFolder(path)) {
        SDL_OpenURL(
            ("file:///" + fs::u8path(path).parent_path().generic_u8string()).c_str());
    }
}

std::string g_delete_state; // save state waiting for delete confirmation
int g_back_consumed_frame = -1;

// Delete confirmation for a save state (opened from its right-click menu)
void DrawDeleteStatePopup()
{
    if (!g_delete_state.empty() &&
        !ImGui::IsPopupOpen("##delete-state", ImGuiPopupFlags_AnyPopupId)) {
        ImGui::OpenPopup("##delete-state");
    }
    if (ImGui::BeginPopupModal("##delete-state", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete the save state \"%s\"?", g_delete_state.c_str());
        ImGui::TextDisabled("This can't be undone.");
        if (ImGui::Button(ICON_FA_TRASH "  Delete")) {
            Error *err = NULL;
            xemu_snapshots_delete(g_delete_state.c_str(), &err);
            if (err) {
                xemu_queue_error_message(error_get_pretty(err));
                error_free(err);
            } else {
                xemu_queue_notification("Save state deleted");
            }
            g_snapshot_mgr.Refresh();
            g_delete_state.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
            g_delete_state.clear();
            GameLibraryConsumeBack();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawSaveStates(LibraryEntry &e, float w, int max_shown = 6)
{
    const float s = g_viewport_mgr.m_scale;
    std::vector<int> mine = SnapshotsFor(e);
    if (mine.empty()) {
        return;
    }

    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::Separator();
    ImGui::TextDisabled("Save states (%d)", (int)mine.size());
    const float thumb_h = 42 * s, thumb_w = thumb_h * 4 / 3;
    int shown = 0;
    std::string load_name; // loaded after the loop: loading refreshes the list
    for (int i : mine) {
        if (shown++ >= max_shown) {
            ImGui::TextDisabled("More in Settings > Snapshots");
            break;
        }
        const QEMUSnapshotInfo &info = g_snapshot_mgr.m_snapshots[i];
        const XemuSnapshotData &d = g_snapshot_mgr.m_extra_data[i];
        ImGui::PushID(info.name);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::Selectable("##snap", false, 0, ImVec2(w, thumb_h));
        ImGui::SetItemTooltip("Load this save state (right-click or Y for more)");
        // Right-click (or Y on a controller): load, delete, show the file
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
            ImGui::OpenPopup("##snapmenu");
        }
        if (ImGui::BeginPopupContextItem("##snapmenu")) {
            ImGui::TextDisabled("%s", info.name);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PLAY "  Load")) {
                load_name = info.name;
            }
            if (ImGui::MenuItem(ICON_FA_TRASH "  Delete...")) {
                g_delete_state = info.name;
            }
            const char *hdd = g_config.sys.files.hdd_path;
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show file location", NULL,
                                false, hdd && hdd[0])) {
                RevealInFolder(hdd);
            }
            ImGui::SetItemTooltip("Save states are stored inside your Xbox hard "
                                  "disk image (%s)",
                                  hdd && hdd[0] ? fs::u8path(hdd).filename().u8string().c_str()
                                                : "none");
            ImGui::EndPopup();
        }
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
    DrawDeleteStatePopup();
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

void DrawScreenshots(const LibraryEntry &e, float w, int per_row = 3,
                     int max_shown = 6)
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
    float gap = 4 * s;
    float tw = (w - gap * (per_row - 1)) / per_row, th = tw * 3 / 4;
    int shown = 0;
    for (auto &path : shots) {
        if (shown == max_shown) {
            break;
        }
        if (shown % per_row) {
            ImGui::SameLine(0, gap);
        }
        shown++;
        RequestScreenshotThumb(path);
        ImGui::PushID(path.c_str());
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##shot", ImVec2(tw, th), ImGuiButtonFlags_EnableNav)) {
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
            dl->AddRect(p, ImVec2(p.x + tw, p.y + th), T().accent, 3 * s, 0, 2 * s);
        }
        ImGui::PopID();
    }
}

// Load a game's resume point, warning first if the BIOS has changed
void ResumeGame(const LibraryEntry &e, const std::string &snapshot_name)
{
    std::string name = snapshot_name; // the list may refresh on load
    auto saved = g_resume_machine.find(e.title_id);
    if (saved != g_resume_machine.end() && saved->second != MachineSignature()) {
        g_resume_warn_name = name;
        g_resume_warn_saved_with = saved->second;
        g_open_resume_warning = true;
    } else {
        RequestPreviewStop();
        ActionLoadSnapshotChecked(name.c_str());
        game_library_window.is_open = false;
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
    // Fixed-size art box (box-art proportions): the panel must not change
    // height when a cover or video finishes loading, or its scrollbar toggles
    // and the whole panel visibly resizes.
    float art = std::floor(w * 1.3f);
    int vw = 0, vh = 0;
    GLuint vtex = playing ? g_preview->Update(&vw, &vh) : 0;
    if (vtex && vw > 0) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + w, a.y + art), IM_COL32(0, 0, 0, 255), 8 * s);
        float sc = std::min(w / vw, art / vh);
        ImVec2 ia(a.x + (w - vw * sc) / 2, a.y + (art - vh * sc) / 2);
        dl->AddImageRounded((ImTextureID)(intptr_t)vtex, ia,
                            ImVec2(ia.x + vw * sc, ia.y + vh * sc), ImVec2(0, 0),
                            ImVec2(1, 1), IM_COL32_WHITE, 8 * s);
    } else {
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
        const bool neon = T().neon;
        ImGui::PushStyleColor(ImGuiCol_Button, neon ? ImVec4(0.75f, 0.10f, 0.42f, 1)
                                                    : ImVec4(0.16f, 0.42f, 0.78f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, neon ? ImVec4(1.0f, 0.18f, 0.53f, 1)
                                                           : ImVec4(0.22f, 0.5f, 0.88f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, neon ? ImVec4(0.6f, 0.08f, 0.33f, 1)
                                                          : ImVec4(0.12f, 0.36f, 0.68f, 1));
        if (ImGui::Button(label.c_str(), ImVec2(w, 40 * s))) {
            ResumeGame(e, info.name);
        }
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Continue from where you left off");
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(T().accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, T().neon ? ImVec4(0.45f, 0.94f, 1, 1)
                                                           : ImVec4(0.45f, 0.86f, 0.16f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, T().neon ? ImVec4(0, 0.70f, 0.80f, 1)
                                                          : ImVec4(0.32f, 0.70f, 0.06f, 1));
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
    DrawCompatRow(e, w * 0.42f);
    if (SupportsOnline(e)) {
        row("Online", "Supported on Insignia");
        if (ImGui::SmallButton("How to play online...")) {
            g_open_insignia_popup = true;
        }
    }
    if (e.valid && g_overrides.count(e.title_id) && g_overrides[e.title_id].enabled) {
        row("Settings", "Custom for this game");
    }
    if (e.valid) {
        int online = OnlinePatchCount(e.title_id);
        if (online) {
            row("Patches", string_format("%d available online", online));
            if (ImGui::SmallButton(ICON_FA_DOWNLOAD "  Get patches...")) {
                OpenPatchesFor(e);
            }
            ImGui::SetItemTooltip("Widescreen, 60 FPS, HD and other patches "
                                  "from Jay's Magic Patches collection");
        }
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

// ---------------------------------------------------------------------------
// Discord status
// ---------------------------------------------------------------------------

const char kDiscordClientId[] = "1563026828640129095";
std::string g_presence_game;       // disc shown in the Discord status
int64_t g_presence_start = 0;      // when it started
std::string g_presence_cover;      // its cover URL, or "" for the app logo
bool g_presence_running = false;

// What the Discord status should show right now
DiscordActivity CurrentPresence()
{
    DiscordActivity a;
    const char *dvd = g_config.sys.files.dvd_path;
    std::string cur = (dvd && dvd[0]) ? OriginalDiscPath(dvd) : "";
    LibraryEntry *e = cur.empty() ? NULL : FindEntry(cur);
    if (cur.empty()) {
        g_presence_game.clear();
        a.details = game_library_window.is_open ? "Browsing the game library"
                                                : "In the Xbox dashboard";
        a.large_image = "logo";
        a.large_text = "xemu Modern GUI";
        return a;
    }
    if (cur != g_presence_game) {
        g_presence_game = cur;
        g_presence_start = (int64_t)time(NULL);
        // Discord can only show covers it can download: xemu's game
        // database ones (the user's own cover files are local)
        std::error_code ec;
        g_presence_cover = e && e->valid && !CoverUrl(e->title_id).empty() &&
                                   fs::exists(DownloadedCoverPath(e->title_id), ec)
                               ? CoverUrl(e->title_id)
                               : "";
    }
    std::string name = e ? e->DisplayName() : fs::u8path(cur).stem().u8string();
    a.details = name;
    a.state = e && e->valid ? FormatTitleId(e->title_id) : "Original Xbox";
    if (!runstate_is_running()) {
        a.state += "  -  Paused";
    }
    a.large_image = g_presence_cover.empty() ? "logo" : g_presence_cover;
    a.large_text = name;
    if (e && SupportsOnline(*e)) {
        a.small_image = "online";
        a.small_text = "Plays online with Insignia";
    }
    a.start = g_presence_start;
    return a;
}

void UpdateDiscordPresence()
{
    if (!g_config.general.discord_presence) {
        if (g_presence_running) {
            DiscordPresenceStop(); // clears the status
            g_presence_running = false;
        }
        return;
    }
    if (!g_presence_running) {
        DiscordPresenceStart(kDiscordClientId);
        g_presence_running = true;
    }
    DiscordPresenceSet(CurrentPresence());
}

// ---------------------------------------------------------------------------
// Dashboard: a console-style home screen with tabs (Home, Library, Insignia,
// Media, Drive, Store, Patches, Settings), switched with LB/RB
// ---------------------------------------------------------------------------

enum DashTab { kTabHome, kTabLibrary, kTabInsignia, kTabMedia, kTabDrive,
               kTabStore, kTabPatches, kTabSettings, kTabCount };
const char *const kTabNames[kTabCount] = { "Home",  "Library", "Insignia",
                                           "Media", "Drive",   "Homebrew",
                                           "Patches", "Settings" };
int g_dash_tab = kTabHome;

std::string g_home_path;        // game shown in the Home hero
std::string g_home_seen_select; // g_selected_path as last seen on Home
std::string g_media_path;       // game shown on the Media tab
bool g_home_focus_play = false; // put controller focus on Play next frame



// Translucent card behind a region
void DashCard(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float s = g_viewport_mgr.m_scale;
    ShapeFill(dl, a, b, T().panel, T().neon ? 18 * s : 16 * s);
    ShapeStroke(dl, a, b, T().border, T().neon ? 18 * s : 16 * s, 1.0f * s);
}

// Small label above a section ("CONTINUE PLAYING", or "// CONTINUE PLAYING")
void DashLabel(const char *text)
{
    if (T().neon) {
        ImGui::PushFont(HudFont());
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(Alpha(T().accent, 210)));
        ImGui::TextUnformatted(("// " + Upper(text)).c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T().dim));
    std::string up;
    for (const char *c = text; *c; c++) {
        up += (char)toupper((unsigned char)*c);
        if (c[1]) {
            up += ' ';
        }
    }
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
}

void DashTitle(const char *text)
{
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_small));
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// "ONLINE"-style badge drawn inline
void DashPill(const char *text, ImU32 bg, ImU32 fg = IM_COL32_WHITE)
{
    const float s = g_viewport_mgr.m_scale;
    ImGui::PushFont(ImGui::GetFont());
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 pad(6 * s, 2 * s);
    ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ts.y + pad.y * 2));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + ts.x + pad.x * 2, p.y + ts.y + pad.y * 2);
    if (T().neon) {
        ShapeFill(dl, p, q, Alpha(bg, 40), 6 * s);
        ShapeStroke(dl, p, q, bg, 6 * s, 1.0f * s);
        dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), bg, text);
    } else {
        dl->AddRectFilled(p, q, bg, 5 * s);
        dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), fg, text);
    }
    ImGui::PopFont();
}

// Big button (Play, Resume...). Neon draws a cut-corner shape: solid when
// the color is opaque, outlined when it's translucent.
bool DashButton(const char *label, ImVec4 bg, ImVec4 fg, ImVec2 size)
{
    const float s = g_viewport_mgr.m_scale;
    if (!T().neon) {
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(std::min(bg.x * 1.15f + 0.04f, 1.f),
                                     std::min(bg.y * 1.15f + 0.04f, 1.f),
                                     std::min(bg.z * 1.15f + 0.04f, 1.f), bg.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(bg.x * 0.85f, bg.y * 0.85f,
                                                            bg.z * 0.85f, bg.w));
        ImGui::PushStyleColor(ImGuiCol_Text, fg);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * s);
        bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        return pressed;
    }
    const char *end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    if (size.x <= 0) {
        size.x = ts.x + pad.x * 2 + 16 * s;
    }
    if (size.y <= 0) {
        size.y = ts.y + pad.y * 2;
    }
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + size.x, a.y + size.y);
    // EnableNav: invisible buttons are skipped by controller navigation
    // unless asked
    bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = std::min(12 * s, size.y * 0.35f);
    bool solid = bg.w > 0.5f;
    ImVec4 fill = bg;
    if (solid) {
        float k = held ? 0.85f : hovered ? 1.12f : 1.0f;
        fill = ImVec4(std::min(bg.x * k, 1.f), std::min(bg.y * k, 1.f),
                      std::min(bg.z * k, 1.f), bg.w);
        if (hovered || focused) {
            ShapeFill(dl, ImVec2(a.x - 3 * s, a.y - 3 * s),
                      ImVec2(b.x + 3 * s, b.y + 3 * s),
                      ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.22f)), r + 3 * s);
        }
        ShapeFill(dl, a, b, ImGui::GetColorU32(fill), r);
    } else {
        ShapeFill(dl, a, b,
                  ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z,
                                            held ? 0.22f : hovered ? 0.14f : bg.w)),
                  r);
        ShapeStroke(dl, a, b,
                    ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z,
                                              hovered || focused ? 0.9f : 0.45f)),
                    r, 1.0f * s);
    }
    if (focused) {
        ShapeStroke(dl, ImVec2(a.x - 4 * s, a.y - 4 * s), ImVec2(b.x + 4 * s, b.y + 4 * s),
                    ImGui::GetColorU32(ImGui::ColorConvertU32ToFloat4(T().accent)),
                    r + 4 * s, 1.5f * s);
    }
    dl->AddText(ImVec2(a.x + (size.x - ts.x) / 2, a.y + (size.y - ts.y) / 2),
                ImGui::GetColorU32(fg), label, end);
    return pressed;
}

LibraryEntry *DefaultHomeGame()
{
    auto recent = game_library_window.GetRecentGames(1);
    if (!recent.empty()) {
        if (LibraryEntry *e = FindEntry(recent[0].path)) {
            return e;
        }
    }
    std::vector<LibraryEntry *> items;
    SortedFiltered(items);
    return items.empty() ? NULL : items[0];
}

void SwitchTab(int tab)
{
    int next = (tab + kTabCount) % kTabCount;
    if (next != g_dash_tab) {
        g_anim.tab_t = ImGui::GetTime();
    }
    g_dash_tab = next;
    RequestPreviewStop();
}

// A soft band of light flowing across the screen (the "wave"): a triangle
// strip, brightest along its middle and fading to nothing at its edges
void DrawRibbon(ImDrawList *dl, ImVec2 a, ImVec2 b, float t, float base, float amp,
                float freq, float speed, float thick, float phase, ImU32 col)
{
    const int n = 72;
    float w = b.x - a.x, h = b.y - a.y;
    ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
    ImU32 clear = col & 0x00FFFFFF;
    dl->PrimReserve(n * 12, (n + 1) * 3);
    ImDrawIdx base_idx = (ImDrawIdx)dl->_VtxCurrentIdx;
    const float tau = 6.2831853f;
    for (int i = 0; i <= n; i++) {
        float u = (float)i / n;
        float y = a.y + h * (base + amp * sinf(u * freq * tau + t * speed + phase) +
                             amp * 0.45f * sinf(u * freq * 1.7f * tau - t * speed * 0.6f +
                                                phase * 2.0f));
        float th = thick * h * (0.55f + 0.45f * sinf(u * 3.1f + t * 0.35f + phase));
        float x = a.x + w * u;
        dl->PrimWriteVtx(ImVec2(x, y - th), uv, clear);
        dl->PrimWriteVtx(ImVec2(x, y), uv, col);
        dl->PrimWriteVtx(ImVec2(x, y + th), uv, clear);
    }
    for (int i = 0; i < n; i++) {
        ImDrawIdx c0 = (ImDrawIdx)(base_idx + i * 3), c1 = (ImDrawIdx)(c0 + 3);
        ImDrawIdx q[12] = { c0, (ImDrawIdx)(c0 + 1), (ImDrawIdx)(c1 + 1),
                            c0, (ImDrawIdx)(c1 + 1), c1,
                            (ImDrawIdx)(c0 + 1), (ImDrawIdx)(c0 + 2), (ImDrawIdx)(c1 + 2),
                            (ImDrawIdx)(c0 + 1), (ImDrawIdx)(c1 + 2), (ImDrawIdx)(c1 + 1) };
        for (ImDrawIdx v : q) {
            dl->PrimWriteIdx(v);
        }
    }
}

// A slowly turning wireframe orb: a nod to the original Xbox dashboard's
// glowing sphere, drawn as a globe of light lines
void DrawOrb(ImDrawList *dl, ImVec2 c, float r, float t, ImU32 front, ImU32 back)
{
    const float pi = 3.14159265f;
    const int seg = 40;
    float rot = t * 0.12f;
    float tilt = 0.28f; // view slightly from above
    ImVec2 pts[seg + 1];
    // Meridians
    for (int k = 0; k < 9; k++) {
        float lon = rot + k * pi / 9;
        bool facing = cosf(lon) > 0;
        for (int i = 0; i <= seg; i++) {
            float lat = -pi / 2 + pi * i / seg;
            float x = cosf(lat) * sinf(lon);
            float z = cosf(lat) * cosf(lon);
            float y = sinf(lat);
            pts[i] = ImVec2(c.x + r * x, c.y - r * (y * cosf(tilt) - z * sinf(tilt)));
        }
        dl->AddPolyline(pts, seg + 1, facing ? front : back, 0, 1.0f);
    }
    // Parallels
    for (int k = 1; k < 6; k++) {
        float lat = -pi / 2 + pi * k / 6;
        for (int i = 0; i <= seg; i++) {
            float lon = 2 * pi * i / seg;
            float x = cosf(lat) * sinf(lon);
            float z = cosf(lat) * cosf(lon);
            float y = sinf(lat);
            pts[i] = ImVec2(c.x + r * x, c.y - r * (y * cosf(tilt) - z * sinf(tilt)));
        }
        dl->AddPolyline(pts, seg + 1, front, 0, 1.0f);
    }
}

// Animated layers behind the dashboard: ribbons, the orb, drifting specks
// and (Neon) a grid floor. Kept faint so the content stays readable.
void DrawDashMotion(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float s = g_viewport_mgr.m_scale;
    const LibTheme &th = T();
    float t = (float)fmod(ImGui::GetTime(), 3600.0);
    float w = b.x - a.x, h = b.y - a.y;

    if (th.neon) {
        // Grid floor scrolling toward the viewer
        float horizon = a.y + h * 0.70f;
        float cx = a.x + w * 0.5f;
        for (int j = -14; j <= 14; j++) {
            float xb = cx + j * w * 0.075f;
            dl->AddLine(ImVec2(cx + j * w * 0.012f, horizon), ImVec2(xb, b.y),
                        IM_COL32(255, 46, 136, 12), 1.0f);
        }
        for (int k = 0; k < 12; k++) {
            float z = (float)fmod(k / 12.0 + t * 0.045, 1.0);
            float y = horizon + (b.y - horizon) * z * z;
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y),
                        IM_COL32(255, 46, 136, (int)(4 + 22 * z)), 1.0f);
        }
        dl->AddRectFilledMultiColor(ImVec2(a.x, horizon - 2 * s), ImVec2(b.x, horizon + 30 * s),
                                    IM_COL32(4, 4, 9, 0), IM_COL32(4, 4, 9, 0),
                                    IM_COL32(4, 4, 9, 120), IM_COL32(4, 4, 9, 120));
    }

    // Orb, upper right, behind the content
    ImU32 orb = th.neon ? th.accent : IM_COL32(120, 230, 60, 255);
    float orb_r = h * 0.34f;
    ImVec2 oc(a.x + w * 0.80f, a.y + h * 0.40f);
    for (int i = 0; i < 4; i++) {
        dl->AddCircleFilled(oc, orb_r * (0.35f + i * 0.2f), Alpha(orb, 6), 48);
    }
    DrawOrb(dl, oc, orb_r, t, Alpha(orb, 46), Alpha(orb, 16));

    // Flowing ribbons
    if (th.neon) {
        DrawRibbon(dl, a, b, t, 0.60f, 0.07f, 0.9f, 0.22f, 0.08f, 0.0f, IM_COL32(0, 229, 255, 52));
        DrawRibbon(dl, a, b, t, 0.68f, 0.06f, 1.2f, 0.17f, 0.06f, 2.1f, IM_COL32(255, 46, 136, 44));
        DrawRibbon(dl, a, b, t, 0.52f, 0.05f, 0.7f, 0.12f, 0.04f, 4.0f, IM_COL32(140, 110, 255, 38));
    } else {
        DrawRibbon(dl, a, b, t, 0.62f, 0.07f, 0.9f, 0.20f, 0.08f, 0.0f, IM_COL32(98, 202, 19, 50));
        DrawRibbon(dl, a, b, t, 0.70f, 0.06f, 1.2f, 0.15f, 0.06f, 2.1f, IM_COL32(170, 240, 90, 34));
        DrawRibbon(dl, a, b, t, 0.54f, 0.05f, 0.7f, 0.11f, 0.04f, 4.0f, IM_COL32(40, 160, 70, 40));
    }

    // Drifting specks of light
    for (int i = 0; i < 42; i++) {
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float fx = (hsh & 0xFFFF) / 65535.0f;
        float fs = ((hsh >> 16) & 0xFF) / 255.0f;
        float fp = ((hsh >> 24) & 0xFF) / 255.0f;
        float speed = 6 + 16 * fs;
        float y = b.y - (float)fmod(t * speed * s + fp * h * 1.2f, h * 1.2f);
        float x = a.x + w * fx + 12 * s * sinf(t * 0.4f + i);
        float tw = 0.5f + 0.5f * sinf(t * (1.5f + fs) + i * 1.7f);
        ImU32 col = th.neon ? (i % 3 == 0 ? th.accent2 : th.accent) : th.accent;
        dl->AddCircleFilled(ImVec2(x, y), (1.0f + 1.4f * fs) * s, Alpha(col, (int)(25 + 70 * tw)), 8);
    }
}

// Background. Green: a soft glow top-right. Neon: near-black violet, faint
// cyan and magenta glows, and scanlines. Both animate when enabled.
void DrawDashBackground(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float s = g_viewport_mgr.m_scale;
    float w = b.x - a.x, h = b.y - a.y;
    if (!T().neon) {
        dl->AddRectFilledMultiColor(a, b, IM_COL32(8, 14, 10, 120),
                                    IM_COL32(40, 90, 20, 120), IM_COL32(8, 10, 9, 120),
                                    IM_COL32(14, 40, 22, 120));
        for (int i = 0; i < 6; i++) {
            dl->AddCircleFilled(ImVec2(a.x + w * 0.80f, a.y - h * 0.05f),
                                w * (0.12f + i * 0.06f), IM_COL32(98, 202, 19, 7), 64);
        }
        if (Animations()) {
            DrawDashMotion(dl, a, b);
        }
        return;
    }
    dl->AddRectFilledMultiColor(a, b, IM_COL32(9, 8, 18, 150), IM_COL32(6, 10, 20, 150),
                                IM_COL32(4, 4, 9, 170), IM_COL32(12, 5, 16, 170));
    for (int i = 0; i < 5; i++) {
        dl->AddCircleFilled(ImVec2(a.x + w * 0.86f, a.y + h * 0.02f),
                            w * (0.10f + i * 0.055f), IM_COL32(0, 229, 255, 5), 64);
        dl->AddCircleFilled(ImVec2(a.x + w * 0.04f, b.y + h * 0.06f),
                            w * (0.08f + i * 0.05f), IM_COL32(255, 46, 136, 5), 64);
    }
    if (Animations()) {
        DrawDashMotion(dl, a, b);
    }
    float step = std::max(3.0f, 3 * s);
    for (float y = a.y; y < b.y; y += step) {
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(0, 0, 0, 34), 1.0f);
    }
}

// Top bar: brand, LB, tabs, RB, then status, clock and close on the right
void DrawDashTopBar()
{
    const float s = g_viewport_mgr.m_scale;
    const LibTheme &th = T();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float bar_h = 46 * s;
    ImVec2 start = ImGui::GetCursorScreenPos();
    float y_mid = start.y + bar_h / 2;

    // Brand: Green has a glowing orb; Neon a diamond and spaced letters
    float r = 11 * s;
    ImVec2 c(start.x + r + 2 * s, y_mid);
    std::string brand = "xemu";
    if (th.neon) {
        ImVec2 d[4] = { ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                        ImVec2(c.x - r, c.y) };
        dl->AddPolyline(d, 4, th.accent, ImDrawFlags_Closed, 2.0f * s);
        ImVec2 di[4] = { ImVec2(c.x, c.y - r * 0.45f), ImVec2(c.x + r * 0.45f, c.y),
                         ImVec2(c.x, c.y + r * 0.45f), ImVec2(c.x - r * 0.45f, c.y) };
        dl->AddConvexPolyFilled(di, 4, th.accent2);
        brand = "X E M U";
    } else {
        dl->AddCircleFilled(c, r * 1.8f, IM_COL32(98, 202, 19, 40), 32);
        dl->AddCircleFilled(c, r, IM_COL32(98, 202, 19, 255), 32);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.45f,
                            IM_COL32(200, 255, 140, 200), 24);
    }
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_small));
    float fh = ImGui::GetFontSize();
    ImGui::SetCursorScreenPos(ImVec2(c.x + r + 10 * s, y_mid - fh / 2));
    ImGui::TextUnformatted(brand.c_str());
    ImGui::PopFont();

    auto chip = [&](const char *t) {
        ImGui::SameLine(0, 18 * s);
        ImGui::PushFont(HudFont());
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImVec2 p(ImGui::GetCursorScreenPos().x, y_mid - ts.y / 2 - 3 * s);
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(ts.x + 12 * s, ts.y + 6 * s));
        ImVec2 q(p.x + ts.x + 12 * s, p.y + ts.y + 6 * s);
        if (th.neon) {
            ShapeStroke(dl, p, q, Alpha(th.accent, 110), 5 * s, 1.0f * s);
            dl->AddText(ImVec2(p.x + 6 * s, p.y + 3 * s), Alpha(th.accent, 220), t);
        } else {
            dl->AddRectFilled(p, q, IM_COL32(42, 51, 42, 255), 6 * s);
            dl->AddText(ImVec2(p.x + 6 * s, p.y + 3 * s), IM_COL32(220, 230, 218, 255), t);
        }
        ImGui::PopFont();
    };
    chip("LB");

    // Tabs. Green: pills. Neon: numbered labels with an underline. The
    // highlight is drawn underneath (channel 0) and glides to the active tab.
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_small));
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImVec2 hl_a(0, 0), hl_b(0, 0);
    for (int t = 0; t < kTabCount; t++) {
        bool on = g_dash_tab == t;
        ImGui::PushID(t);
        if (th.neon) {
            ImGui::SameLine(0, t ? 4 * s : 14 * s);
            std::string name = Upper(kTabNames[t]);
            char num[16];
            snprintf(num, sizeof(num), "%02d", t + 1);
            ImVec2 ts = ImGui::CalcTextSize(name.c_str());
            ImFont *hud = HudFont();
            ImVec2 ns = hud->CalcTextSizeA(hud->FontSize, FLT_MAX, 0, num);
            ImVec2 size(ns.x + 6 * s + ts.x + 24 * s, ts.y + 18 * s);
            ImVec2 p(ImGui::GetCursorScreenPos().x, y_mid - size.y / 2);
            ImGui::SetCursorScreenPos(p);
            if (ImGui::InvisibleButton(kTabNames[t], size, ImGuiButtonFlags_EnableNav)) {
                SwitchTab(t);
            }
            bool hov = ImGui::IsItemHovered();
            float tx = p.x + 12 * s;
            float ty = p.y + (size.y - ts.y) / 2;
            dl->AddText(hud, hud->FontSize, ImVec2(tx, ty + ts.y - ns.y - 2 * s),
                        on ? th.accent2 : Alpha(th.dim, 160), num);
            dl->AddText(ImVec2(tx + ns.x + 6 * s, ty),
                        on ? th.accent : hov ? th.text : th.dim, name.c_str());
            if (on) {
                hl_a = ImVec2(tx, p.y + size.y - 8 * s);
                hl_b = ImVec2(p.x + size.x - 12 * s, p.y + size.y + 1 * s);
            }
        } else {
            ImGui::SameLine(0, t ? 6 * s : 12 * s);
            ImVec2 ts = ImGui::CalcTextSize(kTabNames[t]);
            ImVec2 size(ts.x + 34 * s, ts.y + 14 * s);
            ImGui::SetCursorScreenPos(
                ImVec2(ImGui::GetCursorScreenPos().x, y_mid - size.y / 2));
            ImVec2 tp = ImGui::GetCursorScreenPos();
            if (on) {
                hl_a = tp;
                hl_b = ImVec2(tp.x + size.x, tp.y + size.y);
            }
            // The active pill is drawn underneath, so it can glide
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y / 2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  on ? ImVec4(0, 0, 0, 0) : ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  on ? ImVec4(0, 0, 0, 0) : ImVec4(1, 1, 1, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  on ? th.on_primary : ImVec4(0.60f, 0.66f, 0.59f, 1));
            if (ImGui::Button(kTabNames[t], size)) {
                SwitchTab(t);
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
        }
        ImGui::PopID();
    }
    // Glide the highlight toward the active tab
    dl->ChannelsSetCurrent(0);
    if (hl_b.x > hl_a.x) {
        float k = Animations()
                      ? 1.0f - expf(-ImGui::GetIO().DeltaTime * 16.0f)
                      : 1.0f;
        if (g_anim.hl_x < 0) {
            k = 1.0f;
        }
        g_anim.hl_x += (hl_a.x - g_anim.hl_x) * k;
        g_anim.hl_w += ((hl_b.x - hl_a.x) - g_anim.hl_w) * k;
        ImVec2 ha(g_anim.hl_x, hl_a.y), hb(g_anim.hl_x + g_anim.hl_w, hl_b.y);
        if (th.neon) {
            dl->AddRectFilled(ha, ImVec2(hb.x, hb.y - 2 * s), Alpha(th.accent, 26));
            dl->AddRectFilled(ImVec2(ha.x, hb.y - 3 * s), ImVec2(hb.x, hb.y - 1 * s),
                              th.accent);
        } else {
            dl->AddRectFilled(ha, hb, ImGui::GetColorU32(th.primary), (hb.y - ha.y) / 2);
        }
    }
    dl->ChannelsMerge();
    ImGui::PopFont();
    chip("RB");

    // Right side: scan progress, Insignia status, clock, close
    const char *dvd = g_config.sys.files.dvd_path;
    const char *close_label = (dvd && dvd[0]) ? ICON_FA_XMARK "  Back to game"
                                              : ICON_FA_XMARK "  Close";
    float close_w = ImGui::CalcTextSize(close_label).x + 28 * s;
    char clock[32] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        strftime(clock, sizeof(clock), "%H:%M", tm);
    }
    CheckInsigniaStatus(false);
    std::string ins = "Insignia";
    ImU32 ins_col = IM_COL32(140, 140, 140, 255);
    switch ((InsigniaStatus)(int)g_insignia_status) {
    case InsigniaStatus::Up:
        ins = "Insignia online";
        ins_col = th.ok;
        break;
    case InsigniaStatus::Maintenance:
        ins = "Insignia maintenance";
        ins_col = th.warn;
        break;
    case InsigniaStatus::Down:
        ins = "Insignia unreachable";
        ins_col = th.bad;
        break;
    default:
        break;
    }
    if (th.neon) {
        ins = Upper(ins);
    }
    std::string scan = g_scan_running
                           ? string_format(ICON_FA_ARROWS_ROTATE "  %d / %d",
                                           (int)g_scan_done, (int)g_scan_total)
                           : std::string();
    ImFont *hud = HudFont();
    float hsz = hud->FontSize;
    auto text_w = [&](const std::string &t) {
        return hud->CalcTextSizeA(hsz, FLT_MAX, 0, t.c_str()).x;
    };
    float gap = 22 * s;
    float ins_w = text_w(ins) + 26 * s; // light + text + padding
    float clock_w = text_w(clock);
    float scan_w = scan.empty() ? 0 : ImGui::CalcTextSize(scan.c_str()).x + gap;
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    float x = right - close_w - gap - clock_w - gap - ins_w - scan_w;
    float min_x = ImGui::GetItemRectMax().x + gap;
    if (x >= min_x) {
        float ty = y_mid - hsz / 2;
        if (!scan.empty()) {
            dl->AddText(ImVec2(x, y_mid - ImGui::GetFontSize() / 2), th.dim, scan.c_str());
            x += scan_w;
        }
        // Status in a box tinted with its color, with a glowing light
        ImVec2 ba(x - 8 * s, y_mid - hsz / 2 - 6 * s);
        ImVec2 bb(x + ins_w - 2 * s, y_mid + hsz / 2 + 6 * s);
        float glow = Animations() && g_insignia_status == (int)InsigniaStatus::Up
                         ? 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 2.4f)
                         : 1.0f;
        ShapeFill(dl, ba, bb, Alpha(ins_col, 30), 6 * s);
        ShapeStroke(dl, ba, bb, Alpha(ins_col, 150), 6 * s, 1.0f * s);
        ImVec2 lc(x + 5 * s, y_mid);
        dl->AddCircleFilled(lc, 9 * s, Alpha(ins_col, (int)(45 * glow)), 20);
        if (th.neon) {
            dl->AddRectFilled(ImVec2(lc.x - 4 * s, lc.y - 4 * s),
                              ImVec2(lc.x + 4 * s, lc.y + 4 * s), ins_col);
        } else {
            dl->AddCircleFilled(lc, 5 * s, ins_col, 16);
        }
        dl->AddText(hud, hsz, ImVec2(x + 18 * s, ty), ins_col, ins.c_str());
        x += ins_w + gap;
        dl->AddText(hud, hsz, ImVec2(x, ty),
                    th.neon ? th.accent : IM_COL32(183, 196, 180, 255), clock);
    }
    ImGui::SetCursorScreenPos(ImVec2(right - close_w, y_mid - 16 * s));
    if (DashButton(close_label, th.glass, th.white, ImVec2(close_w, 32 * s))) {
        game_library_window.is_open = false;
    }
    ImGui::SetItemTooltip("Esc or B");
    if (th.neon) {
        // Thin divider under the bar
        float y = start.y + bar_h + 4 * s;
        float wx = ImGui::GetWindowPos().x;
        float ww = ImGui::GetWindowSize().x;
        dl->AddRectFilledMultiColor(ImVec2(wx, y), ImVec2(wx + ww, y + 1 * s),
                                    Alpha(th.accent, 0), Alpha(th.accent, 90),
                                    Alpha(th.accent2, 90), Alpha(th.accent2, 0));
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + bar_h + 10 * s));
}

// Controller hints along the bottom
bool HomebrewInstalledView();

void DrawDashHints(ImVec2 a, float w)
{
    const float s = g_viewport_mgr.m_scale;
    struct Hint {
        const char *key;
        ImU32 col;
        const char *text;
    };
    std::vector<Hint> hints;
    const ImU32 ka = IM_COL32(63, 163, 27, 255), kb = IM_COL32(200, 50, 42, 255),
                ky = IM_COL32(226, 180, 23, 255);
    switch (g_dash_tab) {
    case kTabHome:
    case kTabLibrary:
        hints = { { "A", ka, "Play" }, { "Y", ky, "Favorite" },
                  { "B", kb, "Back to game" } };
        break;
    case kTabInsignia:
        hints = { { "A", ka, "Play online" }, { "B", kb, "Back to game" } };
        break;
    case kTabStore:
        if (HomebrewInstalledView()) {
            hints = { { "A", ka, "Play" }, { "Y", ky, "Options" },
                      { "B", kb, "Back to game" } };
        } else {
            hints = { { "A", ka, "Select" }, { "B", kb, "Back to game" } };
        }
        break;
    default:
        hints = { { "A", ka, "Select" }, { "B", kb, "Back to game" } };
        break;
    }
    const bool neon = T().neon;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float fh = ImGui::GetFontSize();
    float x = a.x, y = a.y + fh * 0.2f;
    float r = fh * 0.62f;
    for (auto &h : hints) {
        ImVec2 c(x + r, y + fh / 2);
        ImVec2 ks = ImGui::CalcTextSize(h.key);
        if (neon) {
            ImVec2 p(c.x - r, c.y - r), q(c.x + r, c.y + r);
            ShapeFill(dl, p, q, Alpha(h.col, 40), 5 * s);
            ShapeStroke(dl, p, q, h.col, 5 * s, 1.0f * s);
            dl->AddText(ImVec2(c.x - ks.x / 2, c.y - ks.y / 2), h.col, h.key);
        } else {
            dl->AddCircleFilled(c, r, h.col, 24);
            dl->AddText(ImVec2(c.x - ks.x / 2, c.y - ks.y / 2),
                        h.col == ky ? IM_COL32(30, 30, 30, 255) : IM_COL32_WHITE, h.key);
        }
        std::string label = neon ? Upper(h.text) : std::string(h.text);
        ImFont *f = neon ? HudFont() : ImGui::GetFont();
        float fs = neon ? f->FontSize : fh;
        dl->AddText(f, fs, ImVec2(x + r * 2 + 8 * s, y + (fh - fs) / 2), T().dim,
                    label.c_str());
        x += r * 2 + 8 * s + f->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str()).x + 26 * s;
    }
    std::string tabs = neon ? "LB / RB  SWITCH TAB" : "LB / RB  Switch tab";
    ImFont *f = neon ? HudFont() : ImGui::GetFont();
    float fs = neon ? f->FontSize : fh;
    if (x + f->CalcTextSizeA(fs, FLT_MAX, 0, tabs.c_str()).x < a.x + w) {
        dl->AddText(f, fs, ImVec2(x, y + (fh - fs) / 2), T().dim, tabs.c_str());
    }
}

// --- Home --------------------------------------------------------------------

void DrawHomeHero(LibraryEntry &e, ImVec2 size, int &tex_budget, bool is_recent)
{
    const float s = g_viewport_mgr.m_scale;
    GameStats &st = g_stats[e.path];
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b(a.x + size.x, a.y + size.y);
    float round = 18 * s;
    EnsureTexture(e, tex_budget);

    const bool neon = T().neon;
    if (e.path != g_anim.hero_path) {
        g_anim.hero_path = e.path;
        g_anim.hero_t = ImGui::GetTime();
    }
    const float show = EaseSince(g_anim.hero_t, 0.40f); // 0 -> 1 on a new game
    // Backdrop: the cover, zoomed to fill and darkened
    ShapeFill(dl, a, b, neon ? IM_COL32(10, 10, 22, 255) : IM_COL32(16, 34, 52, 255),
              round);
    if (e.tex && e.tex_w > 0 && e.tex_h > 0) {
        float ra = size.x / size.y, ri = (float)e.tex_w / e.tex_h;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ri > ra) {
            float u = ra / ri;
            uv0.x = (1 - u) / 2;
            uv1.x = 1 - uv0.x;
        } else {
            float v = ri / ra;
            uv0.y = (1 - v) / 2;
            uv1.y = 1 - uv0.y;
        }
        ShapeImage(dl, (ImTextureID)(intptr_t)e.tex, a, b, uv0, uv1,
                   neon ? IM_COL32(150, 140, 255, (int)(70 * show))
                        : IM_COL32(255, 255, 255, (int)(90 * show)),
                   round);
    }
    ShapeFill(dl, a, b, neon ? IM_COL32(5, 5, 12, 170) : IM_COL32(6, 12, 9, 150), round);
    if (neon) {
        // Left accent bar, a magenta-to-cyan rule and HUD brackets
        dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + round), ImVec2(a.x + 3 * s, b.y),
                                    T().accent, T().accent, Alpha(T().accent2, 200),
                                    Alpha(T().accent2, 200));
        ShapeStroke(dl, a, b, Alpha(T().accent, 60), round, 1.0f * s);
        Brackets(dl, a, b, Alpha(T().accent, 200), 22 * s, 2.0f * s);
    } else {
        dl->AddRect(a, b, T().border, round, 0, 1.0f * s);
    }

    // Box art on the right
    float pad = 30 * s;
    float art_h = size.y - pad * 2;
    bool box3d = Home3DCovers();
    float art_ratio = box3d ? 0.86f : 0.72f; // 3D: room for the spine and floor
    float art_w = std::min(art_h * art_ratio, size.x * 0.3f);
    art_h = std::min(art_h, art_w / art_ratio);
    ImVec2 art_a(b.x - pad - art_w + (1 - show) * 28 * s, a.y + (size.y - art_h) / 2);
    bool show_art = size.x > 620 * s;
    if (show_art && box3d) {
        // Floor glow under the case
        ImVec2 fc(art_a.x + art_w / 2, art_a.y + art_h * 0.84f);
        dl->AddEllipseFilled(fc, ImVec2(art_w * 0.44f, art_h * 0.05f),
                             neon ? Alpha(T().accent, 26) : IM_COL32(120, 200, 120, 22), 0, 40);
        g_draw_3d = true;
        DrawArtwork(dl, art_a, ImVec2(art_a.x + art_w, art_a.y + art_h), e, 12 * s);
        g_draw_3d = false;
    } else if (show_art) {
        ShapeFill(dl, ImVec2(art_a.x + 6 * s, art_a.y + 10 * s),
                  ImVec2(art_a.x + art_w + 6 * s, art_a.y + art_h + 10 * s),
                  neon ? Alpha(T().accent2, 50) : IM_COL32(0, 0, 0, 90), 12 * s);
        DrawArtwork(dl, art_a, ImVec2(art_a.x + art_w, art_a.y + art_h), e, 12 * s);
        if (neon) {
            ShapeStroke(dl, art_a, ImVec2(art_a.x + art_w, art_a.y + art_h),
                        Alpha(T().accent, 120), 12 * s, 1.0f * s);
        }
    }
    float text_w = (show_art ? art_a.x - 24 * s : b.x - pad) - (a.x + pad);

    // Text and buttons, anchored to the bottom left
    float btn_h = 44 * s;
    float fh = ImGui::GetFontSize();
    ImFont *big = FontOr(g_font_mgr.m_menu_font);
    float title_h = big->FontSize * 1.1f;
    float block_h = fh + 6 * s + title_h + 8 * s + fh + 18 * s + btn_h;
    ImVec2 t0(a.x + pad - (1 - show) * 18 * s, b.y - pad - block_h);
    ImGui::SetCursorScreenPos(t0);
    ImGui::PushClipRect(a, ImVec2(a.x + pad + text_w, b.y), true);
    DashLabel(IsCurrentDisc(e) ? "Playing now"
                               : is_recent ? "Continue playing" : "Selected");
    ImGui::SetCursorScreenPos(ImVec2(t0.x, ImGui::GetCursorScreenPos().y + 2 * s));
    ImGui::PushFont(big);
    ImGui::TextUnformatted(neon ? Upper(e.DisplayName()).c_str()
                                : e.DisplayName().c_str());
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(t0.x, ImGui::GetCursorScreenPos().y + 4 * s));

    // Meta line
    ImGui::PushStyleColor(ImGuiCol_Text, neon ? ImVec4(0.66f, 0.70f, 0.82f, 1)
                                              : ImVec4(0.72f, 0.78f, 0.71f, 1));
    bool first = true;
    auto meta = [&](const std::string &t) {
        if (!first) {
            ImGui::SameLine(0, 18 * s);
        }
        first = false;
        ImGui::TextUnformatted(t.c_str());
    };
    if (e.valid) {
        meta(FormatTitleId(e.title_id));
    }
    meta(FormatPlayTime(st.play_seconds) + " played");
    if (!IsCurrentDisc(e) && st.last_played > 0) {
        meta("Last played " + FormatLastPlayed(st.last_played));
    }
    ImGui::PopStyleColor();
    if (SupportsOnline(e)) {
        ImGui::SameLine(0, 18 * s);
        DashPill("ONLINE", T().online);
    }
    CompatStatus hcs = CompatFor(e);
    if (hcs != CompatStatus::None && hcs != CompatStatus::Unknown) {
        ImGui::SameLine(0, 10 * s);
        DashPill(Upper(CompatName(hcs)).c_str(), CompatColor(hcs));
        ImGui::SetItemTooltip("Compatibility: %s", CompatDescription(hcs));
    }
    int npatch = e.valid ? EnabledPatchCount(e.title_id) : 0;
    if (npatch) {
        ImGui::SameLine(0, 18 * s);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T().accent),
                           ICON_FA_WRENCH "  %d patch%s on", npatch,
                           npatch == 1 ? "" : "es");
    }
    ImGui::PopClipRect();

    // Buttons
    ImGui::SetCursorScreenPos(ImVec2(t0.x, b.y - pad - btn_h));
    int resume = FindResumeSnapshot(e);
    if (resume >= 0) {
        std::string name = g_snapshot_mgr.m_snapshots[resume].name;
        if (DashButton(ICON_FA_FORWARD "  Resume", T().resume, T().white,
                       ImVec2(150 * s, btn_h))) {
            ResumeGame(e, name);
        }
        ImGui::SetItemTooltip("Continue from where you left off");
        ImGui::SameLine(0, 12 * s);
    }
    if (g_home_focus_play) {
        ImGui::SetKeyboardFocusHere(); // controller focus starts on Play
        if (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad) {
            ImGui::SetNavCursorVisible(true); // so the first A press plays
        }
        g_home_focus_play = false;
    }
    if (DashButton(IsCurrentDisc(e) ? ICON_FA_ROTATE_RIGHT "  Restart"
                                    : ICON_FA_PLAY "  Play",
                   T().primary, T().on_primary, ImVec2(140 * s, btn_h))) {
        game_library_window.Launch(e.path);
    }
    ImGui::SameLine(0, 12 * s);
    if (DashButton(st.favorite ? ICON_FA_STAR "  Favorited" : ICON_FA_STAR "  Favorite",
                   T().glass, T().white, ImVec2(150 * s, btn_h))) {
        st.favorite = !st.favorite;
        g_dirty = true;
    }
    ImGui::BeginDisabled(!e.valid);
    ImGui::SameLine(0, 12 * s);
    if (DashButton(ICON_FA_GEAR, T().glass, T().white, ImVec2(btn_h, btn_h))) {
        OpenSettingsFor(e);
    }
    ImGui::SetItemTooltip("Settings for this game");
    ImGui::SameLine(0, 12 * s);
    if (DashButton(ICON_FA_WRENCH, T().glass, T().white, ImVec2(btn_h, btn_h))) {
        OpenPatchesFor(e);
    }
    ImGui::SetItemTooltip("Patches for this game");
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(size.x, 0));
}

// One row in a list card: thumbnail, title, subtitle, right-aligned note
bool DashRow(LibraryEntry &e, const std::string &sub, const std::string &note,
             float w, int &tex_budget, bool selected = false,
             ImU32 note_col = 0)
{
    const float s = g_viewport_mgr.m_scale;
    float fh = ImGui::GetFontSize();
    float h = std::max(48 * s, fh * 2 + 18 * s);
    ImGui::PushID(e.path.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Header, Alpha(T().accent, 30));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 16));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 26));
    bool clicked = ImGui::Selectable("##row", selected,
                                     ImGuiSelectableFlags_AllowDoubleClick |
                                         ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                     ImVec2(w, h));
    ImGui::PopStyleColor(3);
    DrawContextMenu(e);
    if (ImGui::IsItemVisible()) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        EnsureTexture(e, tex_budget);
        float th = h - 8 * s;
        DrawArtwork(dl, ImVec2(p.x + 4 * s, p.y + 4 * s),
                    ImVec2(p.x + 4 * s + th * 0.8f, p.y + 4 * s + th), e, 6 * s);
        float tx = p.x + th * 0.8f + 16 * s;
        float nw = note.empty() ? 0 : ImGui::CalcTextSize(note.c_str()).x;
        ImVec4 clip(tx, p.y, p.x + w - nw - 16 * s, p.y + h);
        float ty = p.y + (h - fh * 2 - 4 * s) / 2;
        dl->AddText(ImGui::GetFont(), fh, ImVec2(tx, ty), T().text,
                    e.DisplayName().c_str(), NULL, 0, &clip);
        dl->AddText(ImGui::GetFont(), fh * 0.88f, ImVec2(tx, ty + fh + 4 * s),
                    T().dim, sub.c_str(), NULL, 0, &clip);
        if (!note.empty()) {
            dl->AddText(ImVec2(p.x + w - nw - 8 * s, p.y + (h - fh) / 2),
                        note_col ? note_col : IM_COL32(183, 196, 180, 255),
                        note.c_str());
        }
        if (selected) {
            ShapeStroke(dl, p, ImVec2(p.x + w, p.y + h), T().accent, 10 * s, 2 * s);
        }
    }
    ImGui::PopID();
    return clicked;
}

// Library games that have online patches but none downloaded yet
int GamesWithNewPatches()
{
    int n = 0;
    std::set<uint32_t> seen;
    for (auto &e : g_entries) {
        if (e.valid && !seen.count(e.title_id)) {
            seen.insert(e.title_id);
            bool fetched = false;
            for (auto &p : OnlinePatchPaths(e.title_id)) {
                fetched |= g_jmp_fetched.count(p) > 0;
            }
            n += (!fetched && OnlinePatchCount(e.title_id)) ? 1 : 0;
        }
    }
    return n;
}

void DrawHomeActivity(ImVec2 size, int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    ImVec2 a = ImGui::GetCursorScreenPos();
    DashCard(ImGui::GetWindowDrawList(), a, ImVec2(a.x + size.x, a.y + size.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * s, 18 * s));
    ImGui::BeginChild("##activity", size,
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    float w = ImGui::GetContentRegionAvail().x;
    DashLabel("Activity");
    DashTitle("Recently played");
    ImGui::Dummy(ImVec2(0, 4 * s));
    auto recent = game_library_window.GetRecentGames(6);
    int shown = 0;
    float row_h = std::max(48 * s, ImGui::GetFontSize() * 2 + 18 * s);
    int new_patches = GamesWithNewPatches();
    float reserve = new_patches ? row_h + 10 * s : 0;
    for (auto &r : recent) {
        LibraryEntry *e = FindEntry(r.path);
        if (!e || e->path == g_home_path) {
            continue;
        }
        if (ImGui::GetContentRegionAvail().y < row_h + reserve) {
            break;
        }
        GameStats &st = g_stats[e->path];
        std::string sub = (IsCurrentDisc(*e) ? std::string("Playing now")
                                             : FormatLastPlayed(st.last_played)) +
                          "  -  " + FormatPlayTime(st.play_seconds);
        std::string note = FindResumeSnapshot(*e) >= 0 ? "Resume point"
                           : st.favorite                ? ICON_FA_STAR
                                                        : "";
        if (DashRow(*e, sub, note, w, tex_budget, false,
                    st.favorite && note == ICON_FA_STAR ? IM_COL32(255, 205, 60, 255)
                                                        : 0)) {
            g_home_path = e->path;
            g_selected_path = e->path;
            g_home_seen_select = e->path;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                game_library_window.Launch(e->path);
            }
        }
        if (ActivatedByNav()) {
            game_library_window.Launch(e->path);
        }
        shown++;
    }
    if (!shown) {
        ImGui::TextDisabled("Games you play show up here.");
    }
    if (new_patches) {
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                                      ImGui::GetWindowHeight() - 18 * s - row_h));
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool go = ImGui::Selectable("##newpatches", false,
                                    ImGuiSelectableFlags_NoPadWithHalfSpacing,
                                    ImVec2(w, row_h));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float th = row_h - 8 * s;
        ShapeFill(dl, ImVec2(p.x + 4 * s, p.y + 4 * s),
                  ImVec2(p.x + 4 * s + th * 0.8f, p.y + 4 * s + th),
                  T().neon ? Alpha(T().accent2, 90) : IM_COL32(30, 110, 80, 255), 6 * s);
        ImVec2 is = ImGui::CalcTextSize(ICON_FA_WRENCH);
        dl->AddText(ImVec2(p.x + 4 * s + (th * 0.8f - is.x) / 2,
                           p.y + 4 * s + (th - is.y) / 2),
                    IM_COL32_WHITE, ICON_FA_WRENCH);
        float fh = ImGui::GetFontSize();
        float tx = p.x + th * 0.8f + 16 * s, ty = p.y + (row_h - fh * 2 - 4 * s) / 2;
        dl->AddText(ImVec2(tx, ty), T().text, "New patches available");
        std::string sub = string_format("Widescreen, 60 FPS and more for %d of your "
                                        "games",
                                        new_patches);
        ImVec4 clip(tx, p.y, p.x + w - 60 * s, p.y + row_h);
        dl->AddText(ImGui::GetFont(), fh * 0.88f, ImVec2(tx, ty + fh + 4 * s),
                    T().dim, sub.c_str(), NULL, 0, &clip);
        ImVec2 gs = ImGui::CalcTextSize("Get  " ICON_FA_CHEVRON_RIGHT);
        dl->AddText(ImVec2(p.x + w - gs.x - 8 * s, p.y + (row_h - fh) / 2), T().accent,
                    "Get  " ICON_FA_CHEVRON_RIGHT);
        if (go || ActivatedByNav()) {
            SwitchTab(kTabPatches);
            StartPatchDownload(0, false);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawHomeShelf(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    DashLabel(string_format("Your library  -  %d games", (int)g_entries.size()).c_str());
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12 * s);
    if (ImGui::SmallButton("See all  " ICON_FA_CHEVRON_RIGHT)) {
        SwitchTab(kTabLibrary);
    }
    // Recently played first, then favorites, then the rest by title
    std::vector<LibraryEntry *> order;
    std::set<std::string> added;
    for (auto &r : game_library_window.GetRecentGames(12)) {
        if (LibraryEntry *e = FindEntry(r.path)) {
            if (added.insert(e->path).second) {
                order.push_back(e);
            }
        }
    }
    std::vector<LibraryEntry *> rest;
    SortedFiltered(rest);
    for (LibraryEntry *e : rest) {
        if (order.size() >= 40) {
            break;
        }
        if (added.insert(e->path).second) {
            order.push_back(e);
        }
    }
    float avail_h = ImGui::GetContentRegionAvail().y;
    float fh = ImGui::GetFontSize();
    float text_h = 8 * s + fh * 3 + 4 * s;
    float tile_w = std::floor(std::max(80 * s, std::min(210 * s, avail_h - text_h - 24 * s)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##shelf", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(18 * s, 0));
    g_draw_3d = Home3DCovers();
    for (size_t i = 0; i < order.size(); i++) {
        if (i) {
            ImGui::SameLine();
        }
        DrawTile(*order[i], tile_w, tex_budget);
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) {
            g_home_path = order[i]->path; // the hero follows controller focus
        }
    }
    g_draw_3d = false;
    ImGui::PopStyleVar();
    // Mouse wheel scrolls the shelf sideways
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0 &&
        !ImGui::GetIO().KeyShift) {
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 120 * s);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void DrawHomeTab(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    // The hero shows the most recently played game, or whatever the user
    // picked on the shelf or in the library
    if (g_selected_path != g_home_seen_select) {
        g_home_seen_select = g_selected_path;
        if (FindEntry(g_selected_path)) {
            g_home_path = g_selected_path;
        }
    }
    LibraryEntry *hero = FindEntry(g_home_path);
    if (!hero) {
        hero = DefaultHomeGame();
        if (hero) {
            g_home_path = hero->path;
        }
    }
    if (!hero) {
        ImGui::Dummy(ImVec2(0, 30 * s));
        ImGui::TextDisabled(g_scan_running ? "Looking for games..."
                                           : "No games yet. Add a folder on the "
                                             "Library tab.");
        return;
    }
    auto recent = game_library_window.GetRecentGames(1);
    bool is_recent = !recent.empty() && recent[0].path == hero->path;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = 22 * s;
    float side_w = avail.x > 1000 * s ? std::floor(avail.x * 0.35f) : 0;
    float hero_h = std::floor(std::clamp(avail.y * 0.52f, 230 * s, 440 * s));
    ImVec2 hero_a = ImGui::GetCursorScreenPos();
    DrawHomeHero(*hero, ImVec2(side_w ? avail.x - side_w - gap : avail.x, hero_h),
                 tex_budget, is_recent);
    if (side_w) {
        ImGui::SetCursorScreenPos(ImVec2(hero_a.x + avail.x - side_w, hero_a.y));
        DrawHomeActivity(ImVec2(side_w, hero_h), tex_budget);
        ImGui::SetCursorScreenPos(ImVec2(hero_a.x, hero_a.y + hero_h));
    }
    ImGui::Dummy(ImVec2(0, 18 * s));
    DrawHomeShelf(tex_budget);
}

// --- Insignia ------------------------------------------------------------------

void DrawInsigniaTab(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = 22 * s;
    float left_w = std::floor(std::max(360 * s, avail.x * 0.36f));
    bool two_col = avail.x > 820 * s;
    if (!two_col) {
        left_w = avail.x;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();

    // Status card
    CheckInsigniaStatus(false);
    InsigniaStatus st = (InsigniaStatus)(int)g_insignia_status;
    int owned = 0;
    for (auto &e : g_entries) {
        owned += SupportsOnline(e) ? 1 : 0;
    }
    float status_h = 250 * s;
    DashCard(dl, a, ImVec2(a.x + left_w, a.y + status_h));
    ImGui::SetCursorScreenPos(ImVec2(a.x + 26 * s, a.y + 22 * s));
    ImGui::BeginGroup();
    DashLabel("Insignia  -  Xbox Live revival");
    ImGui::Dummy(ImVec2(0, 8 * s));
    ImVec2 rc = ImGui::GetCursorScreenPos();
    float rr = 28 * s;
    ImU32 ring = st == InsigniaStatus::Up            ? T().ok
                 : st == InsigniaStatus::Maintenance ? T().warn
                 : st == InsigniaStatus::Down        ? T().bad
                                                     : IM_COL32(140, 140, 140, 255);
    ImVec2 cc(rc.x + rr, rc.y + rr);
    if (T().neon) { // a hexagon reads more "HUD" than a ring
        dl->AddNgonFilled(cc, rr + 8 * s, Alpha(ring, 30), 6);
        dl->AddNgon(cc, rr, ring, 6, 3 * s);
    } else {
        dl->AddCircleFilled(cc, rr + 8 * s, (ring & 0x00FFFFFF) | 0x28000000, 48);
        dl->AddCircle(cc, rr, ring, 48, 5 * s);
    }
    const char *icon = st == InsigniaStatus::Up            ? ICON_FA_CHECK
                       : st == InsigniaStatus::Checking    ? ICON_FA_ELLIPSIS
                       : st == InsigniaStatus::Maintenance ? ICON_FA_WRENCH
                       : st == InsigniaStatus::Down        ? ICON_FA_XMARK
                                                           : ICON_FA_QUESTION;
    ImVec2 is = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(cc.x - is.x / 2, cc.y - is.y / 2), ring, icon);
    ImGui::SetCursorScreenPos(ImVec2(rc.x + rr * 2 + 18 * s, rc.y + 2 * s));
    ImGui::BeginGroup();
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_medium));
    ImGui::TextUnformatted(st == InsigniaStatus::Up            ? "Service online"
                           : st == InsigniaStatus::Maintenance ? "Maintenance"
                           : st == InsigniaStatus::Down        ? "Unreachable"
                           : st == InsigniaStatus::Checking    ? "Checking..."
                                                               : "Status unknown");
    ImGui::PopFont();
    uint64_t ago = (SDL_GetTicks() - g_insignia_checked_at) / 60000;
    if (st == InsigniaStatus::Checking || st == InsigniaStatus::Unknown) {
        ImGui::TextDisabled("insignia.live");
    } else {
        ImGui::TextDisabled("Checked %s  -  insignia.live",
                            ago == 0 ? "just now"
                                     : string_format("%d min ago", (int)ago).c_str());
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(rc.x, rc.y + rr * 2 + 20 * s));
    float stat_w = (left_w - 52 * s - 14 * s) / 2;
    auto stat = [&](const std::string &big, const char *what) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = 64 * s;
        dl->AddRectFilled(p, ImVec2(p.x + stat_w, p.y + h), IM_COL32(255, 255, 255, 12),
                          12 * s);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14 * s, p.y + 8 * s));
        ImGui::BeginGroup();
        ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_small));
        ImGui::TextUnformatted(big.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s", what);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x + stat_w + 14 * s, p.y));
    };
    stat(std::to_string(owned), "of your games play online");
    stat(std::to_string((int)(sizeof(kInsigniaTitleIds) / sizeof(kInsigniaTitleIds[0]))),
         "supported games");
    ImGui::EndGroup();

    // Setup checklist
    float setup_y = a.y + status_h + gap;
    float setup_h = two_col ? avail.y - status_h - gap : 300 * s;
    DashCard(dl, ImVec2(a.x, setup_y), ImVec2(a.x + left_w, setup_y + setup_h));
    ImGui::SetCursorScreenPos(ImVec2(a.x + 26 * s, setup_y + 22 * s));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(a.x + left_w - 26 * s);
    DashLabel("Get online in xemu");
    ImGui::Dummy(ImVec2(0, 6 * s));
    bool net_ok = g_config.net.enable &&
                  g_config.net.backend == CONFIG_NET_BACKEND_NAT;
    auto step = [&](int n, bool done, const char *title, const char *sub) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = 13 * s;
        ImVec2 c(p.x + r, p.y + r);
        if (done && T().neon) {
            ShapeFill(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), T().ok, 6 * s);
            ImVec2 ts = ImGui::CalcTextSize(ICON_FA_CHECK);
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(4, 6, 12, 255),
                        ICON_FA_CHECK);
        } else if (!done && T().neon) {
            ShapeStroke(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                        Alpha(T().dim, 200), 6 * s, 1.5f * s);
            std::string num = std::to_string(n);
            ImVec2 ts = ImGui::CalcTextSize(num.c_str());
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), T().dim, num.c_str());
        } else if (done) {
            dl->AddCircleFilled(c, r, IM_COL32(98, 202, 19, 255), 24);
            ImVec2 ts = ImGui::CalcTextSize(ICON_FA_CHECK);
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(7, 16, 10, 255),
                        ICON_FA_CHECK);
        } else {
            dl->AddCircle(c, r, IM_COL32(110, 125, 108, 255), 24, 2 * s);
            std::string num = std::to_string(n);
            ImVec2 ts = ImGui::CalcTextSize(num.c_str());
            dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), T().dim, num.c_str());
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + r * 2 + 14 * s, p.y));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title);
        ImGui::TextDisabled("%s", sub);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetItemRectMax().y + 12 * s));
    };
    step(1, net_ok, "Turn on networking",
         "Settings > Network: Enable, with the NAT backend");
    step(2, false, "Point the Xbox at Insignia",
         "Set the DNS from Insignia's Connect guide in the dashboard's network "
         "settings");
    step(3, false, "Create your account",
         "Register with the Insignia Setup Assistant, then run the network test");
    step(4, false, "Play", "Start a supported game and sign in to Xbox Live");
    if (ImGui::Button("Connect guide")) {
        SDL_OpenURL("https://insignia.live/connect");
    }
    ImGui::SameLine();
    if (ImGui::Button("Who's playing")) {
        SDL_OpenURL("https://insignia.live/games");
    }
    ImGui::SetItemTooltip("Players online and active sessions per game, on "
                          "insignia.live");
    ImGui::SameLine();
    ImGui::BeginDisabled(st == InsigniaStatus::Checking);
    if (ImGui::Button("Check again")) {
        CheckInsigniaStatus(true);
    }
    ImGui::EndDisabled();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();

    // Your online games
    ImVec2 la = two_col ? ImVec2(a.x + left_w + gap, a.y)
                        : ImVec2(a.x, setup_y + setup_h + gap);
    float list_w = two_col ? avail.x - left_w - gap : avail.x;
    float list_h = two_col ? avail.y : 420 * s;
    ImGui::SetCursorScreenPos(la);
    DashCard(dl, la, ImVec2(la.x + list_w, la.y + list_h));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * s, 22 * s));
    ImGui::BeginChild("##online", ImVec2(list_w, list_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened);
    DashLabel("Your library");
    DashTitle("Ready for online play");
    ImGui::TextDisabled("Games you own that Insignia supports. Live player counts "
                        "are on insignia.live.");
    ImGui::Dummy(ImVec2(0, 8 * s));
    std::vector<LibraryEntry *> mine;
    for (auto &e : g_entries) {
        if (SupportsOnline(e)) {
            mine.push_back(&e);
        }
    }
    std::stable_sort(mine.begin(), mine.end(), [](LibraryEntry *x, LibraryEntry *y) {
        int64_t lx = g_stats[x->path].last_played, ly = g_stats[y->path].last_played;
        if (lx != ly) {
            return lx > ly;
        }
        return Lower(x->DisplayName()) < Lower(y->DisplayName());
    });
    if (mine.empty()) {
        ImGui::TextDisabled("None of your games are on Insignia's list yet.");
    }
    float w = ImGui::GetContentRegionAvail().x;
    for (LibraryEntry *e : mine) {
        GameStats &gs = g_stats[e->path];
        std::string sub = FormatTitleId(e->title_id) + "  -  " +
                          (IsCurrentDisc(*e)   ? std::string("Playing now")
                           : gs.last_played > 0 ? "Played " + FormatLastPlayed(gs.last_played)
                                                : std::string("Not played yet"));
        bool sel = g_selected_path == e->path;
        // One click (or A) plays: this list exists to get into a game
        if (DashRow(*e, sub, ICON_FA_PLAY "  Play online", w, tex_budget, sel,
                    T().neon ? T().accent : IM_COL32(168, 236, 107, 255))) {
            g_selected_path = e->path;
            game_library_window.Launch(e->path);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// --- Media ---------------------------------------------------------------------

void DrawMediaTab(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    LibraryEntry *e = FindEntry(g_media_path);
    if (!e) {
        e = FindEntry(g_home_path);
        if (!e) {
            e = DefaultHomeGame();
        }
        if (!e) {
            ImGui::TextDisabled("No games yet.");
            return;
        }
        g_media_path = e->path;
    }
    // Game picker: games with save states or screenshots first
    ImGui::AlignTextToFramePadding();
    DashLabel("Showing");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(420 * s, ImGui::GetContentRegionAvail().x * 0.5f));
    if (ImGui::BeginCombo("##mediagame", e->DisplayName().c_str())) {
        std::vector<LibraryEntry *> items;
        SortedFiltered(items);
        for (LibraryEntry *it : items) {
            size_t saves = SnapshotsFor(*it).size();
            size_t shots = it->valid ? ScreenshotsFor(it->title_id).size() : 0;
            std::string label = it->DisplayName();
            if (saves || shots) {
                label += string_format("   (%zu save%s, %zu screenshot%s)", saves,
                                       saves == 1 ? "" : "s", shots,
                                       shots == 1 ? "" : "s");
            }
            ImGui::PushID(it->path.c_str());
            if (ImGui::Selectable(label.c_str(), it == e)) {
                g_media_path = it->path;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Screenshot folder")) {
        const char *dir = g_config.general.screenshot_dir;
        std::string p =
            fs::absolute(fs::u8path(dir && dir[0] ? dir : ".")).generic_u8string();
        SDL_OpenURL(("file:///" + p).c_str());
    }
    ImGui::Dummy(ImVec2(0, 10 * s));

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = 22 * s;
    float col_w = std::floor((avail.x - gap) / 2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    for (int col = 0; col < 2; col++) {
        ImVec2 ca(a.x + col * (col_w + gap), a.y);
        DashCard(dl, ca, ImVec2(ca.x + col_w, ca.y + avail.y));
        ImGui::SetCursorScreenPos(ca);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * s, 20 * s));
        ImGui::BeginChild(col ? "##shots" : "##saves", ImVec2(col_w, avail.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened);
        float w = ImGui::GetContentRegionAvail().x;
        if (col == 0) {
            DashLabel("Save states");
            DashTitle(e->DisplayName().c_str());
            if (SnapshotsFor(*e).empty()) {
                ImGui::Dummy(ImVec2(0, 8 * s));
                ImGui::TextDisabled("No save states for this game yet. Create one "
                                    "from Machine > Snapshot while playing.");
            } else {
                DrawSaveStates(*e, w, 50);
            }
        } else {
            DashLabel("Screenshots");
            DashTitle(e->DisplayName().c_str());
            if (!e->valid || ScreenshotsFor(e->title_id).empty()) {
                ImGui::Dummy(ImVec2(0, 8 * s));
                ImGui::TextDisabled("No screenshots yet. Press F12 while playing.");
            } else {
                DrawScreenshots(*e, w, w > 600 * s ? 3 : 2, 60);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    (void)tex_budget;
}

// --- Homebrew store ------------------------------------------------------------
//
// XboxHomebrew.store (Team Resurgent): browse homebrew, download it, unpack
// it and copy it to E:\Apps on the Xbox hard drive. Network work runs on
// background queues; results are applied on the main thread by StorePump().

constexpr int kStorePage = 40;

// A background thread running posted work in order. Heap-allocated and never
// freed: a queue may be in the middle of a download when xemu exits.
class StoreQueue
{
public:
    void Post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(m_);
        q_.push_back(std::move(fn));
        if (!started_) {
            started_ = true;
            std::thread([this] { Run(); }).detach();
        }
        cv_.notify_one();
    }
    void Clear()
    {
        std::lock_guard<std::mutex> lock(m_);
        q_.clear();
    }

private:
    void Run()
    {
        Error *err = NULL;
        if (!ensure_libcurl_initialized(&err) && err) {
            error_free(err);
        }
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return !q_.empty(); });
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool started_ = false;
};

StoreQueue *const g_store_api = new StoreQueue; // lists and versions
StoreQueue *const g_store_img = new StoreQueue; // covers and screenshots
StoreQueue *const g_store_dl = new StoreQueue;  // installs

std::mutex g_store_main_mutex;
std::vector<std::function<void()>> *const g_store_main =
    new std::vector<std::function<void()>>;

void StoreOnMain(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(g_store_main_mutex);
    g_store_main->push_back(std::move(fn));
}

// Download progress, shared with the download thread
struct StoreTransfer {
    std::atomic<uint64_t> now{ 0 }, total{ 0 };
    std::atomic<bool> cancel{ false };
    std::atomic<int> phase{ 0 }; // 0 downloading, 1 unpacking
    std::atomic<int> file{ 0 };
    int files = 1;
    uint64_t last_now = 0, last_change = 0; // stall detection (download thread)
};

int StoreProgressCb(http_progress_cb_info *p)
{
    auto *t = static_cast<StoreTransfer *>(p->userptr);
    uint64_t ms = SDL_GetTicks();
    if (p->dlnow != t->last_now || !t->last_change) {
        t->last_now = p->dlnow;
        t->last_change = ms;
    }
    t->now = p->dlnow;
    t->total = p->dltotal;
    // Give up on a connection that has gone quiet for 30 s
    return (t->cancel || ms - t->last_change > 30000) ? 1 : 0;
}

// GET into *body; the HTTP status, or <= 0 if the server couldn't be reached
int StoreGet(const std::string &url, std::string *body, StoreTransfer *t = nullptr)
{
    StoreTransfer local;
    if (!t) {
        t = &local;
    }
    t->last_change = 0;
    http_progress_cb_info pi = {};
    pi.progress = StoreProgressCb;
    pi.userptr = t;
    GByteArray *b = g_byte_array_new();
    int code = http_get(url.c_str(), b, &pi, NULL);
    body->assign((const char *)b->data, b->len);
    g_byte_array_unref(b);
    return code;
}

std::string StoreHttpError(int code)
{
    if (code <= 0) {
        return "Couldn't reach XboxHomebrew.store. Check your internet "
               "connection.";
    }
    return string_format("XboxHomebrew.store answered with an error (HTTP %d).",
                         code);
}

// Ids are base64url; keep file names to that
std::string StoreFileId(const std::string &id)
{
    std::string out;
    for (char c : id) {
        out += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    }
    return out.empty() ? "_" : out;
}

fs::path StoreCacheDir()
{
    return IconCacheDir() / "store";
}

bool WriteFileAtomic(const fs::path &path, const std::string &data)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(data.data(), (std::streamsize)data.size());
        if (!o) {
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

struct StoreState {
    bool started = false;
    std::vector<StoreCategory> cats;
    std::string category; // "" for all
    char search[64] = {};
    std::string search_applied;
    double search_edit_t = -1;

    std::vector<StoreApp> apps;
    int offset = 0;
    bool more = false, loading = false;
    std::string err;
    uint32_t gen = 0;

    std::string sel; // selected app id
    std::vector<StoreVersion> versions;
    bool versions_loading = false;
    std::string versions_err;
    int ver = 0;

    // Install
    enum Phase { Idle, Downloading, Copying, Done, Failed } phase = Idle;
    std::shared_ptr<StoreTransfer> xfer;
    std::string inst_app, inst_name, inst_msg;
    std::string inst_dir; // "Games", "Apps"... on E:
    std::string inst_id;  // store app id
    fs::path staging;
    bool confirm = false;
} g_store;

const StoreApp *StoreSelected()
{
    for (auto &a : g_store.apps) {
        if (a.id == g_store.sel) {
            return &a;
        }
    }
    return nullptr;
}

void QueueStoreImage(const std::string &key, const fs::path &file, int max_h)
{
    ImageJob job{ "store:" + key, { file }, max_h };
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

// Cover ("c:<id>") or screenshot ("s:<id>"), downloaded once into the cache
const StoreImage *GetStoreImage(const std::string &id, bool screenshot)
{
    std::string key = (screenshot ? "s:" : "c:") + id;
    StoreImage &img = g_store_images[key];
    if (img.requested) {
        return &img;
    }
    img.requested = true;
    fs::path file = StoreCacheDir() / (screenshot ? "screens" : "covers") /
                    (StoreFileId(id) + ".img");
    std::string url = screenshot ? StoreUrlScreenshot(id, 640, 360)
                                 : StoreUrlCover(id, 240, 336);
    int max_h = screenshot ? 360 : 336;
    g_store_img->Post([key, file, url, max_h] {
        std::error_code ec;
        if (!fs::exists(file, ec)) {
            std::string body;
            int w, h, n;
            if (StoreGet(url, &body) != 200 ||
                !stbi_info_from_memory((const stbi_uc *)body.data(),
                                       (int)body.size(), &w, &h, &n) ||
                !WriteFileAtomic(file, body)) {
                return;
            }
        }
        StoreOnMain([key, file, max_h] { QueueStoreImage(key, file, max_h); });
    });
    return &img;
}

void StoreFetchApps(bool reset)
{
    if (reset) {
        g_store.apps.clear();
        g_store.offset = 0;
        g_store.more = false;
        g_store.gen++;
    }
    g_store.loading = true;
    g_store.err.clear();
    uint32_t gen = g_store.gen;
    std::string url = StoreUrlApps(g_store.offset, kStorePage, g_store.category,
                                   g_store.search_applied);
    g_store_api->Post([gen, url] {
        std::string body;
        int code = StoreGet(url, &body);
        std::vector<StoreApp> apps;
        bool ok = code == 200 && ParseStoreApps(body, &apps);
        StoreOnMain([gen, ok, code, apps] {
            if (gen != g_store.gen) {
                return; // the filter changed meanwhile
            }
            g_store.loading = false;
            if (!ok) {
                g_store.err = code == 200 ? "The store sent something unexpected."
                                          : StoreHttpError(code);
                return;
            }
            g_store.offset += (int)apps.size();
            g_store.more = (int)apps.size() >= kStorePage;
            for (auto &a : apps) {
                if (StoreAppAllowed(a)) {
                    g_store.apps.push_back(a);
                }
            }
        });
    });
}

void StoreStart()
{
    if (g_store.started) {
        return;
    }
    g_store.started = true;
    g_store_api->Post([] {
        std::string body;
        std::vector<StoreCategory> cats;
        if (StoreGet(StoreUrlCategories(), &body) != 200 ||
            !ParseStoreCategories(body, &cats)) {
            return; // the app list reports connection problems
        }
        StoreOnMain([cats] {
            g_store.cats.clear();
            for (auto &c : cats) {
                if (StoreCategoryAllowed(c.name)) {
                    g_store.cats.push_back(c);
                }
            }
        });
    });
    StoreFetchApps(true);
}

void StoreSelect(const std::string &id)
{
    if (g_store.sel == id) {
        return;
    }
    g_store.sel = id;
    g_store.versions.clear();
    g_store.versions_err.clear();
    g_store.versions_loading = true;
    g_store.ver = 0;
    g_store_api->Post([id] {
        std::string body;
        int code = StoreGet(StoreUrlVersions(id), &body);
        std::vector<StoreVersion> versions;
        bool ok = code == 200 && ParseStoreVersions(body, &versions);
        StoreOnMain([id, ok, code, versions] {
            if (g_store.sel != id) {
                return;
            }
            g_store.versions_loading = false;
            if (!ok) {
                g_store.versions_err = StoreHttpError(code);
                return;
            }
            g_store.versions = versions;
            g_store.ver = 0;
            const StoreApp *app = StoreSelected();
            for (size_t i = 0; app && i < versions.size(); i++) {
                if (versions[i].id == app->latest_version) {
                    g_store.ver = (int)i;
                }
            }
        });
    });
}

bool StoreInstallBusy()
{
    return g_store.phase == StoreState::Downloading ||
           g_store.phase == StoreState::Copying;
}

void StoreDownloadDone(const std::string &err, const fs::path &content)
{
    if (!err.empty()) {
        g_store.phase = StoreState::Failed;
        g_store.inst_msg = err;
        std::error_code ec;
        fs::remove_all(g_store.staging, ec);
        return;
    }
    std::string e;
    if (!XboxExplorerInstallFolder(content.u8string(), 'E', "/" + g_store.inst_dir,
                                   g_store.inst_name,
                                   "Installing " + g_store.inst_app + " to E:\\" +
                                       g_store.inst_dir + "\\" + g_store.inst_name,
                                   &e)) {
        g_store.phase = StoreState::Failed;
        g_store.inst_msg = e;
        std::error_code ec;
        fs::remove_all(g_store.staging, ec);
        return;
    }
    g_store.phase = StoreState::Copying;
}

void StoreInstall(const StoreApp &app, const StoreVersion &v)
{
    if (StoreInstallBusy()) {
        return;
    }
    g_store.inst_app = app.name;
    g_store.inst_dir = StoreInstallFolder(app.category);
    g_store.inst_id = app.id;
    g_store.inst_name =
        StoreSafeFolderName(v.folder_name.empty() ? app.name : v.folder_name);
    g_store.inst_msg.clear();
    g_store.staging = StoreCacheDir() / "downloads" / StoreFileId(v.id);
    std::error_code ec;
    fs::remove_all(g_store.staging, ec);
    auto xfer = std::make_shared<StoreTransfer>();
    xfer->files = std::max<int>(1, (int)v.files.size());
    g_store.xfer = xfer;
    g_store.phase = StoreState::Downloading;
    fs::path stage = g_store.staging;
    std::string vid = v.id;
    std::vector<std::string> files = v.files;
    g_store_dl->Post([xfer, stage, vid, files] {
        std::string err;
        fs::path content = stage / "content";
        std::error_code ec;
        fs::create_directories(content, ec);
        for (int i = 0; i < xfer->files && err.empty(); i++) {
            xfer->file = i;
            xfer->phase = 0;
            xfer->now = 0;
            xfer->total = 0;
            std::string body;
            int code = StoreGet(StoreUrlDownload(vid, i), &body, xfer.get());
            bool zip = body.size() >= 4 && body.compare(0, 4, "PK\x03\x04") == 0;
            if (code == 200 && !zip) {
                // Large files on Google Drive answer with a confirmation page
                std::string confirm = StoreDriveConfirmUrl(body);
                if (!confirm.empty()) {
                    code = StoreGet(confirm, &body, xfer.get());
                    zip = body.size() >= 4 && body.compare(0, 4, "PK\x03\x04") == 0;
                }
            }
            if (xfer->cancel) {
                err = "Cancelled.";
                break;
            }
            if (code != 200 || body.empty()) {
                err = code == 200   ? std::string("The download was empty.")
                      : code <= 0 ? StoreHttpError(code)
                                  : string_format("The download failed (HTTP %d). "
                                                  "Try again later.",
                                                  code);
                break;
            }
            if (zip) {
                xfer->phase = 1;
                fs::path z = stage / "download.zip";
                if (!WriteFileAtomic(z, body)) {
                    err = "Couldn't save the download (is the disk full?)";
                    break;
                }
                body.clear();
                body.shrink_to_fit();
                // Each zip on its own first: one wrapped in a single folder
                // ("2H4Ux/default.xbe") is unwrapped
                fs::path unz = stage / string_format("zip%d", i);
                if (!StoreExtractZip(z.u8string(), unz.u8string(), &err)) {
                    break;
                }
                fs::remove(z, ec);
                fs::path root = fs::u8path(StoreAppRoot(unz.u8string()));
                for (auto &e : fs::directory_iterator(root, ec)) {
                    fs::path to = content / e.path().filename();
                    fs::remove_all(to, ec);
                    fs::rename(e.path(), to, ec);
                    if (ec) {
                        err = "Couldn't unpack the download (is the disk full?)";
                        break;
                    }
                }
                fs::remove_all(unz, ec);
                continue;
            }
            size_t lead = body.find_first_not_of(" \t\r\n");
            if (lead != std::string::npos && body[lead] == '<') {
                err = "The download link gave a web page instead of the file. "
                      "Try again later.";
                break;
            }
            std::string name = i < (int)files.size() && !files[i].empty()
                                   ? fs::u8path(files[i]).filename().u8string()
                                   : std::string("download.bin");
            std::string why;
            if (!XboxValidName(name, &why)) {
                name = StoreSafeFolderName(name);
            }
            if (!WriteFileAtomic(content / fs::u8path(name), body)) {
                err = "Couldn't save the download (is the disk full?)";
            }
        }
        StoreOnMain([err, content] { StoreDownloadDone(err, content); });
    });
}

void HomebrewPump();
void HomebrewMarkDirty();
void StoreRememberInstall(const std::string &dir, const std::string &app_id);

// Every frame (also while the store isn't shown)
void StorePump()
{
    HomebrewPump();
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lock(g_store_main_mutex);
        fns.swap(*g_store_main);
    }
    for (auto &fn : fns) {
        fn();
    }
    if (g_store.phase == StoreState::Copying && !XboxExplorerBusy()) {
        bool error = false;
        std::string status = XboxExplorerStatus(&error);
        g_store.phase = error ? StoreState::Failed : StoreState::Done;
        if (!error) {
            StoreRememberInstall("/" + g_store.inst_dir + "/" + g_store.inst_name,
                                 g_store.inst_id);
            HomebrewMarkDirty(); // list it under Installed
        }
        g_store.inst_msg =
            error ? status
                  : "Installed to E:\\" + g_store.inst_dir + "\\" +
                        g_store.inst_name +
                        ". The Microsoft dashboard doesn't list homebrew: start "
                        "it from a homebrew dashboard such as XBMC4Gamers, which "
                        "finds E:\\Games, E:\\Apps and E:\\Emulators.";
        std::error_code ec;
        fs::remove_all(g_store.staging, ec);
    }
}

std::string StoreSize(uint64_t n)
{
    if (n >= (1ull << 30)) {
        return string_format("%.2f GB", n / 1073741824.0);
    }
    if (n >= (1ull << 20)) {
        return string_format("%.1f MB", n / 1048576.0);
    }
    return string_format("%.0f KB", std::max<uint64_t>(n, 1) / 1024.0 + 0.5);
}

// Image scaled to cover [a, b], cropped to keep its shape
void StoreImageFill(ImDrawList *dl, const StoreImage *img, ImVec2 a, ImVec2 b, float r)
{
    float bw = b.x - a.x, bh = b.y - a.y;
    float ia = (float)img->w / img->h, ba = bw / bh;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ia > ba) {
        float u = ba / ia;
        uv0.x = (1 - u) / 2;
        uv1.x = 1 - uv0.x;
    } else {
        float v = ia / ba;
        uv0.y = (1 - v) / 2;
        uv1.y = 1 - uv0.y;
    }
    ShapeImage(dl, (ImTextureID)(intptr_t)img->tex, a, b, uv0, uv1,
               IM_COL32_WHITE, r);
}

// Placeholder: the app's initials on a tinted card
void StorePlaceholder(ImDrawList *dl, const std::string &name, ImVec2 a, ImVec2 b,
                      float r)
{
    uint32_t h = 2166136261u;
    for (char c : name) {
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    ImU32 col = IM_COL32(40 + h % 60, 50 + (h >> 8) % 60, 70 + (h >> 16) % 70, 255);
    ShapeFill(dl, a, b, col, r);
    std::string ini;
    for (size_t i = 0; i < name.size() && ini.size() < 2; i++) {
        if (isalnum((unsigned char)name[i]) &&
            (i == 0 || name[i - 1] == ' ' || ini.empty())) {
            ini += (char)toupper((unsigned char)name[i]);
        }
    }
    ImFont *f = FontOr(g_font_mgr.m_menu_font_medium);
    float fs_ = f->FontSize;
    ImVec2 ts = f->CalcTextSizeA(fs_, FLT_MAX, 0, ini.c_str());
    dl->AddText(f, fs_, ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2),
                IM_COL32(255, 255, 255, 170), ini.c_str());
}

// One app card; true when chosen
bool StoreCard(const StoreApp &app, float w)
{
    const float s = g_viewport_mgr.m_scale;
    float cover_h = w * 1.4f;
    float h = cover_h + ImGui::GetFontSize() * 2.6f;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::PushID(app.id.c_str());
    bool pressed = ImGui::InvisibleButton("##card", ImVec2(w, h),
                                          ImGuiButtonFlags_EnableNav);
    bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (ImGui::IsItemFocused() && g_store.sel != app.id &&
        ImGui::GetIO().NavVisible) {
        StoreSelect(app.id); // follow the controller
    }
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 cb(a.x + w, a.y + cover_h);
    float r = 10 * s;
    if (ImGui::IsRectVisible(a, ImVec2(a.x + w, a.y + h))) {
        const StoreImage *img = GetStoreImage(app.id, false);
        if (img->tex) {
            StoreImageFill(dl, img, a, cb, r);
        } else {
            StorePlaceholder(dl, app.name, a, cb, r);
        }
        bool sel = g_store.sel == app.id;
        if (sel || hovered) {
            ShapeStroke(dl, a, cb, sel ? T().accent : Alpha(T().accent, 140), r,
                        (sel ? 3.0f : 2.0f) * s);
        }
        float ty = cb.y + 6 * s;
        ImGui::PushClipRect(ImVec2(a.x, ty), ImVec2(a.x + w, a.y + h), true);
        dl->AddText(ImVec2(a.x + 2 * s, ty), T().text, app.name.c_str());
        dl->AddText(ImVec2(a.x + 2 * s, ty + ImGui::GetFontSize() * 1.15f), T().dim,
                    app.author.empty() ? app.category.c_str() : app.author.c_str());
        ImGui::PopClipRect();
    }
    return pressed;
}

void DrawStoreDetails(const StoreApp &app, float w)
{
    const float s = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // Screenshot banner, cover over its corner
    ImVec2 a = ImGui::GetCursorScreenPos();
    float bh = w * 9 / 16;
    const StoreImage *shot = GetStoreImage(app.id, true);
    ImVec2 bb(a.x + w, a.y + bh);
    if (shot->tex) {
        StoreImageFill(dl, shot, a, bb, 12 * s);
    } else {
        ShapeFill(dl, a, bb, Alpha(T().accent, 26), 12 * s);
    }
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + bh * 0.45f), bb, IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 200),
                                IM_COL32(0, 0, 0, 200));
    float cw = std::min(110 * s, w * 0.28f), ch = cw * 1.4f;
    ImVec2 ca(a.x + 14 * s, bb.y - ch * 0.6f), cbb(ca.x + cw, ca.y + ch);
    const StoreImage *cover = GetStoreImage(app.id, false);
    if (cover->tex) {
        StoreImageFill(dl, cover, ca, cbb, 8 * s);
    } else {
        StorePlaceholder(dl, app.name, ca, cbb, 8 * s);
    }
    ShapeStroke(dl, ca, cbb, IM_COL32(255, 255, 255, 60), 8 * s, 1.0f * s);
    ImGui::SetCursorScreenPos(ImVec2(cbb.x + 14 * s, bb.y + 8 * s));
    ImGui::BeginGroup();
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_small));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(app.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::TextDisabled("%s%s%s", app.author.empty() ? "" : "by ",
                        app.author.c_str(),
                        app.category.empty() ? ""
                                             : ("  -  " + app.category).c_str());
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(a.x, std::max(cbb.y, ImGui::GetItemRectMax().y) +
                                              12 * s));

    // Version, install
    const StoreVersion *v = nullptr;
    if (g_store.versions_loading) {
        ImGui::TextDisabled("Loading versions...");
    } else if (!g_store.versions_err.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T().warn), "%s",
                           g_store.versions_err.c_str());
    } else if (g_store.versions.empty()) {
        ImGui::TextDisabled("No downloads for this app yet.");
    } else {
        g_store.ver = std::min(g_store.ver, (int)g_store.versions.size() - 1);
        v = &g_store.versions[g_store.ver];
    }
    if (v) {
        if (g_store.versions.size() > 1) {
            ImGui::SetNextItemWidth(std::min(220 * s, w));
            std::string label = "Version " + v->version;
            if (ImGui::BeginCombo("##storever", label.c_str())) {
                for (int i = 0; i < (int)g_store.versions.size(); i++) {
                    std::string l = g_store.versions[i].version;
                    if (!g_store.versions[i].release_date.empty()) {
                        l += "   (" + g_store.versions[i].release_date + ")";
                    }
                    if (ImGui::Selectable(l.c_str(), i == g_store.ver)) {
                        g_store.ver = i;
                    }
                }
                ImGui::EndCombo();
            }
            v = &g_store.versions[g_store.ver];
        } else {
            ImGui::Text("Version %s", v->version.c_str());
        }
        std::string meta;
        auto add = [&](const std::string &t) {
            if (!t.empty()) {
                meta += (meta.empty() ? "" : "   ") + t;
            }
        };
        add(v->release_date);
        add(v->size ? StoreSize(v->size) : "");
        add(v->region == "GLO" ? "All regions" : v->region);
        ImGui::TextDisabled("%s", meta.c_str());
        ImGui::Dummy(ImVec2(0, 4 * s));

        bool busy = StoreInstallBusy();
        bool this_one = busy && g_store.inst_app == app.name;
        if (this_one && g_store.phase == StoreState::Downloading) {
            StoreTransfer &t = *g_store.xfer;
            float frac = t.total ? (float)((double)t.now / t.total) : 0.0f;
            std::string label =
                t.phase == 1 ? std::string("Unpacking...")
                : t.total
                    ? string_format("Downloading %s / %s", StoreSize(t.now).c_str(),
                                    StoreSize(t.total).c_str())
                    : std::string("Downloading...");
            if (t.files > 1) {
                label += string_format("  (file %d of %d)", t.file + 1, t.files);
            }
            ImGui::ProgressBar(t.phase == 1 ? 1.0f : frac,
                               ImVec2(w - 110 * s, 0), label.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100 * s, 0))) {
                t.cancel = true;
            }
        } else if (this_one && g_store.phase == StoreState::Copying) {
            ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(w, 0),
                               "Copying to the Xbox...");
        } else {
            ImGui::BeginDisabled(busy || XboxExplorerBusy());
            if (DashButton(ICON_FA_DOWNLOAD "  Install to Xbox", T().primary,
                           T().on_primary, ImVec2(0, 38 * s))) {
                g_store.confirm = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0, 12 * s);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (38 * s - ImGui::GetFontSize()) / 2);
            ImGui::TextDisabled("to E:\\%s\\%s",
                                StoreInstallFolder(app.category).c_str(),
                                StoreSafeFolderName(v->folder_name.empty()
                                                        ? app.name
                                                        : v->folder_name)
                                    .c_str());
        }
        if (!busy && !g_store.inst_msg.empty() && g_store.inst_app == app.name) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(
                                   g_store.phase == StoreState::Failed ? T().bad
                                                                       : T().ok),
                               "%s %s",
                               g_store.phase == StoreState::Failed
                                   ? ICON_FA_TRIANGLE_EXCLAMATION
                                   : ICON_FA_CHECK,
                               g_store.inst_msg.c_str());
            ImGui::PopTextWrapPos();
            if (g_store.phase == StoreState::Done &&
                DashButton(ICON_FA_HARD_DRIVE "  Show on the Xbox drive", T().glass,
                           T().white, ImVec2(0, 32 * s))) {
                SwitchTab(kTabDrive); // the explorer is already in that folder
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 8 * s));
    ImGui::PushTextWrapPos(0.0f);
    if (!app.description.empty()) {
        ImGui::TextUnformatted(app.description.c_str());
    }
    if (v && !v->changelog.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * s));
        DashLabel("What's new");
        ImGui::TextDisabled("%s", v->changelog.c_str());
    }
    ImGui::PopTextWrapPos();

    // Confirmation
    if (g_store.confirm) {
        g_store.confirm = false;
        ImGui::OpenPopup("##store-confirm");
    }
    if (ImGui::BeginPopupModal("##store-confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string folder = v ? StoreSafeFolderName(v->folder_name.empty()
                                                         ? app.name
                                                         : v->folder_name)
                               : "";
        ImGui::Text("Install %s%s?", app.name.c_str(),
                    v ? (" " + v->version).c_str() : "");
        ImGui::TextDisabled("Goes to E:\\%s\\%s%s",
                            StoreInstallFolder(app.category).c_str(), folder.c_str(),
                            v && v->size ? ("  (" + StoreSize(v->size) + ")").c_str()
                                         : "");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped(
            "When the download is done the Xbox pauses while the files are "
            "copied, then restarts. Progress in a running game that hasn't been "
            "saved will be lost. An older copy in that folder is replaced.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button(ICON_FA_DOWNLOAD " Install") && v) {
            ImGui::CloseCurrentPopup();
            StoreInstall(app, *v);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            GameLibraryConsumeBack();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// --- My homebrew: what's installed on the Xbox drive, started from a disc
// image made of its folder (xemu boots discs, not files on the hard drive)

struct HomebrewState {
    bool loaded = false;
    std::vector<XboxHomebrewApp> apps;
    std::string err;
    int view = -1; // 0 installed, 1 store; -1 until the first list
    bool preparing = false;
    std::string iso, title, msg;
    bool msg_err = false;
    // Options menu and delete
    XboxHomebrewApp menu_app;
    ImVec2 menu_pos;
    bool open_menu = false, open_delete = false, deleting = false;
    std::set<std::string> searched; // store lookups tried this session
} g_hb;

fs::path StoreInstalledPath()
{
    return StoreCacheDir() / "installed.json";
}

// Store app id by install location ("E:/Games/2H4Ux"), for covers
std::map<std::string, std::string> &StoreInstalled()
{
    static std::map<std::string, std::string> m;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(StoreInstalledPath());
        json j = json::parse(f, nullptr, false);
        if (j.is_object()) {
            for (auto &kv : j.items()) {
                if (kv.value().is_string()) {
                    m[kv.key()] = kv.value().get<std::string>();
                }
            }
        }
    }
    return m;
}

std::string HomebrewKey(char vol, const std::string &dir)
{
    std::string k = std::string(1, vol) + ":" + dir;
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    return k;
}

void SaveStoreInstalled()
{
    json j = json::object();
    for (auto &kv : StoreInstalled()) {
        j[kv.first] = kv.second;
    }
    WriteFileAtomic(StoreInstalledPath(), j.dump(1));
}

void StoreRememberInstall(const std::string &dir, const std::string &app_id)
{
    StoreInstalled()[HomebrewKey('E', dir)] = app_id;
    SaveStoreInstalled();
}

// Letters and digits only, lower case: "Abbaye X" == "abbaye_x"
std::string LooseName(const std::string &n)
{
    std::string o;
    for (unsigned char c : n) {
        if (isalnum(c)) {
            o += (char)tolower(c);
        }
    }
    return o;
}

// Homebrew copied on by hand: find it in the store by name for its cover
void StoreLookUpCover(const XboxHomebrewApp &a)
{
    std::string key = HomebrewKey(a.vol, a.dir);
    if (StoreInstalled().count(key) || !g_hb.searched.insert(key).second) {
        return;
    }
    std::vector<std::string> names = { a.title };
    if (LooseName(a.folder) != LooseName(a.title)) {
        names.push_back(a.folder);
    }
    g_store_api->Post([key, names] {
        for (auto &n : names) {
            std::string body;
            std::vector<StoreApp> apps;
            if (LooseName(n).empty() ||
                StoreGet(StoreUrlApps(0, 20, "", n), &body) != 200 ||
                !ParseStoreApps(body, &apps)) {
                continue;
            }
            for (auto &app : apps) {
                if (StoreAppAllowed(app) && LooseName(app.name) == LooseName(n)) {
                    std::string id = app.id;
                    StoreOnMain([key, id] {
                        StoreInstalled()[key] = id;
                        SaveStoreInstalled();
                    });
                    return;
                }
            }
        }
    });
}

// Covers for one installed app: the store's, one of yours in a covers
// folder (by title ID, like games), and the XBE's own title image
void HomebrewLoadArt(const XboxHomebrewApp &a)
{
    std::string key = HomebrewKey(a.vol, a.dir);
    StoreLookUpCover(a);
    if (a.title_id) {
        std::vector<fs::path> cands;
        for (auto &folder : AllFolders()) {
            fs::path covers = fs::u8path(folder.path) / "covers";
            for (const char *ext : { ".png", ".jpg", ".jpeg" }) {
                cands.push_back(covers / (TitleIdHex(a.title_id) + ext));
                cands.push_back(covers / (FormatTitleId(a.title_id) + ext));
            }
        }
        StoreImage &h = g_store_images["h:" + key];
        if (!h.requested) {
            h.requested = true;
            ImageJob job{ "store:h:" + key, cands, 336 };
            std::lock_guard<std::mutex> lock(g_img_mutex);
            if (!g_img_quit) {
                g_img_jobs.push_back(std::move(job));
            }
        }
    }
    if (!g_img_thread.joinable()) {
        g_img_thread = std::thread(ImageWorker);
    }
    g_img_cv.notify_one();
    StoreImage &x = g_store_images["x:" + key];
    if (!x.requested && !a.title_image.empty()) {
        x.requested = true;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (DecodeXprImage(a.title_image.data(), a.title_image.size(), w, h, rgba) &&
            !rgba.empty()) {
            x.tex = UploadTexture(rgba.data(), w, h);
            x.w = w;
            x.h = h;
        }
    }
}

bool HomebrewInstalledView()
{
    return g_hb.view == 0;
}

void HomebrewMarkDirty()
{
    g_hb.loaded = false;
}

void HomebrewRefresh()
{
    g_hb.loaded = true;
    g_hb.err.clear();
    if (!XboxExplorerFindHomebrew(&g_hb.apps, &g_hb.err)) {
        g_hb.apps.clear();
    }
    for (auto &a : g_hb.apps) {
        HomebrewLoadArt(a);
    }
    if (g_hb.view < 0) {
        g_hb.view = g_hb.apps.empty() ? 1 : 0;
    }
}

std::string HomebrewDisplayDir(const XboxHomebrewApp &a)
{
    std::string d = std::string(1, a.vol) + ":" + a.dir;
    std::replace(d.begin(), d.end(), '/', '\\');
    return d;
}

void HomebrewPlay(const XboxHomebrewApp &a)
{
    if (g_hb.preparing || XboxExplorerBusy()) {
        return;
    }
    std::string name = StoreSafeFolderName(a.title);
    fs::path iso = IconCacheDir() / "homebrew" /
                   StoreFileId(std::string(1, a.vol) + a.dir) / (name + ".iso");
    bool ready = false;
    std::string err;
    g_hb.msg.clear();
    g_hb.title = a.title;
    g_hb.iso = iso.u8string();
    if (!XboxExplorerMakeXiso(a.vol, a.dir, g_hb.iso, &ready, &err)) {
        g_hb.msg = err;
        g_hb.msg_err = true;
        return;
    }
    if (ready) {
        game_library_window.Launch(g_hb.iso);
        return;
    }
    g_hb.preparing = true;
}

void HomebrewDelete(const XboxHomebrewApp &a)
{
    std::string err;
    g_hb.msg.clear();
    if (!XboxExplorerDeleteFolder(a.vol, a.dir, a.title, &err)) {
        g_hb.msg = err;
        g_hb.msg_err = true;
        return;
    }
    g_hb.deleting = true;
    g_hb.title = a.title;
    // Its disc image and remembered store cover go too
    std::string key = HomebrewKey(a.vol, a.dir);
    std::error_code ec;
    fs::remove_all(IconCacheDir() / "homebrew" /
                       StoreFileId(std::string(1, a.vol) + a.dir),
                   ec);
    if (StoreInstalled().erase(key)) {
        SaveStoreInstalled();
    }
    g_hb.searched.erase(key);
    for (const char *p : { "h:", "x:" }) {
        auto it = g_store_images.find(p + key);
        if (it != g_store_images.end()) {
            DeleteTextureLater(it->second.tex);
            g_store_images.erase(it);
        }
    }
}

// Every frame, from StorePump()
void HomebrewPump()
{
    if (g_hb.deleting && !XboxExplorerBusy()) {
        g_hb.deleting = false;
        bool error = false;
        std::string status = XboxExplorerStatus(&error);
        g_hb.msg = error ? status : g_hb.title + " was deleted.";
        g_hb.msg_err = error;
        g_hb.loaded = false;
    }
    if (!g_hb.preparing || XboxExplorerBusy()) {
        return;
    }
    g_hb.preparing = false;
    bool error = false;
    std::string status = XboxExplorerStatus(&error);
    std::error_code ec;
    if (error || !fs::exists(fs::u8path(g_hb.iso), ec)) {
        g_hb.msg = error ? status : "Cancelled";
        g_hb.msg_err = error;
        return;
    }
    game_library_window.Launch(g_hb.iso);
}

bool HomebrewCard(const XboxHomebrewApp &a, float w)
{
    const float s = g_viewport_mgr.m_scale;
    float cover_h = w * 1.4f;
    float h = cover_h + ImGui::GetFontSize() * 2.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID((std::string(1, a.vol) + a.dir).c_str());
    bool pressed = ImGui::InvisibleButton("##hb", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%s\nRight-click for more", a.title.c_str(),
                          HomebrewDisplayDir(a).c_str());
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
        (ImGui::IsItemFocused() &&
         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))) {
        g_hb.menu_app = a;
        g_hb.open_menu = true;
        // Beside the mouse, or over the card when opened with Y
        g_hb.menu_pos = ImGui::IsItemHovered()
                            ? ImGui::GetMousePos()
                            : ImVec2(p.x + w * 0.2f, p.y + w * 0.4f);
    }
    ImGui::PopID();
    if (!ImGui::IsRectVisible(p, ImVec2(p.x + w, p.y + h))) {
        return pressed;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 cb(p.x + w, p.y + cover_h);
    float r = 10 * s;
    std::string key = HomebrewKey(a.vol, a.dir);
    auto it = StoreInstalled().find(key);
    const StoreImage *img =
        it != StoreInstalled().end() ? GetStoreImage(it->second, false) : nullptr;
    auto own = g_store_images.find("h:" + key);
    auto icon = g_store_images.find("x:" + key);
    if (img && img->tex) {
        StoreImageFill(dl, img, p, cb, r);
    } else if (own != g_store_images.end() && own->second.tex) {
        StoreImageFill(dl, &own->second, p, cb, r);
    } else if (icon != g_store_images.end() && icon->second.tex) {
        // The XBE's square title image on its own blurred, darkened copy
        StoreImageFill(dl, &icon->second, p, cb, r);
        dl->AddRectFilled(p, cb, IM_COL32(0, 0, 0, 150), r);
        float side = w * 0.86f;
        ImVec2 ia((p.x + cb.x - side) / 2, p.y + (cover_h - side) / 2);
        ShapeImage(dl, (ImTextureID)(intptr_t)icon->second.tex, ia,
                   ImVec2(ia.x + side, ia.y + side), ImVec2(0, 0), ImVec2(1, 1),
                   IM_COL32_WHITE, 6 * s);
    } else {
        StorePlaceholder(dl, a.title, p, cb, r);
    }
    if (hovered) {
        ShapeStroke(dl, p, cb, T().accent, r, 2.5f * s);
        // Play badge
        ImVec2 c((p.x + cb.x) / 2, (p.y + cb.y) / 2);
        float br = w * 0.16f;
        dl->AddCircleFilled(c, br, IM_COL32(0, 0, 0, 150));
        ImFont *f = ImGui::GetFont();
        ImVec2 ts = ImGui::CalcTextSize(ICON_FA_PLAY);
        dl->AddText(f, f->FontSize, ImVec2(c.x - ts.x / 2 + 2 * s, c.y - ts.y / 2),
                    IM_COL32_WHITE, ICON_FA_PLAY);
    }
    float ty = cb.y + 6 * s;
    ImGui::PushClipRect(ImVec2(p.x, ty), ImVec2(p.x + w, p.y + h), true);
    dl->AddText(ImVec2(p.x + 2 * s, ty), T().text, a.title.c_str());
    std::string sub = a.section + (a.vol != 'E' ? std::string("  -  ") + a.vol + ":" : "");
    dl->AddText(ImVec2(p.x + 2 * s, ty + ImGui::GetFontSize() * 1.15f), T().dim,
                sub.c_str());
    ImGui::PopClipRect();
    return pressed;
}

void DrawMyHomebrew()
{
    const float s = g_viewport_mgr.m_scale;
    if (!g_hb.loaded && !XboxExplorerBusy()) {
        HomebrewRefresh();
    }
    if (DashButton(ICON_FA_ROTATE "  Refresh", T().glass, T().white,
                   ImVec2(0, 32 * s)) &&
        !XboxExplorerBusy()) {
        HomebrewRefresh();
    }
    ImGui::SameLine(0, 14 * s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (32 * s - ImGui::GetFontSize()) / 2);
    if (g_hb.preparing) {
        ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(320 * s, 0),
                           ("Getting " + g_hb.title + " ready...").c_str());
    } else if (g_hb.deleting) {
        ImGui::ProgressBar(XboxExplorerProgress(), ImVec2(320 * s, 0),
                           ("Deleting " + g_hb.title + "...").c_str());
    } else if (!g_hb.msg.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_hb.msg_err ? T().bad
                                                                       : T().dim),
                           "%s", g_hb.msg.c_str());
    } else {
        ImGui::TextDisabled("Pick one to play; right-click (or Y) to delete. It "
                            "runs from the DVD drive, so anything it saves to its "
                            "own folder isn't kept.");
    }
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 a = ImGui::GetCursorScreenPos();
    DashCard(ImGui::GetWindowDrawList(), a, ImVec2(a.x + avail.x, a.y + avail.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 16 * s));
    ImGui::BeginChild("##myhb", avail,
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened);
    if (!g_hb.err.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T().warn), "%s",
                           g_hb.err.c_str());
    } else if (g_hb.apps.empty()) {
        ImGui::TextDisabled("No homebrew on the Xbox hard drive yet. Apps in "
                            "E:\\Games, E:\\Apps or E:\\Emulators show up here.");
        ImGui::Dummy(ImVec2(0, 8 * s));
        if (DashButton(ICON_FA_DOWNLOAD "  Get homebrew from the store", T().primary,
                       T().on_primary, ImVec2(0, 38 * s))) {
            g_hb.view = 1;
        }
    }
    float inner = ImGui::GetContentRegionAvail().x;
    float gap = 14 * s;
    int cols = std::max(1, (int)((inner + gap) / (150 * s + gap)));
    float card_w = std::floor((inner - gap * (cols - 1)) / cols);
    for (size_t i = 0; i < g_hb.apps.size(); i++) {
        if (i % cols) {
            ImGui::SameLine(0, gap);
        }
        XboxHomebrewApp app = g_hb.apps[i]; // Play can refresh the list
        if (HomebrewCard(app, card_w)) {
            HomebrewPlay(app);
        }
        if (i % cols == (size_t)cols - 1) {
            ImGui::Dummy(ImVec2(0, 4 * s));
        }
    }

    // Options for one app (right-click or Y)
    if (g_hb.open_menu) {
        g_hb.open_menu = false;
        ImGui::OpenPopup("##hbmenu");
        ImGui::SetNextWindowPos(g_hb.menu_pos);
    }
    if (ImGui::BeginPopup("##hbmenu")) {
        const XboxHomebrewApp &m = g_hb.menu_app;
        ImGui::TextDisabled("%s", m.title.c_str());
        ImGui::Separator();
        bool busy = g_hb.preparing || g_hb.deleting || XboxExplorerBusy();
        if (ImGui::MenuItem(ICON_FA_PLAY "  Play", nullptr, false, !busy)) {
            HomebrewPlay(m);
        }
        if (ImGui::MenuItem(ICON_FA_HARD_DRIVE "  Show on the Xbox drive")) {
            SwitchTab(kTabDrive);
            XboxExplorerShowFolder(m.vol, m.dir);
        }
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete...", nullptr, false, !busy)) {
            g_hb.open_delete = true;
        }
        ImGui::EndPopup();
    }
    if (g_hb.open_delete) {
        g_hb.open_delete = false;
        ImGui::OpenPopup("##hbdelete");
    }
    if (ImGui::BeginPopupModal("##hbdelete", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const XboxHomebrewApp &m = g_hb.menu_app;
        ImGui::Text("Delete %s?", m.title.c_str());
        ImGui::TextDisabled("Removes %s and everything in it.",
                            HomebrewDisplayDir(m).c_str());
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextWrapped("This can't be undone. The Xbox pauses and restarts "
                           "to make the change, so save your game first.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button(ICON_FA_TRASH " Delete")) {
            ImGui::CloseCurrentPopup();
            HomebrewDelete(m);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            GameLibraryConsumeBack();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void DrawStoreBrowser()
{
    const float s = g_viewport_mgr.m_scale;
    StoreStart();
    ImGui::TextDisabled("XboxHomebrew.store, run by Team Resurgent. Games install "
                        "to E:\\Games, emulators to E:\\Emulators, the rest to "
                        "E:\\Apps.");
    ImGui::Dummy(ImVec2(0, 2 * s));

    // Search and categories
    ImGui::SetNextItemWidth(260 * s);
    if (ImGui::InputTextWithHint("##storesearch", ICON_FA_MAGNIFYING_GLASS " Search",
                                 g_store.search, sizeof(g_store.search))) {
        g_store.search_edit_t = ImGui::GetTime();
    }
    if (g_store.search_edit_t >= 0 && ImGui::GetTime() - g_store.search_edit_t > 0.4) {
        g_store.search_edit_t = -1;
        if (g_store.search_applied != g_store.search) {
            g_store.search_applied = g_store.search;
            StoreFetchApps(true);
        }
    }
    auto chip = [&](const std::string &name, const std::string &label) {
        bool on = g_store.category == name;
        ImGui::SameLine(0, 6 * s);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::ColorConvertU32ToFloat4(
                                                        Alpha(T().accent, 90))
                                                  : T().glass);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12 * s);
        if (ImGui::Button(label.c_str()) && !on) {
            g_store.category = name;
            StoreFetchApps(true);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    };
    chip("", "All");
    for (auto &c : g_store.cats) {
        chip(c.name, c.count ? string_format("%s  %d", c.name.c_str(), c.count)
                             : c.name);
    }
    ImGui::Dummy(ImVec2(0, 6 * s));

    // Grid, and the selected app beside it
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const StoreApp *sel = StoreSelected();
    float gap = 18 * s;
    float detail_w = sel ? std::min(460 * s, avail.x * 0.42f) : 0;
    float grid_w = avail.x - (sel ? detail_w + gap : 0);
    ImVec2 a = ImGui::GetCursorScreenPos();
    DashCard(ImGui::GetWindowDrawList(), a, ImVec2(a.x + grid_w, a.y + avail.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * s, 16 * s));
    ImGui::BeginChild("##storegrid", ImVec2(grid_w, avail.y),
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened);
    float inner = ImGui::GetContentRegionAvail().x;
    float card_gap = 14 * s;
    int cols = std::max(1, (int)((inner + card_gap) / (150 * s + card_gap)));
    float card_w = std::floor((inner - card_gap * (cols - 1)) / cols);
    for (size_t i = 0; i < g_store.apps.size(); i++) {
        if (i % cols) {
            ImGui::SameLine(0, card_gap);
        }
        if (StoreCard(g_store.apps[i], card_w)) {
            StoreSelect(g_store.apps[i].id);
        }
        if (i % cols == (size_t)cols - 1) {
            ImGui::Dummy(ImVec2(0, 4 * s));
        }
    }
    if (g_store.apps.size() % cols) {
        ImGui::Dummy(ImVec2(0, 4 * s));
    }
    if (g_store.loading) {
        ImGui::TextDisabled("Loading...");
    } else if (!g_store.err.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T().warn), "%s",
                           g_store.err.c_str());
        if (ImGui::Button("Try again")) {
            g_store.err.clear();
            if (g_store.cats.empty()) {
                g_store.started = false;
                StoreStart();
            } else {
                StoreFetchApps(g_store.apps.empty());
            }
        }
    } else if (g_store.apps.empty()) {
        ImGui::TextDisabled(g_store.search_applied.empty()
                                ? "Nothing here yet."
                                : "No apps match your search.");
    } else if (g_store.more) {
        // Also loads by itself when scrolled to the end
        if (DashButton("Load more", T().glass, T().white, ImVec2(160 * s, 34 * s)) ||
            (ImGui::GetScrollMaxY() > 0 &&
             ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40 * s)) {
            StoreFetchApps(false);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (sel) {
        ImGui::SameLine(0, gap);
        ImVec2 da = ImGui::GetCursorScreenPos();
        DashCard(ImGui::GetWindowDrawList(), da,
                 ImVec2(da.x + detail_w, da.y + avail.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * s, 16 * s));
        ImGui::BeginChild("##storedetail", ImVec2(detail_w, avail.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened);
        StoreApp copy = *sel; // the list can change while drawing
        DrawStoreDetails(copy, ImGui::GetContentRegionAvail().x);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
}

void DrawStoreTab()
{
    const float s = g_viewport_mgr.m_scale;
    if (!g_hb.loaded && !XboxExplorerBusy()) {
        HomebrewRefresh();
    }
    DashLabel("Homebrew");
    DashTitle("Play homebrew on your Xbox hard drive, or get more");
    ImGui::Dummy(ImVec2(0, 2 * s));
    auto seg = [&](int v, const std::string &label) {
        bool on = std::max(g_hb.view, 0) == v;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              on ? ImGui::ColorConvertU32ToFloat4(Alpha(T().accent, 90))
                                 : T().glass);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14 * s);
        if (ImGui::Button(label.c_str(), ImVec2(0, 30 * s))) {
            g_hb.view = v;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    };
    seg(0, string_format(ICON_FA_GAMEPAD "  Installed (%d)", (int)g_hb.apps.size()));
    ImGui::SameLine(0, 8 * s);
    seg(1, ICON_FA_DOWNLOAD "  Store");
    ImGui::Dummy(ImVec2(0, 4 * s));
    if (g_hb.view == 1) {
        DrawStoreBrowser();
    } else {
        DrawMyHomebrew();
    }
}

// --- Patches -------------------------------------------------------------------

void DrawDriveTab()
{
    DashLabel("Xbox hard drive");
    DashTitle("Files on the Xbox's C, E, F, X, Y and Z drives");
    ImGui::Dummy(ImVec2(0, 4 * g_viewport_mgr.m_scale));
    XboxExplorerDraw();
}

void DrawPatchesTab(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    DashLabel("Patches");
    DashTitle("Widescreen, 60 FPS, HD and fixes for your games");
    ImGui::TextDisabled("From Jay's Magic Patches collection on GitHub, plus any "
                        "patches you add yourself. Downloaded patches start "
                        "switched off.");
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::BeginDisabled(g_patchdl_running);
    if (DashButton(g_patchdl_running
                       ? ICON_FA_DOWNLOAD "  Downloading..."
                       : ICON_FA_DOWNLOAD "  Download patches for all games",
                   T().primary, T().on_primary, ImVec2(0, 38 * s))) {
        StartPatchDownload(0, false);
    }
    ImGui::SameLine(0, 12 * s);
    if (DashButton(ICON_FA_MAGNIFYING_GLASS "  Check for new patches", T().glass,
                   T().white, ImVec2(0, 38 * s))) {
        StartPatchDownload(0, true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 18 * s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Built-in list from %s", kJmpIndexDate);
    ImGui::Dummy(ImVec2(0, 12 * s));

    // Games with patches installed or available
    std::vector<LibraryEntry *> games;
    std::set<uint32_t> seen;
    std::vector<LibraryEntry *> items;
    SortedFiltered(items);
    for (LibraryEntry *e : items) {
        if (!e->valid || seen.count(e->title_id)) {
            continue;
        }
        auto it = g_patches.find(e->title_id);
        bool installed = it != g_patches.end() && !it->second.empty();
        if (installed || OnlinePatchCount(e->title_id)) {
            seen.insert(e->title_id);
            games.push_back(e);
        }
    }
    std::stable_sort(games.begin(), games.end(), [](LibraryEntry *x, LibraryEntry *y) {
        return EnabledPatchCount(x->title_id) > EnabledPatchCount(y->title_id);
    });
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 a = ImGui::GetCursorScreenPos();
    DashCard(ImGui::GetWindowDrawList(), a, ImVec2(a.x + avail.x, a.y + avail.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * s, 18 * s));
    ImGui::BeginChild("##patchgames", avail,
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened);
    if (games.empty()) {
        ImGui::TextDisabled("None of your games have patches yet.");
    }
    float w = ImGui::GetContentRegionAvail().x;
    float btn_w = 120 * s;
    for (LibraryEntry *e : games) {
        auto it = g_patches.find(e->title_id);
        int installed = it == g_patches.end() ? 0 : (int)it->second.size();
        int on = EnabledPatchCount(e->title_id);
        int online = OnlinePatchCount(e->title_id);
        std::string sub = FormatTitleId(e->title_id) + "  -  " +
                          string_format("%d installed, %d available online",
                                        installed, online);
        std::string note = on ? string_format(ICON_FA_WRENCH "  %d on", on)
                              : installed ? std::string("all off") : std::string();
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (DashRow(*e, sub, note, w - btn_w - 12 * s, tex_budget, false,
                    on ? T().accent : 0) ||
            ActivatedByNav()) {
            OpenPatchesFor(*e);
        }
        ImVec2 q = ImGui::GetItemRectMax();
        ImGui::SetCursorScreenPos(
            ImVec2(p.x + w - btn_w, p.y + (q.y - p.y - 32 * s) / 2));
        ImGui::PushID(e->path.c_str());
        if (DashButton("Manage", T().glass, T().white, ImVec2(btn_w, 32 * s))) {
            OpenPatchesFor(*e);
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(p.x, q.y + 4 * s));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// --- Settings ------------------------------------------------------------------

void DrawDashSettingsTab()
{
    const float s = g_viewport_mgr.m_scale;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float gap = 22 * s;
    bool two_col = avail.x > 900 * s;
    float col_w = two_col ? std::floor((avail.x - gap) / 2) : avail.x;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();

    auto card = [&](int col, const char *id, auto body) {
        ImVec2 ca(a.x + (two_col ? col * (col_w + gap) : 0), a.y);
        float h = avail.y;
        DashCard(dl, ca, ImVec2(ca.x + col_w, ca.y + h));
        ImGui::SetCursorScreenPos(ca);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * s, 22 * s));
        ImGui::BeginChild(id, ImVec2(col_w, h),
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened);
        ImGui::PushItemWidth(-FLT_MIN);
        body();
        ImGui::PopItemWidth();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    };
    auto label = [&](const char *t) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(t);
    };

    card(0, "##picture", [&] {
        DashLabel("Picture");
        DashTitle("Display and upscaling");
        label("Internal resolution");
        int scale = std::clamp(g_config.display.quality.surface_scale, 1, 10) - 1;
        if (ImGui::Combo("##scale", &scale,
                         "1x (native)\0" "2x\0" "3x\0" "4x\0" "5x\0" "6x\0" "7x\0"
                         "8x\0" "9x\0" "10x\0")) {
            nv2a_set_surface_scale_factor(scale + 1);
        }
        ImGui::TextDisabled("The heaviest setting. 1x-2x with FSR is fast and sharp.");
        label("Upscaling filter");
        ImGui::Combo("##filter", &g_config.display.filtering,
                     "Linear\0Nearest\0AMD FidelityFX FSR 1.0\0Bicubic (Catmull-Rom)\0"
                     "Lanczos\0Snapdragon GSR 1\0NVIDIA Image Scaling (NIS)\0"
                     "NVIDIA RTX Video (AI)\0");
        if (g_config.display.filtering == CONFIG_DISPLAY_FILTERING_RTX) {
            ImGui::TextWrapped("RTX Video: %s", RtxVideoStatus());
            if (DashButton("Check again", T().glass, T().white, ImVec2(0, 30 * s))) {
                RtxVideoRetry();
            }
        } else if (g_config.display.filtering >= 2) {
            label("Sharpening");
            ImGui::SliderFloat("##sharp", &g_config.display.fsr_sharpness, 0.0f, 1.0f,
                               "%.2f");
        }
        ImGui::Dummy(ImVec2(0, 8 * s));
        ImGui::Checkbox("Performance overlay (F9)",
                        &g_config.display.ui.show_perf_overlay);
        {
            static int clear_step = 0; // 0 button, 1 confirm, 2 done
            static std::string clear_msg;
            if (clear_step == 0) {
                if (DashButton(ICON_FA_TRASH "  Clear all shader caches...",
                               T().glass, T().white, ImVec2(0, 32 * s))) {
                    clear_step = 1;
                }
                ImGui::SetItemTooltip("For every game. Single games: right-click "
                                      "> Clear shader cache");
            } else if (clear_step == 1) {
                ImGui::TextWrapped("Delete every saved shader for every game? "
                                   "They're rebuilt as you play, with some "
                                   "stutter the first time.");
                if (DashButton("Clear all", T().primary, T().on_primary,
                               ImVec2(0, 30 * s))) {
                    ShaderCacheClearAll(&clear_msg);
                    clear_step = 2;
                }
                ImGui::SameLine();
                if (DashButton("Keep", T().glass, T().white, ImVec2(0, 30 * s))) {
                    clear_step = 0;
                }
            } else {
                ImGui::TextWrapped("%s", clear_msg.c_str());
            }
        }
        if (DashButton(ICON_FA_GAUGE_HIGH "  Performance capture...", T().glass,
                       T().white, ImVec2(0, 32 * s))) {
            PerfCaptureOpenWindow();
            game_library_window.is_open = false;
        }
        ImGui::SetItemTooltip("Record a slow or glitchy part of a game and see "
                              "why (Shift+F9 while playing)");

        ImGui::Dummy(ImVec2(0, 14 * s));
        DashLabel("Console");
        DashTitle("Video modes");
        uint32_t flags = 0;
        if (!xbox_eeprom_get_video_flags(&flags)) {
            ImGui::TextDisabled("Available once the console is running.");
        } else {
            static const struct {
                const char *label;
                uint32_t flag;
            } modes[] = { { "Widescreen (16:9)", XBOX_EEPROM_VIDEO_WIDESCREEN },
                          { "480p", XBOX_EEPROM_VIDEO_480P },
                          { "720p", XBOX_EEPROM_VIDEO_720P },
                          { "1080i", XBOX_EEPROM_VIDEO_1080I } };
            for (auto &m : modes) {
                bool v = (flags & m.flag) != 0;
                if (ImGui::Checkbox(m.label, &v)) {
                    uint32_t nf = v ? (flags | m.flag) : (flags & ~m.flag);
                    if (xbox_eeprom_set_video_flags(nf)) {
                        flags = nf;
                        xemu_queue_notification("Video modes saved. Games use "
                                                "them the next time they start.");
                    } else {
                        xemu_queue_error_message("Could not save the video modes "
                                                 "to the EEPROM file");
                    }
                }
            }
            ImGui::TextDisabled("Same as the Xbox dashboard's video settings. "
                                "Games read them when they start.");
        }
    });
    if (!two_col) {
        return; // the second card only fits side by side
    }
    card(1, "##general", [&] {
        DashLabel("General");
        DashTitle("Library and play");
        ImGui::Checkbox("Resume where you left off",
                        &g_config.general.library.auto_resume);
        ImGui::TextDisabled("Save a resume point whenever you leave a game.");
        ImGui::Checkbox("Show this dashboard when xemu starts",
                        &g_config.general.library.show_on_startup);
        ImGui::Checkbox("Show what I'm playing on Discord",
                        &g_config.general.discord_presence);
        ImGui::Checkbox("Animations and moving background",
                        &g_config.display.ui.use_animations);
        label("Library style");
        ImGui::Combo("##style", &g_config.general.library.style,
                     "Dashboard (tabs)\0Classic (library window)\0"
                     "Blades (sliding panels)\0Metro (live tiles)\0");
        label("Dashboard theme");
        ImGui::Combo("##theme", &g_config.general.library.theme,
                     "Neon (dark, cyan and magenta)\0Green (xemu)\0");
        label("Home screen covers");
        ImGui::Combo("##covers", &g_config.general.library.home_covers,
                     "3D game cases\0Flat\0");
        ImGui::TextDisabled("Your own 3D art: covers\\<title ID>-3d.png");
        ImGui::Dummy(ImVec2(0, 10 * s));
        if (DashButton(ICON_FA_FOLDER_OPEN "  Game folders...", T().glass, T().white,
                       ImVec2(0, 36 * s))) {
            g_open_folders_popup = true;
        }
        ImGui::SameLine();
        if (DashButton(ICON_FA_IMAGE "  Get covers", T().glass, T().white,
                       ImVec2(0, 36 * s))) {
            StartCoverDownload();
        }
        ImGui::Dummy(ImVec2(0, 14 * s));
        DashLabel("Everything else");
        DashTitle("All settings");
        ImGui::TextDisabled("Input, audio, network, system files, snapshots and "
                            "more.");
        ImGui::Dummy(ImVec2(0, 4 * s));
        if (DashButton(ICON_FA_GEAR "  Open all settings", T().primary, T().on_primary,
                       ImVec2(0, 38 * s))) {
            GameLibraryShowAllSettings();
        }
    });
}

// Classic style: the library window's own title row
void DrawClassicHeader()
{
    const float s = g_viewport_mgr.m_scale;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(T().accent), ICON_FA_GAMEPAD);
    ImGui::SameLine();
    ImGui::PushFont(FontOr(g_font_mgr.m_menu_font_medium));
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
    const char *style_label = "Dashboard style";
    float close_w = ImGui::CalcTextSize(close_label).x +
                    ImGui::GetStyle().FramePadding.x * 2;
    float style_w = ImGui::CalcTextSize(style_label).x +
                    ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w - style_w -
                    ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(style_label)) {
        g_config.general.library.style = CONFIG_GENERAL_LIBRARY_STYLE_DASHBOARD;
        SwitchTab(kTabHome);
    }
    ImGui::SetItemTooltip("Switch to the tabbed dashboard (also in Games > "
                          "Library Style)");
    ImGui::SameLine();
    if (ImGui::Button(close_label)) {
        game_library_window.is_open = false;
    }
    ImGui::SetItemTooltip("Esc or B");
    ImGui::Dummy(ImVec2(0, 2 * s));
}

// The full library: toolbar, grid or list, and the details panel
void DrawLibraryTab(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    auto folders = AllFolders();
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
        if (ClassicStyle()) {
            g_open_insignia_popup = true;
        } else {
            SwitchTab(kTabInsignia);
        }
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
                 "Sort: Title\0Sort: Last played\0Sort: Play time\0"
                 "Sort: Compatibility\0");
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
    if (!ClassicStyle()) { // Classic shows the count in its title row
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d games", (int)g_entries.size());
    }
    ImGui::Dummy(ImVec2(0, 4 * s));

    std::vector<LibraryEntry *> items;
    SortedFiltered(items);
    LibraryEntry *selected = FindEntry(g_selected_path);
    if (!selected && !items.empty()) {
        selected = items[0];
        g_selected_path = selected->path;
    }

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
        ImGui::PushStyleColor(ImGuiCol_ChildBg, T().neon
                                                    ? ImVec4(0.05f, 0.05f, 0.10f, 0.90f)
                                                    : ImVec4(0.11f, 0.11f, 0.12f, 0.88f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 14 * s));
        // Always reserve the scrollbar, so content arriving (save states,
        // screenshots) never changes the panel's width
        ImGui::BeginChild("##details", ImVec2(0, 0),
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawDetailsPanel(*selected, tex_budget);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

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
    g_home_focus_play = !ClassicStyle() && g_dash_tab == kTabHome;
    if (ImGui::GetCurrentContext()) {
        g_anim.tab_t = ImGui::GetTime(); // play the appear animation
    }
}

void GameLibraryWindow::OpenInsignia()
{
    Open();
    if (ClassicStyle()) {
        g_open_insignia_popup = true;
    } else {
        SwitchTab(kTabInsignia);
    }
}

void GameLibraryWindow::DownloadCovers()
{
    Open();
    StartCoverDownload();
}

void GameLibraryWindow::DownloadPatches()
{
    Open();
    StartPatchDownload(0, false);
}

void GameLibraryWindow::CheckNewPatches()
{
    Open();
    StartPatchDownload(0, true);
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
    FreeBox3DArt();
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
    StorePump();
    {
        // Note shader cache files against the game in the drive
        // (every 2 s: the library may still be loading at first)
        static uint64_t last_check = 0, last_flush = 0;
        if (SDL_GetTicks() - last_check > 2000) {
            last_check = SDL_GetTicks();
            LibraryEntry *cur = g_loaded ? CurrentGame() : nullptr;
            ShaderCacheSetTitle(cur ? cur->title_id : 0);
        }
        if (SDL_GetTicks() - last_flush > 10000) {
            last_flush = SDL_GetTicks();
            ShaderCacheFlush();
        }
    }
    if (g_compat_downloaded.exchange(false)) {
        LoadCompatCache();
    }
    if (!g_loaded) {
        g_loaded = true;
        atexit(GameLibraryShutdown);
        LoadLibrary();
        StartCompatRefresh();
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
    MergePatchResults();
    UploadDecodedImages(2);
    UpdateDiscordPresence();

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

// ---- Blades and Metro dashboard styles -------------------------------------
//
// Two more ways to lay out the same dashboard sections. Blades: full-height
// panels side by side; the current one opens wide and the rest stay as slim
// labelled strips you flip through. Metro: big lowercase section headers
// over a slim start page in the style of XBMC4Gamers (game list, faded art,
// details).

bool BladesStyle()
{
    return g_config.general.library.style == CONFIG_GENERAL_LIBRARY_STYLE_BLADES;
}

bool MetroStyle()
{
    return g_config.general.library.style == CONFIG_GENERAL_LIBRARY_STYLE_METRO;
}

void DrawTabContent(int tab, int &tex_budget)
{
    switch (tab) {
    case kTabHome:
        DrawHomeTab(tex_budget);
        break;
    case kTabInsignia:
        DrawInsigniaTab(tex_budget);
        break;
    case kTabMedia:
        DrawMediaTab(tex_budget);
        break;
    case kTabDrive:
        DrawDriveTab();
        break;
    case kTabStore:
        DrawStoreTab();
        break;
    case kTabPatches:
        DrawPatchesTab(tex_budget);
        break;
    case kTabSettings:
        DrawDashSettingsTab();
        break;
    default:
        DrawLibraryTab(tex_budget);
        break;
    }
}

// Text turned a quarter turn, reading from bottom to top, starting at pos
void AddTextVertical(ImDrawList *dl, ImFont *font, float size, ImVec2 pos,
                     ImU32 col, const char *text)
{
    dl->PushClipRectFullScreen();
    int start = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, col, text);
    for (int i = start; i < dl->VtxBuffer.Size; i++) {
        ImVec2 &p = dl->VtxBuffer[i].pos;
        float dx = p.x - pos.x, dy = p.y - pos.y;
        p = ImVec2(pos.x + dy, pos.y - dx);
    }
    dl->PopClipRect();
}

// Insignia status as a short word and its colour
void InsigniaWord(const char **word, ImU32 *col)
{
    CheckInsigniaStatus(false);
    *word = "insignia";
    *col = IM_COL32(150, 150, 150, 255);
    switch ((InsigniaStatus)(int)g_insignia_status) {
    case InsigniaStatus::Up:
        *word = "insignia online";
        *col = T().ok;
        break;
    case InsigniaStatus::Maintenance:
        *word = "insignia maintenance";
        *col = T().warn;
        break;
    case InsigniaStatus::Down:
        *word = "insignia offline";
        *col = T().bad;
        break;
    default:
        break;
    }
}

// Status, clock and close, right-aligned at (right, y)
void DrawStyleStatus(float right, float y)
{
    const float s = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const char *dvd = g_config.sys.files.dvd_path;
    const char *close_label = (dvd && dvd[0]) ? ICON_FA_XMARK "  Back to game"
                                              : ICON_FA_XMARK "  Close";
    float close_w = ImGui::CalcTextSize(close_label).x + 28 * s;
    float bh = 32 * s;
    ImGui::SetCursorScreenPos(ImVec2(right - close_w, y));
    if (DashButton(close_label, T().glass, T().white, ImVec2(close_w, bh))) {
        game_library_window.is_open = false;
    }
    char clock[32] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        strftime(clock, sizeof(clock), "%H:%M", tm);
    }
    float fh = ImGui::GetFontSize();
    float ty = y + (bh - fh) / 2;
    float x = right - close_w - 18 * s - ImGui::CalcTextSize(clock).x;
    dl->AddText(ImVec2(x, ty), IM_COL32(230, 230, 230, 255), clock);
    const char *word;
    ImU32 col;
    InsigniaWord(&word, &col);
    float ww = ImGui::CalcTextSize(word).x;
    x -= 22 * s + ww;
    dl->AddCircleFilled(ImVec2(x - 10 * s, ty + fh / 2), 4.5f * s, col, 16);
    dl->AddText(ImVec2(x, ty), col, word);
}

// Section colours shared by Blades and Metro
ImU32 SectionColor(int tab)
{
    static const ImU32 green[kTabCount] = {
        IM_COL32(86, 178, 22, 255),  IM_COL32(32, 132, 72, 255),
        IM_COL32(0, 150, 136, 255),  IM_COL32(52, 120, 200, 255),
        IM_COL32(110, 90, 190, 255), IM_COL32(196, 64, 96, 255),
        IM_COL32(214, 120, 30, 255), IM_COL32(96, 110, 104, 255),
    };
    static const ImU32 neon[kTabCount] = {
        IM_COL32(0, 200, 230, 255),  IM_COL32(40, 110, 255, 255),
        IM_COL32(57, 210, 130, 255), IM_COL32(150, 70, 255, 255),
        IM_COL32(255, 46, 136, 255), IM_COL32(255, 90, 60, 255),
        IM_COL32(255, 160, 0, 255),  IM_COL32(110, 116, 150, 255),
    };
    int i = tab < 0 ? 0 : (tab >= kTabCount ? kTabCount - 1 : tab);
    return (T().neon ? neon : green)[i];
}

// Ease a value toward a target, frame-rate independent
float Approach(float cur, float target, float speed)
{
    if (!Animations()) {
        return target;
    }
    float k = 1.0f - expf(-speed * ImGui::GetIO().DeltaTime);
    float v = cur + (target - cur) * k;
    return fabsf(target - v) < 0.5f ? target : v;
}

// ---- Blades ----

void DrawBladesFrame(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImVec2 wb(wp.x + ws.x, wp.y + ws.y);

    // Backdrop: soft vertical light behind the panels
    ImU32 glow = SectionColor(g_dash_tab);
    dl->AddRectFilledMultiColor(wp, wb, IM_COL32(10, 12, 16, 200),
                                IM_COL32(10, 12, 16, 200), IM_COL32(4, 5, 8, 220),
                                IM_COL32(4, 5, 8, 220));
    for (int i = 0; i < 5; i++) {
        dl->AddCircleFilled(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 1.1f),
                            ws.x * (0.25f + i * 0.08f), Alpha(glow, 6), 64);
    }
    if (Animations()) {
        DrawDashMotion(dl, wp, wb);
    }

    // Header: brand on the left, status on the right
    const float pad = 20 * s, head_h = 54 * s;
    ImFont *big = FontOr(g_font_mgr.m_menu_font_medium);
    dl->AddText(big, big->FontSize, ImVec2(wp.x + pad, wp.y + 14 * s),
                IM_COL32(235, 235, 235, 255), "xemu");
    DrawStyleStatus(wb.x - pad, wp.y + 12 * s);

    // Blade positions: strips on either side of the open blade
    const float hints_h = ImGui::GetFontSize() * 2.2f;
    ImVec2 a(wp.x + pad, wp.y + head_h), b(wb.x - pad, wb.y - hints_h - 4 * s);
    const float sw = 46 * s, gap = 6 * s;
    static float pos_x[kTabCount];
    static bool init = false;
    float target[kTabCount];
    for (int i = 0; i < kTabCount; i++) {
        if (i <= g_dash_tab) {
            target[i] = a.x + i * (sw + gap);
        } else {
            target[i] = b.x - (kTabCount - i) * (sw + gap) + gap;
        }
        pos_x[i] = init ? Approach(pos_x[i], target[i], 14.0f) : target[i];
    }
    init = true;

    // Strips (all but the open blade)
    ImFont *label_font = FontOr(g_font_mgr.m_menu_font_small);
    for (int i = 0; i < kTabCount; i++) {
        if (i == g_dash_tab) {
            continue;
        }
        float x0 = pos_x[i];
        ImVec2 p(x0, a.y), q(x0 + sw, b.y);
        ImU32 col = SectionColor(i);
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##blade", ImVec2(sw, b.y - a.y));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ShapeFill(dl, p, q, IM_COL32(18, 20, 26, hov ? 235 : 210), 10 * s);
        dl->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + (q.y - p.y) * 0.5f),
                                    Alpha(col, hov ? 90 : 60), Alpha(col, hov ? 90 : 60),
                                    Alpha(col, 0), Alpha(col, 0));
        ShapeStroke(dl, p, q, Alpha(col, hov ? 200 : 120), 10 * s, 1.0f * s);
        dl->AddRectFilled(ImVec2(p.x + 6 * s, p.y + 10 * s),
                          ImVec2(p.x + 9 * s, p.y + 40 * s), col);
        std::string name = kTabNames[i];
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        float fs = label_font->FontSize;
        AddTextVertical(dl, label_font, fs,
                        ImVec2(p.x + (sw - fs) / 2, q.y - 16 * s),
                        IM_COL32(235, 235, 235, hov ? 255 : 200), name.c_str());
        if (clicked) {
            SwitchTab(i);
        }
    }

    // The open blade
    float ox0 = pos_x[g_dash_tab] + (g_dash_tab > 0 ? 0 : 0);
    float ox1 = g_dash_tab + 1 < kTabCount ? pos_x[g_dash_tab + 1] - gap : b.x;
    ImVec2 p(ox0, a.y), q(ox1, b.y);
    ImU32 col = SectionColor(g_dash_tab);
    ShapeFill(dl, p, q, IM_COL32(14, 16, 22, 238), 12 * s);
    dl->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + 90 * s), Alpha(col, 70),
                                Alpha(col, 20), Alpha(col, 0), Alpha(col, 0));
    ShapeStroke(dl, p, q, Alpha(col, 170), 12 * s, 1.2f * s);
    dl->AddRectFilled(ImVec2(p.x, p.y + 12 * s), ImVec2(p.x + 4 * s, q.y - 12 * s),
                      col);
    std::string title = kTabNames[g_dash_tab];
    std::transform(title.begin(), title.end(), title.begin(), ::tolower);
    ImFont *tf = FontOr(g_font_mgr.m_menu_font);
    dl->AddText(tf, tf->FontSize, ImVec2(p.x + 22 * s, p.y + 10 * s),
                IM_COL32(245, 245, 245, 255), title.c_str());

    // Content slides in with the blade
    float slide = (1.0f - EaseSince(g_anim.tab_t, 0.30f)) * 30 * s;
    float ty = p.y + tf->FontSize + 22 * s;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 22 * s + slide, ty));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##bladecontent",
                      ImVec2(std::max(10.0f, q.x - p.x - 40 * s),
                             std::max(10.0f, q.y - ty - 14 * s)),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    DrawTabContent(g_dash_tab, tex_budget);
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, wb.y - hints_h + 4 * s));
    DrawDashHints(ImGui::GetCursorScreenPos(), ws.x - 2 * pad);
}

// ---- Metro ----

// Metro start page, in the slim style of XBMC4Gamers: a text list of games
// on the left, the selected game's art faded across the background, and its
// cover and details on the right
void DrawMetroStart(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Recently played first, then the rest of the library
    std::vector<LibraryEntry *> games;
    std::set<std::string> seen;
    for (auto &r : game_library_window.GetRecentGames(12)) {
        LibraryEntry *e = FindEntry(r.path);
        if (e && seen.insert(e->path).second) {
            games.push_back(e);
        }
    }
    int recent_n = (int)games.size();
    std::vector<LibraryEntry *> all;
    SortedFiltered(all);
    for (LibraryEntry *e : all) {
        if (seen.insert(e->path).second) {
            games.push_back(e);
        }
    }
    if (games.empty()) {
        ImGui::TextDisabled("No games yet.");
        if (DashButton(ICON_FA_FOLDER_OPEN "  Add a games folder", T().glass,
                       T().white, ImVec2(0, 34 * s))) {
            AddGameFolder();
        }
        return;
    }
    LibraryEntry *cur = FindEntry(g_home_path);
    if (!cur) {
        cur = games[0];
        g_home_path = cur->path;
    }

    // Background art: the selected game's cover, wide and faded
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 ra(o.x + avail.x * 0.36f, o.y), rb(o.x + avail.x, o.y + avail.y);
    EnsureTexture(*cur, tex_budget);
    if (cur->tex && cur->tex_w > 0) {
        float ta = (float)cur->tex_w / cur->tex_h, aa = (rb.x - ra.x) / (rb.y - ra.y);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ta < aa) { // crop top and bottom
            float v = ta / aa;
            uv0.y = (1 - v) * 0.3f;
            uv1.y = uv0.y + v;
        }
        dl->AddImage((ImTextureID)(intptr_t)cur->tex, ra, rb, uv0, uv1,
                     IM_COL32(255, 255, 255, 46));
        // Fade into the list on the left and the bar at the bottom
        ImU32 bg = T().neon ? IM_COL32(8, 8, 14, 255) : IM_COL32(10, 14, 12, 255);
        dl->AddRectFilledMultiColor(ra, ImVec2(ra.x + (rb.x - ra.x) * 0.45f, rb.y), bg,
                                    Alpha(bg, 0), Alpha(bg, 0), bg);
        dl->AddRectFilledMultiColor(ImVec2(ra.x, rb.y - 80 * s), rb, Alpha(bg, 0),
                                    Alpha(bg, 0), bg, bg);
    }

    // Game list
    float list_w = std::floor(avail.x * 0.34f);
    ImFont *row_font = FontOr(g_font_mgr.m_menu_font_small);
    float row_h = std::floor(row_font->FontSize * 1.55f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##metrolist", ImVec2(list_w, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleColor();
    ImDrawList *ldl = ImGui::GetWindowDrawList();
    ImU32 accent = SectionColor(kTabHome);
    for (int i = 0; i < (int)games.size(); i++) {
        LibraryEntry *e = games[i];
        if (i == recent_n && recent_n > 0) {
            ImGui::Dummy(ImVec2(0, 6 * s));
            ImGui::TextDisabled("ALL GAMES");
        } else if (i == 0 && recent_n > 0) {
            ImGui::TextDisabled("RECENTLY PLAYED");
        }
        ImGui::PushID(e->path.c_str());
        bool selected = e == cur;
        if (selected && g_home_focus_play) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
            ImGui::SetScrollHereY(0.3f);
            g_home_focus_play = false;
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, row_h),
                                              ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered();
        bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (focus || (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
            g_home_path = e->path;
        }
        if (clicked && (ImGui::GetIO().NavVisible ||
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            game_library_window.Launch(e->path);
        }
        if (ImGui::IsItemFocused() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) {
            g_stats[e->path].favorite = !g_stats[e->path].favorite;
            g_dirty = true;
        }
        if (selected) {
            ldl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + row_h),
                                         Alpha(accent, 70), Alpha(accent, 0),
                                         Alpha(accent, 0), Alpha(accent, 70));
            ldl->AddRectFilled(p, ImVec2(p.x + 3 * s, p.y + row_h), accent);
        } else if (hov) {
            ldl->AddRectFilled(p, ImVec2(p.x + w, p.y + row_h), IM_COL32(255, 255, 255, 12));
        }
        std::string name = e->DisplayName();
        ImVec4 clip(p.x, p.y, p.x + w - 34 * s, p.y + row_h);
        ldl->AddText(row_font, row_font->FontSize,
                     ImVec2(p.x + 14 * s, p.y + (row_h - row_font->FontSize) / 2),
                     selected ? IM_COL32_WHITE : IM_COL32(200, 200, 205, 255),
                     name.c_str(), NULL, 0, &clip);
        if (g_stats[e->path].favorite) {
            ldl->AddText(ImVec2(p.x + w - 24 * s, p.y + (row_h - ImGui::GetFontSize()) / 2),
                         IM_COL32(255, 200, 0, 255), ICON_FA_STAR);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // Details of the selected game
    float dx = o.x + list_w + 40 * s;
    float dw = o.x + avail.x - dx;
    float cover_h = std::min(avail.y * 0.62f, dw * 0.5f);
    float cover_w = cover_h * (Home3DCovers() ? 0.86f : 0.72f);
    ImVec2 ca(o.x + avail.x - cover_w - 10 * s, o.y + 10 * s);
    if (Home3DCovers()) {
        DrawCover3D(dl, ca, ImVec2(ca.x + cover_w, ca.y + cover_h), *cur, 24.0f);
    } else {
        DrawArtwork(dl, ca, ImVec2(ca.x + cover_w, ca.y + cover_h), *cur, 4 * s);
    }
    float text_w = ca.x - dx - 20 * s;
    ImFont *tf = FontOr(g_font_mgr.m_menu_font);
    float y = o.y + 16 * s;
    std::string title = cur->DisplayName();
    ImVec4 tclip(dx, y, dx + text_w, y + tf->FontSize * 3);
    dl->AddText(tf, tf->FontSize, ImVec2(dx, y), IM_COL32_WHITE, title.c_str(),
                NULL, text_w, &tclip);
    ImVec2 ts = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, text_w, title.c_str());
    y += ts.y + 10 * s;
    const GameStats &st = g_stats[cur->path];
    float fh = ImGui::GetFontSize();
    auto line = [&](const char *label, const std::string &value) {
        dl->AddText(ImVec2(dx, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        dl->AddText(ImVec2(dx + 110 * s, y), IM_COL32(225, 225, 230, 255),
                    value.c_str());
        y += fh + 6 * s;
    };
    if (cur->valid) {
        line("Title ID", FormatTitleId(cur->title_id));
        line("Region", FormatRegion(cur->region));
    }
    line("Play time", FormatPlayTime(st.play_seconds));
    line("Last played", FormatLastPlayed(st.last_played));
    CompatStatus mcs = CompatFor(*cur);
    if (mcs != CompatStatus::None) {
        dl->AddText(ImVec2(dx, y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    "Compatibility");
        dl->AddCircleFilled(ImVec2(dx + 110 * s + fh * 0.3f, y + fh * 0.55f),
                            fh * 0.28f, CompatColor(mcs), 16);
        dl->AddText(ImVec2(dx + 110 * s + fh * 0.8f, y), CompatColor(mcs),
                    CompatName(mcs));
        y += fh + 6 * s;
    }
    if (SupportsOnline(*cur)) {
        line("Online", "Plays on Insignia");
    }
    int states = (int)SnapshotsFor(*cur).size();
    if (states) {
        line("Save states", string_format("%d", states));
    }
    y += 10 * s;
    ImGui::SetCursorScreenPos(ImVec2(dx, y));
    if (DashButton(ICON_FA_PLAY "  Play", T().primary, T().on_primary,
                   ImVec2(120 * s, 34 * s))) {
        game_library_window.Launch(cur->path);
    }
    ImGui::SameLine(0, 10 * s);
    if (DashButton(ICON_FA_WRENCH "  Patches", T().glass, T().white,
                   ImVec2(0, 34 * s))) {
        OpenPatchesFor(*cur);
    }
}

void DrawMetroFrame(int &tex_budget)
{
    const float s = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImVec2 wb(wp.x + ws.x, wp.y + ws.y);
    // Flat backdrop
    dl->AddRectFilled(wp, wb, T().neon ? IM_COL32(8, 8, 14, 236)
                                       : IM_COL32(10, 14, 12, 236));
    const float pad = 28 * s;
    DrawStyleStatus(wb.x - pad, wp.y + 12 * s);

    // Section headers: big lowercase words that slide so the current one
    // sits at the left
    ImFont *hf = FontOr(g_font_mgr.m_menu_font);
    float hsz = hf->FontSize;
    float hgap = 26 * s;
    float xs[kTabCount], x = 0;
    std::string names[kTabCount];
    for (int i = 0; i < kTabCount; i++) {
        names[i] = kTabNames[i];
        std::transform(names[i].begin(), names[i].end(), names[i].begin(),
                       ::tolower);
        xs[i] = x;
        x += hf->CalcTextSizeA(hsz, FLT_MAX, 0, names[i].c_str()).x + hgap;
    }
    static float off = 0;
    off = Approach(off, -xs[g_dash_tab], 12.0f);
    float hy = wp.y + 44 * s;
    float hx0 = wp.x + pad;
    dl->PushClipRect(ImVec2(wp.x, hy - 4 * s), ImVec2(wb.x, hy + hsz + 6 * s), true);
    for (int i = 0; i < kTabCount; i++) {
        ImVec2 p(hx0 + off + xs[i], hy);
        float w = hf->CalcTextSizeA(hsz, FLT_MAX, 0, names[i].c_str()).x;
        if (p.x + w < wp.x || p.x > wb.x) {
            continue;
        }
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##pivot", ImVec2(w, hsz));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        int alpha = i == g_dash_tab ? 255 : (hov ? 150 : 80);
        dl->AddText(hf, hsz, p, IM_COL32(255, 255, 255, alpha), names[i].c_str());
        if (clicked) {
            SwitchTab(i);
        }
    }
    dl->PopClipRect();

    // Page content, sliding in from the right
    const float hints_h = ImGui::GetFontSize() * 2.2f;
    float slide = (1.0f - EaseSince(g_anim.tab_t, 0.30f)) * 60 * s;
    float cy = hy + hsz + 18 * s;
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad + slide, cy));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##metrocontent",
                      ImVec2(ws.x - 2 * pad, std::max(10.0f, wb.y - cy - hints_h - 6 * s)),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    if (g_dash_tab == kTabHome) {
        DrawMetroStart(tex_budget);
    } else {
        DrawTabContent(g_dash_tab, tex_budget);
    }
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, wb.y - hints_h + 4 * s));
    DrawDashHints(ImGui::GetCursorScreenPos(), ws.x - 2 * pad);
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
    // Dashboard colors (popped at End): Green has glassy controls with green
    // accents; Neon has dark controls with cyan outlines
    struct ThemeColor {
        ImGuiCol idx;
        ImVec4 col;
    };
    static const ThemeColor green_theme[] = {
        { ImGuiCol_Button, ImVec4(1, 1, 1, 0.08f) },
        { ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f) },
        { ImGuiCol_ButtonActive, ImVec4(0.384f, 0.792f, 0.075f, 0.55f) },
        { ImGuiCol_FrameBg, ImVec4(1, 1, 1, 0.07f) },
        { ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.11f) },
        { ImGuiCol_FrameBgActive, ImVec4(1, 1, 1, 0.15f) },
        { ImGuiCol_CheckMark, ImVec4(0.384f, 0.792f, 0.075f, 1) },
        { ImGuiCol_SliderGrab, ImVec4(0.384f, 0.792f, 0.075f, 1) },
        { ImGuiCol_SliderGrabActive, ImVec4(0.55f, 0.90f, 0.25f, 1) },
        { ImGuiCol_Header, ImVec4(0.384f, 0.792f, 0.075f, 0.22f) },
        { ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.08f) },
        { ImGuiCol_HeaderActive, ImVec4(0.384f, 0.792f, 0.075f, 0.35f) },
        { ImGuiCol_PopupBg, ImVec4(0.07f, 0.09f, 0.08f, 0.98f) },
        { ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0) },
        { ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.16f) },
        { ImGuiCol_Separator, ImVec4(1, 1, 1, 0.09f) },
        { ImGuiCol_NavCursor, ImVec4(0.384f, 0.792f, 0.075f, 1) },
        { ImGuiCol_TextSelectedBg, ImVec4(0.384f, 0.792f, 0.075f, 0.35f) },
    };
    static const ThemeColor neon_theme[] = {
        { ImGuiCol_Text, ImVec4(0.89f, 0.91f, 0.96f, 1) },
        { ImGuiCol_TextDisabled, ImVec4(0.46f, 0.50f, 0.64f, 1) },
        { ImGuiCol_Button, ImVec4(0, 0.898f, 1, 0.06f) },
        { ImGuiCol_ButtonHovered, ImVec4(0, 0.898f, 1, 0.18f) },
        { ImGuiCol_ButtonActive, ImVec4(0, 0.898f, 1, 0.34f) },
        { ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.10f, 0.85f) },
        { ImGuiCol_FrameBgHovered, ImVec4(0, 0.898f, 1, 0.10f) },
        { ImGuiCol_FrameBgActive, ImVec4(0, 0.898f, 1, 0.18f) },
        { ImGuiCol_Border, ImVec4(0, 0.898f, 1, 0.22f) },
        { ImGuiCol_CheckMark, ImVec4(0, 0.898f, 1, 1) },
        { ImGuiCol_SliderGrab, ImVec4(0, 0.898f, 1, 1) },
        { ImGuiCol_SliderGrabActive, ImVec4(1, 0.18f, 0.53f, 1) },
        { ImGuiCol_Header, ImVec4(0, 0.898f, 1, 0.16f) },
        { ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.06f) },
        { ImGuiCol_HeaderActive, ImVec4(0, 0.898f, 1, 0.28f) },
        { ImGuiCol_PopupBg, ImVec4(0.04f, 0.04f, 0.08f, 0.98f) },
        { ImGuiCol_TitleBgActive, ImVec4(0.08f, 0.06f, 0.16f, 1) },
        { ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0) },
        { ImGuiCol_ScrollbarGrab, ImVec4(0, 0.898f, 1, 0.22f) },
        { ImGuiCol_ScrollbarGrabHovered, ImVec4(0, 0.898f, 1, 0.40f) },
        { ImGuiCol_Separator, ImVec4(0, 0.898f, 1, 0.16f) },
        { ImGuiCol_NavCursor, ImVec4(0, 0.898f, 1, 1) },
        { ImGuiCol_TextSelectedBg, ImVec4(0, 0.898f, 1, 0.30f) },
        { ImGuiCol_ModalWindowDimBg, ImVec4(0.02f, 0.01f, 0.05f, 0.60f) },
    };
    const bool classic = ClassicStyle();
    const ThemeColor *theme = T().neon ? neon_theme : green_theme;
    const int theme_count =
        classic ? 0
        : T().neon ? (int)(sizeof(neon_theme) / sizeof(neon_theme[0]))
                   : (int)(sizeof(green_theme) / sizeof(green_theme[0]));
    for (int i = 0; i < theme_count; i++) {
        ImGui::PushStyleColor(theme[i].idx, theme[i].col);
    }
    // Slightly see-through so the boot animation or game shows behind it
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          classic    ? ImVec4(0.06f, 0.06f, 0.07f, 0.80f)
                          : T().neon ? ImVec4(0.025f, 0.022f, 0.05f, 0.86f)
                                     : ImVec4(0.04f, 0.06f, 0.05f, 0.82f));
    bool visible = ImGui::Begin(
        "###Game Library", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    // Neon: square-ish controls with thin outlines (popped at End)
    const int theme_vars = (!classic && T().neon) ? 3 : 0;
    if (theme_vars) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2 * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2 * s);
    }
    if (!visible) {
        ImGui::End();
        ImGui::PopStyleColor(theme_count);
        ImGui::PopStyleVar(theme_vars);
        ImGui::PopStyleVar();
        return;
    }

    const bool own_frame = BladesStyle() || MetroStyle();
    if (classic) {
        DrawClassicHeader();
    } else if (!own_frame) {
        // Dashboard: background, top bar with tabs (LB/RB to switch)
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        DrawDashBackground(ImGui::GetWindowDrawList(), wp,
                           ImVec2(wp.x + ws.x, wp.y + ws.y));
        DrawDashTopBar();
    }
    if (!classic && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::IsAnyItemActive() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
            (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageUp, false))) {
            SwitchTab(g_dash_tab - 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
            (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_PageDown, false))) {
            SwitchTab(g_dash_tab + 1);
        }
    }

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
        ImGui::PopStyleColor(theme_count);
        ImGui::PopStyleVar(theme_vars);
        ImGui::PopStyleVar();
        return;
    }

    int tex_budget = 6; // decode at most a few images per frame
    if (classic) {
        DrawLibraryTab(tex_budget);
    } else if (BladesStyle()) {
        DrawBladesFrame(tex_budget);
    } else if (MetroStyle()) {
        DrawMetroFrame(tex_budget);
    } else {
        const float hints_h = ImGui::GetFontSize() * 2.2f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##tabcontent", ImVec2(0, -hints_h), ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        switch (g_dash_tab) {
        case kTabHome:
            DrawHomeTab(tex_budget);
            break;
        case kTabInsignia:
            DrawInsigniaTab(tex_budget);
            break;
        case kTabMedia:
            DrawMediaTab(tex_budget);
            break;
        case kTabDrive:
            DrawDriveTab();
            break;
        case kTabStore:
            DrawStoreTab();
            break;
        case kTabPatches:
            DrawPatchesTab(tex_budget);
            break;
        case kTabSettings:
            DrawDashSettingsTab();
            break;
        default:
            DrawLibraryTab(tex_budget);
            break;
        }
        // Appear: the content fades in under a sweeping scan line. Drawn on
        // the foreground list so it also covers nested panels.
        float appear = EaseSince(g_anim.tab_t, 0.32f);
        if (appear < 1.0f) {
            ImVec2 ca = ImGui::GetWindowPos();
            ImVec2 cb(ca.x + ImGui::GetWindowWidth(), ca.y + ImGui::GetWindowHeight());
            ImDrawList *fg = ImGui::GetForegroundDrawList();
            fg->PushClipRect(ca, cb, false);
            fg->AddRectFilled(ca, cb, IM_COL32(4, 4, 10, (int)((1 - appear) * 230)));
            float y = ca.y + (cb.y - ca.y) * appear;
            ImU32 line = T().accent;
            fg->AddRectFilled(ImVec2(ca.x, y - 1 * s), ImVec2(cb.x, y + 1 * s),
                              Alpha(line, (int)(200 * (1 - appear))));
            fg->AddRectFilledMultiColor(ImVec2(ca.x, y - 24 * s), ImVec2(cb.x, y),
                                        Alpha(line, 0), Alpha(line, 0),
                                        Alpha(line, (int)(40 * (1 - appear))),
                                        Alpha(line, (int)(40 * (1 - appear))));
            fg->PopClipRect();
        }
        ImGui::EndChild();
        DrawDashHints(ImVec2(ImGui::GetCursorScreenPos().x,
                             ImGui::GetCursorScreenPos().y + 6 * s),
                      ImGui::GetContentRegionAvail().x);
    }

    DrawSettingsPopup();
    DrawFoldersPopup();
    DrawPatchesPopup();
    DrawInsigniaPopup();
    DrawResumeWarningPopup();

    // Esc / B closes the library when nothing else wants the key
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        !ImGui::IsAnyItemActive() &&
        g_back_consumed_frame != ImGui::GetFrameCount() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
         ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        is_open = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(theme_count);
    ImGui::PopStyleVar(theme_vars);
    ImGui::PopStyleVar();
}

std::string GameLibraryTitleName(uint32_t title_id)
{
    // Your library first (your own names and regions), then xemu's list
    for (auto &e : g_entries) {
        if (e.valid && e.title_id == title_id) {
            return e.DisplayName();
        }
    }
    auto it = g_compat_names.find(title_id);
    if (it == g_compat_names.end()) {
        it = g_compat_names.find(CompatListedId(title_id));
    }
    if (it != g_compat_names.end()) {
        return it->second;
    }
    switch (title_id) {
    case 0xFFFE0000: return "Xbox Dashboard";
    case 0xFFFF0051: return "Xbox Dashboard (system)";
    default: return "";
    }
}

std::string GameLibraryCurrentTitle(void)
{
    if (LibraryEntry *e = CurrentGame()) {
        return e->DisplayName();
    }
    const char *dvd = g_config.sys.files.dvd_path;
    return dvd && dvd[0] ? fs::u8path(dvd).stem().u8string() : std::string();
}

void GameLibraryConsumeBack(void)
{
    g_back_consumed_frame = ImGui::GetFrameCount();
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
    ShaderCacheFlush();
    g_scan_cancel = true;
    if (g_scan_thread.joinable()) {
        g_scan_thread.join();
    }
    if (g_cover_thread.joinable()) {
        g_cover_thread.join();
    }
    if (g_patchdl_thread.joinable()) {
        g_patchdl_thread.join();
    }
    DiscordPresenceStop();
    g_preview->Close(false);
    StopImageWorker();
    if (g_insignia_thread.joinable()) {
        g_insignia_thread.join();
    }
    if (g_compat_thread.joinable()) {
        g_compat_thread.join();
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
