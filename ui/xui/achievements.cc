//
// xemu User Interface - achievements
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "achievements.hh"
#include "common.hh"
#include "font-manager.hh"
#include "game-library.hh"
#include "i18n.hh"
#include "IconsFontAwesome6.h"
#include "viewport-manager.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifndef ACH_TEST
#include "qemu/http.h"
#else
extern "C" int http_post_form(const char *url, const char *body, GByteArray *response_body,
                              void *errp);
#endif

using json = nlohmann::json;

namespace {

struct Def {
    const char *id;
    const char *name;
    const char *desc;
    const char *category;
    int points;
    const char *key; // the count it looks at
    int64_t target;
};

// Names and descriptions go through tr() where they're shown
const Def kDefs[] = {
    // Play
    { "power_on", N_("Power On"), N_("Start a game from the library."), N_("Play"), 5,
      "launches", 1 },
    { "regular", N_("Regular"), N_("Start games 10 times."), N_("Play"), 10, "launches", 10 },
    { "game_night", N_("Game Night"), N_("Play 5 different games."), N_("Play"), 10, "games", 5 },
    { "full_shelf", N_("Full Shelf"), N_("Have 25 games in your library."), N_("Play"), 10,
      "library", 25 },
    { "collection", N_("The Collection"), N_("Have 100 games in your library."), N_("Play"), 25,
      "library", 100 },
    { "favorites", N_("Favorites"), N_("Mark 5 games as favorites."), N_("Play"), 5, "favorites", 5 },
    // Time
    { "warming_up", N_("Warming Up"), N_("Play for 1 hour in total."), N_("Time"), 10, "play_s", 3600 },
    { "marathon", N_("Marathon"), N_("Play for 10 hours in total."), N_("Time"), 25, "play_s",
      36000 },
    { "dedicated", N_("Dedicated"), N_("Play for 50 hours in total."), N_("Time"), 50, "play_s",
      180000 },
    { "lost_in_time", N_("Lost in Time"), N_("Play for 100 hours in total."), N_("Time"), 100,
      "play_s", 360000 },
    { "night_owl", N_("Night Owl"), N_("Start a game between midnight and 5 AM."), N_("Time"), 10,
      "night", 1 },
    // Saves
    { "time_traveler", N_("Time Traveler"), N_("Make a save state."), N_("Saves"), 5, "states", 1 },
    { "safety_net", N_("Safety Net"), N_("Make 10 save states."), N_("Saves"), 15, "states", 10 },
    { "quick_fingers", N_("Quick Fingers"), N_("Quick save a game."), N_("Saves"), 5,
      "quick_saves", 1 },
    { "rewind", N_("Rewind"), N_("Quick load a game."), N_("Saves"), 5, "quick_loads", 1 },
    { "backed_up", N_("Backed Up"), N_("Back up the Xbox's game saves to your PC."), N_("Saves"), 10,
      "backups", 1 },
    // Homebrew and the drive
    { "tinkerer", N_("Tinkerer"), N_("Install a homebrew app or game."), N_("Homebrew"), 10,
      "homebrew", 1 },
    { "homebrewer", N_("Homebrewer"), N_("Install 5 homebrew apps or games."), N_("Homebrew"), 25,
      "homebrew", 5 },
    { "file_manager", N_("File Manager"), N_("Change files on the Xbox's hard drive."),
      N_("Homebrew"), 5, "drive_edits", 1 },
    // Picture and sound
    { "smooth_operator", N_("Smooth Operator"), N_("Turn on 60 FPS for a game."),
      N_("Picture"), 10, "sixty", 1 },
    { "patched_up", N_("Patched Up"), N_("Download a patch for a game."), N_("Picture"), 5,
      "patches", 1 },
    { "retro_look", N_("Retro Look"), N_("Play with the CRT TV or Scanlines filter."),
      N_("Picture"), 5, "crt", 1 },
    // Online
    { "going_online", N_("Going Online"), N_("Set up Insignia with the Setup Assistant."),
      N_("Online"), 15, "insignia_setup", 1 },
    { "multiplayer", N_("Multiplayer"), N_("Start a game online."), N_("Online"), 10, "online", 1 },
    // Your way
    { "world_traveler", N_("World Traveler"), N_("Switch xemu to another language."),
      N_("Your way"), 5, "lang", 1 },
    { "mouse_keyboard", N_("Mouse and Keyboard"), N_("Play with the mouse captured."),
      N_("Your way"), 10, "mouse", 1 },
    { "typist", N_("Typist"), N_("Use the on-screen keyboard."), N_("Your way"), 5, "osk", 1 },
    { "out_of_the_way", N_("Out of the Way"), N_("Minimize the dashboard."), N_("Your way"), 5,
      "collapse", 1 },
    { "explorer", N_("Explorer"), N_("Visit every tab of the dashboard."), N_("Your way"), 10,
      "tabs", 9 },
    // Everything else
    { "completionist", N_("Completionist"), N_("Unlock every other achievement."),
      N_("Everything"), 50, "others", 0 },
};
const int kDefCount = (int)(sizeof(kDefs) / sizeof(kDefs[0]));

struct Toast {
    int def;
    double t0;
};

std::mutex g_lock;
std::string g_path;
bool g_dirty = false;
double g_last_save = 0;
std::map<std::string, int64_t> g_ctr;     // counts and stats
std::map<std::string, int64_t> g_unlocked; // id -> unix time
std::set<uint32_t> g_titles;               // distinct games started
std::set<int> g_tabs;                      // dashboard tabs visited
bool g_popups = true;
std::deque<Toast> g_pending;               // waiting to show
std::vector<Toast> g_toasts;               // showing now

// RetroAchievements
RaInfo g_ra;
std::string g_ra_token;
std::thread g_ra_thread;
bool g_ra_refreshed = false;

int64_t Count(const std::string &key)
{
    if (key == "games") {
        return (int64_t)g_titles.size();
    }
    if (key == "tabs") {
        return (int64_t)g_tabs.size();
    }
    auto it = g_ctr.find(key);
    return it == g_ctr.end() ? 0 : it->second;
}

int64_t Target(const Def &d)
{
    if (!strcmp(d.key, "others")) {
        return kDefCount - 1;
    }
    return d.target;
}

int64_t Progress(const Def &d)
{
    if (!strcmp(d.key, "others")) {
        int64_t n = 0;
        for (int i = 0; i < kDefCount; i++) {
            if (&kDefs[i] != &d && g_unlocked.count(kDefs[i].id)) {
                n++;
            }
        }
        return n;
    }
    return std::min(Count(d.key), d.target);
}

void ShowToast(int def)
{
    if (g_popups) {
        g_pending.push_back({ def, 0 });
    }
}

// Unlocks whatever has just been reached
void CheckLocked()
{
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < kDefCount; i++) {
            const Def &d = kDefs[i];
            if (g_unlocked.count(d.id) || Progress(d) < Target(d)) {
                continue;
            }
            g_unlocked[d.id] = (int64_t)time(NULL);
            g_dirty = true;
            ShowToast(i);
            again = true; // Completionist may now be reached
        }
    }
}

