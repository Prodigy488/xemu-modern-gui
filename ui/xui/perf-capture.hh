//
// xemu User Interface - performance capture
//
// Records the nv2a profiler's per-frame counters while a game runs, marks
// moments the user flags as glitched, and turns it into a report (see
// perf-analysis.hh) with files to share: report.txt, frames.csv and a
// Chrome/Perfetto trace.
//
// Shift+F9 starts and stops a recording; Shift+F12 marks a glitch.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Every frame, before drawing the UI
void PerfCaptureUpdate(void);
// The window and the recording indicator
void PerfCaptureDraw(void);

void PerfCaptureOpenWindow(void);
void PerfCaptureToggleRecording(void);
void PerfCaptureMarkGlitch(void);
bool PerfCaptureRecording(void);
