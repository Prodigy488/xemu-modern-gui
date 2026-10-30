//
// xemu User Interface - per-game performance settings and safe mode
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Profiles live in <settings folder>/game_perf.txt, one line per game:
//   <title ID, 8 hex digits> cpu=-1 gpu=-1 seed=-1 real=-1 hz=0 safe=0
// While a game runs, <settings folder>/running_game.txt names it and says
// whether the boosts were on. A clean exit removes it, so finding it at
// start-up means the last run died (a crash, an assert, a kill).
//
#include "game-perf.hh"
#include "game-library-scan.hh"
#include "i18n.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <glib.h>
#include <glib/gstdio.h>

#include "../xemu-settings.h"
#include "../xemu-notifications.h"

extern "C" void nv2a_set_vblank_hz(int hz);
extern "C" int xemu_x87_exact;
extern "C" int xemu_x87_title_exact;
extern "C" void xemu_x87_set_mode(int exact);
extern "C" char xemu_vk_bench_msg[240];

namespace
{
struct Eff {
    bool cpu, gpu, seed, real;
    int hz;
};

std::map<uint32_t, GamePerfProfile> g_profiles;
bool g_loaded = false;

uint32_t g_cur_id = 0;
std::string g_cur_name;
bool g_cur_ok = false;

Eff g_base;       // the user's own values
Eff g_applied;    // what is set now
bool g_started = false;
bool g_run_cpu = false, g_run_gpu = false; // read once at start-up
std::string g_notice;

std::string File(const char *name)
{
    const char *base = xemu_settings_get_base_path();
    return std::string(base ? base : "") + name;
}

void Load()
{
    if (g_loaded) {
        return;
    }
    g_loaded = true;
    FILE *f = g_fopen(File("game_perf.txt").c_str(), "rb");
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned id = 0;
        GamePerfProfile p;
        int safe = 0;
        if (sscanf(line, "%x cpu=%d gpu=%d seed=%d real=%d hz=%d safe=%d", &id,
                   &p.cpu, &p.gpu, &p.seed, &p.real, &p.hz, &safe) == 7) {
            p.safe = safe != 0;
            g_profiles[id] = p;
        }
    }
    fclose(f);
}

void Save()
{
    FILE *f = g_fopen(File("game_perf.txt").c_str(), "wb");
    if (!f) {
        return;
    }
    for (auto &[id, p] : g_profiles) {
        if (p.cpu < 0 && p.gpu < 0 && p.seed < 0 && p.real < 0 && p.hz == 0 &&
            !p.safe) {
            continue;
        }
        fprintf(f, "%08X cpu=%d gpu=%d seed=%d real=%d hz=%d safe=%d\n", id,
                p.cpu, p.gpu, p.seed, p.real, p.hz, p.safe ? 1 : 0);
    }
    fclose(f);
}

Eff Resolve(const GamePerfProfile *p)
{
    Eff e = g_base;
    if (!p) {
        return e;
    }
    if (p->safe) {
        e.cpu = false;
        e.gpu = false;
        e.hz = 60;
    } else {
        if (p->cpu >= 0) e.cpu = p->cpu != 0;
        if (p->gpu >= 0) e.gpu = p->gpu != 0;
        if (p->hz > 0) e.hz = p->hz;
    }
    if (p->seed >= 0) e.seed = p->seed != 0;
    if (p->real >= 0) e.real = p->real != 0;
    return e;
}

void SetLive(const Eff &e)
{
    g_config.perf.shader_seeding = e.seed;
    g_config.perf.real_hw_speed = e.real;
    if (e.hz != g_applied.hz) {
        nv2a_set_vblank_hz(e.hz);
    }
    g_applied.seed = e.seed;
    g_applied.real = e.real;
    g_applied.hz = e.hz;
}

// What this run started with, for CPU and GPU boost, goes into the config
// before the emulator reads it
void SetStartup(const Eff &e)
{
    g_config.perf.native_sse = e.cpu;
    g_config.perf.optimizations = e.gpu;
    g_config.perf.shader_seeding = e.seed;
    g_config.perf.real_hw_speed = e.real;
    g_config.perf.vblank_hz = e.hz;
    g_applied = e;
    g_run_cpu = e.cpu;
    g_run_gpu = e.gpu;
}

void WriteMarker(uint32_t id)
{
    FILE *f = g_fopen(File("running_game.txt").c_str(), "wb");
    if (!f) {
        return;
    }
    fprintf(f, "%08X boosts=%d\n", id, (g_run_cpu || g_run_gpu) ? 1 : 0);
    fclose(f);
}

void ClearMarker()
{
    g_remove(File("running_game.txt").c_str());
}

void CleanExit()
{
    ClearMarker();
}

bool TitleOf(const std::string &path, uint32_t &id, std::string &name)
{
    DiscTitleInfo info;
    if (path.empty() || !ReadDiscTitleInfo(path, info) || !info.ok) {
        return false;
    }
    id = info.title_id;
    name = info.title_name;
    return true;
}

