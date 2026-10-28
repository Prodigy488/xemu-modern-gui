/*
 * xemu stick dead zone
 *
 * Optional extra dead zone for analog sticks, for controllers that drift.
 * Radial: a stick pushed less than `dz` (0..1) of the way reads as centred,
 * and the rest of its travel is stretched back to the full range, so full
 * push is still full push. dz of 0 changes nothing.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_DEADZONE_H
#define XEMU_DEADZONE_H

#include <math.h>
#include <stdint.h>

static inline void xemu_apply_stick_deadzone(int16_t *x, int16_t *y, float dz)
{
    if (!(dz > 0.0f)) {
        return;
    }
    if (dz > 0.6f) {
        dz = 0.6f;
    }
    float fx = (float)*x, fy = (float)*y;
    float mag = sqrtf(fx * fx + fy * fy) / 32768.0f;
    if (mag <= dz) {
        *x = 0;
        *y = 0;
        return;
    }
    float scale = ((mag - dz) / (1.0f - dz)) / mag;
    float nx = fx * scale, ny = fy * scale;
    if (nx > 32767.0f) nx = 32767.0f;
    if (nx < -32768.0f) nx = -32768.0f;
    if (ny > 32767.0f) ny = 32767.0f;
    if (ny < -32768.0f) ny = -32768.0f;
    *x = (int16_t)nx;
    *y = (int16_t)ny;
}

#endif
