//
// xemu User Interface - Performance overlay
//
// Small corner readout of frame rate, frame time and the active video
// settings, toggled with F9 or View > Performance Overlay.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

void DrawPerformanceOverlay(void);

// "Preparing shaders" in the lower left while the game is building shaders
// or pipelines; fades out when they're done
void DrawShaderIndicator(void);
