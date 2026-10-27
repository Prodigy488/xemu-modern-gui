//
// xemu User Interface - the address finder
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "ach-finder.hh"
#include "ach-memory.hh"
#include "achievements.hh"
#include "common.hh"
#include "game-library.hh"
#include "i18n.hh"
#include "misc.hh"
#include "osk.hh"
#include "viewport-manager.hh"
#include "xemu-guestmem.h"
#include "xemu-xbe.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Watch {
    uint32_t addr;
    AchSize size;
};

struct State {
    bool open = false;
    uint32_t tid = 0;          // the game whose achievements we're adding to
    // the game that's running
    uint32_t run_tid = 0;
    uint32_t run_build = 0;
    double next_id = 0;
    // search
    AchSearch search;
    int type = 2;              // index into kTypes
    char value[32] = "";
    std::string message;
    std::vector<Watch> watches;
    // "add to achievement" popup
    bool popup_request = false;
    Watch target{ 0, kAchU32 };
    int ach_index = 0;
    int op = 0;
    char cond_value[32] = "";
    bool any = false;
} g;

struct TypeInfo {
    const char *label;
    AchSize size;
};

const TypeInfo kTypes[] = {
    { N_("1 byte (0 to 255)"), kAchU8 },
    { N_("2 bytes (0 to 65535)"), kAchU16 },
    { N_("4 bytes (whole number)"), kAchU32 },
    { N_("4 bytes (can be negative)"), kAchI32 },
    { N_("Decimal number"), kAchF32 },
};

bool GuestRead(uint32_t addr, void *buf, uint32_t len, void *)
{
    return xemu_guest_read(addr, buf, len);
}

// A number typed by hand: 12, -3.5 or 0x1F
bool ParseTyped(const char *s, double *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end = nullptr;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        unsigned long v = strtoul(s, &end, 16);
        if (end && *end == 0 && end != s + 2) {
            *out = (double)v;
            return true;
        }
        return false;
    }
    double v = strtod(s, &end);
    if (end && *end == 0 && end != s && v == v && v > -1e15 && v < 1e15) {
        *out = v;
        return true;
    }
    return false;
}

std::string ShowValue(AchSize s, double v)
{
    char b[48];
    if (s == kAchF32) {
        snprintf(b, sizeof(b), "%g", v);
    } else if (v >= 0) {
        snprintf(b, sizeof(b), "%lld (0x%X)", (long long)v, (unsigned)(long long)v);
    } else {
        snprintf(b, sizeof(b), "%lld", (long long)v);
    }
    return b;
}

bool ReadNow(uint32_t addr, AchSize s, double *out)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (!xemu_guest_read(addr, b, AchSizeBytes(s))) {
        return false;
    }
    uint32_t raw = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    *out = AchRawToValue(s, raw);
    return true;
}

std::string Hex(uint32_t a)
{
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", a);
    return b;
}

// Which game is running, and its build (once a second)
void UpdateRunning()
{
    double now = ImGui::GetTime();
    if (now < g.next_id) {
        return;
    }
    g.next_id = now + 1.0;
    g.run_tid = 0;
    g.run_build = 0;
    if (!xemu_guest_available()) {
        return;
    }
    struct xbe *x = xemu_get_xbe_info();
    if (x && x->cert && x->header) {
        g.run_tid = x->cert->m_titleid;
        g.run_build = x->header->m_timedate;
    }
}

void DoSearch(bool first, bool unknown, AchScanKind kind)
{
    double v = 0;
    bool need_value = first ? !unknown : kind == kScanEqual;
    const AchSize size = first ? kTypes[g.type].size : g.search.Size();
    if (need_value && !ParseTyped(g.value, &v)) {
        g.message = tr("Type the value you are looking for first.");
        return;
    }
    xemu_guest_sync();
    long n;
    if (first) {
        static std::vector<XemuGuestPage> pages(40000);
        int count = xemu_guest_pages(pages.data(), (int)pages.size());
        std::vector<uint32_t> list;
        list.reserve(count);
        for (int i = 0; i < count; i++) {
            list.push_back(pages[i].vaddr);
        }
        n = g.search.First(size, unknown, v, list, GuestRead, nullptr);
    } else {
        n = g.search.Next(kind, v, GuestRead, nullptr);
    }
    if (n == -2) {
        g.message = tr("Couldn't read the game's memory. Is a game running?");
    } else if (n == -1) {
        g.message = first ? tr("Too many matches to list. Try a less common value, or search "
                               "without a value and use Changed / Increased / Decreased.")
                          : tr("Too many matches to list yet. Do something in the game, then "
                               "press Changed, Increased or Decreased again.");
    } else if (g.search.Remembering()) {
        g.message = tr("Remembered the game's memory. Now do something in the game that "
                       "changes your number, then press Changed, Increased or Decreased.");
    } else if (n == 0) {
        g.message = tr("Nothing matches. Start over and try again.");
    } else {
        g.message = string_format(tr("%d addresses left."), (int)n);
    }
}