void Bump(const char *key, int64_t by = 1)
{
    g_ctr[key] += by;
    g_dirty = true;
    CheckLocked();
}

// ---- Saving ----

void SaveLocked()
{
    if (g_path.empty()) {
        return;
    }
    json j;
    j["unlocked"] = g_unlocked;
    j["counters"] = g_ctr;
    j["titles"] = std::vector<uint32_t>(g_titles.begin(), g_titles.end());
    j["tabs"] = std::vector<int>(g_tabs.begin(), g_tabs.end());
    j["popups"] = g_popups;
    if (!g_ra_token.empty()) {
        j["retroachievements"] = { { "user", g_ra.user }, { "token", g_ra_token } };
    }
    std::string tmp = g_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return;
        }
        f << j.dump(1);
        if (!f.good()) {
            return;
        }
    }
    std::remove(g_path.c_str());
    if (std::rename(tmp.c_str(), g_path.c_str()) == 0) {
        g_dirty = false;
    }
}

} // namespace

void AchievementsLoad(const std::string &path)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_path = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return;
    }
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    try {
        if (j.contains("unlocked") && j["unlocked"].is_object()) {
            for (auto &kv : j["unlocked"].items()) {
                if (kv.value().is_number_integer()) {
                    g_unlocked[kv.key()] = kv.value().get<int64_t>();
                }
            }
        }
        if (j.contains("counters") && j["counters"].is_object()) {
            for (auto &kv : j["counters"].items()) {
                if (kv.value().is_number_integer()) {
                    g_ctr[kv.key()] = kv.value().get<int64_t>();
                }
            }
        }
        if (j.contains("titles") && j["titles"].is_array()) {
            for (auto &v : j["titles"]) {
                if (v.is_number_unsigned()) {
                    g_titles.insert(v.get<uint32_t>());
                }
            }
        }
        if (j.contains("tabs") && j["tabs"].is_array()) {
            for (auto &v : j["tabs"]) {
                if (v.is_number_integer()) {
                    g_tabs.insert(v.get<int>());
                }
            }
        }
        if (j.contains("popups") && j["popups"].is_boolean()) {
            g_popups = j["popups"].get<bool>();
        }
        if (j.contains("retroachievements") && j["retroachievements"].is_object()) {
            auto &ra = j["retroachievements"];
            if (ra.contains("token") && ra["token"].is_string() && ra.contains("user") &&
                ra["user"].is_string()) {
                g_ra_token = ra["token"].get<std::string>();
                g_ra.user = ra["user"].get<std::string>();
                g_ra.state = g_ra_token.empty() ? kRaSignedOut : kRaSignedIn;
            }
        }
    } catch (...) {
        // A damaged file: keep what was read
    }
}

