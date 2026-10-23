//
// xemu User Interface - NVIDIA RTX Video Super Resolution
//
// Upscales the game image with the RTX Video Super Resolution built into
// NVIDIA's driver (the AI upscaler browsers use for video), reached through
// the Direct3D 11 video processor and WGL_NV_DX_interop2. Nothing from
// NVIDIA is bundled: the driver provides it. Windows and NVIDIA RTX only;
// everywhere else these calls report unavailable.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <epoxy/gl.h>

// Upscale the tw x th image in read framebuffer src_fbo to dw x dh. On
// success returns true and a GL texture holding the result (top row
// first, so draw it flipped); call RtxVideoEndFrame() after drawing it.
// Returns false (and the caller should use another scaler) when RTX Video
// is unavailable or not engaged by the driver.
bool RtxVideoUpscale(GLuint src_fbo, int tw, int th, int dw, int dh,
                     GLuint *out_tex);
void RtxVideoEndFrame(void);

// One-line status for the settings UI, e.g. "Active" or why it isn't.
const char *RtxVideoStatus(void);

// Forget the last result so the next use checks the driver again (after
// the user changes NVIDIA settings).
void RtxVideoRetry(void);

void RtxVideoShutdown(void);
