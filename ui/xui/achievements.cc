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
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#ifndef ACH_TEST
#include "qemu/http.h"
#else
extern "C" int http_post_form(const char *url, const char *body, GByteArray *response_body,
                              void *errp);
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A game's achievements, merged from all its pack files
struct Game {
    std::string name;
    std::vector<std::string> authors;
    std::vector<AchievementInfo> list; // done / done_at are filled in when asked
};

struct Toast {
    std::string name;
    int points;
    double t0;
};

std::mutex g_lock;
std::string g_path;       // progress file
std::string g_packs_dir;
bool g_dirty = false;
double g_last_save = 0;
std::map<uint32_t, Game> g_games;
std::map<uint32_t, std::map<std::string, int64_t>> g_done; // title -> id -> unix time
bool g_popups = true;
std::deque<Toast> g_pending; // waiting to show
std::vector<Toast> g_toasts; // showing now

// RetroAchievements
RaInfo g_ra;
std::string g_ra_token;
std::thread g_ra_thread;
bool g_ra_refreshed = false;

std::string HexId(uint32_t id)
{
    char b[16];
    snprintf(b, sizeof(b), "%08X", id);
    return b;
}

// 1 to 8 hex digits, all of them
bool ParseHexId(const std::string &s, uint32_t *out)
{
    if (s.empty() || s.size() > 8) {
        return false;
    }
    uint32_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            return false;
        }
        v = v * 16 + (uint32_t)d;
    }
    *out = v;
    return true;
}

// At most n bytes, without cutting a UTF-8 character in half
std::string Clip(std::string s, size_t n)
{
    if (s.size() <= n) {
        return s;
    }
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) {
        k--;
    }
    s.resize(k);
    return s;
}

std::string Str(const json &j, const char *key, size_t max)
{
    if (j.is_object() && j.contains(key) && j[key].is_string()) {
        return Clip(j[key].get<std::string>(), max);
    }
    return "";
}

// Adds one pack's achievements to `games`. Returns how many were added, or
// -1 if it isn't a usable pack. Pack files come from other people, so
// every value is checked and limited.
int ParsePackInto(const std::string &text, uint32_t file_tid, bool mine,
                  std::map<uint32_t, Game> &games)
{
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("achievements") || !j["achievements"].is_array()) {
        return -1;
    }
    uint32_t tid = file_tid;
    if (j.contains("title_id") && j["title_id"].is_string()) {
        uint32_t t;
        if (ParseHexId(j["title_id"].get<std::string>(), &t)) {
            tid = t;
        }
    }
    if (!tid) {
        return -1;
    }
    Game &g = games[tid];
    std::string gname = Str(j, "game", 80);
    if (g.name.empty() && !gname.empty()) {
        g.name = gname;
    }
    std::string author = mine ? "" : Str(j, "author", 60);
    if (!author.empty() &&
        std::find(g.authors.begin(), g.authors.end(), author) == g.authors.end()) {
        g.authors.push_back(author);
    }
    std::set<std::string> seen;
    for (auto &a : g.list) {
        seen.insert(a.id);
    }
    int added = 0, idx = 0;
    for (auto &e : j["achievements"]) {
        if (idx++ >= 500 || g.list.size() >= 500) {
            break;
        }
        if (!e.is_object()) {
            continue;
        }
        AchievementInfo a;
        a.name = Str(e, "name", 80);
        if (a.name.empty()) {
            continue;
        }
        a.id = Str(e, "id", 40);
        if (a.id.empty()) {
            a.id = "n" + std::to_string(idx);
        }
        if (!seen.insert(a.id).second) {
            continue;
        }
        a.desc = Str(e, "description", 300);
        a.group = Str(e, "group", 40);
        a.points = 10;
        if (e.contains("points") && e["points"].is_number_integer()) {
            a.points = (int)std::clamp<int64_t>(e["points"].get<int64_t>(), 0, 1000);
        }
        a.secret = e.contains("secret") && e["secret"].is_boolean() && e["secret"].get<bool>();
        a.mine = mine;
        g.list.push_back(std::move(a));
        added++;
    }
    return added;
}

