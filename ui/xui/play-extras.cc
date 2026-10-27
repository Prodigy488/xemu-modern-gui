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
    /* With "Keyboard, mouse and controller" the mouse is taken as soon as
     * you're playing, so there's nothing to turn on or click. F3 or Esc lets
     * go and a click takes it back. */
    static bool let_go = false;
    if (captured) {
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            let_go = true;
            MouseRelease(tr("Mouse released"));
        }
        return;
    }
    bool want = ImGui::IsKeyPressed(ImGuiKey_F3, false) ||
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left, false) &&
                 !io.WantCaptureMouse && !io.WantCaptureKeyboard);
    bool automatic = !let_go && !want && focused && xemu_input_combined_active() &&
                     !io.WantCaptureMouse && !io.WantCaptureKeyboard;
    if (!want && !automatic) {
        return;
    }
    if (want) {
        let_go = false;
    }
    ControllerState *kbd = xemu_input_get_keyboard();
    if (!kbd || (kbd->bound < 0 && !xemu_input_auto_switch_standby())) {
        if (want) {
            xemu_queue_notification(
                tr("Bind the keyboard to a controller port first (Settings > Input)"));
        }
        return;
    }
    xemu_input_mouse_set_captured(true);
    SDL_SetWindowRelativeMouseMode(w, true);
    static bool told = false;
    if (want || !told) {
        told = true;
        xemu_queue_notification(automatic ? tr("Mouse captured for play. F3 lets go, a click takes it back.")
                                          : tr("Mouse captured. F3 or Esc lets go."));
    }
}

// ---------------------------------------------------------------------------
// Keyboard and mouse setups: one for everything, and one per game
// ---------------------------------------------------------------------------

namespace {
int *MouseBtnField(int i)
{
    auto &m = g_config.input.mouse;
    int *f[7] = { &m.left, &m.right, &m.middle, &m.x1, &m.x2, &m.wheel_up, &m.wheel_down };
    return f[i];
}
} // namespace

bool InputProfile::operator==(const InputProfile &o) const
{
    return std::equal(keys, keys + 25, o.keys) && mouse_on == o.mouse_on &&
           mouse_stick == o.mouse_stick && mouse_sens == o.mouse_sens &&
           mouse_invert == o.mouse_invert && std::equal(mouse_btn, mouse_btn + 7, o.mouse_btn);
}

InputProfile InputProfileGet(void)
{
    InputProfile p;
    for (int i = 0; i < 25; i++) {
        p.keys[i] = *g_keyboard_scancode_map[i];
    }
    p.mouse_on = g_config.input.mouse.enabled;
    p.mouse_stick = g_config.input.mouse.stick == CONFIG_INPUT_MOUSE_STICK_LEFT ? 1 : 0;
    p.mouse_sens = g_config.input.mouse.sensitivity;
    p.mouse_invert = g_config.input.mouse.invert_y;
    for (int i = 0; i < 7; i++) {
        p.mouse_btn[i] = *MouseBtnField(i);
    }
    return p;
}

void InputProfileSet(const InputProfile &p)
{
    for (int i = 0; i < 25; i++) {
        *g_keyboard_scancode_map[i] = p.keys[i];
    }
    g_config.input.mouse.enabled = p.mouse_on;
    g_config.input.mouse.stick =
        p.mouse_stick ? CONFIG_INPUT_MOUSE_STICK_LEFT : CONFIG_INPUT_MOUSE_STICK_RIGHT;
    g_config.input.mouse.sensitivity = p.mouse_sens;
    g_config.input.mouse.invert_y = p.mouse_invert;
    for (int i = 0; i < 7; i++) {
        *MouseBtnField(i) = p.mouse_btn[i];
    }
}

InputProfile InputProfileDefault(void)
{
    // In the order of g_keyboard_scancode_map; SDL scancodes
    static const int keys[25] = {
        4, 5, 27, 28,        // A B X Y: a b x y
        42, 34, 40,          // Back, Guide, Start: backspace, 5, return
        32, 33,              // stick buttons: 3 4
        30, 31,              // White, Black: 1 2
        82, 81, 80, 79,      // D-pad: up down left right arrows
        8, 22, 9, 7,         // left stick: e s f d
        26,                  // left trigger: w
        12, 13, 15, 14,      // right stick: i j l k
        18,                  // right trigger: o
    };
    InputProfile p;
    std::copy(keys, keys + 25, p.keys);
    return p;
}

