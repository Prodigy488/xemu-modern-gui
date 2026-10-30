//
// xemu User Interface - game folders as discs
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <functional>
#include <string>

// True for a game folder whose disc image isn't there yet or is out of date
bool FolderDiscNeedsPrepare(const std::string &folder);

// Builds the folder's disc image in the background, then calls done() on the
// main thread (not called if it failed; the error is shown). `title` is what
// the progress messages call the game.
void FolderDiscPrepare(const std::string &folder, const std::string &title,
                       std::function<void()> done);

// Every frame, on the main thread
void FolderDiscPump();
