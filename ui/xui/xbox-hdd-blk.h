/*
 * xemu User Interface - emulated hard disk access for the drive explorer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XBOX_HDD_BLK_H
#define XBOX_HDD_BLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool xemu_hdd_available(void);
bool xemu_hdd_writable(void);
bool xemu_hdd_read(uint64_t offset, void *buf, size_t len);
bool xemu_hdd_write(uint64_t offset, const void *buf, size_t len);
uint64_t xemu_hdd_size(void);
void xemu_hdd_flush(void);

bool xemu_xbox_running(void);
void xemu_xbox_pause(void);
void xemu_xbox_restart(void);

#ifdef __cplusplus
}
#endif

#endif
