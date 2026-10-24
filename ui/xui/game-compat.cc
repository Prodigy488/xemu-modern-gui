//
// xemu User Interface - game compatibility ratings
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "game-compat.hh"
#include "i18n.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

const char *const kCompatJsonUrl = "https://xemu.app/compat.json";

namespace
{
// Releases whose title ID differs from the one their game is listed under,
// generated from the xemu-project/xdb title database (commit d83d221):
// { release title ID, listed title ID }
const struct {
    uint32_t release, listed;
} kAliases[] = {
    { 0x4156000C, 0x4156000A },
    { 0x41560036, 0x41560026 },
    { 0x42550002, 0x42550001 },
    { 0x43430006, 0x43430001 },
    { 0x4343000A, 0x41560006 },
    { 0x434D000C, 0x434D0021 },
    { 0x434D000E, 0x434D0002 },
    { 0x434D0012, 0x434D0021 },
    { 0x434D0013, 0x434D0021 },
    { 0x434D0014, 0x434D0021 },
    { 0x434D0015, 0x434D0021 },
    { 0x434D0016, 0x434D0021 },
    { 0x434D0017, 0x434D0021 },
    { 0x434D0018, 0x434D0021 },
    { 0x434D0019, 0x434D0021 },
    { 0x434D001B, 0x434D0021 },
    { 0x434D001C, 0x434D0021 },
    { 0x434D001D, 0x434D0021 },
    { 0x434D001E, 0x434D0021 },
    { 0x434D001F, 0x434D0021 },
    { 0x434D0020, 0x434D0021 },
    { 0x434D0022, 0x434D0021 },
    { 0x434D0023, 0x434D0021 },
    { 0x434D002F, 0x434D003F },
    { 0x434D0030, 0x434D003F },
    { 0x434D0031, 0x434D003F },
    { 0x434D0032, 0x434D003F },
    { 0x434D0033, 0x434D003F },
    { 0x434D0035, 0x434D003F },
    { 0x434D0036, 0x434D003F },
    { 0x434D0037, 0x434D003F },
    { 0x434D0038, 0x434D003F },
    { 0x434D0039, 0x434D003F },
    { 0x434D003A, 0x434D003F },
    { 0x434D003B, 0x434D003F },
    { 0x434D003C, 0x434D003F },
    { 0x434D003D, 0x434D003F },
    { 0x434D003E, 0x434D003F },
    { 0x434D0040, 0x434D003F },
    { 0x434D0041, 0x434D003F },
    { 0x434D0042, 0x434D003F },
    { 0x434D0043, 0x434D003F },
    { 0x434D0044, 0x434D003F },
    { 0x434D0045, 0x434D003F },
    { 0x43540001, 0x4156000F },
    { 0x44430006, 0x44430004 },
    { 0x45410022, 0x4156000F },
    { 0x4541002E, 0x4541002A },
    { 0x45410067, 0x45410049 },
    { 0x4541006A, 0x4541004E },
    { 0x4541006F, 0x4541005A },
    { 0x45410071, 0x4541005A },
    { 0x4541007F, 0x45410062 },
    { 0x45410081, 0x45410076 },
    { 0x45410088, 0x4541007B },
    { 0x45410093, 0x4541008B },
    { 0x45410095, 0x45410089 },
    { 0x454100A8, 0x4541009F },
    { 0x4541038A, 0x45410089 },
    { 0x47450001, 0x43560001 },
    { 0x47450003, 0x47450002 },
    { 0x49470040, 0x49470027 },
    { 0x49470041, 0x49470025 },
    { 0x4B420004, 0x4B420003 },
    { 0x4C410001, 0x4156000F },
    { 0x4C410009, 0x41560015 },
    { 0x4C410019, 0x4C410013 },
    { 0x4D4A0017, 0x434D0052 },
    { 0x4D530033, 0x4D53000F },
    { 0x4D530037, 0x4D530028 },
    { 0x4D530057, 0x4D530004 },
    { 0x4D530079, 0x4D53004B },
    { 0x4D53007B, 0x4D530021 },
    { 0x4D53007D, 0x4D530046 },
    { 0x4D5300CA, 0x4D53006B },
    { 0x4D5380CD, 0x4D5300CD },
    { 0x4D570023, 0x4D570019 },
    { 0x4D570028, 0x4D570021 },
    { 0x4D570033, 0x4D570022 },
    { 0x4D580001, 0x41590004 },
    { 0x4E4D0005, 0x4541002C },
    { 0x504C0001, 0x47450002 },
    { 0x504C0004, 0x504C0002 },
    { 0x504C0005, 0x504C0002 },
    { 0x52410008, 0x4D4A0003 },
    { 0x5343000B, 0x41540004 },
    { 0x53450003, 0x4947002C },
    { 0x53450007, 0x4947002B },
    { 0x5345000A, 0x49470018 },
    { 0x5345000B, 0x49470017 },
    { 0x53450014, 0x49470029 },
    { 0x53450016, 0x49470018 },
    { 0x53450019, 0x53450004 },
    { 0x5345001C, 0x4947002B },
    { 0x5345001E, 0x53450013 },
    { 0x5345001F, 0x53450011 },
    { 0x53450020, 0x53450017 },
    { 0x5345002C, 0x53450022 },
    { 0x5345002D, 0x53450026 },
    { 0x5345002E, 0x53450023 },
    { 0x53450080, 0x53450031 },
    { 0x5345008A, 0x53450084 },
    { 0x5345008B, 0x53450084 },
    { 0x534E0002, 0x494F0005 },
    { 0x53530007, 0x53530003 },
    { 0x53538005, 0x53530005 },
    { 0x54430001, 0x4D53002D },
    { 0x5443000C, 0x54430009 },
    { 0x54510006, 0x4B420006 },
    { 0x5451000A, 0x53550006 },
    { 0x5451000C, 0x45430001 },
    { 0x54510013, 0x494C0001 },
    { 0x5451001C, 0x53550006 },
    { 0x545100F3, 0x545100EF },
    { 0x545100F9, 0x54510024 },
    { 0x54510102, 0x545100F8 },
    { 0x54540005, 0x53430001 },
    { 0x54540073, 0x5454000F },
    { 0x5454007F, 0x5454001B },
    { 0x54540084, 0x5454007C },
    { 0x54540086, 0x54540082 },
    { 0x54540095, 0x54540082 },
    { 0x545400A1, 0x54540090 },
    { 0x545400A2, 0x5454008D },
    { 0x545400A3, 0x54540082 },
    { 0x545400A4, 0x54540082 },
    { 0x545400A7, 0x5454008C },
    { 0x5553000A, 0x55530006 },
    { 0x5553000D, 0x43560002 },
    { 0x5553000F, 0x4156000F },
    { 0x55530011, 0x4D500001 },
    { 0x55530014, 0x52410007 },
    { 0x55530015, 0x42530001 },
    { 0x5553001F, 0x55530013 },
    { 0x55530041, 0x55530038 },
    { 0x5553005B, 0x5553003C },
    { 0x5553005C, 0x43430016 },
    { 0x56550007, 0x564E0004 },
    { 0x56550008, 0x46490002 },
    { 0x56550009, 0x454D0001 },
    { 0x5655000E, 0x53570001 },
    { 0x56550010, 0x544D0006 },
    { 0x56550013, 0x454D0003 },
    { 0x56550017, 0x544D0009 },
    { 0x5655001A, 0x564E0006 },
    { 0x5655002E, 0x5655000A },
    { 0x5655003B, 0x56550026 },
    { 0x56550040, 0x56550019 },
    { 0x5655801B, 0x5655001B },
    { 0x56560001, 0x56550003 },
    { 0x56560002, 0x56550016 },
    { 0x56560003, 0x5655000D },
    { 0x56560005, 0x56550004 },
    { 0x56560008, 0x56550011 },
    { 0x5656000A, 0x56550021 },
    { 0x56560010, 0x56550014 },
    { 0x56560011, 0x56550018 },
    { 0x56560021, 0x4D4A0001 },
    { 0x58490001, 0x494C0001 },
    { 0x58490007, 0x5656000D },
    { 0x584C0014, 0x4D53006E },
    { 0x584C8013, 0x584C0013 },
    { 0x584D0002, 0x4253000A },
    { 0x59420001, 0x46530001 },
    { 0x5A420300, 0x41430004 },
    { 0x5A430265, 0x45530019 },
    { 0x5A4302F9, 0x4D57002E },
    { 0x5A44019D, 0x454D0020 },
};

CompatStatus StatusFromName(const std::string &s)
{
    if (s == "Perfect") return CompatStatus::Perfect;
    if (s == "Playable") return CompatStatus::Playable;
    if (s == "Starts") return CompatStatus::Starts;
    if (s == "Intro") return CompatStatus::Intro;
    if (s == "Broken" || s == "Nothing") return CompatStatus::Broken;
    return CompatStatus::Unknown;
}
} // namespace