bool ReadFile(const fs::path &p, std::string &out, size_t max)
{
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz > max) {
        return false;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void LoadPacksLocked()
{
    g_games.clear();
    if (g_packs_dir.empty()) {
        return;
    }
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto &de : fs::directory_iterator(fs::u8path(g_packs_dir), ec)) {
        if (!de.is_regular_file(ec)) {
            continue;
        }
        std::string ext = de.path().extension().u8string();
        for (auto &c : ext) {
            c = (char)tolower((unsigned char)c);
        }
        if (ext == ".json") {
            files.push_back(de.path());
        }
    }
    // Your own come after the packs, so they list last
    auto is_mine = [](const fs::path &p) {
        std::string s = p.stem().u8string();
        return s.size() > 5 && s.compare(s.size() - 5, 5, "-mine") == 0;
    };
    std::sort(files.begin(), files.end(), [&](const fs::path &a, const fs::path &b) {
        bool ma = is_mine(a), mb = is_mine(b);
        return ma != mb ? mb : a < b;
    });
    for (auto &p : files) {
        std::string stem = p.stem().u8string();
        uint32_t tid = 0;
        // 4D530004.json, 4D530004-mine.json, 4D530004 something.json
        ParseHexId(stem.substr(0, std::min<size_t>(8, stem.size())), &tid);
        bool mine = stem.size() > 5 && stem.compare(stem.size() - 5, 5, "-mine") == 0;
        std::string text;
        if (ReadFile(p, text, 1 << 20)) {
            ParsePackInto(text, tid, mine, g_games);
        }
    }
    for (auto it = g_games.begin(); it != g_games.end();) {
        it = it->second.list.empty() ? g_games.erase(it) : std::next(it);
    }
}

// ---- Saving ----

void SaveLocked()
{
    if (g_path.empty()) {
        return;
    }
    json j;
    j["version"] = 2;
    json done = json::object();
    for (auto &[tid, m] : g_done) {
        if (!m.empty()) {
            done[HexId(tid)] = m;
        }
    }
    j["done"] = done;
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

void ShowToast(const std::string &name, int points)
{
    if (g_popups) {
        g_pending.push_back({ name, points, 0 });
    }
}

AchievementInfo *FindLocked(uint32_t tid, const std::string &id)
{
    auto it = g_games.find(tid);
    if (it == g_games.end()) {
        return nullptr;
    }
    for (auto &a : it->second.list) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

// Your own achievements live in <TITLEID>-mine.json
fs::path MinePath(uint32_t tid)
{
    return fs::u8path(g_packs_dir) / fs::u8path(HexId(tid) + "-mine.json");
}

json MineRead(uint32_t tid, const std::string &game_name)
{
    std::string text;
    json j;
    if (ReadFile(MinePath(tid), text, 1 << 20)) {
        j = json::parse(text, nullptr, false);
    }
    if (!j.is_object() || !j.contains("achievements") || !j["achievements"].is_array()) {
        j = json::object();
        j["achievements"] = json::array();
    }
    j["format"] = 1;
    j["title_id"] = HexId(tid);
    if (!game_name.empty()) {
        j["game"] = Clip(game_name, 80);
    }
    j["author"] = "me";
    return j;
}

bool MineWrite(uint32_t tid, const json &j)
{
    std::error_code ec;
    fs::create_directories(fs::u8path(g_packs_dir), ec);
    fs::path p = MinePath(tid);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(1);
        if (!f.good()) {
            return false;
        }
    }
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
    return !ec;
}

json MineEntry(const std::string &id, const std::string &name, const std::string &desc,
               int points, const std::string &group)
{
    json e = { { "id", id }, { "name", Clip(name, 80) }, { "description", Clip(desc, 300) },
               { "points", std::clamp(points, 0, 1000) } };
    if (!group.empty()) {
        e["group"] = Clip(group, 40);
    }
    return e;
}

} // namespace

