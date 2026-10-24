//
// xemu User Interface - shader cache per game
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>

// The game whose shaders are being noted (0: none)
void ShaderCacheSetTitle(uint32_t title_id);
// Write out what was noted (called now and then, and at exit)
void ShaderCacheFlush(void);

// Shader files known to belong to this game
int ShaderCacheCountFor(uint32_t title_id);
// Delete this game's shader files and the Vulkan pipeline cache (which
// can't be split by game). Returns files removed; *msg says what happened.
int ShaderCacheClearFor(uint32_t title_id, std::string *msg);
// Delete every cached shader for every game
int ShaderCacheClearAll(std::string *msg);
