/*
 * xemu User Interface - emulated hard disk access for the drive explorer
 *
 * Small C shim over QEMU's block layer and run state, so the C++ explorer
 * doesn't include QEMU's C-only headers. Callers hold the BQL.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "xbox-hdd-blk.h"

static BlockBackend *hdd_blk(void)
{
    BlockBackend *blk = blk_by_name("ide0-hd0");
    if (!blk || !blk_is_available(blk)) {
        return NULL;
    }
    return blk;
}

bool xemu_hdd_available(void)
{
    return hdd_blk() != NULL;
}

bool xemu_hdd_writable(void)
{
    BlockBackend *blk = hdd_blk();
    return blk && blk_is_writable(blk);
}

bool xemu_hdd_read(uint64_t offset, void *buf, size_t len)
{
    BlockBackend *blk = hdd_blk();
    return blk && blk_pread(blk, (int64_t)offset, (int64_t)len, buf, 0) >= 0;
}

bool xemu_hdd_write(uint64_t offset, const void *buf, size_t len)
{
    BlockBackend *blk = hdd_blk();
    return blk && blk_pwrite(blk, (int64_t)offset, (int64_t)len, buf, 0) >= 0;
}

uint64_t xemu_hdd_size(void)
{
    BlockBackend *blk = hdd_blk();
    int64_t n = blk ? blk_getlength(blk) : 0;
    return n > 0 ? (uint64_t)n : 0;
}

void xemu_hdd_flush(void)
{
    BlockBackend *blk = hdd_blk();
    if (blk) {
        blk_flush(blk);
    }
}

bool xemu_xbox_running(void)
{
    return runstate_is_running();
}

void xemu_xbox_pause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

void xemu_xbox_restart(void)
{
    /* The Xbox caches its file system: restart so it sees the changes */
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    if (!runstate_is_running()) {
        vm_start();
    }
}
