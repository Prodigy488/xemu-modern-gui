//
// xemu User Interface - things for while you play
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "play-extras.hh"
#include "common.hh"
#include "game-library.hh"
#include "misc.hh"
#include "../xemu-input.h"
#include "hw/xbox/mcpx/apu/apu_output.h"

#include <algorithm>
#include <cmath>

namespace {
float g_gain = 1.0f;         // what the audio is set to now
float g_gain_sent = -1.0f;   // last value passed on
uint32_t g_prev_buttons = 0;
uint64_t g_last_quick_ms = 0;
} // namespace

void PlayExtrasUpdate(void)
{
    // Audio while another window is active: fade over about a quarter second
    float target = 1.0f;
    SDL_Window *w = xemu_get_window();
    bool focused = !w || (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
    if (!focused) {
        switch (g_config.audio.background) {
        case CONFIG_AUDIO_BACKGROUND_QUIETER:
            target = 0.25f;
            break;
        case CONFIG_AUDIO_BACKGROUND_MUTE:
            target = 0.0f;
            break;
        default:
            break;
        }
    }
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    float step = dt * 4.0f;
    if (fabsf(target - g_gain) <= step) {
        g_gain = target;
    } else {
        g_gain += target > g_gain ? step : -step;
    }
    // Small steps can wait; the end of a fade is always passed on
    if (g_gain != g_gain_sent &&
        (fabsf(g_gain - g_gain_sent) > 0.001f || g_gain == target)) {
        mcpx_apu_monitor_set_ui_gain(g_gain);
        g_gain_sent = g_gain;
    }
}

void PlayExtrasButtons(uint32_t buttons)
{
    uint32_t pressed = buttons & ~g_prev_buttons;
    g_prev_buttons = buttons;
    if (!g_config.input.quick_save_buttons || !(buttons & CONTROLLER_BUTTON_BACK)) {
        return;
    }
    // LB and RB are the Xbox controller's White and Black buttons
    bool save = pressed & CONTROLLER_BUTTON_WHITE;
    bool load = pressed & CONTROLLER_BUTTON_BLACK;
    if (!save && !load) {
        return;
    }
    // One at a time: saving takes a moment
    uint64_t now = SDL_GetTicks();
    if (now - g_last_quick_ms < 1500) {
        return;
    }
    g_last_quick_ms = now;
    GameLibraryQuickSave(load);
}

std::vector<std::string> AudioOutputDevices(void)
{
    std::vector<std::string> out = { "" };
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&count);
    for (int i = 0; ids && i < count; i++) {
        const char *n = SDL_GetAudioDeviceName(ids[i]);
        if (n && n[0] && std::find(out.begin(), out.end(), n) == out.end()) {
            out.push_back(n);
        }
    }
    SDL_free(ids);
    return out;
}

void AudioOutputDeviceApply(void)
{
    mcpx_apu_monitor_set_device(g_config.audio.output_device);
}

bool AudioOutputDeviceCombo(const char *label)
{
    // The list is only read while the box is open (devices come and go)
    static std::vector<std::string> devices;
    const char *cur = g_config.audio.output_device;
    std::string shown = (cur && cur[0]) ? cur : tr("Windows default");
    bool changed = false;
    if (ImGui::BeginCombo(label, shown.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            devices = AudioOutputDevices();
        }
        bool listed = false;
        for (auto &d : devices) {
            bool sel = d == (cur ? cur : "");
            listed |= sel;
            if (ImGui::Selectable(d.empty() ? tr("Windows default") : d.c_str(), sel)) {
                xemu_settings_set_string(&g_config.audio.output_device, d.c_str());
                AudioOutputDeviceApply();
                changed = true;
            }
        }
        if (!listed && cur && cur[0]) {
            // Chosen before but not plugged in now
            ImGui::BeginDisabled();
            ImGui::Selectable(string_format(tr("%s (not connected)"), cur).c_str(), true);
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    return changed;
}
