//
// xemu User Interface - Xbox hard drive explorer
//
// Browse the emulated Xbox hard drive (C, E, F, X, Y, Z) and copy files and
// folders in and out, create folders, rename and delete. Shown as the
// dashboard's Drive tab and as a window (Games > Xbox Hard Drive).
//
// Writing changes the Xbox's file system underneath it, and the Xbox caches
// its file system, so writes happen while the Xbox is paused and it restarts
// when they are done.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Draw the explorer into the current window
void XboxExplorerDraw(void);
// Run copy/delete jobs a little each frame (call every frame)
void XboxExplorerUpdate(void);
// The standalone window
void XboxExplorerWindowDraw(void);
void XboxExplorerOpenWindow(void);
bool XboxExplorerBusy(void);

// Copy the contents of host_dir onto the Xbox as <vol>:<parent>/<name>,
// creating parent if needed and replacing an existing folder of that name.
// Starts right away (the caller has asked the user): the Xbox pauses and
// restarts when done. False with *err if it can't start.
bool XboxExplorerInstallFolder(const std::string &host_dir, char vol,
                               const std::string &parent, const std::string &name,
                               const std::string &title, std::string *err);
// Make a bootable disc image (XISO) of the folder <vol>:<dir> at iso_path,
// so homebrew on the hard drive can be started from the DVD drive. Only
// reads the drive; the Xbox keeps running. *ready is set when an up-to-date
// image is already there; otherwise a job starts (see XboxExplorerBusy).
bool XboxExplorerMakeXiso(char vol, const std::string &dir,
                          const std::string &iso_path, bool *ready,
                          std::string *err);

// Homebrew installed in the usual places: <drive>:\Games, \Apps and
// \Emulators (one folder each, with a default.xbe), on E: and F:
struct XboxHomebrewApp {
    char vol = 'E';
    std::string dir;     // "/Games/2H4Ux"
    std::string folder;  // "2H4Ux"
    std::string section; // "Games"
    std::string title;   // from the XBE, or the folder name
    uint32_t title_id = 0;
    uint64_t xbe_size = 0;
};
bool XboxExplorerFindHomebrew(std::vector<XboxHomebrewApp> *out, std::string *err);

// Last status line ("Copying...", "... - done.", an error), and progress 0-1
std::string XboxExplorerStatus(bool *is_error);
float XboxExplorerProgress(void);