// Games that need the x87 unit to keep full precision on loads (it costs
// speed in others, so it is not the default)
bool WantsExactX87(uint32_t id)
{
    return id == 0x4D53004D; // NFL Fever 2004
}

const char *OnOff(bool b)
{
    return b ? "on" : "off";
}
} // namespace

void xemu_game_perf_startup(const char *dvd_path)
{
    Load();
    g_base = { g_config.perf.native_sse, g_config.perf.optimizations,
               g_config.perf.shader_seeding, g_config.perf.real_hw_speed,
               g_config.perf.vblank_hz > 0 ? g_config.perf.vblank_hz : 60 };
    g_applied = g_base;
    g_run_cpu = g_base.cpu;
    g_run_gpu = g_base.gpu;

    // The last run died with the boosts on: that game starts in safe mode
    FILE *m = g_fopen(File("running_game.txt").c_str(), "rb");
    if (m) {
        unsigned id = 0;
        int boosts = 0;
        if (fscanf(m, "%x boosts=%d", &id, &boosts) == 2 && boosts) {
            g_profiles[id].safe = true;
            Save();
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Last time %08X stopped unexpectedly with the boosts "
                     "on; it starts in safe mode now", id);
            fprintf(stderr, "game perf: %s\n", msg);
            g_notice = msg;
        }
        fclose(m);
        ClearMarker();
    }
    atexit(CleanExit);
    g_started = true;

    uint32_t id;
    std::string name;
    if (dvd_path && dvd_path[0] && TitleOf(dvd_path, id, name)) {
        g_cur_id = id;
        g_cur_name = name;
        g_cur_ok = true;
        xemu_x87_title_exact = WantsExactX87(id) ? 1 : 0;
        auto it = g_profiles.find(id);
        Eff e = Resolve(it != g_profiles.end() ? &it->second : nullptr);
        SetStartup(e);
        fprintf(stderr,
                "game perf: %08X \"%s\": CPU boost %s, GPU boost %s, shader "
                "seeding %s, real hardware speed %s, refresh %d Hz%s\n",
                id, name.c_str(), OnOff(e.cpu), OnOff(e.gpu), OnOff(e.seed),
                OnOff(e.real), e.hz,
                it != g_profiles.end() ? (it->second.safe ? " (safe mode)"
                                                          : " (its own settings)")
                                       : "");
        WriteMarker(id);
    } else {
        fprintf(stderr,
                "game perf: no game at start-up; CPU boost %s, GPU boost %s\n",
                OnOff(g_base.cpu), OnOff(g_base.gpu));
    }
}

void xemu_game_perf_disc_changed(const char *path)
{
    if (!g_started) {
        return;
    }
    uint32_t id = 0;
    std::string name;
    if (!path || !path[0]) {
        g_cur_ok = false;
        g_cur_id = 0;
        g_cur_name.clear();
        ClearMarker();
        SetLive(g_base);
        return;
    }
    if (!TitleOf(path, id, name)) {
        g_cur_ok = false;
        g_cur_name.clear();
        ClearMarker();
        return;
    }
    if (g_cur_ok && g_cur_id == id) {
        return;
    }
    g_cur_id = id;
    g_cur_name = name;
    g_cur_ok = true;
    auto it = g_profiles.find(id);
    Eff e = Resolve(it != g_profiles.end() ? &it->second : nullptr);
    SetLive(e);
    WriteMarker(id);
    if (!getenv("XEMU_X87")) {
        // The floating-point mode can change on the fly (blocks are rebuilt)
        xemu_x87_title_exact = WantsExactX87(id) ? 1 : 0;
        if (xemu_x87_title_exact != xemu_x87_exact) {
            fprintf(stderr, "game perf: %08X wants x87 %s\n", id,
                    xemu_x87_title_exact ? "exact loads" : "the fast path");
            xemu_x87_set_mode(xemu_x87_title_exact);
        }
    }
    if (e.cpu != g_run_cpu || e.gpu != g_run_gpu) {
        fprintf(stderr, "game perf: %08X wants CPU boost %s, GPU boost %s; "
                        "this run has %s and %s\n",
                id, OnOff(e.cpu), OnOff(e.gpu), OnOff(g_run_cpu),
                OnOff(g_run_gpu));
        g_notice = it != g_profiles.end() && it->second.safe
                       ? "This game starts in safe mode: restart xemu to turn "
                         "the boosts off for it"
                       : "This game has its own boost settings: restart xemu "
                         "to use them";
    }
}

void xemu_game_perf_tick(void)
{
    if (xemu_vk_bench_msg[0]) {
        xemu_queue_notification(xemu_vk_bench_msg);
        xemu_vk_bench_msg[0] = 0;
    }
    if (!g_notice.empty()) {
        xemu_queue_notification(g_notice.c_str());
        g_notice.clear();
    }
}

