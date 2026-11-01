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
#include "modern-version.hh"
#include "qemu/whpx-probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
    bool whpx;
};

// The hardware CPU only exists on Windows builds
#ifdef _WIN32
constexpr bool kWhpxPossible = true;
#else
constexpr bool kWhpxPossible = false;
#endif

std::map<uint32_t, GamePerfProfile> g_profiles;
bool g_loaded = false;

uint32_t g_cur_id = 0;
std::string g_cur_name;
bool g_cur_ok = false;

Eff g_base;       // the user's own values
Eff g_applied;    // what is set now
bool g_started = false;
bool g_run_cpu = false, g_run_gpu = false; // read once at start-up
bool g_run_whpx = false;
int g_marker_whpx = 0;
long long g_marker_start = 0;   // when this run began (seconds)
long long g_marker_alive = 0;   // last time the marker was refreshed
std::string g_notice;

std::string File(const char *name)
{
    const char *base = xemu_settings_get_base_path();
    return std::string(base ? base : "") + name;
}

// One line: <title ID> cpu= gpu= seed= real= hz= safe= [whpx= preset=]
bool ParseProfile(const char *line, unsigned &id, GamePerfProfile &p)
{
    int safe = 0;
    int whpx = -1;
    int preset = 0;
    int n = sscanf(line,
                   "%x cpu=%d gpu=%d seed=%d real=%d hz=%d safe=%d whpx=%d "
                   "preset=%d",
                   &id, &p.cpu, &p.gpu, &p.seed, &p.real, &p.hz, &safe, &whpx,
                   &preset);
    if (n < 7) {
        return false;
    }
    p.safe = safe != 0;
    p.whpx = n >= 8 ? whpx : -1;
    p.preset = n >= 9 ? preset : 0;
    return true;
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
        if (ParseProfile(line, id, p)) {
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
            !p.safe && p.whpx < 0 && p.preset == 0) {
            continue;
        }
        fprintf(f,
                "%08X cpu=%d gpu=%d seed=%d real=%d hz=%d safe=%d whpx=%d "
                "preset=%d\n",
                id, p.cpu, p.gpu, p.seed, p.real, p.hz, p.safe ? 1 : 0, p.whpx,
                p.preset);
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
        e.whpx = false;
    } else {
        if (p->cpu >= 0) e.cpu = p->cpu != 0;
        if (p->gpu >= 0) e.gpu = p->gpu != 0;
        if (p->hz > 0) e.hz = p->hz;
        if (p->whpx >= 0) e.whpx = p->whpx != 0;
    }
    if (p->seed >= 0) e.seed = p->seed != 0;
    if (p->real >= 0) e.real = p->real != 0;
    e.whpx = e.whpx && kWhpxPossible;
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
    g_config.perf.whpx = e.whpx;
    g_applied = e;
    g_run_cpu = e.cpu;
    g_run_gpu = e.gpu;
    g_run_whpx = e.whpx;
}

// ---------------------------------------------------------------------------
// Hardware CPU (Windows Hypervisor Platform) test results
//
// <settings folder>/whpx_compat.txt keeps, per game, how it did on the
// hardware CPU (tab separated: title ID, status, seconds, exits, device
// accesses, name, detail). <settings folder>/whpx_report.txt is the same in
// words, with this run's counters: it is the file to send back after a test.
// ---------------------------------------------------------------------------
struct WhpxResult {
    std::string status; // works, failed, crashed
    std::string name;
    std::string detail;
    long long secs = 0;
    unsigned long long exits = 0, mmio = 0;
};

std::map<uint32_t, WhpxResult> g_whpx_results;
std::mutex g_whpx_mu;
bool g_whpx_loaded = false;
int64_t g_title_t0 = 0; // seconds when this game began
unsigned long long g_title_exits0 = 0, g_title_mmio0 = 0;
unsigned g_title_fail0 = 0;
bool g_works_recorded = false;
int64_t g_works_written = 0;
unsigned g_seen_failures = 0;
bool g_whpx_noted = false;
// The running game, for the thread that can not touch the strings above
volatile uint32_t g_flush_id = 0;
char g_flush_name[100];

int64_t NowSeconds()
{
    return g_get_monotonic_time() / 1000000;
}

std::string Clean(const std::string &s)
{
    std::string r = s;
    for (auto &c : r) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return r;
}

