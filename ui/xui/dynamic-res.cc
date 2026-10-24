//
// xemu User Interface - dynamic resolution (experimental)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "dynamic-res.hh"

#include <string>

#include "common.hh"
#include "dynamic-res-logic.hh"
#include "viewport-manager.hh"

namespace
{
DynResLogic g_logic;
uint64_t g_last_sample = 0;
std::string g_message;
double g_message_t = -100;
int g_saved_scale = 0; // during a settings save

void Apply(const DynResLogic::Result &r)
{
    if (r.set_scale > 0) {
        nv2a_set_surface_scale_factor(r.set_scale);
    }
    if (!r.message.empty()) {
        g_message = r.message;
        g_message_t = ImGui::GetTime();
    }
}
} // namespace

void DynamicResUpdate(void)
{
    const char *dvd = g_config.sys.files.dvd_path;
    std::string game = dvd ? dvd : "";
    int scale = g_config.display.quality.surface_scale;
    if (!g_config.display.quality.dynamic_resolution) {
        int restore = g_logic.Release();
        if (restore && scale != restore) {
            nv2a_set_surface_scale_factor(restore);
        }
        return;
    }
    // A new game is noticed at once (before its own settings are applied);
    // the frame rate is sampled four times a second
    uint64_t now = SDL_GetTicks();
    if (now - g_last_sample < 250 && g_logic.SameGame(game)) {
        return;
    }
    g_last_sample = now;
    Apply(g_logic.Update((double)now, (int)g_nv2a_stats.increment_fps, scale,
                         runstate_is_running(), game));
}

void DynamicResDraw(void)
{
    double since = ImGui::GetTime() - g_message_t;
    if (since > 3.5 || g_message.empty()) {
        return;
    }
    float alpha = since < 3.0 ? 1.0f : (float)(1.0 - (since - 3.0) / 0.5);
    const float s = g_viewport_mgr.m_scale;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList(vp);
    ImVec2 ts = ImGui::CalcTextSize(g_message.c_str());
    // Above where the shader indicator goes
    ImVec2 p(vp->WorkPos.x + 16 * s,
             vp->WorkPos.y + vp->WorkSize.y - 2 * ts.y - 34 * s);
    dl->AddRectFilled(ImVec2(p.x - 8 * s, p.y - 5 * s),
                      ImVec2(p.x + ts.x + 8 * s, p.y + ts.y + 5 * s),
                      IM_COL32(0, 0, 0, (int)(120 * alpha)), 6 * s);
    dl->AddText(p, IM_COL32(255, 255, 255, (int)(215 * alpha)), g_message.c_str());
}

int DynamicResUserScale(void)
{
    return g_logic.Lowered() ? g_logic.UserScale() : 0;
}

void DynamicResBeforeSettingsSave(void)
{
    g_saved_scale = 0;
    if (g_logic.Lowered()) {
        g_saved_scale = g_config.display.quality.surface_scale;
        g_config.display.quality.surface_scale = g_logic.UserScale();
    }
}

void DynamicResAfterSettingsSave(void)
{
    if (g_saved_scale) {
        g_config.display.quality.surface_scale = g_saved_scale;
        g_saved_scale = 0;
    }
}
