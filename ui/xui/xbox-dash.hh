//
// xemu User Interface - the "Original Xbox" dashboard style
//
// A homage to the look of the first Xbox's menu: black with green light, a
// list of sections on the left and a glowing wireframe globe, drawn from
// scratch (no Microsoft artwork, logos or fonts). The main menu lists the
// sections; picking one opens it in a window beside the list, and B closes the window.
//
// These are drawing helpers only, so the layout can be checked on its own;
// game-library.cc supplies the sections and their content.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <imgui.h>

// The black-and-green backdrop with slow light ribbons and drifting specks.
// menu_view: the main menu (brighter, with the globe's glow) or a section.
void XboxDashBackground(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale, double time,
                        bool menu_view, bool animate);

// The glowing wireframe globe. pulse (0..1) flares it, e.g. when the
// selection changes.
void XboxDashGlobe(ImDrawList *dl, ImVec2 center, float radius, double time, float pulse,
                   bool animate);

// The main menu inside a, b: the list of sections on the left (with `brand`
// above it) and the globe on the right. `sel` is the highlighted row (follows
// the controller or keyboard focus and the mouse). `focus_sel` moves the
// controller focus to that row (use when the menu appears). Returns the row
// that was picked, or -1.
//
// open_t (0..1) slides the list into a narrow column at the left to make room
// for a section's window, which then shows over the globe. While it is open
// the highlight no longer follows the mouse (a click picks), so the caller
// switches section when `sel` changes. *list_right gets the right edge of the
// list, where the window starts.
int XboxDashMenu(const char *brand, const char *const *labels, int n, int &sel, ImVec2 a,
                 ImVec2 b, float scale, double time, ImFont *font, ImFont *small_font,
                 bool focus_sel, float open_t, float *list_right, bool animate);

// A section's window, to the right of the list at `left`: glass over the
// globe with the title at its top. Returns the screen rectangle to put the
// content in. open_t (0..1) fades and slides it in.
struct XboxDashRect {
    ImVec2 min, max;
};
XboxDashRect XboxDashWindow(const char *title, ImVec2 a, ImVec2 b, float scale, ImFont *font,
                            float left, float bottom_margin, float open_t);