bool ParseCompatJson(const std::string &text,
                     std::map<uint32_t, CompatStatus> *out)
{
    out->clear();
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) {
        return false;
    }
    for (auto &t : j) {
        if (!t.is_object() || !t.contains("url") || !t["url"].is_string() ||
            !t.contains("status") || !t["status"].is_string()) {
            continue;
        }
        // "url": "/titles/4d530004#Halo-Combat-Evolved"
        std::string url = t["url"].get<std::string>();
        size_t p = url.find("/titles/");
        if (p == std::string::npos) {
            continue;
        }
        std::string hex = url.substr(p + 8, 8);
        char *end = nullptr;
        unsigned long id = strtoul(hex.c_str(), &end, 16);
        if (hex.size() != 8 || !end || *end) {
            continue;
        }
        (*out)[(uint32_t)id] = StatusFromName(t["status"].get<std::string>());
    }
    return !out->empty();
}

bool ParseCompatNames(const std::string &text,
                      std::map<uint32_t, std::string> *out)
{
    out->clear();
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_array()) {
        return false;
    }
    for (auto &t : j) {
        if (!t.is_object() || !t.contains("url") || !t["url"].is_string() ||
            !t.contains("name") || !t["name"].is_string()) {
            continue;
        }
        std::string url = t["url"].get<std::string>();
        size_t p = url.find("/titles/");
        if (p == std::string::npos) {
            continue;
        }
        std::string hex = url.substr(p + 8, 8);
        char *end = nullptr;
        unsigned long id = strtoul(hex.c_str(), &end, 16);
        if (hex.size() != 8 || !end || *end) {
            continue;
        }
        std::string name = t["name"].get<std::string>();
        if (!name.empty()) {
            (*out)[(uint32_t)id] = name;
        }
    }
    return !out->empty();
}

