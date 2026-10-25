/*
 * xemu: where the Xbox's audio goes on the PC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_MCPX_APU_APU_OUTPUT_H
#define HW_XBOX_MCPX_APU_APU_OUTPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Play through this device (by name; NULL or "" for the system default).
 * Switches on the audio thread at its next frame. */
void mcpx_apu_monitor_set_device(const char *name);

/* Extra gain on top of the volume setting, 0-1 (e.g. quieter while xemu
 * isn't the active window) */
void mcpx_apu_monitor_set_ui_gain(float gain);

#ifdef __cplusplus
}
#endif

#endif
