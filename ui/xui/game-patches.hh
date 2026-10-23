//
// xemu User Interface - Game patches
//
// Applies user patches to a game's default.xbe without modifying the disc
// image: the patched XBE is written into a small qcow2 overlay that uses the
// original image as its backing file, and the overlay is loaded instead.
//
// Supported patch files, applied in order:
//   .xbe  full replacement default.xbe (the "custom XBE" way on modded Xboxes)
//   .ips  IPS patch
//   .bps  BPS patch (source/target/patch checksums are verified)
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
#include <vector>

struct XbeLocation {
    uint64_t image_size = 0;
    uint64_t xbe_offset = 0;        // byte offset of default.xbe in the image
    uint32_t xbe_size = 0;          // its size per the directory entry
    uint64_t size_field_offset = 0; // offset of that size field in the image
};

// Find default.xbe in an Xbox disc image (XISO or Redump-style).
bool LocateDefaultXbe(const std::string &image_path, XbeLocation &loc,
                      std::string &error);

bool ApplyIpsPatch(std::vector<uint8_t> &data, const std::vector<uint8_t> &ips,
                   std::string &error);
bool ApplyBpsPatch(const std::vector<uint8_t> &source,
                   const std::vector<uint8_t> &bps,
                   std::vector<uint8_t> &target, std::string &error);

// Apply patch files (by extension) to an XBE, in order.
bool ApplyPatchFiles(const std::vector<uint8_t> &original,
                     const std::vector<std::string> &patch_paths,
                     std::vector<uint8_t> &patched, std::string &error);

// Write a qcow2 (version 2) overlay of `virtual_size` bytes backed by the raw
// image at `backing_path`, containing `writes` (offset, bytes) on top.
bool WriteQcow2Overlay(
    const std::string &overlay_path, const std::string &backing_path,
    uint64_t virtual_size,
    const std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &writes,
    std::string &error);

// Everything together: patch the image's default.xbe and write the overlay.
// Fails (without writing) if the patched XBE does not fit in the space the
// original occupies on the disc.
bool BuildPatchedOverlay(const std::string &image_path,
                         const std::vector<std::string> &patch_paths,
                         const std::string &overlay_path, std::string &error);

uint32_t Crc32(const uint8_t *data, size_t len);
