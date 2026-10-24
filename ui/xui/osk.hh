//
// xemu User Interface - on-screen keyboard
//
// Appears at the bottom of the screen when a text box is opened with a
// controller, and types into it: A types the key, X deletes, Y is a space,
// LB/RB move the cursor, LT switches to symbols, RT to capitals, B closes
// it, Start closes it and confirms (Enter). Turned on or off with
// display.ui.onscreen_keyboard.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// Right after ImGui::NewFrame(): takes the controller buttons while the
// keyboard is up, and acts on them
void OskBeginFrame();
// After all other UI, before ImGui::Render(): draws the keyboard
void OskDraw();
// True while it's showing
bool OskVisible();
// Call right after an editable text box: lets A (not only Y) open it with
// a controller while the keyboard is on
void OskTextBox();