InputProfile InputProfileWasd(void)
{
    // SDL scancodes: letters A=4 ... Z=29, 1-4 = 30-33, Return 40, Tab 43,
    // Space 44, Left Shift 225
    InputProfile p = InputProfileDefault();
    int *k = p.keys;
    k[0] = 44;   // A: Space
    k[1] = 8;    // B: E
    k[2] = 21;   // X: R
    k[3] = 20;   // Y: Q
    k[4] = 43;   // Back: Tab
    k[6] = 40;   // Start: Return
    k[7] = 225;  // left stick button: Left Shift
    k[8] = 25;   // right stick button: V
    k[9] = 10;   // White: G
    k[10] = 9;   // Black: F
    k[11] = 30;  // D-pad: 1-4
    k[12] = 31;
    k[13] = 32;
    k[14] = 33;
    k[15] = 26;  // left stick: W A S D
    k[16] = 4;
    k[17] = 7;
    k[18] = 22;
    k[19] = 29;  // left trigger: Z
    k[20] = 82;  // right stick: the arrow keys
    k[21] = 80;
    k[22] = 79;
    k[23] = 81;
    k[24] = 27;  // right trigger: X
    p.mouse_on = true;
    return p;
}

int InputPresetCount(void)
{
    return 4;
}

const char *InputPresetName(int preset)
{
    switch (preset) {
    case 1: return tr("Shooter or action (WASD + mouse)");
    case 2: return tr("Racing");
    case 3: return tr("Fighting");
    case 4: return tr("Sports and platformers (keyboard only)");
    }
    return "";
}

InputProfile InputPresetProfile(int preset)
{
    // SDL scancodes: A=4 ... Z=29, 1-4 = 30-33, Return 40, Tab 43, Space 44,
    // arrows right 79 left 80 down 81 up 82, Left Shift 225
    InputProfile p = InputProfileDefault();
    int *k = p.keys;
    auto arrows = [&](int up, int left, int right, int down) {
        k[up] = 82; k[left] = 80; k[right] = 79; k[down] = 81;
    };
    switch (preset) {
    case 1:
        return InputProfileWasd();
    case 2: // racing: A / D steer, W accelerate, S brake and reverse
        k[15] = 26; k[16] = 4; k[17] = 7; k[18] = 22; // left stick: W A S D
        k[15] = 0;   // stick up and down aren't used for steering
        k[18] = 0;
        k[24] = 26;  // right trigger (gas): W
        k[19] = 22;  // left trigger (brake): S
        k[0] = 44;   // A: Space
        k[1] = 5;    // B: B
        k[2] = 27;   // X: X
        k[3] = 28;   // Y: Y
        k[9] = 20;   // White: Q (shift down)
        k[10] = 8;   // Black: E (shift up)
        k[6] = 40;   // Start: Return
        k[4] = 43;   // Back: Tab
        arrows(20, 21, 22, 23); // right stick (camera): arrow keys
        k[11] = 30; k[12] = 31; k[13] = 32; k[14] = 33;
        k[7] = 225;  // left stick button: Left Shift
        k[8] = 25;   // right stick button: V
        p.mouse_on = false;
        return p;
    case 3: // fighting: D-pad on WASD, buttons on the right hand
        k[11] = 26; k[13] = 4; k[14] = 7; k[12] = 22; // D-pad: W A D S
        k[0] = 13;   // A: J
        k[1] = 14;   // B: K
        k[2] = 24;   // X: U
        k[3] = 12;   // Y: I
        k[9] = 18;   // White: O
        k[10] = 15;  // Black: L
        k[19] = 20;  // left trigger: Q
        k[24] = 8;   // right trigger: E
        arrows(15, 16, 17, 18); // left stick: arrow keys
        k[20] = k[21] = k[22] = k[23] = 0; // right stick: not used
        k[6] = 40; k[4] = 43;
        p.mouse_on = false;
        return p;
    case 4: // keyboard only: arrows and Z X C V
        arrows(15, 16, 17, 18);
        k[0] = 29;   // A: Z
        k[1] = 27;   // B: X
        k[2] = 6;    // X: C
        k[3] = 25;   // Y: V
        k[9] = 4;    // White: A
        k[10] = 22;  // Black: S
        k[19] = 20;  // left trigger: Q
        k[24] = 8;   // right trigger: E
        k[11] = 30; k[12] = 31; k[13] = 32; k[14] = 33;
        k[7] = 225;  // left stick button: Left Shift
        k[8] = 0;
        k[6] = 40; k[4] = 43;
        k[20] = 12;  // right stick: I J L K k[21] = 13; k[22] = 15; k[23] = 14;
        p.mouse_on = false;
        return p;
    }
    return p;
}