static bool g_swapped[4];

void xemu_game_perf_before_save(void)
{
    // Write the user's values, not the game's; a value the user changed
    // while the game ran is theirs
    bool *sw = g_swapped;
    sw[0] = sw[1] = sw[2] = sw[3] = false;
    if (!g_started) {
        return;
    }
    if (g_config.perf.shader_seeding == g_applied.seed) {
        if (g_applied.seed != g_base.seed) { sw[0] = true; }
        g_config.perf.shader_seeding = g_base.seed;
    } else {
        g_base.seed = g_config.perf.shader_seeding;
    }
    if (g_config.perf.real_hw_speed == g_applied.real) {
        if (g_applied.real != g_base.real) { sw[1] = true; }
        g_config.perf.real_hw_speed = g_base.real;
    } else {
        g_base.real = g_config.perf.real_hw_speed;
    }
    // Read once at start-up: what the config holds is what this run started
    // with, which may be a game's
    if (g_config.perf.native_sse == g_run_cpu && g_run_cpu != g_base.cpu) {
        sw[2] = true;
        g_config.perf.native_sse = g_base.cpu;
    }
    if (g_config.perf.optimizations == g_run_gpu && g_run_gpu != g_base.gpu) {
        sw[3] = true;
        g_config.perf.optimizations = g_base.gpu;
    }
    if (g_config.perf.vblank_hz == g_applied.hz && g_applied.hz != g_base.hz) {
        g_config.perf.vblank_hz = g_base.hz;
    }
}

void xemu_game_perf_after_save(void)
{
    if (!g_started) {
        return;
    }
    if (g_swapped[0]) g_config.perf.shader_seeding = g_applied.seed;
    if (g_swapped[1]) g_config.perf.real_hw_speed = g_applied.real;
    if (g_swapped[2]) g_config.perf.native_sse = g_run_cpu;
    if (g_swapped[3]) g_config.perf.optimizations = g_run_gpu;
    if (g_applied.hz != g_base.hz) g_config.perf.vblank_hz = g_applied.hz;
}

bool GamePerfCurrent(uint32_t &title_id, std::string &name)
{
    if (!g_started || !g_cur_ok) {
        return false;
    }
    title_id = g_cur_id;
    name = g_cur_name;
    return true;
}

GamePerfProfile &GamePerfEdit()
{
    return g_profiles[g_cur_id];
}

void GamePerfChanged()
{
    Save();
    auto it = g_profiles.find(g_cur_id);
    SetLive(Resolve(it != g_profiles.end() ? &it->second : nullptr));
}

int GamePerfGlobalHz()
{
    if (g_started) {
        return g_base.hz;
    }
    return g_config.perf.vblank_hz > 0 ? g_config.perf.vblank_hz : 60;
}

void GamePerfSetGlobalHz(int hz)
{
    if (!g_started) {
        g_config.perf.vblank_hz = hz;
        nv2a_set_vblank_hz(hz);
        return;
    }
    auto it = g_profiles.find(g_cur_id);
    bool overridden = g_cur_ok && it != g_profiles.end() &&
                      (it->second.safe || it->second.hz > 0);
    g_base.hz = hz;
    g_config.perf.vblank_hz = hz;
    if (!overridden) {
        g_applied.hz = hz;
        nv2a_set_vblank_hz(hz);
    }
}

int GamePerfHzFor(uint32_t title_id)
{
    Load();
    auto it = g_profiles.find(title_id);
    return it != g_profiles.end() ? it->second.hz : 0;
}

void GamePerfSetHzFor(uint32_t title_id, int hz)
{
    Load();
    if (g_started && g_cur_ok && g_cur_id == title_id) {
        GamePerfEdit().hz = hz;
        GamePerfChanged();
        return;
    }
    g_profiles[title_id].hz = hz;
    Save();
}

void GamePerfForget()
{
    g_profiles.erase(g_cur_id);
    GamePerfChanged();
}

bool GamePerfRestartPending()
{
    if (!g_started || !g_cur_ok) {
        return false;
    }
    auto it = g_profiles.find(g_cur_id);
    Eff e = Resolve(it != g_profiles.end() ? &it->second : nullptr);
    return e.cpu != g_run_cpu || e.gpu != g_run_gpu;
}

void GamePerfRestart()
{
#ifdef _WIN32
    xemu_settings_save(); // the new run reads the disc and settings from it
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) {
        return;
    }
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\"";
    // The same settings file; the disc comes from the saved settings, not
    // from a stale -dvd_path
    wchar_t wpath[MAX_PATH * 2];
    if (MultiByteToWideChar(CP_UTF8, 0, xemu_settings_get_path(), -1, wpath,
                            (int)(sizeof(wpath) / sizeof(wpath[0])))) {
        cmd += L" -config_path \"";
        cmd += wpath;
        cmd += L"\"";
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(exe, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si,
                       &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        exit(0);
    }
#endif
}
