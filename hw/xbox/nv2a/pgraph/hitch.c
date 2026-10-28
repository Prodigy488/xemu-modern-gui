/*
 * xemu frame hitch monitor (see hitch.h)
 *
 * A frame is a hitch when it took more than HITCH_MIN_US and more than
 * HITCH_FACTOR times the recent normal frame time (so a game that runs at 30
 * does not count every frame). It is blamed on building when the time spent
 * building shaders and pipelines during it was over a third of it.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "hitch.h"

#define HITCH_MIN_US 50000
#define HITCH_FACTOR 2.5
#define PAUSE_US 3000000        /* a longer gap is a pause or a load, not a hitch */
#define MAX_LOGGED 20

static int64_t g_build_us;      /* atomic: building since the last frame */

static struct {
    int64_t last_us;
    double normal_us;           /* running average of ordinary frames */
    uint64_t frames, hitches, build_hitches;
    int64_t worst_us, worst_build_us;
    int64_t build_total_us;
} g;

int64_t nv2a_hitch_begin(void)
{
    return qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

void nv2a_hitch_end_build(int64_t t0)
{
    const int64_t d = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
    if (d > 0) {
        qatomic_add(&g_build_us, d);
    }
}

void nv2a_hitch_frame(void)
{
    const int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t built = qatomic_xchg(&g_build_us, 0);
    const int64_t gap = now - g.last_us;
    g.last_us = now;
    if (gap <= 0 || gap > PAUSE_US || gap == now) {
        g.normal_us = 0;
        return;
    }
    g.frames++;
    g.build_total_us += built;
    if (gap > HITCH_MIN_US && g.normal_us > 0 && gap > g.normal_us * HITCH_FACTOR) {
        const bool by_build = built * 3 > gap;
        g.hitches++;
        if (by_build) {
            g.build_hitches++;
            if (built > g.worst_build_us) {
                g.worst_build_us = built;
            }
        }
        if (gap > g.worst_us) {
            g.worst_us = gap;
        }
        if (g.hitches <= MAX_LOGGED) {
            fprintf(stderr, "nv2a hitch: frame took %.0f ms (normal %.0f ms), "
                    "%.0f ms of it building shaders/pipelines%s\n",
                    gap / 1000.0, g.normal_us / 1000.0, built / 1000.0,
                    g.hitches == MAX_LOGGED ? " (not logging more)" : "");
        }
        return;
    }
    g.normal_us = g.normal_us > 0 ? g.normal_us * 0.95 + gap * 0.05 : gap;
}

void nv2a_hitch_summary(char *buf, size_t len)
{
    if (len == 0) {
        return;
    }
    buf[0] = 0;
    if (g.frames < 60) {
        return;
    }
    snprintf(buf, len,
             "%" PRIu64 " frames, %" PRIu64 " hitches over %d ms (%" PRIu64
             " while building shaders or pipelines), worst %.0f ms, "
             "%.1f s spent building",
             g.frames, g.hitches, HITCH_MIN_US / 1000, g.build_hitches,
             g.worst_us / 1000.0, g.build_total_us / 1e6);
}