void LoadWhpxResultsLocked()
{
    if (g_whpx_loaded) {
        return;
    }
    g_whpx_loaded = true;
    FILE *f = g_fopen(File("whpx_compat.txt").c_str(), "rb");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
            l.pop_back();
        }
        std::vector<std::string> parts;
        size_t from = 0;
        for (;;) {
            size_t t = l.find('\t', from);
            if (t == std::string::npos) {
                parts.push_back(l.substr(from));
                break;
            }
            parts.push_back(l.substr(from, t - from));
            from = t + 1;
        }
        if (parts.size() < 7) {
            continue;
        }
        unsigned id = 0;
        if (sscanf(parts[0].c_str(), "%x", &id) != 1) {
            continue;
        }
        WhpxResult r;
        r.status = parts[1];
        r.secs = atoll(parts[2].c_str());
        r.exits = strtoull(parts[3].c_str(), nullptr, 10);
        r.mmio = strtoull(parts[4].c_str(), nullptr, 10);
        r.name = parts[5];
        r.detail = parts[6];
        g_whpx_results[id] = r;
    }
    fclose(f);
}

void SaveWhpxResultsLocked()
{
    FILE *f = g_fopen(File("whpx_compat.txt").c_str(), "wb");
    if (!f) {
        return;
    }
    for (auto &[id, r] : g_whpx_results) {
        fprintf(f, "%08X\t%s\t%lld\t%llu\t%llu\t%s\t%s\n", id, r.status.c_str(),
                r.secs, r.exits, r.mmio, Clean(r.name).c_str(),
                Clean(r.detail).c_str());
    }
    fclose(f);
}

void WriteWhpxReportLocked()
{
    FILE *f = g_fopen(File("whpx_report.txt").c_str(), "wb");
    if (!f) {
        return;
    }
    char counters[1500];
    xemu_whpx_probe_describe(counters, sizeof(counters));
    fprintf(f, "xemu Modern GUI %s - hardware CPU (Windows Hypervisor "
               "Platform) report\n\n",
            XEMU_MODERN_GUI_VERSION);
    fprintf(f, "This run\n--------\n%s\n", counters);
    fprintf(f, "Games\n-----\n");
    if (g_whpx_results.empty()) {
        fprintf(f, "(none tried yet)\n");
    }
    for (const char *want : { "failed", "crashed", "works" }) {
        for (auto &[id, r] : g_whpx_results) {
            if (r.status != want) {
                continue;
            }
            fprintf(f, "%08X  %-8s  %s\n", id, r.status.c_str(),
                    r.name.empty() ? "(name unknown)" : r.name.c_str());
            if (r.status == "works") {
                fprintf(f,
                        "          ran %lld s, %llu CPU exits, %llu of them "
                        "device accesses\n",
                        r.secs, r.exits, r.mmio);
            } else if (!r.detail.empty()) {
                fprintf(f, "          %s\n", r.detail.c_str());
            }
        }
    }
    fclose(f);
}

void RecordWhpx(uint32_t id, const std::string &name, const char *status,
                const std::string &detail, long long secs,
                unsigned long long exits, unsigned long long mmio)
{
    std::lock_guard<std::mutex> lock(g_whpx_mu);
    LoadWhpxResultsLocked();
    WhpxResult &r = g_whpx_results[id];
    if (!name.empty()) {
        r.name = name;
    }
    r.status = status;
    r.detail = detail;
    r.secs = secs;
    r.exits = exits;
    r.mmio = mmio;
    SaveWhpxResultsLocked();
    WriteWhpxReportLocked();
}

// Called on the CPU thread when the hardware CPU hits something it can not
// run, possibly just before the program closes: get it on disk now
void WhpxFlush()
{
    uint32_t id = g_flush_id;
    if (!id) {
        std::lock_guard<std::mutex> lock(g_whpx_mu);
        WriteWhpxReportLocked();
        return;
    }
    RecordWhpx(id, g_flush_name, "failed", xemu_whpx_probe_last_failure(),
               NowSeconds() - g_title_t0, xemu_whpx_probe_total_exits() -
                   g_title_exits0, xemu_whpx_probe_exits(XEMU_WHPX_EXIT_MMIO) -
                   g_title_mmio0);
}

// The game began: remember where the counters stood
void StartTitle()
{
    g_title_t0 = NowSeconds();
    g_title_exits0 = xemu_whpx_probe_total_exits();
    g_title_mmio0 = xemu_whpx_probe_exits(XEMU_WHPX_EXIT_MMIO);
    g_title_fail0 = xemu_whpx_probe_failures();
    g_works_recorded = false;
    g_works_written = 0;
    snprintf(g_flush_name, sizeof(g_flush_name), "%s", g_cur_name.c_str());
    g_flush_id = g_cur_ok ? g_cur_id : 0;
}

