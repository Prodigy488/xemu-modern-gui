/*
 * xemu CPU sampling profiler (Windows)
 *
 * For finding where an emulation thread spends its time. It looks at the
 * busiest threads every few milliseconds for a while and writes where they
 * were (as offsets into xemu.exe, to be resolved against the matching
 * unstripped build) to profile.txt in the settings folder.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_PROFILER_H
#define XEMU_PROFILER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log where the process died when it hits an unhandled exception */
void xemu_crash_reporter_install(void);

/* Start a capture of the given length; false when one is already running */
bool xemu_profiler_start(int seconds);

/* True while a capture is running */
bool xemu_profiler_running(void);

/* When a capture finished since the last call: its output path in out
 * (up to len bytes) and true */
bool xemu_profiler_take_result(char *out, int len);

#ifdef __cplusplus
}
#endif

#endif
