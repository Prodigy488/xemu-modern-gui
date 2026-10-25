//
// xemu User Interface - things for while you play
//
// Audio when xemu isn't the active window (keep playing, quieter or mute,
// with a short fade), the audio output device, and quick save / quick load
// from the controller (Back + LB, Back + RB).
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
// While a game has the controller (no menu open): the buttons held now
void PlayExtrasButtons(uint32_t buttons);

// Playback devices by name, for the settings (first: "" = system default)
std::vector<std::string> AudioOutputDevices(void);
// Switch to g_config.audio.output_device (after the setting changes)
void AudioOutputDeviceApply(void);
// An "Output device" combo box for the settings; true if changed
bool AudioOutputDeviceCombo(const char *label);