// A game that ran a while on the hardware CPU without a failure worked
void UpdateWorks(bool final)
{
    if (!g_cur_ok || !xemu_whpx_probe_active() ||
        xemu_whpx_probe_failures() != g_title_fail0) {
        return;
    }
    int64_t now = NowSeconds();
    int64_t secs = now - g_title_t0;
    if (secs < 90) {
        return;
    }
    unsigned long long exits = xemu_whpx_probe_total_exits() - g_title_exits0;
    unsigned long long mmio =
        xemu_whpx_probe_exits(XEMU_WHPX_EXIT_MMIO) - g_title_mmio0;
    if (exits < 100000 || mmio < 1000) {
        return; // the machine was not really running
    }
    if (!final && g_works_recorded && now - g_works_written < 300) {
        return;
    }
    g_works_recorded = true;
    g_works_written = now;
    RecordWhpx(g_cur_id, g_cur_name, "works", "", secs, exits, mmio);
}

void FinishTitle()
{
    if (g_cur_ok) {
        UpdateWorks(true);
    }
    g_flush_id = 0;
}

// The hardware CPU is in use in the run now going (it was asked for and
// started, or has not said it could not)
bool RunHasWhpx()
{
    return g_run_whpx && !xemu_whpx_probe_start_note()[0];
}

// ---------------------------------------------------------------------------
// Presets: settings a game gets the first time it is seen, from the lines
// below and from <settings folder>/game_presets.txt (the game_perf.txt line
// format). A game that already has a line there is left alone, and
// "Forget this game's settings" does not bring the preset back.
// ---------------------------------------------------------------------------
// Filled in from test reports; nothing is guessed
const char *const kBuiltinPresets[] = { "" };

std::map<uint32_t, GamePerfProfile> g_presets;
bool g_presets_loaded = false;

void LoadPresets()
{
    if (g_presets_loaded) {
        return;
    }
    g_presets_loaded = true;
    for (const char *line : kBuiltinPresets) {
        unsigned id = 0;
        GamePerfProfile p;
        if (ParseProfile(line, id, p)) {
            g_presets[id] = p;
        }
    }
    FILE *f = g_fopen(File("game_presets.txt").c_str(), "rb");
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned id = 0;
        GamePerfProfile p;
        if (ParseProfile(line, id, p)) {
            g_presets[id] = p;
        }
    }
    fclose(f);
}

bool ApplyPreset(uint32_t id)
{
    LoadPresets();
    auto it = g_presets.find(id);
    if (it == g_presets.end() || g_profiles.count(id)) {
        return false;
    }
    GamePerfProfile p = it->second;
    p.safe = false;
    p.preset = 1;
    g_profiles[id] = p;
    Save();
    fprintf(stderr, "game perf: %08X set up from the preset list\n", id);
    return true;
}

void WriteMarker(uint32_t id)
{
    FILE *f = g_fopen(File("running_game.txt").c_str(), "wb");
    if (!f) {
        return;
    }
    g_marker_whpx = xemu_whpx_probe_active() ? 1 : 0;
    long long now = (long long)time(nullptr);
    if (!g_marker_start) {
        g_marker_start = now;
    }
    g_marker_alive = now;
    fprintf(f, "%08X boosts=%d whpx=%d start=%lld alive=%lld\n", id,
            (g_run_cpu || g_run_gpu) ? 1 : 0, g_marker_whpx, g_marker_start,
            g_marker_alive);
    fclose(f);
}

void ClearMarker()
{
    g_remove(File("running_game.txt").c_str());
}

