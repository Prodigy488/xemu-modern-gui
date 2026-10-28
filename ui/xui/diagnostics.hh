//
// xemu User Interface: diagnostics for bug reports
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

// A short text describing this build, the computer's graphics and the main
// settings, for pasting into a bug report. It leaves out file paths, names,
// tokens and anything else personal.
std::string DiagnosticsText();

// Copies DiagnosticsText() to the clipboard and says so
void DiagnosticsCopy();
