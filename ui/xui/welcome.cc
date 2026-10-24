//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "ui/xui/viewport-manager.hh"
#include "common.hh"
#include "imgui.h"
#include "viewport-manager.hh"
#include "welcome.hh"
#include <algorithm>
#include <string>
#include "game-library.hh"
#include "widgets.hh"
#include "misc.hh"
#include "gl-helpers.hh"
#include "xemu-version.h"
#include "main-menu.hh"

FirstBootWindow::FirstBootWindow()
{
    is_open = false;
}

void FirstBootWindow::Draw()
{
    if (!is_open) return;

    ImVec2 size(400*g_viewport_mgr.m_scale, 300*g_viewport_mgr.m_scale);
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 window_pos = ImVec2((io.DisplaySize.x - size.x)/2, (io.DisplaySize.y - size.y)/2);
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);

    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    if (!ImGui::Begin("First Boot", &is_open, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    Logo();

    const char *msg = tr("Configure machine settings to get started");
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2);
    ImGui::Text("%s", msg);

    ImGui::Dummy(ImVec2(0,14*g_viewport_mgr.m_scale));

    // Language first: it starts in Windows' language, and each one is
    // listed by its own name so it can be found from any other
    {
        float cw = 260*g_viewport_mgr.m_scale;
        const char *label = XuiLanguageLabel();
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(label).x)/2);
        ImGui::TextDisabled("%s", label);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-cw)/2);
        ImGui::SetNextItemWidth(cw);
        int lang = XuiLanguageSettingIndex();
        std::string cur = XuiLanguageItemName(lang);
        if (ImGui::BeginCombo("##welcome-language", cur.c_str())) {
            for (int i = 0; i < (int)XuiLanguages().size(); i++) {
                std::string name = XuiLanguageItemName(i);
                if (ImGui::Selectable(name.c_str(), i == lang)) {
                    XuiLanguageChoose(i);
                }
                if (i == lang) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Dummy(ImVec2(0,16*g_viewport_mgr.m_scale));

    // Guided setup on the dashboard, or xemu's own settings
    float gap = 10*g_viewport_mgr.m_scale;
    float bw = std::max(120*g_viewport_mgr.m_scale,
                        std::max(ImGui::CalcTextSize(tr("Guided setup")).x,
                                 ImGui::CalcTextSize(tr("Settings")).x) +
                            ImGui::GetStyle().FramePadding.x * 4);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-2*bw-gap)/2);
    if (ImGui::Button(tr("Guided setup"), ImVec2(bw, 0))) {
        GameLibraryOpenSetup();
        g_config.general.show_welcome = false;
        is_open = false;
    }
    ImGui::SameLine(0, gap);
    if (ImGui::Button(tr("Settings"), ImVec2(bw, 0))) {
        g_main_menu.ShowSystem();
        g_config.general.show_welcome = false;
    }

    ImGui::Dummy(ImVec2(0,50*g_viewport_mgr.m_scale));

    msg = tr("Visit https://xemu.app for more information");
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(msg).x)/2);
    Hyperlink(msg, "https://xemu.app");

    ImGui::Dummy(ImVec2(400*g_viewport_mgr.m_scale,20*g_viewport_mgr.m_scale));

    ImGui::End();
}

FirstBootWindow first_boot_window;