void AchievementsSave(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_dirty) {
        SaveLocked();
    }
}

void AchievementsShutdown(void)
{
    if (g_ra_thread.joinable()) {
        g_ra_thread.join();
    }
    AchievementsSave();
}

void AchievementsEvent(AchEvent e, uint32_t arg)
{
    std::lock_guard<std::mutex> lk(g_lock);
    switch (e) {
    case kAchLaunch: {
        g_titles.insert(arg);
        time_t now = time(NULL);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        if (lt.tm_hour < 5) {
            g_ctr["night"] = 1;
        }
        Bump("launches");
        break;
    }
    case kAchSaveState:
        Bump("states");
        break;
    case kAchQuickSave:
        Bump("quick_saves");
        break;
    case kAchQuickLoad:
        Bump("quick_loads");
        break;
    case kAchBackup:
        Bump("backups");
        break;
    case kAchHomebrew:
        Bump("homebrew");
        break;
    case kAchDriveEdit:
        Bump("drive_edits");
        break;
    case kAchSixtyFps:
        Bump("sixty");
        break;
    case kAchPatches:
        Bump("patches");
        break;
    case kAchInsigniaSetup:
        Bump("insignia_setup");
        break;
    case kAchOnlinePlay:
        Bump("online");
        break;
    case kAchMouse:
        Bump("mouse");
        break;
    case kAchKeyboard:
        Bump("osk");
        break;
    case kAchCollapse:
        Bump("collapse");
        break;
    case kAchTab:
        if (g_tabs.insert((int)arg).second) {
            g_dirty = true;
            CheckLocked();
        }
        break;
    }
}

void AchievementsSetStat(const char *key, int64_t value)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_ctr.find(key);
    if (it == g_ctr.end() || value > it->second) {
        g_ctr[key] = value;
        g_dirty = true;
        CheckLocked();
    }
}

void AchievementsUpdate(bool filtering_crt, const char *language)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (filtering_crt && Count("crt") == 0) {
        g_ctr["crt"] = 1;
        g_dirty = true;
        CheckLocked();
    }
    if (language && language[0] && strcmp(language, "en") && Count("lang") == 0) {
        g_ctr["lang"] = 1;
        g_dirty = true;
        CheckLocked();
    }
    // Save a little after something changes
    double now = ImGui::GetTime();
    if (g_dirty && now - g_last_save > 5.0) {
        g_last_save = now;
        SaveLocked();
    }
}

int AchievementCount(void)
{
    return kDefCount;
}

AchievementInfo AchievementAt(int i)
{
    std::lock_guard<std::mutex> lk(g_lock);
    i = std::clamp(i, 0, kDefCount - 1);
    const Def &d = kDefs[i];
    auto it = g_unlocked.find(d.id);
    AchievementInfo a;
    a.id = d.id;
    a.name = d.name;
    a.desc = d.desc;
    a.category = d.category;
    a.points = d.points;
    a.target = Target(d);
    a.unlocked = it != g_unlocked.end();
    a.unlocked_at = a.unlocked ? it->second : 0;
    a.progress = a.unlocked ? a.target : Progress(d);
    return a;
}

int AchievementsUnlocked(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int n = 0;
    for (int i = 0; i < kDefCount; i++) {
        n += g_unlocked.count(kDefs[i].id) ? 1 : 0;
    }
    return n;
}

int AchievementsPoints(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int n = 0;
    for (int i = 0; i < kDefCount; i++) {
        n += g_unlocked.count(kDefs[i].id) ? kDefs[i].points : 0;
    }
    return n;
}

int AchievementsPointsTotal(void)
{
    int n = 0;
    for (int i = 0; i < kDefCount; i++) {
        n += kDefs[i].points;
    }
    return n;
}

bool AchievementsPopups(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_popups;
}

void AchievementsSetPopups(bool on)
{
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_popups != on) {
        g_popups = on;
        g_dirty = true;
    }
}

