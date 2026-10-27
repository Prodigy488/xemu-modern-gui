/*
 * xemu guest memory access for achievements
 *
 * Reads the emulated Xbox's memory (read only) so achievement triggers can
 * watch values in a running game, and so the address finder can search it.
 * Addresses are the game's own virtual addresses, the same ones a debugger or
 * the game's XBE sections show.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_GUESTMEM_H
#define XEMU_GUESTMEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A page of guest memory: where the game sees it, and where it really is
typedef struct XemuGuestPage {
    uint32_t vaddr;
    uint32_t paddr;
} XemuGuestPage;

// Call once before a batch of reads or a page list (some accelerators keep
// the CPU state out of reach until it is synchronized). Cheap with TCG.
void xemu_guest_sync(void);

// Whether there is a running guest to read from
bool xemu_guest_available(void);

// Reads len bytes at a virtual address. False if any of it isn't mapped.
bool xemu_guest_read(uint32_t vaddr, void *buf, uint32_t len);

// Reads from a physical address (RAM only)
bool xemu_guest_read_phys(uint32_t paddr, void *buf, uint32_t len);

// Lists the pages a game can have data in, each physical page once (the user
// address if it has one, otherwise the kernel's 0x80000000 mapping). Returns
// how many were written, at most max.
int xemu_guest_pages(XemuGuestPage *out, int max);

#ifdef __cplusplus
}
#endif

#endif
