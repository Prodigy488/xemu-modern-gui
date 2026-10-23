//
// xemu User Interface - Game library disc scanning
//
// Reads title metadata (name, title ID, region, built-in title image) from
// the default.xbe inside an Xbox disc image (XISO or full Redump-style ISO).
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

struct DiscTitleInfo {
    bool ok = false;
    uint32_t title_id = 0;
    std::string title_name; // UTF-8
    uint32_t region = 0;
    uint32_t version = 0;
    uint32_t disc_number = 0;
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> icon_rgba; // icon_w * icon_h * 4, may be empty
    std::string error;
};

// Parse a disc image. Returns true and fills `out` on success.
bool ReadDiscTitleInfo(const std::string &path, DiscTitleInfo &out);

// Decode an XPR0 texture bundle (as found in the $$XTIMAGE section) to RGBA.
bool DecodeXprImage(const uint8_t *data, size_t len, int &w, int &h,
                    std::vector<uint8_t> &rgba);

// "MS-004" style title ID string
std::string FormatTitleId(uint32_t title_id);

// "NTSC-U", "PAL", "NTSC-J / NTSC-U", "Region free", ...
std::string FormatRegion(uint32_t region);
