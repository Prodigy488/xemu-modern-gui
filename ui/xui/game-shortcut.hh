//
// xemu User Interface - Desktop shortcuts for games
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

// Create a desktop shortcut that starts xemu with `image_path` loaded.
// Returns false and sets `error` on failure. Windows only.
bool CreateGameDesktopShortcut(const std::string &title,
                               const std::string &image_path,
                               std::string &error);
bool GameShortcutsSupported(void);
