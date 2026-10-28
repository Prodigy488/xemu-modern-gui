/*
 * xemu frame hitch monitor (see hitch.h)
 *
 * A frame is a hitch when it took more than HITCH_MIN_US and more than
 * HITCH_FACTOR times the recent normal frame time (so a game that runs at 30
 * does not count every frame). It is blamed on what took over a third of it:
 * building shaders or pipelines, waiting for the GPU, or reading the disc
 * image. Otherwise it is "other" (the emulated CPU, the game itself loading).
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
#define MAX_LOGGED 40

static int64_t g_build_us;      /* atomic: since the last frame */
static int64_t g_wait_us;
static int64_t g_disc_us;
static int64_t g_xlat_us;
static int64_t g_ui_us;
static int64_t g_disc_t0;       /* atomic: the request in flight, 0 for none */
static const char *g_wait_reason;
static int64_t g_wait_worst_us;

enum { BY_BUILD, BY_GPU, BY_DISC, BY_XLAT, BY_UI, BY_OTHER, BY_N };
static const char *const by_name[BY_N] = { "building shaders/pipelines",
                                           "waiting for the GPU",
                                           "reading the disc",
                                           "translating guest code",
                                           "interface holding the lock", "other" };

static struct {
    int64_t last_us;
    double normal_us;           /* running average of ordinary frames */
    uint64_t frames, hitches, by[BY_N];
    int64_t worst_us;
    int64_t build_total_us, wait_total_us, disc_total_us, xlat_total_us,
        ui_total_us;
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

void nv2a_hitch_end_wait(int64_t t0, const char *reason)
{
    const int64_t d = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
    if (d > 0) {
        qatomic_add(&g_wait_us, d);
        if (d > g_wait_worst_us) {     /* GPU thread only; a racy label is fine */
            g_wait_worst_us = d;
            g_wait_reason = reason;
        }
    }
}

void nv2a_hitch_end_xlat(int64_t t0)
{
    const int64_t d = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
    if (d > 0) {
        qatomic_add(&g_xlat_us, d);
    }
}

void nv2a_hitch_end_ui_lock(int64_t t0)
{
    const int64_t d = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
    if (d > 0) {
        qatomic_add(&g_ui_us, d);
    }
}

void nv2a_hitch_disc_begin(void)
{
    qatomic_set(&g_disc_t0, qemu_clock_get_us(QEMU_CLOCK_REALTIME));
}

void nv2a_hitch_disc_end(void)
{
    const int64_t t0 = qatomic_xchg(&g_disc_t0, 0);
    if (t0) {
        const int64_t d = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0;
        if (d > 0) {
            qatomic_add(&g_disc_us, d);
        }
    }
}

void nv2a_hitch_frame(void)
{
    const int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t built = qatomic_xchg(&g_build_us, 0);
    const int64_t waited = qatomic_xchg(&g_wait_us, 0);
    /* A request still in flight counts up to now */
    const int64_t t0 = qatomic_read(&g_disc_t0);
    int64_t disc = qatomic_xchg(&g_disc_us, 0);
    if (t0) {
        disc += now - t0;
        qatomic_set(&g_disc_t0, now);
    }
    const int64_t xlat = qatomic_xchg(&g_xlat_us, 0);
    const int64_t ui = qatomic_xchg(&g_ui_us, 0);
    const char *wait_reason = g_wait_reason ? g_wait_reason : "";
    g_wait_reason = NULL;
    g_wait_worst_us = 0;

    const int64_t gap = now - g.last_us;
    g.last_us = now;
    if (gap <= 0 || gap > PAUSE_US || gap == now) {
        g.normal_us = 0;
        return;
    }
    g.frames++;
    g.build_total_us += built;
    g.wait_total_us += waited;
    g.disc_total_us += disc;
    g.xlat_total_us += xlat;
    g.ui_total_us += ui;
    if (gap > HITCH_MIN_US && g.normal_us > 0 && gap > g.normal_us * HITCH_FACTOR) {
        int by = BY_OTHER;
        int64_t most = gap / 3;
        if (built > most) { by = BY_BUILD; most = built; }
        if (waited > most) { by = BY_GPU; most = waited; }
        if (disc > most) { by = BY_DISC; most = disc; }
        if (xlat > most) { by = BY_XLAT; most = xlat; }
        if (ui > most) { by = BY_UI; }
        g.hitches++;
        g.by[by]++;
        if (gap > g.worst_us) {
            g.worst_us = gap;
        }
        if (g.hitches <= MAX_LOGGED) {
            fprintf(stderr, "nv2a hitch: frame took %.0f ms (normal %.0f ms): "
                    "%.0f ms building shaders/pipelines, %.0f ms waiting for the GPU%s%s%s, "
                    "%.0f ms reading the disc, %.0f ms translating guest code, "
                    "%.0f ms with the interface holding the lock -> %s%s\n",
                    gap / 1000.0, g.normal_us / 1000.0, built / 1000.0, waited / 1000.0,
                    wait_reason[0] ? " (mostly " : "", wait_reason, wait_reason[0] ? ")" : "",
                    disc / 1000.0, xlat / 1000.0, ui / 1000.0, by_name[by],
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
             " building shaders or pipelines, %" PRIu64 " waiting for the GPU, %"
             PRIu64 " reading the disc, %" PRIu64 " translating guest code, %" PRIu64
             " interface lock, %" PRIu64 " other), worst %.0f ms; "
             "totals: %.1f s building, %.1f s GPU waits, %.1f s disc, %.1f s translating, "
             "%.1f s interface lock",
             g.frames, g.hitches, HITCH_MIN_US / 1000, g.by[BY_BUILD], g.by[BY_GPU],
             g.by[BY_DISC], g.by[BY_XLAT], g.by[BY_UI], g.by[BY_OTHER], g.worst_us / 1000.0,
             g.build_total_us / 1e6, g.wait_total_us / 1e6, g.disc_total_us / 1e6,
             g.xlat_total_us / 1e6, g.ui_total_us / 1e6);
}
