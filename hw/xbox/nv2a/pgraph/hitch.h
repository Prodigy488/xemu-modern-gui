/*
 * xemu frame hitch monitor
 *
 * Notes which frames came late and whether the time went to building shaders
 * or pipelines on the render thread, the usual cause of stutter. The totals
 * go into the bug-report text (Help menu) so a stutter report says where it
 * came from. Cost: two clock reads per build, one per frame.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HW_XBOX_NV2A_PGRAPH_HITCH_H
#define HW_XBOX_NV2A_PGRAPH_HITCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Around work that builds a shader or pipeline: t0 = nv2a_hitch_begin() before,
 * nv2a_hitch_end_build(t0) after. Any thread. */
int64_t nv2a_hitch_begin(void);
void nv2a_hitch_end_build(int64_t t0);

/* Once per frame the game flips, from the GPU thread */
void nv2a_hitch_frame(void);

/* A line or two of text for the bug report; "" before any frame was seen */
void nv2a_hitch_summary(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
