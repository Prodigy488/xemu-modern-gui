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
