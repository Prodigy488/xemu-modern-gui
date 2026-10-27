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
#include "perf-capture.hh"
#include "ui/xemu-notifications.h"
#include "common.hh"
#include "main-menu.hh"
#include "menubar.hh"
#include "misc.hh"
#include "widgets.hh"
#include "monitor.hh"
#include "debug.hh"
#include "actions.hh"
#include "compat.hh"
#include "xbox-drive-explorer.hh"
#include "game-library.hh"
#include "github-issue.hh"
#include "update.hh"
#include "../xemu-os-utils.h"

extern float g_main_menu_height; // FIXME

#ifdef CONFIG_RENDERDOC
bool g_capture_renderdoc_frame = false;
#endif

#if defined(__APPLE__)
#define SHORTCUT_MENU_TEXT(c) "Cmd+" #c
#else
#define SHORTCUT_MENU_TEXT(c) "Ctrl+" #c
#endif

void GameLibraryShowAllSettings(void)
{
    g_main_menu.ShowSettings();
}

void ProcessKeyboardShortcuts(void)
{
    if (IsShortcutKeyPressed(ImGuiKey_E)) {
        ActionEjectDisc();
    }

    if (IsShortcutKeyPressed(ImGuiKey_O)) {
        ActionLoadDisc();
    }

    if (IsShortcutKeyPressed(ImGuiKey_L)) {
        game_library_window.ToggleOpen();
    }

    if (IsShortcutKeyPressed(ImGuiKey_P)) {
        ActionTogglePause();
    }

    if (IsShortcutKeyPressed(ImGuiKey_R)) {
        ActionReset();
    }

    if (IsShortcutKeyPressed(ImGuiKey_Q)) {
        ActionShutdown();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent)) {
        monitor_window.ToggleOpen();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) {
        if (ImGui::GetIO().KeyShift) {
            PerfCaptureMarkGlitch(); // also takes a screenshot
        } else {
            ActionScreenshot();
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F11)) {
        xemu_toggle_fullscreen();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F9)) {
        if (ImGui::GetIO().KeyShift) {
            PerfCaptureToggleRecording();
        } else {
            g_config.display.ui.show_perf_overlay =
                !g_config.display.ui.show_perf_overlay;
        }
    }

#ifdef CONFIG_RENDERDOC
    if (ImGui::IsKeyPressed(ImGuiKey_F10) && nv2a_dbg_renderdoc_available()) {
        ImGuiIO& io = ImGui::GetIO();
        int num_frames = io.KeyShift ? 5 : 1;
        nv2a_dbg_renderdoc_capture_frames(num_frames, io.KeyCtrl);
    }
#endif
}

