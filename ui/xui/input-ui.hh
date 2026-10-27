//
// xemu User Interface - controller port pickers and the controller test
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

// A drop-down for what is plugged into a controller port (0-3): not connected,
// "Keyboard, mouse and controller", or one of the connected controllers. The
// same choices as Settings > Input, for the dashboard.
void InputPortCombo(const char *id, int port);

// Shows a connected controller live: buttons light up and the sticks and
// triggers move as you use them. Has its own device picker.
void InputTestWidget(const char *id);

// True while the controller test is running: the dashboard should ignore the
// controller so the buttons can be tried without scrolling, going back or
// switching tabs.
bool InputTestCapturing(void);
