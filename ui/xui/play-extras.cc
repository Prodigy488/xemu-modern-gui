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
#include "../xemu-notifications.h"
#include "../xemu-input.h"
#include "hw/xbox/mcpx/apu/apu_output.h"

#include <algorithm>
#include <cmath>

namespace {
float g_gain = 1.0f;         // what the audio is set to now
float g_gain_sent = -1.0f;   // last value passed on
uint32_t g_prev_buttons = 0;
uint32_t g_prev_triggers = 0;
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

void PlayExtrasButtons(uint32_t buttons, uint32_t triggers)
{
    uint32_t pressed = buttons & ~g_prev_buttons;
    uint32_t trig_pressed = triggers & ~g_prev_triggers;
    g_prev_buttons = buttons;
    g_prev_triggers = triggers;
    if (!g_config.input.quick_save_buttons || !(buttons & CONTROLLER_BUTTON_BACK)) {
        return;
    }
    bool save = false, load = false;
    switch (g_config.input.quick_save_combo) {
    case CONFIG_INPUT_QUICK_SAVE_COMBO_TRIGGERS:
        save = trig_pressed & 1;
        load = trig_pressed & 2;
        break;
    case CONFIG_INPUT_QUICK_SAVE_COMBO_STICKS:
        save = pressed & CONTROLLER_BUTTON_LSTICK;
        load = pressed & CONTROLLER_BUTTON_RSTICK;
        break;
    case CONFIG_INPUT_QUICK_SAVE_COMBO_DPAD:
        save = pressed & CONTROLLER_BUTTON_DPAD_LEFT;
        load = pressed & CONTROLLER_BUTTON_DPAD_RIGHT;
        break;
    default:
        // LB and RB are the Xbox controller's White and Black buttons
        save = pressed & CONTROLLER_BUTTON_WHITE;
        load = pressed & CONTROLLER_BUTTON_BLACK;
        break;
    }
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

std::string QuickSaveButtonName(bool load)
{
    // What the controller in the first port calls its buttons
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    for (int i = 0; i < 4; i++) {
        ControllerState *c = xemu_input_get_bound(i);
        if (c && c->type == INPUT_DEVICE_SDL_GAMEPAD && c->sdl_gamepad) {
            type = SDL_GetGamepadType(c->sdl_gamepad);
            break;
        }
    }
    bool ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
              type == SDL_GAMEPAD_TYPE_PS5;
    std::string back = "Back";
    if (type == SDL_GAMEPAD_TYPE_PS4) {
        back = "Share";
    } else if (type == SDL_GAMEPAD_TYPE_PS5) {
        back = "Create";
    } else if (type == SDL_GAMEPAD_TYPE_PS3) {
        back = "Select";
    } else if (type == SDL_GAMEPAD_TYPE_XBOXONE) {
        back = "View";
    }
    const char *second;
    switch (g_config.input.quick_save_combo) {
    case CONFIG_INPUT_QUICK_SAVE_COMBO_TRIGGERS:
        second = ps ? (load ? "R2" : "L2") : (load ? "RT" : "LT");
        break;
    case CONFIG_INPUT_QUICK_SAVE_COMBO_STICKS:
        second = load ? "R3" : "L3";
        break;
    case CONFIG_INPUT_QUICK_SAVE_COMBO_DPAD:
        second = load ? "D-pad right" : "D-pad left";
        break;
    default:
        second = ps ? (load ? "R1" : "L1") : (load ? "RB" : "LB");
        break;
    }
    return back + " + " + second;
}

bool PlayExtrasMouseCaptured(void)
{
    return xemu_input_mouse_is_captured();
}

static void MouseRelease(const char *why)
{
    if (!xemu_input_mouse_is_captured()) {
        return;
    }
    xemu_input_mouse_set_captured(false);
    SDL_Window *w = xemu_get_window();
    if (w) {
        SDL_SetWindowRelativeMouseMode(w, false);
    }
    if (why) {
        xemu_queue_notification(why);
    }
}

void PlayExtrasMouse(bool playing)
{
    SDL_Window *w = xemu_get_window();
    bool on = g_config.input.mouse.enabled;
    bool captured = xemu_input_mouse_is_captured();
    bool focused = !w || (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
    if (captured && (!on || !playing || !focused)) {
        MouseRelease(nullptr);
        return;
    }
    if (!on || !playing || !w) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    if (captured) {
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            MouseRelease(tr("Mouse released"));
        }
        return;
    }
    bool want = ImGui::IsKeyPressed(ImGuiKey_F3, false) ||
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) &&
                 !io.WantCaptureMouse && !io.WantCaptureKeyboard);
    if (!want) {
        return;
    }
    ControllerState *kbd = xemu_input_get_keyboard();
    if (!kbd || kbd->bound < 0) {
        xemu_queue_notification(
            tr("Bind the keyboard to a controller port first (Settings > Input)"));
        return;
    }
    xemu_input_mouse_set_captured(true);
    SDL_SetWindowRelativeMouseMode(w, true);
    xemu_queue_notification(tr("Mouse captured. F3 or Esc lets go."));
}

std::string KeyboardMouseLayoutApply(void)
{
    // SDL scancodes: letters A=4 ... Z=29, 1-4 = 30-33, Return 40, Tab 43,
    // Space 44, Left Shift 225
    auto &m = g_config.input.keyboard_controller_scancode_map;
    m.lstick_up = 26;    // W
    m.lstick_left = 4;   // A
    m.lstick_down = 22;  // S
    m.lstick_right = 7;  // D
    m.rstick_up = 82;    // arrow keys
    m.rstick_left = 80;
    m.rstick_down = 81;
    m.rstick_right = 79;
    m.a = 44;            // Space
    m.b = 8;             // E
    m.x = 21;            // R
    m.y = 20;            // Q
    m.white = 10;        // G
    m.black = 9;         // F
    m.lstick_btn = 225;  // Left Shift
    m.rstick_btn = 25;   // V
    m.start = 40;        // Return
    m.back = 43;         // Tab
    m.dpad_up = 30;      // 1-4
    m.dpad_down = 31;
    m.dpad_left = 32;
    m.dpad_right = 33;
    m.ltrigger = 29;     // Z
    m.rtrigger = 27;     // X
    g_config.input.mouse.enabled = true;
    int port = xemu_input_bind_keyboard();
    if (port < 0) {
        return tr("All four controller ports are in use. Unplug one, then try again.");
    }
    return string_format(tr("Keyboard and mouse ready on port %d. Click the game to "
                            "capture the mouse."),
                         port + 1);
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
