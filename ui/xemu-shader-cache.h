/*
 * xemu shader cache bookkeeping per game
 *
 * The renderers keep compiled shaders on disk for all games together. To
 * clear one game's shaders, the files each game creates or uses are noted
 * here while it runs and listed in <base>/shader_cache_titles/<ID>.txt.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_SHADER_CACHE_H
#define XEMU_SHADER_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A shader cache file was written or read for the running game (any
 * thread) */
void xemu_shader_cache_note(const char *path);

/* After a clear: don't write the Vulkan pipeline cache again this session,
 * so what was in memory doesn't come back */
bool xemu_shader_cache_pipelines_discarded(void);

#ifdef __cplusplus
}
#endif

#endif