uint32_t CompatListedId(uint32_t title_id)
{
    for (auto &a : kAliases) {
        if (a.release == title_id) {
            return a.listed;
        }
    }
    return title_id;
}

CompatStatus CompatLookup(const std::map<uint32_t, CompatStatus> &list,
                          uint32_t title_id)
{
    auto it = list.find(title_id);
    if (it == list.end()) {
        it = list.find(CompatListedId(title_id));
    }
    return it == list.end() ? CompatStatus::None : it->second;
}

const char *CompatName(CompatStatus s)
{
    switch (s) {
    case CompatStatus::Perfect: return tr("Perfect");
    case CompatStatus::Playable: return tr("Playable");
    case CompatStatus::Starts: return tr("Starts");
    case CompatStatus::Intro: return tr("Intro");
    case CompatStatus::Broken: return tr("Broken");
    case CompatStatus::Unknown: return tr("Untested");
    default: return tr("Not listed");
    }
}

const char *CompatDescription(CompatStatus s)
{
    // As worded on xemu.app
    switch (s) {
    case CompatStatus::Perfect:
        return tr("Playable from start to finish with no noticeable issues.");
    case CompatStatus::Playable:
        return tr("Playable, with minor issues.");
    case CompatStatus::Starts:
        return tr("Starts, but may crash or have significant issues.");
    case CompatStatus::Intro:
        return tr("Displays an intro sequence, but fails to make it to gameplay.");
    case CompatStatus::Broken:
        return tr("Crashes very soon after launching, or displays nothing at all.");
    case CompatStatus::Unknown:
        return tr("A compatibility test has not been recorded for this title.");
    default:
        return tr("This title isn't in xemu's compatibility list.");
    }
}

std::string CompatTitleUrl(uint32_t title_id)
{
    char b[64];
    snprintf(b, sizeof(b), "https://xemu.app/titles/%08x", CompatListedId(title_id));
    return b;
}
