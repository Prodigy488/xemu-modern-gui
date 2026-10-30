/*
 * Game folders as discs
 *
 * A folder holding a game's files (default.xbe and the rest) can be played
 * like a disc image: the first time, xemu builds a disc image (XDVDFS, like
 * an XISO) from the folder and keeps it in its folder_discs directory; the
 * drive then loads that image exactly as it would any other.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_FOLDER_DISC_H
#define XEMU_FOLDER_DISC_H

#ifdef __cplusplus
extern "C" {
#endif

/* The prepared disc image for this folder, to be g_free()d, or NULL with
 * *err set to a message (g_free() it too) when there is none yet or it is out
 * of date because the folder changed. The library prepares it first. */
char *xemu_folder_disc_ready(const char *folder, char **err);

#ifdef __cplusplus
}
#endif

#endif
