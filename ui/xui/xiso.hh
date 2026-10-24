//
// xemu User Interface - Xbox disc image (XISO / XDVDFS) layout
//
// Lays out a folder tree as an XDVDFS disc image, the format of Xbox game
// discs, so homebrew installed on the hard drive can be booted from the
// emulated DVD drive. The caller supplies the tree and copies the file data
// to the sectors chosen here.
//
// Pure C++17 with no emulator dependencies.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

constexpr uint32_t kXisoSectorSize = 2048;

struct XisoNode {
    std::string name;
    bool dir = false;
    uint64_t size = 0;           // files
    std::vector<XisoNode> kids;  // folders
    std::string source;          // the caller's reference to the data
    uint32_t sector = 0;         // set by XisoLayout
};

struct XisoPlan {
    // Volume descriptor and directory tables: (byte offset, bytes)
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> blobs;
    // Files with data, in disc order; copy each to sector * kXisoSectorSize
    std::vector<const XisoNode *> files;
    uint64_t data_bytes = 0;  // total file data
    uint64_t image_size = 0;  // bytes; the image is padded to this
};

// Name order used by the Xbox for directory lookups (case-insensitive)
int XisoCompareNames(const std::string &a, const std::string &b);

// Assign sectors to everything under root (its own name is ignored) and
// build the directory tables. Empty folders are kept. False with *err if the
// tree can't be stored (a file over 4 GB, a name over 255 bytes, a folder
// with too many entries).
bool XisoLayout(XisoNode &root, XisoPlan *plan, std::string *err);