int InputPresetSuggest(const std::string &game_name)
{
    std::string n = game_name;
    for (auto &c : n) {
        c = (char)tolower((unsigned char)c);
    }
    auto has = [&](std::initializer_list<const char *> words) {
        for (const char *w : words) {
            if (n.find(w) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    if (has({ "forza", "project gotham", "burnout", "need for speed", "midtown madness",
              "rallisport", "colin mcrae", "outrun", "out run", "toca ", "midnight club",
              "wreckless", "flatout", "juiced", "racing", "rally", "motogp", "moto gp",
              "mx unleashed", "mx vs", "atv ", "crash 'n' burn", "test drive", "sega gt",
              "rpm tuning", "pro race driver" })) {
        return 2;
    }
    if (has({ "soulcalibur", "soul calibur", "dead or alive", "mortal kombat",
              "virtua fighter", "tekken", "street fighter", "ultimate fighting",
              "smackdown", "def jam", "fight night", "wrestle", "wwe", "tao feng",
              "kakuto", "capcom vs", "king of fighters" })) {
        return 3;
    }
    if (has({ "halo", "doom", "quake", "unreal", "half-life", "far cry", "counter-strike",
              "brothers in arms", "call of duty", "medal of honor", "ghost recon",
              "rainbow six", "timesplitters", "red faction", "turok", "goldeneye",
              "battlefront", "republic commando", "perfect dark", "condemned",
              "splinter cell", "ninja gaiden", "chronicles of riddick",
              "mercenaries", "crimson skies", "armed and dangerous", "psi-ops",
              "conflict: ", "area 51", "blood rayne", "bloodrayne", "painkiller",
              "project snowblind", "wolfenstein", "tom clancy" })) {
        return 1;
    }
    if (has({ "madden", "nba ", "nhl ", "fifa", "mlb", "tennis", "golf", "tiger woods",
              "ssx", "tony hawk", "amped", "pro evolution", "pes ", "sonic", "crash ",
              "spyro", "jet set radio", "rayman", "sly cooper", "kung fu chaos",
              "lego", "shrek", "spongebob" })) {
        return 4;
    }
    return 0;
}

int InputKeyCount(void)
{
    return 25;
}

const char *InputKeyName(int i)
{
    static const char *const names[25] = {
        "A", "B", "X", "Y", N_("Back"), N_("Guide"), N_("Start"),
        N_("Left Stick Button"), N_("Right Stick Button"), N_("White"), N_("Black"),
        N_("DPad Up"), N_("DPad Down"), N_("DPad Left"), N_("DPad Right"),
        N_("Left Stick Up"), N_("Left Stick Left"), N_("Left Stick Right"),
        N_("Left Stick Down"), N_("Left Trigger"),
        N_("Right Stick Up"), N_("Right Stick Left"), N_("Right Stick Right"),
        N_("Right Stick Down"), N_("Right Trigger"),
    };
    return tr(names[std::clamp(i, 0, 24)]);
}

const char *MouseButtonName(int i)
{
    static const char *const names[7] = {
        N_("Left click"), N_("Right click"), N_("Middle click"), N_("Side button 1"),
        N_("Side button 2"), N_("Wheel up"), N_("Wheel down"),
    };
    return tr(names[std::clamp(i, 0, 6)]);
}

int MouseTargetCount(void)
{
    return 17;
}

const char *MouseTargetName(int t)
{
    static const char *const names[17] = {
        N_("Nothing"), "A", "B", "X", "Y", N_("Back"), N_("Start"),
        N_("Left Stick Button"), N_("Right Stick Button"), N_("White"), N_("Black"),
        N_("DPad Up"), N_("DPad Down"), N_("DPad Left"), N_("DPad Right"),
        N_("Left Trigger"), N_("Right Trigger"),
    };
    return tr(names[std::clamp(t, 0, 16)]);
}

std::string KeyboardMouseLayoutApply(void)
{
    InputProfileSet(InputProfileWasd());
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