void CleanExit()
{
    if (xemu_whpx_probe_requested_flag()) {
        UpdateWorks(true);
        std::lock_guard<std::mutex> lock(g_whpx_mu);
        LoadWhpxResultsLocked();
        WriteWhpxReportLocked();
    }
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
               g_config.perf.vblank_hz > 0 ? g_config.perf.vblank_hz : 60,
               g_config.perf.whpx && kWhpxPossible };
    g_applied = g_base;
    g_run_cpu = g_base.cpu;
    g_run_gpu = g_base.gpu;
    g_run_whpx = g_base.whpx;
    xemu_whpx_probe_set_flush(WhpxFlush);

    // The last run died with the boosts on: that game starts in safe mode
    FILE *m = g_fopen(File("running_game.txt").c_str(), "rb");
    if (m) {
        unsigned id = 0;
        int boosts = 0;
        int hw = 0;
        long long t_start = 0, t_alive = 0;
        int n = fscanf(m, "%x boosts=%d whpx=%d start=%lld alive=%lld", &id,
                       &boosts, &hw, &t_start, &t_alive);
        // A run that went on for a while and then just ended (window closed,
        // task killed, power) is not a boost crash: only a run that died
        // soon after it started counts.
        long long ran = (n >= 5 && t_alive > t_start) ? t_alive - t_start : 0;
        if (ran >= 90 && !hw) {
            fprintf(stderr,
                    "game perf: last run of %08X ended without a clean exit "
                    "after %lld s; not treating that as a crash\n", id, ran);
            boosts = 0;
        }
        if (n >= 3 && hw) {
            // The hardware CPU is the new part, so it is blamed first: this
            // game goes back to the normal CPU, and the boosts are not
            // touched
            g_profiles[id].whpx = 0;
            Save();
            {
                std::lock_guard<std::mutex> lock(g_whpx_mu);
                LoadWhpxResultsLocked();
            }
            bool already = false;
            {
                std::lock_guard<std::mutex> lock(g_whpx_mu);
                auto r = g_whpx_results.find(id);
                already = r != g_whpx_results.end() && r->second.status == "failed";
            }
            if (!already) {
                RecordWhpx(id, "", "crashed",
                           "xemu closed unexpectedly while this game ran on "
                           "the hardware CPU",
                           0, 0, 0);
            }
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "Last time %08X stopped unexpectedly on the hardware CPU; "
                     "it uses the normal CPU for this game now", id);
            fprintf(stderr, "game perf: %s\n", msg);
            g_notice = msg;
        } else if (n >= 2 && boosts) {
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
        bool from_preset = ApplyPreset(id);
        if (from_preset) {
            g_notice = "Settings for this game were set up from the preset list";
        }
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
        fprintf(stderr, "game perf: hardware CPU %s\n", OnOff(e.whpx));
        if (it != g_profiles.end() && it->second.safe && g_notice.empty()) {
            g_notice = "This game is in safe mode (boosts off, so it runs "
                       "slower). Settings > General > This game > Leave safe "
                       "mode, then restart";
        }
        WriteMarker(id);
        StartTitle();
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
        FinishTitle();
        g_cur_ok = false;
        g_cur_id = 0;
        g_cur_name.clear();
        ClearMarker();
        SetLive(g_base);
        return;
    }
    if (!TitleOf(path, id, name)) {
        FinishTitle();
        g_cur_ok = false;
        g_cur_name.clear();
        ClearMarker();
        return;
    }
    if (g_cur_ok && g_cur_id == id) {
        return;
    }
    FinishTitle();
    g_cur_id = id;
    g_cur_name = name;
    g_cur_ok = true;
    if (ApplyPreset(id)) {
        xemu_queue_notification(
            "Settings for this game were set up from the preset list");
    }
    auto it = g_profiles.find(id);
    Eff e = Resolve(it != g_profiles.end() ? &it->second : nullptr);
    SetLive(e);
    WriteMarker(id);
    StartTitle();
    if (!getenv("XEMU_X87")) {
        // The floating-point mode can change on the fly (blocks are rebuilt)
        xemu_x87_title_exact = WantsExactX87(id) ? 1 : 0;
        if (xemu_x87_title_exact != xemu_x87_exact) {
            fprintf(stderr, "game perf: %08X wants x87 %s\n", id,
                    xemu_x87_title_exact ? "exact loads" : "the fast path");
            xemu_x87_set_mode(xemu_x87_title_exact);
        }
    }
    if (e.cpu != g_run_cpu || e.gpu != g_run_gpu || e.whpx != RunHasWhpx()) {
        fprintf(stderr, "game perf: %08X wants CPU boost %s, GPU boost %s, "
                        "hardware CPU %s; this run has %s, %s and %s\n",
                id, OnOff(e.cpu), OnOff(e.gpu), OnOff(e.whpx), OnOff(g_run_cpu),
                OnOff(g_run_gpu), OnOff(RunHasWhpx()));
        g_notice = it != g_profiles.end() && it->second.safe
                       ? "This game starts in safe mode: restart xemu to turn "
                         "the boosts off for it"
                       : "This game has its own boost settings: restart xemu "
                         "to use them";
    }
}

