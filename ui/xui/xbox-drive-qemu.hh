//
// xemu User Interface - Xbox hard drive access through QEMU's block layer
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include "xbox-drive.hh"

// The emulated Xbox's hard disk image
std::unique_ptr<XboxDiskIO> XboxDiskOpenEmulated(std::string *err);
bool XboxDiskEmulatedWritable(void);

// The Xbox caches its file system, so changes are made while it is paused
// and it is restarted afterwards
bool XboxIsRunning(void);
void XboxPause(void);
void XboxRestart(void);
