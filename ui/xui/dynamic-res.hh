//
// xemu User Interface - dynamic resolution (experimental)
//
// When enabled (display.quality.dynamic_resolution), lowers the internal
// resolution while a game is running below its usual frame rate and raises
// it again when there's room; see dynamic-res-logic.hh.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Every frame, before the game library's update
void DynamicResUpdate(void);
// The brief "2x -> 1x" note in the lower left
void DynamicResDraw(void);
// The user's own resolution while it's lowered, else 0
int DynamicResUserScale(void);
// Around writing the settings file: save the user's resolution, not ours
void DynamicResBeforeSettingsSave(void);
void DynamicResAfterSettingsSave(void);