void ShowMainMenu()
{
    bool running = runstate_is_running();

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu(tr("Games")))
        {
            if (ImGui::MenuItem(tr("Game Library"), SHORTCUT_MENU_TEXT(L))) {
                game_library_window.Open();
            }
            if (ImGui::MenuItem(tr("Scan Folders"))) {
                game_library_window.Rescan();
            }
            if (ImGui::MenuItem(tr("Insignia (Play Online)..."))) {
                game_library_window.OpenInsignia();
            }
            if (ImGui::MenuItem(tr("Download Covers"))) {
                game_library_window.DownloadCovers();
            }
            if (ImGui::MenuItem(tr("Xbox Hard Drive..."))) {
                XboxExplorerOpenWindow();
            }
            if (ImGui::MenuItem(tr("Download Patches"))) {
                game_library_window.DownloadPatches();
            }
            if (ImGui::MenuItem(tr("Check for New Patches"))) {
                game_library_window.CheckNewPatches();
            }
            if (ImGui::MenuItem(tr("Add Game..."))) {
                game_library_window.AddGame();
            }
            if (ImGui::MenuItem(tr("Manage Folders..."))) {
                game_library_window.OpenFolderManager();
            }
            if (ImGui::BeginMenu(tr("Library Style"))) {
                int &style = g_config.general.library.style;
                if (ImGui::MenuItem(tr("Dashboard"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_DASHBOARD)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_DASHBOARD;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Classic"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_CLASSIC)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_CLASSIC;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Blades"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_BLADES)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_BLADES;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Metro"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_METRO)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_METRO;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Original Xbox"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_XBOX)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_XBOX;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Original Blades"), NULL,
                                    style == CONFIG_GENERAL_LIBRARY_STYLE_XBLADES)) {
                    style = CONFIG_GENERAL_LIBRARY_STYLE_XBLADES;
                    game_library_window.Open();
                }
                ImGui::Separator();
                ImGui::TextDisabled(tr("Dashboard theme"));
                int &theme = g_config.general.library.theme;
                if (ImGui::MenuItem(tr("Neon"), NULL,
                                    theme == CONFIG_GENERAL_LIBRARY_THEME_NEON)) {
                    theme = CONFIG_GENERAL_LIBRARY_THEME_NEON;
                    style = CONFIG_GENERAL_LIBRARY_STYLE_DASHBOARD;
                    game_library_window.Open();
                }
                if (ImGui::MenuItem(tr("Green"), NULL,
                                    theme == CONFIG_GENERAL_LIBRARY_THEME_GREEN)) {
                    theme = CONFIG_GENERAL_LIBRARY_THEME_GREEN;
                    style = CONFIG_GENERAL_LIBRARY_STYLE_DASHBOARD;
                    game_library_window.Open();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            auto recent = game_library_window.GetRecentGames(8);
            if (recent.empty()) {
                ImGui::MenuItem(tr("No recently played games"), NULL, false, false);
            } else {
                ImGui::TextDisabled(tr("Recently played"));
                for (auto &r : recent) {
                    ImGui::PushID(r.path.c_str());
                    if (ImGui::MenuItem(r.title.c_str())) {
                        game_library_window.Launch(r.path);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Machine")))
        {
            if (ImGui::MenuItem(running ? tr("Pause") : tr("Resume"), SHORTCUT_MENU_TEXT(P))) ActionTogglePause();
            if (ImGui::MenuItem(tr("Screenshot"), "F12")) ActionScreenshot();

            if (ImGui::BeginMenu(tr("Snapshot"))) {
                if (ImGui::MenuItem(tr("Create Snapshot"))) {
                    xemu_snapshots_save(NULL, NULL);
                    xemu_queue_notification(tr("Created new snapshot"));
                }

                for (int i = 0; i < 4; ++i) {
                    char *hotkey = g_strdup_printf("Shift+F%d", i + 5);

                    char *load_name;
                    char *save_name;

                    assert(g_snapshot_shortcut_index_key_map[i]);
                    bool bound = *(g_snapshot_shortcut_index_key_map[i]) &&
                            (**(g_snapshot_shortcut_index_key_map[i]) != 0);

                    if (bound) {
                        load_name = g_strdup_printf(tr("Load '%s'"), *(g_snapshot_shortcut_index_key_map[i]));
                        save_name = g_strdup_printf(tr("Save '%s'"), *(g_snapshot_shortcut_index_key_map[i]));
                    } else {
                        load_name = g_strdup_printf(tr("Load F%d (Unbound)"), i + 5);
                        save_name = g_strdup_printf(tr("Save F%d (Unbound)"), i + 5);
                    }

                    ImGui::Separator();

                    if (ImGui::MenuItem(load_name, hotkey + sizeof("Shift+") - 1, false, bound)) {
                        ActionActivateBoundSnapshot(i, false);
                    }

                    if (ImGui::MenuItem(save_name, hotkey, false, bound)) {
                        ActionActivateBoundSnapshot(i, true);
                    }

                    g_free(hotkey);
                    g_free(load_name);
                    g_free(save_name);
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem(tr("Eject Disc"), SHORTCUT_MENU_TEXT(E))) ActionEjectDisc();
            if (ImGui::MenuItem(tr("Load Disc..."), SHORTCUT_MENU_TEXT(O))) ActionLoadDisc();

            ImGui::Separator();

            if (ImGui::MenuItem(tr("Settings..."))) g_main_menu.ShowSettings();

            ImGui::Separator();

            if (ImGui::MenuItem(tr("Reset"), SHORTCUT_MENU_TEXT(R))) ActionReset();
            if (ImGui::MenuItem(tr("Exit"), SHORTCUT_MENU_TEXT(Q))) ActionShutdown();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("View")))
        {
            int ui_scale_idx;
            if (g_config.display.ui.auto_scale) {
                ui_scale_idx = 0;
            } else {
                ui_scale_idx = g_config.display.ui.scale;
                if (ui_scale_idx < 1) ui_scale_idx = 1;
                else if (ui_scale_idx > 2) ui_scale_idx = 2;
            }
            if (ImGui::Combo(tr("UI Scale"), &ui_scale_idx,
                             tr_items("Auto\0"
                             "1x\0"
                             "2x\0"))) {
                if (ui_scale_idx == 0) {
                    g_config.display.ui.auto_scale = true;
                } else {
                    g_config.display.ui.auto_scale = false;
                    g_config.display.ui.scale = ui_scale_idx;
                }
            }

            ImGui::Combo(tr("Backend"), &g_config.display.renderer,
                 tr_items("Null\0"
                          "OpenGL\0"
#ifdef CONFIG_VULKAN
                          "Vulkan\0"
#endif
                          ));

            int rendering_scale = nv2a_get_surface_scale_factor() - 1;
            if (ImGui::Combo(tr("Int. Resolution Scale"), &rendering_scale,
                             tr_items("1x\0"
                             "2x\0"
                             "3x\0"
                             "4x\0"
                             "5x\0"
                             "6x\0"
                             "7x\0"
                             "8x\0"
                             "9x\0"
                             "10x\0"))) {
                nv2a_set_surface_scale_factor(rendering_scale + 1);
            }

            ImGui::Combo(tr("Display Mode"), &g_config.display.ui.fit,
                         tr_items("Center\0Scale\0Stretch\0Integer (pixel-perfect)\0"));
            ImGui::SameLine();
            HelpMarker(tr("Controls how the rendered content should be scaled "
                       "into the window"));
            ImGui::Combo(tr("Filter Method"), &g_config.display.filtering,
                         tr_items("Linear\0Nearest\0AMD FSR 1.0\0Bicubic\0"
                         "Lanczos\0Snapdragon GSR\0NVIDIA NIS\0"
                         "NVIDIA RTX Video (AI)\0CRT TV\0Scanlines\0"));
            ImGui::Combo(tr("Aspect Ratio"), &g_config.display.ui.aspect_ratio,
                         tr_items("Native\0Auto\0""4:3\0""16:9\0"));
            if (ImGui::MenuItem(tr("Fullscreen"), "F11",
                                xemu_is_fullscreen(), true)) {
                xemu_toggle_fullscreen();
            }
            ImGui::MenuItem(tr("Performance Overlay"), "F9",
                            &g_config.display.ui.show_perf_overlay);
            ImGui::MenuItem(tr("Shader Indicator"), NULL,
                            &g_config.display.ui.show_shader_indicator);

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Debug")))
        {
            ImGui::MenuItem(tr("Monitor"), "~", &monitor_window.is_open);
            ImGui::MenuItem(tr("Audio"), NULL, &apu_window.m_is_open);
            ImGui::MenuItem(tr("Video"), NULL, &video_window.m_is_open);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Performance Capture..."), "Shift+F9")) {
                PerfCaptureOpenWindow();
            }
#ifdef CONFIG_RENDERDOC
            if (nv2a_dbg_renderdoc_available()) {
                ImGui::MenuItem(tr("RenderDoc: Capture"), NULL, &g_capture_renderdoc_frame);
            }
#endif
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(tr("Help")))
        {
            if (ImGui::MenuItem(tr("Help"), NULL)) {
                SDL_OpenURL("https://xemu.app/docs/getting-started/");
            }

            ImGui::MenuItem(tr("Report Compatibility..."), NULL,
                            &compatibility_reporter_window.is_open);

            ShowReportGitHubIssueMenuItem();
#if defined(_WIN32)
            ImGui::MenuItem(tr("Check for Updates..."), NULL, &update_window.is_open);
#endif

            ImGui::Separator();
            if (ImGui::MenuItem(tr("About"))) g_main_menu.ShowAbout();
            ImGui::EndMenu();
        }

        g_main_menu_height = ImGui::GetWindowHeight();
        ImGui::EndMainMenuBar();
    }
}