void DrawAddPopup()
{
    const float s = g_viewport_mgr.m_scale;
    if (g.popup_request) {
        ImGui::OpenPopup(trid("Use as a trigger"));
        g.popup_request = false;
    }
    ImGui::SetNextWindowSize(ImVec2(520 * s, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(trid("Use as a trigger"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    // Your own achievements for the game
    std::vector<AchievementInfo> mine;
    for (int i = 0; i < AchievementCount(g.tid); i++) {
        AchievementInfo a = AchievementAt(g.tid, i);
        if (a.mine) {
            mine.push_back(a);
        }
    }
    ImGui::PushTextWrapPos(480 * s);
    ImGui::TextUnformatted(string_format(tr("The number at %s"), Hex(g.target.addr).c_str()).c_str());
    double cur;
    if (ReadNow(g.target.addr, g.target.size, &cur)) {
        ImGui::TextDisabled("%s", string_format(tr("Right now it is %s"),
                                                ShowValue(g.target.size, cur).c_str()).c_str());
    }
    ImGui::Dummy(ImVec2(0, 6 * s));
    if (mine.empty()) {
        ImGui::TextWrapped("%s", tr("This game has none of your own achievements yet. Close this, "
                                    "press \"Add an achievement\" in the Achievements tab, then "
                                    "come back."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Close"), ImVec2(120 * s, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    g.ach_index = std::clamp(g.ach_index, 0, (int)mine.size() - 1);
    ImGui::TextUnformatted(tr("Unlock this achievement"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##ach", mine[g.ach_index].name.c_str())) {
        for (int i = 0; i < (int)mine.size(); i++) {
            if (ImGui::Selectable(mine[i].name.c_str(), i == g.ach_index)) {
                g.ach_index = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::TextUnformatted(tr("when this number"));
    // 0-5 compare with a number; 6-8 are about a change since the last look
    const char *labels[] = { tr("is equal to"), tr("is not"), tr("is below"), tr("is at most"),
                             tr("is above"), tr("is at least"), tr("has gone up"),
                             tr("has gone down"), tr("has changed") };
    ImGui::SetNextItemWidth(180 * s);
    if (ImGui::BeginCombo("##op", labels[g.op])) {
        for (int i = 0; i < 9; i++) {
            if (ImGui::Selectable(labels[i], i == g.op)) {
                g.op = i;
            }
        }
        ImGui::EndCombo();
    }
    const bool by_change = g.op >= 6;
    if (!by_change) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140 * s);
        ImGui::InputText("##val", g.cond_value, sizeof(g.cond_value));
        OskTextBox();
    } else {
        ImGui::TextDisabled("%s", tr("Once the game has done this, it counts until you restart "
                                     "the game."));
    }
    ImGui::Checkbox(tr("Only one of several conditions needs to hold"), &g.any);
    ImGui::SetItemTooltip("%s", tr("Leave this off to need every condition you add. Turn it on "
                                   "for \"this or that\"."));
    ImGui::Dummy(ImVec2(0, 4 * s));
    ImGui::TextDisabled("%s", tr("The achievement unlocks when the conditions hold for about a "
                                 "third of a second, only while this version of the game runs. "
                                 "You can still tick it off yourself."));
    ImGui::PopTextWrapPos();

    double v = 0;
    bool ok = by_change || ParseTyped(g.cond_value, &v);
    ImGui::Dummy(ImVec2(0, 6 * s));
    ImGui::Separator();
    ImGui::BeginDisabled(!ok);
    if (ImGui::Button(tr("Add to achievement"), ImVec2(180 * s, 0))) {
        AchCond c;
        c.addr = g.target.addr;
        c.size = g.target.size;
        if (by_change) {
            c.vs_prev = true;
            c.op = g.op == 6 ? kAchGt : g.op == 7 ? kAchLt : kAchNe;
        } else {
            c.op = (AchOp)g.op;
            c.value = v;
        }
        if (AchievementsAddCondition(g.tid, mine[g.ach_index].id, c, g.any, g.run_build)) {
            g.message = string_format(tr("Added to \"%s\"."), mine[g.ach_index].name.c_str());
        } else {
            g.message = tr("Couldn't add it (an achievement can have up to 16 conditions).");
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), ImVec2(120 * s, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void AchFinderOpen(uint32_t title_id)
{
    if (g.tid != title_id) {
        g.search.Reset();
        g.watches.clear();
        g.message.clear();
    }
    g.tid = title_id;
    g.open = true;
    g.next_id = 0;
}

void AchFinderDraw(void)
{
    if (!g.open) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;
    ImGui::SetNextWindowSize(ImVec2(560 * s, 640 * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24 * s, 60 * s), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(trid("Address finder (experimental)"), &open)) {
        ImGui::End();
        g.open = open;
        return;
    }
    g.open = open;
    UpdateRunning();
    ImGui::PushTextWrapPos(0.0f);

    std::string target_name = GameLibraryTitleName(g.tid);
    if (target_name.empty()) {
        target_name = AchievementsGameName(g.tid);
    }
    ImGui::TextUnformatted(string_format(tr("Finding addresses for %s"),
                                         target_name.empty() ? Hex(g.tid).c_str()
                                                             : target_name.c_str()).c_str());
    const bool running = g.run_tid != 0;
    if (!running) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           tr("No game is running. Start it, then come back here."));
    } else if (g.run_tid != g.tid) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           tr("A different game is running. Start this one to search it."));
    } else {
        ImGui::TextDisabled("%s", string_format(tr("Game version %s"), Hex(g.run_build).c_str())
                                      .c_str());
    }
    if (ImGui::CollapsingHeader(tr("How this works"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BulletText("%s", tr("1. Find a number in the game that shows your goal: lives, "
                                   "level, score, items. Type its value and press Search."));
        ImGui::BulletText("%s", tr("2. Change it in the game (lose a life, pick up an item), "
                                   "type the new value and press Equals to narrow it down."));
        ImGui::BulletText("%s", tr("3. Repeat until only a few addresses are left. Press Watch on "
                                   "them and keep playing to see which one follows your number."));
        ImGui::BulletText("%s", tr("4. Press Use as a trigger on the right one and choose "
                                   "the achievement and the value that means you got it."));
        ImGui::TextDisabled("%s", tr("Don't know the value? Search without one, then use Changed, "
                                     "Unchanged, Increased and Decreased. If the mouse is "
                                     "captured by the game, press F3 to get it back."));
    }

    ImGui::Dummy(ImVec2(0, 4 * s));
    const bool can = running && g.run_tid == g.tid;
    ImGui::BeginDisabled(!can);
    if (!g.search.Active()) {
        ImGui::TextUnformatted(tr("What kind of number?"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##type", tr(kTypes[g.type].label))) {
            for (int i = 0; i < 5; i++) {
                if (ImGui::Selectable(tr(kTypes[i].label), i == g.type)) {
                    g.type = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", tr("If unsure, try 4 bytes first, then 1 byte or 2 bytes."));
    }
    ImGui::TextUnformatted(tr("Value"));
    ImGui::SetNextItemWidth(160 * s);
    ImGui::InputText("##value", g.value, sizeof(g.value));
    OskTextBox();
    ImGui::SameLine();
    if (!g.search.Active()) {
        if (ImGui::Button(tr("Search"), ImVec2(110 * s, 0))) {
            DoSearch(true, false, kScanEqual);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Search without a value"), ImVec2(0, 0))) {
            DoSearch(true, true, kScanEqual);
        }
    } else {
        if (ImGui::Button(tr("Equals"), ImVec2(110 * s, 0))) {
            DoSearch(false, false, kScanEqual);
        }
        if (ImGui::Button(tr("Changed"), ImVec2(110 * s, 0))) {
            DoSearch(false, false, kScanChanged);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Unchanged"), ImVec2(110 * s, 0))) {
            DoSearch(false, false, kScanUnchanged);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Increased"), ImVec2(110 * s, 0))) {
            DoSearch(false, false, kScanIncreased);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Decreased"), ImVec2(110 * s, 0))) {
            DoSearch(false, false, kScanDecreased);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Start over"), ImVec2(0, 0))) {
            g.search.Reset();
            g.message.clear();
        }
    }
    ImGui::EndDisabled();
    if (!g.message.empty()) {
        ImGui::TextWrapped("%s", g.message.c_str());
    }

    // What's left
    if (g.search.Active() && !g.search.Remembering()) {
        const auto &hits = g.search.Hits();
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(string_format(tr("%d addresses"), (int)hits.size()).c_str());
        if (hits.size() > 200) {
            ImGui::TextDisabled("%s", tr("Too many to show yet: narrow it down first."));
        } else {
            xemu_guest_sync();
            ImGui::BeginChild("##hits", ImVec2(0, 160 * s), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < hits.size(); i++) {
                ImGui::PushID((int)i);
                double now = 0;
                bool rd = ReadNow(hits[i].addr, g.search.Size(), &now);
                ImGui::Text("%s   %s", Hex(hits[i].addr).c_str(),
                            rd ? ShowValue(g.search.Size(), now).c_str() : "?");
                ImGui::SameLine(ImGui::GetContentRegionMax().x - 70 * s);
                if (ImGui::SmallButton(tr("Watch"))) {
                    if (g.watches.size() < 20) {
                        g.watches.push_back({ hits[i].addr, g.search.Size() });
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }

    // Addresses being watched
    if (!g.watches.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::TextUnformatted(tr("Watching"));
        xemu_guest_sync();
        int remove = -1;
        for (size_t i = 0; i < g.watches.size(); i++) {
            const Watch &w = g.watches[i];
            ImGui::PushID((int)i);
            double now = 0;
            bool rd = ReadNow(w.addr, w.size, &now);
            ImGui::Text("%s   %s", Hex(w.addr).c_str(), rd ? ShowValue(w.size, now).c_str() : "?");
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 230 * s);
            if (ImGui::SmallButton(tr("Use as a trigger"))) {
                g.target = w;
                g.popup_request = true;
                g.op = 0;
                if (rd) {
                    snprintf(g.cond_value, sizeof(g.cond_value), "%g", now);
                } else {
                    g.cond_value[0] = 0;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Copy"))) {
                AchCond c;
                c.addr = w.addr;
                c.size = w.size;
                c.value = rd ? now : 0;
                ImGui::SetClipboardText(AchCondToJson(c).dump().c_str());
                g.message = tr("Copied the condition as text, for a pack file.");
            }
            ImGui::SetItemTooltip("%s", tr("Copies this address as a condition you can paste into "
                                           "a pack file's \"trigger\"."));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Remove"))) {
                remove = (int)i;
            }
            ImGui::PopID();
        }
        if (remove >= 0) {
            g.watches.erase(g.watches.begin() + remove);
        }
    }

    // The triggers already on your achievements for this game
    ImGui::Dummy(ImVec2(0, 6 * s));
    if (ImGui::CollapsingHeader(tr("Triggers on your achievements"))) {
        bool any = false;
        for (int i = 0; i < AchievementCount(g.tid); i++) {
            AchievementInfo a = AchievementAt(g.tid, i);
            if (!a.trigger) {
                continue;
            }
            any = true;
            ImGui::PushID(i);
            ImGui::TextUnformatted(a.name.c_str());
            for (auto &c : a.trigger->all) {
                ImGui::BulletText("%s", AchCondText(c).c_str());
            }
            for (auto &c : a.trigger->any) {
                ImGui::BulletText("%s %s", tr("one of:"), AchCondText(c).c_str());
            }
            if (a.mine && ImGui::SmallButton(tr("Remove trigger"))) {
                AchievementsClearTrigger(g.tid, a.id);
            }
            ImGui::PopID();
        }
        if (!any) {
            ImGui::TextDisabled("%s", tr("None yet."));
        }
    }
    ImGui::PopTextWrapPos();
    DrawAddPopup();
    ImGui::End();
}
