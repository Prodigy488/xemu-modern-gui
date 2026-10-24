//
// xemu User Interface - updater: reading a GitHub release
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "update-logic.hh"

#include <cctype>
#include <nlohmann/json.hpp>
#include <vector>

bool UpdateIsNewer(const std::string &release, const std::string &current)
{
    auto parse = [](const std::string &v, std::vector<int> *out) {
        size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
        while (i < v.size()) {
            if (!isdigit((unsigned char)v[i])) {
                return false;
            }
            int n = 0;
            while (i < v.size() && isdigit((unsigned char)v[i])) {
                n = n * 10 + (v[i++] - '0');
            }
            out->push_back(n);
            if (i < v.size()) {
                if (v[i] != '.') {
                    return false;
                }
                i++;
            }
        }
        return !out->empty();
    };
    std::vector<int> r, c;
    if (!parse(release, &r) || !parse(current, &c)) {
        return false;
    }
    // 1.6 == 1.6.0
    while (r.size() < c.size()) {
        r.push_back(0);
    }
    while (c.size() < r.size()) {
        c.push_back(0);
    }
    return r > c;
}

bool UpdateParseRelease(const std::string &text, const char *arch,
                        UpdateRelease *out, std::string *err)
{
    *out = UpdateRelease();
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) {
        *err = "Not a release";
        return false;
    }
    if (j.value("draft", false) || j.value("prerelease", false)) {
        *err = "Not a published release";
        return false;
    }
    out->version = j["tag_name"].get<std::string>();
    if (!out->version.empty() && (out->version[0] == 'v' || out->version[0] == 'V')) {
        out->version.erase(0, 1);
    }
    out->url = j.value("html_url", std::string());
    // xemu-<xemu version>-custom-windows-<arch>.zip (GitHub may add ".1" to
    // a re-uploaded name)
    std::string want = std::string("-custom-windows-") + arch;
    if (j.contains("assets") && j["assets"].is_array()) {
        for (auto &a : j["assets"]) {
            if (!a.is_object() || !a.contains("name") || !a["name"].is_string() ||
                !a.contains("browser_download_url") ||
                !a["browser_download_url"].is_string()) {
                continue;
            }
            std::string name = a["name"].get<std::string>();
            if (name.rfind("xemu-", 0) == 0 && name.find(want) != std::string::npos &&
                name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
                out->package_url = a["browser_download_url"].get<std::string>();
                break;
            }
        }
    }
    if (out->package_url.empty()) {
        *err = "The release " + out->version + " has no Windows download";
        return false;
    }
    return true;
}
