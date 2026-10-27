//
// xemu User Interface - things for while you play
//
// Audio when xemu isn't the active window (keep playing, quieter or mute,
// with a short fade), the audio output device, and quick save / quick load
// from the controller (Back + two buttons, your choice of which).
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Every frame
void PlayExtrasUpdate(void);
// While a game has the controller (no menu open): the buttons held now, and
// which triggers are pulled (bit 0 LT, bit 1 RT)
void PlayExtrasButtons(uint32_t buttons, uint32_t triggers);
// Every frame; playing = a game has the controller and no menu is open.
// Captures the mouse for mouse-and-keyboard play (click the game or F3) and
// lets go when a menu opens, the window loses focus, F3 or Esc is pressed.
void PlayExtrasMouse(bool playing);
bool PlayExtrasMouseCaptured(void);
// Keyboard keys and mouse settings, as one piece: what a game can have of its
// own. keys follow g_keyboard_scancode_map (25 SDL scancodes).
struct InputProfile {
    int keys[25] = {};
    bool mouse_on = false;
    int mouse_stick = 0;       // 0 right stick, 1 left stick
    float mouse_sens = 1;
    bool mouse_invert = false;
    // Left, right, middle, side 1, side 2, wheel up, wheel down: what each
    // does (0 nothing, 1 A ... 14 D-pad right, 15 LT, 16 RT)
    int mouse_btn[7] = { 16, 15, 8, 0, 0, 0, 0 };
    bool operator==(const InputProfile &o) const;
    bool operator!=(const InputProfile &o) const { return !(*this == o); }
};
InputProfile InputProfileGet(void);          // what is set now
void InputProfileSet(const InputProfile &p);
InputProfile InputProfileWasd(void);         // the WASD + mouse layout
// Starting points by kind of game. 1 shooter or action (WASD + mouse), 2
// racing, 3 fighting, 4 sports and platformers (keyboard only). Each sets
// keys and mouse; nothing is claimed about any one game's own button use.
int InputPresetCount(void);
const char *InputPresetName(int preset);     // translated
InputProfile InputPresetProfile(int preset);
// A preset that suits a game by its name (franchises I know), or 0 if none
int InputPresetSuggest(const std::string &game_name);
InputProfile InputProfileDefault(void);      // xemu's own keys, mouse as it ships
int InputKeyCount(void);
// "A", "Left Stick Up"... (translated), for the 25 keys
const char *InputKeyName(int i);
const char *MouseButtonName(int i);          // "Left click"... (translated)
int MouseTargetCount(void);
const char *MouseTargetName(int t);          // "Nothing", "A"... (translated)
// Set the keyboard to a WASD layout, turn the mouse on and bind the keyboard
// to a port; the message to show
std::string KeyboardMouseLayoutApply(void);

// The buttons for quick save / quick load as the connected controller calls
// them: "View + LB" on an Xbox One pad, "Share + L1" on a PS4 pad
std::string QuickSaveButtonName(bool load);

// Playback devices by name, for the settings (first: "" = system default)
std::vector<std::string> AudioOutputDevices(void);
// Switch to g_config.audio.output_device (after the setting changes)
void AudioOutputDeviceApply(void);
// An "Output device" combo box for the settings; true if changed
bool AudioOutputDeviceCombo(const char *label);
