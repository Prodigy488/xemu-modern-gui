//
// xemu User Interface - game compatibility ratings
//
// The community compatibility ratings published on xemu.app
// (https://xemu.app/compat.json), matched to games by title ID.
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
#include <map>
#include <string>

// Ordered from worst to best; None means the title isn't in the list
enum class CompatStatus { None = -1, Unknown, Broken, Intro, Starts, Playable, Perfect };

extern const char *const kCompatJsonUrl;

// Parse xemu.app's compat.json into title ID -> status. False if it doesn't
// look like the compatibility list.
bool ParseCompatJson(const std::string &text,
                     std::map<uint32_t, CompatStatus> *out);

// The title ID a release is listed under on xemu.app (regional releases
// with their own ID share their game's entry)
uint32_t CompatListedId(uint32_t title_id);

// Game names from the same list, by title ID
bool ParseCompatNames(const std::string &text,
                      std::map<uint32_t, std::string> *out);

CompatStatus CompatLookup(const std::map<uint32_t, CompatStatus> &list,
                          uint32_t title_id);

const char *CompatName(CompatStatus s);        // "Playable"
const char *CompatDescription(CompatStatus s); // one sentence
std::string CompatTitleUrl(uint32_t title_id); // the game's page on xemu.app