void xemu_game_perf_tick(void)
{
    // Keep the marker's "alive" time fresh so a later start can tell a long
    // run that ended abruptly from a crash on start-up
    if (g_started && g_cur_ok && g_marker_start) {
        long long now = (long long)time(nullptr);
        if (now - g_marker_alive >= 30) {
            WriteMarker(g_cur_id);
        }
    }
    if (xemu_vk_bench_msg[0]) {
        xemu_queue_notification(xemu_vk_bench_msg);
        xemu_vk_bench_msg[0] = 0;
    }
    if (!g_notice.empty()) {
        xemu_queue_notification(g_notice.c_str());
        g_notice.clear();
    }
    if (!g_started || !xemu_whpx_probe_requested_flag()) {
        return;
    }
    // The hardware CPU said whether it could start
    if (!g_whpx_noted && (xemu_whpx_probe_active() ||
                          xemu_whpx_probe_start_note()[0])) {
        g_whpx_noted = true;
        if (!xemu_whpx_probe_active()) {
            std::string m = std::string("Hardware CPU did not start: ") +
                            xemu_whpx_probe_start_note() +
                            ". Using normal emulation.";
            xemu_queue_notification(m.c_str());
        }
        std::lock_guard<std::mutex> lock(g_whpx_mu);
        LoadWhpxResultsLocked();
        WriteWhpxReportLocked();
    }
    // The marker says whether the hardware CPU was really running
    if ((xemu_whpx_probe_active() ? 1 : 0) != g_marker_whpx && g_cur_ok) {
        WriteMarker(g_cur_id);
    }
    // It failed but the machine is still up: this game goes back to the
    // normal CPU from the next start
    unsigned fails = xemu_whpx_probe_failures();
    if (fails != g_seen_failures) {
        g_seen_failures = fails;
        if (g_cur_ok) {
            g_profiles[g_cur_id].whpx = 0;
            Save();
        }
        std::string m = std::string("Hardware CPU problem: ") +
                        xemu_whpx_probe_last_failure();
        xemu_queue_notification(m.c_str());
    }
    UpdateWorks(false);
}

static bool g_swapped[5];

void xemu_game_perf_before_save(void)
{
    // Write the user's values, not the game's; a value the user changed
    // while the game ran is theirs
    bool *sw = g_swapped;
    sw[0] = sw[1] = sw[2] = sw[3] = sw[4] = false;
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
    if (g_config.perf.whpx == g_run_whpx && g_run_whpx != g_base.whpx) {
        sw[4] = true;
        g_config.perf.whpx = g_base.whpx;
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
    if (g_swapped[4]) g_config.perf.whpx = g_run_whpx;
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
    // Keep a line behind when the game has a preset, so it stays forgotten
    LoadPresets();
    GamePerfProfile fresh;
    fresh.preset = g_presets.count(g_cur_id) ? 2 : 0;
    g_profiles[g_cur_id] = fresh;
    GamePerfChanged();
}

int GamePerfSafeModeCount()
{
    Load();
    int n = 0;
    for (auto &[id, p] : g_profiles) {
        n += p.safe ? 1 : 0;
    }
    return n;
}

void GamePerfLeaveSafeModeAll()
{
    Load();
    for (auto &[id, p] : g_profiles) {
        p.safe = false;
    }
    GamePerfChanged();
}

bool GamePerfRestartPending()
{
    if (!g_started || !g_cur_ok) {
        return false;
    }
    auto it = g_profiles.find(g_cur_id);
    Eff e = Resolve(it != g_profiles.end() ? &it->second : nullptr);
    return e.cpu != g_run_cpu || e.gpu != g_run_gpu || e.whpx != RunHasWhpx();
}

std::string GamePerfWhpxStatus()
{
    if (!kWhpxPossible) {
        return "";
    }
    if (!xemu_whpx_probe_requested_flag()) {
        return g_config.perf.whpx ? "Not in use in this run (it starts with "
                                    "xemu, or this game turns it off)"
                                  : "";
    }
    if (xemu_whpx_probe_start_note()[0]) {
        return std::string("Could not start: ") + xemu_whpx_probe_start_note();
    }
    if (!xemu_whpx_probe_active()) {
        return "Starting...";
    }
    unsigned f = xemu_whpx_probe_failures();
    return f ? std::string("Running, with ") + std::to_string(f) +
                   " problem(s): " + xemu_whpx_probe_last_failure()
             : "Running";
}

std::string GamePerfWhpxResult(uint32_t title_id)
{
    std::lock_guard<std::mutex> lock(g_whpx_mu);
    LoadWhpxResultsLocked();
    auto it = g_whpx_results.find(title_id);
    if (it == g_whpx_results.end()) {
        return "";
    }
    const WhpxResult &r = it->second;
    if (r.status == "works") {
        return "Worked (ran " + std::to_string(r.secs / 60) + " min)";
    }
    if (r.status == "crashed") {
        return "xemu closed unexpectedly";
    }
    return "Failed: " + r.detail;
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
