//
// xemu User Interface - per-game performance settings
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include "../xemu-game-perf.h"

struct GamePerfProfile {
    int cpu = -1;   // CPU boost: -1 as the global setting, 0 off, 1 on
    int gpu = -1;   // GPU boost
    int seed = -1;  // shader seeding
    int real = -1;  // real hardware speed
    int hz = 0;     // display refresh in Hz, 0 as the global setting
    bool safe = false; // started with the boosts off after a crash
};

// The game in the drive; false when none or its title could not be read
bool GamePerfCurrent(uint32_t &title_id, std::string &name);

// The game's profile (empty if it has none yet); call GamePerfChanged()
// after editing it
GamePerfProfile &GamePerfEdit();
void GamePerfChanged();
void GamePerfForget();

// A game's own display refresh by title ID, without it being loaded (0 as the
// global setting); this is what the game library's settings page edits. If the
// game is the one running, it applies at once
int GamePerfHzFor(uint32_t title_id);
void GamePerfSetHzFor(uint32_t title_id, int hz);

// The user's own display refresh in Hz (the global setting, not a game's),
// and a change of it from the settings: applies at once to a game that does
// not set its own refresh
int GamePerfGlobalHz();
void GamePerfSetGlobalHz(int hz);

// True when the profile asks for a CPU or GPU boost setting other than the
// one this run started with (those are read once, at start-up)
bool GamePerfRestartPending();
void GamePerfRestart();
