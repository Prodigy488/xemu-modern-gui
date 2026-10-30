/*
 * Game folders as discs
 *
 * A folder holding a game's files (default.xbe and the rest) can be played
 * without first making a disc image of it: the folder is laid out as an
 * XDVDFS disc and the emulated drive reads the files straight from it.
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

/* Lays out the folder and writes its disc plan; returns the name to open the
 * drive's medium with ("xplan:<plan file>"), to be g_free()d, or NULL with
 * *err set to a message (g_free() it too). */
char *xemu_folder_disc_prepare(const char *folder, char **err);

#ifdef __cplusplus
}
#endif

#endif
