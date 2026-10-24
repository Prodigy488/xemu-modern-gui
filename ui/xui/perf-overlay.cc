//
// xemu User Interface - Performance overlay
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include <cmath>

#include "common.hh"
#include "perf-overlay.hh"
#include "dynamic-res.hh"
#include "viewport-manager.hh"

extern float g_main_menu_height;

static const char *FilterName(int filtering)
{
    switch (filtering) {
    case CONFIG_DISPLAY_FILTERING_NEAREST: return "Nearest";
    case CONFIG_DISPLAY_FILTERING_FSR: return "FSR 1.0";
    case CONFIG_DISPLAY_FILTERING_BICUBIC: return "Bicubic";
    case CONFIG_DISPLAY_FILTERING_LANCZOS: return "Lanczos";
    case CONFIG_DISPLAY_FILTERING_SGSR: return "Snapdragon GSR";
    case CONFIG_DISPLAY_FILTERING_NIS: return "NVIDIA NIS";
    case CONFIG_DISPLAY_FILTERING_RTX: return "NVIDIA RTX Video";
    default: return "Linear";
    }
}

void DrawPerformanceOverlay(void)
{
    if (!g_config.display.ui.show_perf_overlay) {
        return;
    }
    const float s = g_viewport_mgr.m_scale;

    // Frame time: average of the last 30 frames from the nv2a profiler
    const int n = 30;
    int sum = 0, worst = 0;
    unsigned int ptr = g_nv2a_stats.frame_ptr;
    for (int i = 1; i <= n; i++) {
        int mspf = g_nv2a_stats.frame_history[(ptr + NV2A_PROF_NUM_FRAMES - i) %
                                              NV2A_PROF_NUM_FRAMES].mspf;
        sum += mspf;
        worst = mspf > worst ? mspf : worst;
    }
    float avg = sum / (float)n;
    unsigned int fps = g_nv2a_stats.increment_fps;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 10 * s,
                                   vp->WorkPos.y + g_main_menu_height + 10 * s));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 6 * s));
    if (ImGui::Begin("##perf-overlay", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
        ImVec4 col = fps >= 55   ? ImVec4(0.45f, 0.9f, 0.3f, 1)
                     : fps >= 28 ? ImVec4(0.95f, 0.8f, 0.25f, 1)
                                 : ImVec4(1.0f, 0.45f, 0.4f, 1);
        ImGui::TextColored(col, tr("%u FPS"), fps);
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%.1f ms (worst %d)"), avg, worst);
        const char *renderer =
            g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN ? "Vulkan"
            : g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL
                ? "OpenGL"
                : "Null";
        if (int user = DynamicResUserScale()) {
            ImGui::TextDisabled(tr("%s  %ux (dynamic, set %dx)  %s"), renderer,
                                nv2a_get_surface_scale_factor(), user,
                                FilterName(g_config.display.filtering));
        } else {
            ImGui::TextDisabled("%s  %ux  %s", renderer,
                                nv2a_get_surface_scale_factor(),
                                FilterName(g_config.display.filtering));
        }
        if (!runstate_is_running()) {
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1), tr("Paused"));
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DrawShaderIndicator(void)
{
    // New shaders and pipelines per frame, from the nv2a profiler
    static unsigned int last_count = 0;
    static double last_activity = -100;
    static double recent_t[64];
    static int recent_n[64], recent_head = 0;
    unsigned int count = g_nv2a_stats.frame_count;
    unsigned int n = count - last_count;
    if (n > NV2A_PROF_NUM_FRAMES - 1) {
        n = NV2A_PROF_NUM_FRAMES - 1;
    }
    last_count = count;
    unsigned int ptr = g_nv2a_stats.frame_ptr;
    int built = 0;
    for (unsigned int k = 0; k < n; k++) {
        const auto &f =
            g_nv2a_stats.frame_history[(ptr + NV2A_PROF_NUM_FRAMES - n + k) %
                                       NV2A_PROF_NUM_FRAMES];
        built += f.counters[NV2A_PROF_SHADER_GEN] + f.counters[NV2A_PROF_PIPELINE_GEN];
    }
    double now = ImGui::GetTime();
    if (built > 0) {
        last_activity = now;
        recent_t[recent_head] = now;
        recent_n[recent_head] = built;
        recent_head = (recent_head + 1) % 64;
    }
    if (!g_config.display.ui.show_shader_indicator || !runstate_is_running()) {
        return;
    }
    // Visible while active, then gone after a short fade
    const double hold = 1.2, fade = 0.6;
    double since = now - last_activity;
    if (since > hold + fade) {
        return;
    }
    float alpha = since <= hold ? 1.0f : (float)(1.0 - (since - hold) / fade);
    int last_second = 0;
    for (int i = 0; i < 64; i++) {
        if (now - recent_t[i] <= 1.0) {
            last_second += recent_n[i];
        }
    }

    const float s = g_viewport_mgr.m_scale;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList(vp);
    char text[160];
    if (last_second > 0) {
        snprintf(text, sizeof(text), "%s  %d", tr("Preparing shaders"), last_second);
    } else {
        snprintf(text, sizeof(text), "%s", tr("Preparing shaders"));
    }
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.38f;
    ImVec2 p(vp->WorkPos.x + 16 * s, vp->WorkPos.y + vp->WorkSize.y - ts.y - 14 * s);
    ImVec2 a(p.x - 8 * s, p.y - 5 * s);
    ImVec2 b(p.x + r * 2 + 8 * s + ts.x + 8 * s, p.y + ts.y + 5 * s);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(120 * alpha)), 6 * s);
    // Spinner: a short arc going round while active
    ImVec2 c(p.x + r, p.y + ts.y / 2);
    float start = (float)std::fmod(now * 6.0, 6.283185307179586);
    dl->PathArcTo(c, r, start, start + 4.2f, 16);
    dl->PathStroke(IM_COL32(255, 255, 255, (int)(200 * alpha)), 0, 2.0f * s);
    dl->AddText(ImVec2(p.x + r * 2 + 8 * s, p.y),
                IM_COL32(255, 255, 255, (int)(215 * alpha)), text);
}