void AchievementsPreviewToast(int i)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_pending.push_back({ std::clamp(i, 0, kDefCount - 1), 0 });
}

// ---------------------------------------------------------------------------
// Pop-ups
// ---------------------------------------------------------------------------

namespace {

float EaseOut(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

ImU32 WithAlpha(ImU32 c, float a)
{
    int base = (int)((c >> IM_COL32_A_SHIFT) & 0xFF);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(base * a) << IM_COL32_A_SHIFT);
}

// The dashboard's shape: cut corners for Neon, rounded for Green
void Shape(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float r, bool neon,
           float stroke = 0)
{
    if (!neon) {
        if (stroke > 0) {
            dl->AddRect(a, b, col, r, 0, stroke);
        } else {
            dl->AddRectFilled(a, b, col, r);
        }
        return;
    }
    float c = std::min(r * 0.9f, std::min(b.x - a.x, b.y - a.y) * 0.3f);
    ImVec2 pts[6] = { ImVec2(a.x + c, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - c),
                      ImVec2(b.x - c, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + c) };
    if (stroke > 0) {
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, stroke);
    } else {
        dl->AddConvexPolyFilled(pts, 6, col);
    }
}

} // namespace

void AchievementsDraw(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    const double now = ImGui::GetTime();
    const float show = 6.0f, in_dur = 0.35f, out_dur = 0.45f;
    // Up to three at once; the next one waits for room
    while (!g_pending.empty() && g_toasts.size() < 3) {
        Toast t = g_pending.front();
        g_pending.pop_front();
        t.t0 = now;
        g_toasts.push_back(t);
    }
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(),
                                  [&](const Toast &t) {
                                      return now - t.t0 > show + out_dur;
                                  }),
                   g_toasts.end());
    if (g_toasts.empty()) {
        return;
    }

    const GameLibraryColors th = GameLibraryThemeColors();
    const float s = g_viewport_mgr.m_scale;
    const bool anim = g_config.display.ui.use_animations;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList(vp);
    ImFont *font = g_font_mgr.m_menu_font_small ? g_font_mgr.m_menu_font_small
                                                : ImGui::GetFont();
    ImFont *small = ImGui::GetFont();
    ImFont *icon_font = ImGui::GetIO().Fonts->Fonts[0];
    const float w = std::min(380.0f * s, vp->WorkSize.x - 32 * s);
    const float h = 82 * s;
    const float gap = 10 * s;
    const float margin = 24 * s;
    const float r = 14 * s;

    float y = vp->WorkPos.y + vp->WorkSize.y - margin - h; // newest at the bottom
    for (int idx = (int)g_toasts.size() - 1; idx >= 0; idx--) {
        const Toast &t = g_toasts[idx];
        const Def &d = kDefs[t.def];
        float age = (float)(now - t.t0);
        float k = 1.0f; // 0 hidden .. 1 shown
        if (anim) {
            if (age < in_dur) {
                k = EaseOut(age / in_dur);
            } else if (age > show) {
                k = 1 - EaseOut((age - show) / out_dur);
            }
        } else if (age > show) {
            k = 0;
        }
        if (k <= 0.001f) {
            y -= h + gap;
            continue;
        }
        float slide = (1 - k) * (w + margin);
        ImVec2 a(vp->WorkPos.x + vp->WorkSize.x - margin - w + slide, y);
        ImVec2 b(a.x + w, a.y + h);

        Shape(dl, ImVec2(a.x, a.y + 6 * s), ImVec2(b.x, b.y + 6 * s),
              WithAlpha(IM_COL32(0, 0, 0, 120), k), r, th.neon);
        Shape(dl, a, b,
              WithAlpha(th.neon ? IM_COL32(10, 11, 24, 248) : IM_COL32(22, 28, 22, 248), k), r,
              th.neon);
        Shape(dl, a, b,
              WithAlpha(th.neon ? th.accent : IM_COL32(255, 255, 255, 40), k * (th.neon ? 0.7f : 1)),
              r, th.neon, 1.5f * s);

        // Trophy badge
        float badge = h - 28 * s;
        ImVec2 c(a.x + 16 * s + badge / 2, a.y + h / 2);
        dl->AddCircleFilled(c, badge / 2, WithAlpha(th.accent, k), 32);
        float isz = icon_font->FontSize * 1.5f;
        ImVec2 ts = icon_font->CalcTextSizeA(isz, FLT_MAX, 0, ICON_FA_TROPHY);
        dl->AddText(icon_font, isz, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2),
                    WithAlpha(th.on_accent, k), ICON_FA_TROPHY);

        // Text
        float tx = a.x + 16 * s + badge + 14 * s;
        float max_w = b.x - tx - 14 * s;
        std::string head = tr("Achievement unlocked");
        for (auto &ch : head) {
            ch = (char)toupper((unsigned char)ch);
        }
        float hs = small->FontSize * 0.78f;
        dl->AddText(small, hs, ImVec2(tx, a.y + 12 * s), WithAlpha(th.accent, k), head.c_str());
        std::string name = tr(d.name);
        ImVec4 clip(tx, a.y, tx + max_w, b.y);
        dl->AddText(font, font->FontSize, ImVec2(tx, a.y + 12 * s + hs + 4 * s),
                    WithAlpha(th.text, k), name.c_str(), nullptr, 0, &clip);
        std::string pts = std::string("+") + std::to_string(d.points) + "  " + tr("points");
        dl->AddText(small, hs, ImVec2(tx, b.y - 12 * s - hs), WithAlpha(th.dim, k),
                    pts.c_str());
        y -= h + gap;
    }
}

