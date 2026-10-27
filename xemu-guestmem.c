/*
 * xemu guest memory access for achievements
 *
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "xemu-guestmem.h"
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "system/hw_accel.h"
#include "system/dma.h"
#include "cpu.h"
#include "exec/target_page.h"

// The game's own memory lives in the low part of the address space. Pool
// memory the kernel hands out is also visible at 0x80000000 + physical.
#define USER_START   0x00010000u
#define USER_END     0x40000000u
#define KERNEL_START 0x80000000u
#define KERNEL_END   0x88000000u
// Xbox RAM is 64 MB (128 MB with some mods)
#define GUEST_RAM_LIMIT 0x08000000ull

static CPUState *guest_cpu(void)
{
    return qemu_get_cpu(0);
}

void xemu_guest_sync(void)
{
    CPUState *cs = guest_cpu();
    if (cs) {
        cpu_synchronize_state(cs);
    }
}

bool xemu_guest_available(void)
{
    return guest_cpu() != NULL;
}

static bool virt_to_phys(CPUState *cs, uint32_t vaddr, hwaddr *phys)
{
    MemTxAttrs attrs;
    hwaddr gpa = cpu_get_phys_page_attrs_debug(cs, vaddr & TARGET_PAGE_MASK,
                                               &attrs);
    if (gpa == (hwaddr)-1) {
        return false;
    }
    *phys = gpa + (vaddr & ~TARGET_PAGE_MASK);
    return true;
}

bool xemu_guest_read_phys(uint32_t paddr, void *buf, uint32_t len)
{
    // RAM only: reading a device could change its state
    if ((uint64_t)paddr + len > GUEST_RAM_LIMIT) {
        return false;
    }
    MemTxResult r = address_space_read(&address_space_memory, paddr,
                                       MEMTXATTRS_UNSPECIFIED, buf, len);
    return r == MEMTX_OK;
}

bool xemu_guest_read(uint32_t vaddr, void *buf, uint32_t len)
{
    CPUState *cs = guest_cpu();
    if (!cs || len == 0 || len > 0x10000 || vaddr + len < vaddr) {
        return false;
    }
    uint8_t *out = buf;
    uint32_t done = 0;
    while (done < len) {
        hwaddr phys;
        if (!virt_to_phys(cs, vaddr + done, &phys)) {
            return false;
        }
        uint32_t room = TARGET_PAGE_SIZE - (uint32_t)(phys & ~TARGET_PAGE_MASK);
        uint32_t n = len - done < room ? len - done : room;
        if (!xemu_guest_read_phys((uint32_t)phys, out + done, n)) {
            return false;
        }
        done += n;
    }
    return true;
}

int xemu_guest_pages(XemuGuestPage *out, int max)
{
    CPUState *cs = guest_cpu();
    if (!cs || !out || max <= 0) {
        return 0;
    }
    // One bit per physical page of the 4 GB space, so no page is listed twice
    static uint32_t seen[(1u << 20) / 32];
    memset(seen, 0, sizeof(seen));

    int n = 0;
    for (int pass = 0; pass < 2 && n < max; pass++) {
        uint64_t a = pass ? KERNEL_START : USER_START;
        uint64_t end = pass ? KERNEL_END : USER_END;
        for (; a < end && n < max; a += TARGET_PAGE_SIZE) {
            hwaddr p;
            if (!virt_to_phys(cs, (uint32_t)a, &p) || p >= GUEST_RAM_LIMIT) {
                continue; // not mapped, or not RAM
            }
            uint32_t pg = (uint32_t)(p >> 12);
            if (seen[pg >> 5] & (1u << (pg & 31))) {
                continue;
            }
            seen[pg >> 5] |= 1u << (pg & 31);
            out[n].vaddr = (uint32_t)a;
            out[n].paddr = (uint32_t)(p & ~(hwaddr)0xFFF);
            n++;
        }
    }
    return n;
}
