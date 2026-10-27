//
// xemu User Interface - the address finder
//
// A window for finding where a running game keeps a number (lives, level,
// score, a "finished" flag) so an achievement can unlock by itself when it
// reaches a value. It works like a cheat search: look for a value, change it
// in the game, look again, and keep what fits. Experimental.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>

// Opens the window for the achievements of this game (title ID)
void AchFinderOpen(uint32_t title_id);
// Every frame, with the other windows
void AchFinderDraw(void);
