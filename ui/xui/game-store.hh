//
// xemu User Interface - homebrew store (XboxHomebrew.store by Team Resurgent)
//
// Talks to the store's public API (the one its Xbox app uses): categories,
// a paged app list, versions with their files, covers and downloads. Only
// homebrew categories are offered; see StoreCategoryAllowed().
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

struct StoreCategory {
    std::string name;
    int count = 0;
};

struct StoreApp {
    std::string id;
    std::string name;
    std::string author;
    std::string category;
    std::string description;
    std::string latest_version; // version id
    int state = 0;
};

struct StoreVersion {
    std::string id;
    std::string version;      // "1.2"
    std::string release_date;
    std::string changelog;
    std::string folder_name;  // install folder suggested by the store
    std::string title_id;
    std::string region;
    uint64_t size = 0;
    std::vector<std::string> files; // download file names, by index
};

extern const char *const kStoreApiBase;

bool ParseStoreCategories(const std::string &json, std::vector<StoreCategory> *out);
bool ParseStoreApps(const std::string &json, std::vector<StoreApp> *out);
bool ParseStoreVersions(const std::string &json, std::vector<StoreVersion> *out);

// Homebrew only: games here are homebrew ports; firmware (modified BIOS
// images) and Microsoft's own dashboards are not offered
bool StoreCategoryAllowed(const std::string &category);
bool StoreAppAllowed(const StoreApp &app);

std::string StoreUrlCategories();
std::string StoreUrlApps(int offset, int count, const std::string &category,
                         const std::string &search);
std::string StoreUrlVersions(const std::string &app_id);
std::string StoreUrlCover(const std::string &app_id, int width, int height);
std::string StoreUrlScreenshot(const std::string &app_id, int width, int height);
std::string StoreUrlDownload(const std::string &version_id, int file_index);

// Large Google Drive files answer with a "can't scan for viruses" page; this
// returns the URL that confirms and downloads, or "" if html isn't that page
std::string StoreDriveConfirmUrl(const std::string &html);

// A folder name the Xbox can use (FATX: 42 characters, no reserved ones)
std::string StoreSafeFolderName(const std::string &name);

std::string StoreUrlEncode(const std::string &s);
