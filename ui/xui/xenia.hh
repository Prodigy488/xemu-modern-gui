//
// xemu User Interface - Xbox 360 games through Xenia
//
// xemu does not emulate the Xbox 360. This module lets the game library list
// Xbox 360 games (disc images, extracted games, Games on Demand and Xbox Live
// Arcade packages) and start them in the user's own copy of Xenia / Xenia
// Canary. Xenia is never bundled or modified.
//
// Pure C++17 with no emulator dependencies.
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

struct XeniaSettings {
    bool enabled = false;       // list Xbox 360 games in the library
    std::string exe;            // path to xenia.exe / xenia_canary.exe
    bool fullscreen = true;     // pass --fullscreen=true
    std::string extra_args;     // appended as given
    bool pause_xemu = true;     // pause a running Xbox game while Xenia runs
};

extern XeniaSettings g_xenia;

struct X360TitleInfo {
    bool ok = false;
    uint32_t title_id = 0;
    std::string title_name;        // may be empty (discs and .xex have none)
    std::string kind;              // "xex", "iso", "god"
    std::vector<uint8_t> icon_png; // PNG from the package, may be empty
    std::string error;
};

// True for file names that could be an Xbox 360 game and are worth opening:
// .xex, .iso, .zar, or no extension at all (Games on Demand headers).
bool X360LooksLikeCandidate(const std::string &filename);

// Read what can be known about a 360 game without running it.
bool X360ReadTitleInfo(const std::string &path, X360TitleInfo &out);

// "Halo 3" from "Halo_3 (USA) (En,Fr) [disc 1].iso"
std::string X360NameFromFile(const std::string &path);

// Title ID of a XEX image in memory; false if it isn't one.
bool X360ParseXex(const uint8_t *data, size_t len, uint32_t *title_id);

// Start the game in Xenia. False with *err (plain words) on failure.
bool XeniaLaunch(const std::string &game_path, std::string *err);
bool XeniaConfigured();      // exe is set and exists
bool XeniaIsRunning();
// Call every frame. Returns true once when Xenia has just exited; *path is
// the game that was running and *seconds how long it ran.
bool XeniaPoll(std::string *path, double *seconds);
// Ask a running Xenia to quit (does nothing if none)
void XeniaStop();
std::string XeniaRunningPath();
