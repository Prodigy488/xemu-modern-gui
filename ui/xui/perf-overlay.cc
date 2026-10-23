//
// xemu User Interface - Performance overlay
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "common.hh"
#include "perf-overlay.hh"
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
        ImGui::TextColored(col, "%u FPS", fps);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f ms (worst %d)", avg, worst);
        const char *renderer =
            g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN ? "Vulkan"
            : g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL
                ? "OpenGL"
                : "Null";
        ImGui::TextDisabled("%s  %ux  %s", renderer, nv2a_get_surface_scale_factor(),
                            FilterName(g_config.display.filtering));
        if (!runstate_is_running()) {
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1), "Paused");
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