void AchievementsLoad(const std::string &progress_path, const std::string &packs_dir)
{
    std::lock_guard<std::mutex> lk(g_lock);
    g_path = progress_path;
    g_packs_dir = packs_dir;
    LoadPacksLocked();
    std::ifstream f(progress_path, std::ios::binary);
    if (!f) {
        return;
    }
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        return;
    }
    try {
        // Version 1 had built-in achievements ("unlocked", "counters"): dropped
        if (j.contains("done") && j["done"].is_object()) {
            for (auto &g : j["done"].items()) {
                uint32_t tid;
                if (!g.value().is_object() || !ParseHexId(g.key(), &tid)) {
                    continue;
                }
                for (auto &kv : g.value().items()) {
                    if (kv.value().is_number_integer()) {
                        g_done[tid][kv.key()] = kv.value().get<int64_t>();
                    }
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

void AchievementsReloadPacks(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    LoadPacksLocked();
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

void AchievementsUpdate(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    // Save a little after something changes
    double now = ImGui::GetTime();
    if (g_dirty && now - g_last_save > 5.0) {
        g_last_save = now;
        SaveLocked();
    }
}

std::vector<uint32_t> AchievementsGames(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<uint32_t> v;
    for (auto &[tid, g] : g_games) {
        v.push_back(tid);
    }
    return v;
}

std::string AchievementsGameName(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_games.find(tid);
    return it == g_games.end() ? "" : it->second.name;
}

std::string AchievementsAuthors(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_games.find(tid);
    std::string s;
    if (it != g_games.end()) {
        for (auto &a : it->second.authors) {
            s += (s.empty() ? "" : ", ") + a;
        }
    }
    return s;
}

int AchievementCount(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_games.find(tid);
    return it == g_games.end() ? 0 : (int)it->second.list.size();
}

AchievementInfo AchievementAt(uint32_t tid, int i)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_games.find(tid);
    if (it == g_games.end() || i < 0 || i >= (int)it->second.list.size()) {
        return AchievementInfo();
    }
    AchievementInfo a = it->second.list[i];
    auto d = g_done.find(tid);
    if (d != g_done.end()) {
        auto k = d->second.find(a.id);
        if (k != d->second.end()) {
            a.done = true;
            a.done_at = k->second;
        }
    }
    return a;
}

namespace {
void CountFor(uint32_t tid, int *done, int *pts, int *pts_total, int *count)
{
    auto it = g_games.find(tid);
    if (it == g_games.end()) {
        return;
    }
    auto d = g_done.find(tid);
    for (auto &a : it->second.list) {
        bool is_done = d != g_done.end() && d->second.count(a.id);
        *done += is_done ? 1 : 0;
        *pts += is_done ? a.points : 0;
        *pts_total += a.points;
        *count += 1;
    }
}
} // namespace

int AchievementsDoneFor(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    CountFor(tid, &d, &p, &pt, &c);
    return d;
}

int AchievementsPointsFor(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    CountFor(tid, &d, &p, &pt, &c);
    return p;
}

int AchievementsPointsTotalFor(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    CountFor(tid, &d, &p, &pt, &c);
    return pt;
}

int AchievementsDoneAll(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    for (auto &[tid, g] : g_games) {
        CountFor(tid, &d, &p, &pt, &c);
    }
    return d;
}

int AchievementsCountAll(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    for (auto &[tid, g] : g_games) {
        CountFor(tid, &d, &p, &pt, &c);
    }
    return c;
}

int AchievementsPointsAll(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    int d = 0, p = 0, pt = 0, c = 0;
    for (auto &[tid, g] : g_games) {
        CountFor(tid, &d, &p, &pt, &c);
    }
    return p;
}

void AchievementsSet(uint32_t tid, const std::string &id, bool done)
{
    std::lock_guard<std::mutex> lk(g_lock);
    AchievementInfo *a = FindLocked(tid, id);
    if (!a) {
        return;
    }
    auto &m = g_done[tid];
    if (done && !m.count(id)) {
        m[id] = (int64_t)time(NULL);
        g_dirty = true;
        ShowToast(a->name, a->points);
    } else if (!done && m.count(id)) {
        m.erase(id);
        g_dirty = true;
    }
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

void AchievementsPreviewToast(uint32_t tid)
{
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_games.find(tid);
    if (it != g_games.end() && !it->second.list.empty()) {
        g_pending.push_back({ it->second.list[0].name, it->second.list[0].points, 0 });
    } else {
        g_pending.push_back({ tr("Example achievement"), 10, 0 });
    }
}

std::string AchievementsPacksDir(void)
{
    std::lock_guard<std::mutex> lk(g_lock);
    std::error_code ec;
    if (!g_packs_dir.empty()) {
        fs::create_directories(fs::u8path(g_packs_dir), ec);
    }
    return g_packs_dir;
}

int AchievementsParsePack(const std::string &text, uint32_t file_tid)
{
    std::map<uint32_t, Game> tmp;
    return ParsePackInto(text, file_tid, false, tmp);
}

std::string AchievementsAddMine(uint32_t tid, const std::string &game_name,
                                const std::string &name, const std::string &desc, int points,
                                const std::string &group)
{
    if (name.empty() || g_packs_dir.empty()) {
        return "";
    }
    std::lock_guard<std::mutex> lk(g_lock);
    json j = MineRead(tid, game_name);
    int n = 0;
    for (auto &e : j["achievements"]) {
        std::string id = e.is_object() ? Str(e, "id", 40) : "";
        if (id.rfind("mine-", 0) == 0) {
            n = std::max(n, atoi(id.c_str() + 5));
        }
    }
    std::string id = "mine-" + std::to_string(n + 1);
    j["achievements"].push_back(MineEntry(id, name, desc, points, group));
    if (!MineWrite(tid, j)) {
        return "";
    }
    LoadPacksLocked();
    return id;
}

bool AchievementsEditMine(uint32_t tid, const std::string &id, const std::string &name,
                          const std::string &desc, int points, const std::string &group)
{
    if (name.empty() || g_packs_dir.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    json j = MineRead(tid, "");
    bool found = false;
    for (auto &e : j["achievements"]) {
        if (e.is_object() && Str(e, "id", 40) == id) {
            e = MineEntry(id, name, desc, points, group);
            found = true;
        }
    }
    if (!found || !MineWrite(tid, j)) {
        return false;
    }
    LoadPacksLocked();
    return true;
}

bool AchievementsDeleteMine(uint32_t tid, const std::string &id)
{
    if (g_packs_dir.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    json j = MineRead(tid, "");
    json keep = json::array();
    bool found = false;
    for (auto &e : j["achievements"]) {
        if (e.is_object() && Str(e, "id", 40) == id) {
            found = true;
        } else {
            keep.push_back(e);
        }
    }
    if (!found) {
        return false;
    }
    j["achievements"] = keep;
    if (!MineWrite(tid, j)) {
        return false;
    }
    g_done[tid].erase(id);
    g_dirty = true;
    LoadPacksLocked();
    return true;
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
        std::string name = t.name;
        ImVec4 clip(tx, a.y, tx + max_w, b.y);
        dl->AddText(font, font->FontSize, ImVec2(tx, a.y + 12 * s + hs + 4 * s),
                    WithAlpha(th.text, k), name.c_str(), nullptr, 0, &clip);
        if (t.points > 0) {
            std::string pts = std::string("+") + std::to_string(t.points) + "  " + tr("points");
            dl->AddText(small, hs, ImVec2(tx, b.y - 12 * s - hs), WithAlpha(th.dim, k),
                        pts.c_str());
        }
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

bool AchievementsPackFileNameOk(const std::string &name)
{
    if (name.size() < 6 || name.size() > 96 || name[0] == '.') {
        return false;
    }
    for (unsigned char c : name) {
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    if (name.compare(name.size() - 5, 5, ".json") != 0) {
        return false;
    }
    // Never a file of your own, and never a leftover temporary
    if (name.size() >= 10 && name.compare(name.size() - 10, 10, "-mine.json") == 0) {
        return false;
    }
    return name.find(".tmp") == std::string::npos;
}

bool AchievementsParseCommunityIndex(const std::string &text, std::vector<CommunityPack> &out)
{
    out.clear();
    json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("packs") || !j["packs"].is_array()) {
        return false;
    }
    for (auto &e : j["packs"]) {
        if (out.size() >= 1000) {
            break;
        }
        if (!e.is_object()) {
            continue;
        }
        CommunityPack p;
        p.file = Str(e, "file", 96);
        if (!AchievementsPackFileNameOk(p.file)) {
            continue;
        }
        std::string id = Str(e, "title_id", 8);
        if (id.size() != 8 || !ParseHexId(id, &p.title_id)) {
            continue;
        }
        p.game = Str(e, "game", 80);
        p.author = Str(e, "author", 60);
        p.note = Str(e, "note", 160);
        if (e.contains("count") && e["count"].is_number_integer()) {
            long long n = e["count"].get<long long>();
            p.count = n < 0 ? 0 : (n > 500 ? 500 : (int)n);
        }
        out.push_back(p);
    }
    return true;
}

bool AchievementsPackInstalled(const CommunityPack &pack)
{
    std::string dir = AchievementsPacksDir();
    if (dir.empty() || !AchievementsPackFileNameOk(pack.file)) {
        return false;
    }
    std::error_code ec;
    return fs::exists(fs::u8path(dir) / fs::u8path(pack.file), ec);
}

std::string AchievementsInstallPack(const CommunityPack &pack, const std::string &text)
{
    if (!AchievementsPackFileNameOk(pack.file)) {
        return tr("That pack has a file name that isn't allowed");
    }
    if (text.size() > 1024 * 1024) {
        return tr("That pack is too big");
    }
    if (AchievementsParsePack(text, pack.title_id) <= 0) {
        return tr("That file isn't a usable achievement pack");
    }
    std::string dir = AchievementsPacksDir();
    if (dir.empty()) {
        return tr("The achievements folder isn't ready yet");
    }
    std::error_code ec;
    fs::path target = fs::u8path(dir) / fs::u8path(pack.file);
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(text.data(), (std::streamsize)text.size());
        if (!o) {
            return tr("Couldn't save the pack");
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return tr("Couldn't save the pack");
    }
    AchievementsReloadPacks();
    return "";
}

