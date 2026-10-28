/*
 * xemu User Interface - slow frame profiler
 *
 * The interface runs with the emulator's global lock held, so a slow frame
 * stops the emulated machine. This notes where the time of such a frame went
 * and logs it, so a stutter can be traced to the code that caused it.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#pragma once

void UiProfBegin();
// The time since the previous mark (or Begin) goes to this section
void UiProfMark(const char *name);
// Logs the frame when it was slow
void UiProfEnd();
