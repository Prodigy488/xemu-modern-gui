/*
 * Per-game performance settings and automatic safe mode
 *
 * A game can have its own CPU boost, GPU boost, shader seeding, real
 * hardware speed and display refresh settings, kept by title ID. A game
 * that crashes while the boosts are on is started in safe mode (boosts off)
 * the next time, and says so.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_GAME_PERF_H
#define XEMU_GAME_PERF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After the settings are loaded and before emulation starts: finds out
 * whether the last run crashed, and applies the settings of the game in the
 * drive (dvd_path may be empty). */
void xemu_game_perf_startup(const char *dvd_path);

/* The disc in the drive changed ("" = none), as the game library sees it. */
void xemu_game_perf_disc_changed(const char *path);

/* Once per UI frame: shows a message the start-up could not. */
void xemu_game_perf_tick(void);

/* Around writing the settings file: the user's own values, not a game's. */
void xemu_game_perf_before_save(void);
void xemu_game_perf_after_save(void);

#ifdef __cplusplus
}
#endif

#endif