// ---------------------------------------------------------------------------
// RetroAchievements account
// ---------------------------------------------------------------------------

namespace {

std::string UrlEscape(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// One request to retroachievements.org; true if the server answered
bool RaPost(const std::string &body, std::string &reply)
{
    GByteArray *data = g_byte_array_new();
    int code = http_post_form("https://retroachievements.org/dorequest.php", body.c_str(),
                              data, NULL);
    reply.assign((const char *)data->data, data->len);
    g_byte_array_unref(data);
    return code == 200;
}

void RaLogin(std::string user, std::string secret, bool is_password)
{
    std::string body = "r=login2&u=" + UrlEscape(user) + (is_password ? "&p=" : "&t=") +
                       UrlEscape(secret);
    std::string reply;
    bool reached = RaPost(body, reply);
    RaInfo info;
    std::string token;
    bool ok = reached && RaParseLogin(reply, info, token);
    std::lock_guard<std::mutex> lk(g_lock);
    if (ok) {
        g_ra = info;
        g_ra_token = token;
        g_dirty = true;
        SaveLocked();
        return;
    }
    if (!reached) {
        // Keep a saved sign-in when it's only the network
        g_ra.error = "Could not reach retroachievements.org"; // shown through tr()
        g_ra.state = g_ra_token.empty() ? kRaFailed : kRaSignedIn;
        return;
    }
    g_ra.state = kRaFailed;
    g_ra.error = info.error.empty() ? "RetroAchievements didn't accept that" : info.error;
    if (!is_password) {
        // The saved token no longer works
        g_ra_token.clear();
        g_dirty = true;
    }
}

} // namespace

bool RaParseLogin(const std::string &text, RaInfo &out, std::string &token)
{
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        out.error = "";
        return false;
    }
    bool success = j.value("Success", false);
    if (!success) {
        out.error = j.contains("Error") && j["Error"].is_string() ? j["Error"].get<std::string>()
                                                                   : std::string();
        return false;
    }
    if (!j.contains("User") || !j["User"].is_string() || !j.contains("Token") ||
        !j["Token"].is_string()) {
        return false;
    }
    out.user = j["User"].get<std::string>();
    token = j["Token"].get<std::string>();
    out.points = j.value("Score", (int64_t)0);
    out.softcore = j.value("SoftcoreScore", (int64_t)0);
    out.state = kRaSignedIn;
    out.error.clear();
    return !token.empty();
}

RaInfo RaGetInfo(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    return g_ra;
}

void RaSignIn(const std::string &user, const std::string &password)
{
    if (user.empty() || password.empty()) {
        return;
    }
    if (g_ra_thread.joinable()) {
        g_ra_thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_ra.state == kRaWorking) {
            return;
        }
        g_ra.state = kRaWorking;
        g_ra.error.clear();
    }
    g_ra_thread = std::thread(RaLogin, user, password, true);
}

void RaRefresh(void)
{
    std::string user, token;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_ra_refreshed || g_ra_token.empty() || g_ra.state == kRaWorking) {
            return;
        }
        g_ra_refreshed = true;
        user = g_ra.user;
        token = g_ra_token;
    }
    if (g_ra_thread.joinable()) {
        g_ra_thread.join();
    }
    g_ra_thread = std::thread(RaLogin, user, token, false);
}

void RaSignOut(void)
{
    if (g_ra_thread.joinable()) {
        g_ra_thread.join();
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_ra = RaInfo();
    g_ra_token.clear();
    g_ra_refreshed = false;
    g_dirty = true;
    SaveLocked();
}
