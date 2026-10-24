//
// xemu User Interface - updater: reading a GitHub release
//
// Pure C++17 so it can be tested without Windows.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

struct UpdateRelease {
    std::string version;     // "1.6" (tag without the leading v)
    std::string url;         // the release page
    std::string package_url; // the Windows zip for this architecture
};

// From api.github.com/repos/<owner>/<repo>/releases/latest. False with *err
// when it isn't a release or has no Windows build for arch ("x86_64").
bool UpdateParseRelease(const std::string &json, const char *arch,
                        UpdateRelease *out, std::string *err);

// "v1.10" > "1.9". Anything other than numbers and dots ("1.5-classic")
// marks a different edition and is never newer.
bool UpdateIsNewer(const std::string &release, const std::string &current);
