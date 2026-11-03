//
// xemu User Interface - the Xbox dashboard's custom soundtrack database
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "soundtrack-db.hh"

#include <algorithm>
#include <cstring>

namespace
{
constexpr uint32_t kHeaderMagic = 1;
constexpr uint32_t kSoundtrackMagic = 0x00021371;
constexpr uint32_t kGroupMagic = 0x00031073;
constexpr size_t kSoundtrackBase = 0x200;
constexpr size_t kGroupBase = 0x200 + 100 * 0x200; // 0xCA00

// Header fields
constexpr size_t kHdCount = 4, kHdNextSt = 8, kHdIds = 12, kHdNextSong = 412;
// Soundtrack fields
constexpr size_t kStId = 4, kStSongs = 8, kStGroups = 12, kStTotal = 348, kStName = 352;
constexpr int kStNameUnits = 80;
// Group fields
constexpr size_t kGrSoundtrack = 4, kGrId = 8, kGrPad = 12, kGrSongs = 16,
                 kGrTimes = 40, kGrNames = 64;
constexpr int kGrNameUnits = 32;

uint32_t Get(const std::vector<uint8_t> &b, size_t off)
{
    uint32_t v;
    memcpy(&v, &b[off], 4);
    return v;
}

void Put(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    memcpy(&b[off], &v, 4);
}

std::u16string GetName(const std::vector<uint8_t> &b, size_t off, int units)
{
    std::u16string s;
    for (int i = 0; i < units; i++) {
        char16_t c;
        memcpy(&c, &b[off + i * 2], 2);
        if (!c) {
            break;
        }
        s.push_back(c);
    }
    return s;
}

void PutName(std::vector<uint8_t> &b, size_t off, int units, const std::u16string &s)
{
    memset(&b[off], 0, units * 2);
    for (int i = 0; i < units - 1 && i < (int)s.size(); i++) {
        char16_t c = s[i];
        memcpy(&b[off + i * 2], &c, 2);
    }
}

bool Validate(const std::vector<uint8_t> &db, std::string *err)
{
    if (db.size() < kGroupBase) {
        *err = "This isn't a soundtrack database (too small)";
        return false;
    }
    if (Get(db, 0) != kHeaderMagic) {
        *err = "This isn't a soundtrack database (unknown header)";
        return false;
    }
    uint32_t n = Get(db, kHdCount);
    if (n > (uint32_t)kStMaxSoundtracks) {
        *err = "The soundtrack database looks damaged";
        return false;
    }
    return true;
}

size_t SoundtrackOff(int32_t id)
{
    return kSoundtrackBase + (size_t)id * kStRecord;
}

size_t GroupOff(size_t index)
{
    return kGroupBase + index * kStRecord;
}

bool InList(const std::vector<uint8_t> &db, int32_t id)
{
    uint32_t n = Get(db, kHdCount);
    for (uint32_t i = 0; i < n; i++) {
        if ((int32_t)Get(db, kHdIds + i * 4) == id) {
            return true;
        }
    }
    return false;
}
} // namespace

std::u16string StUtf8ToUtf16(const std::string &s)
{
    std::u16string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        uint32_t cp = 0xFFFD;
        int len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6 && i + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            len = 2;
        } else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            len = 3;
        } else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
                 ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((char16_t)(0xD800 + (cp >> 10)));
            out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((char16_t)cp);
        }
    }
    return out;
}

std::string StUtf16ToUtf8(const std::u16string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string StClipName(const std::string &s, size_t n)
{
    std::u16string u = StUtf8ToUtf16(s);
    if (u.size() > n) {
        u.resize(n);
        if (!u.empty() && u.back() >= 0xD800 && u.back() < 0xDC00) {
            u.pop_back(); // half of a pair
        }
    }
    // Trim spaces left at the ends
    while (!u.empty() && u.back() == u' ') {
        u.pop_back();
    }
    size_t start = 0;
    while (start < u.size() && u[start] == u' ') {
        start++;
    }
    return StUtf16ToUtf8(u.substr(start));
}

bool StList(const std::vector<uint8_t> &db, std::vector<StSoundtrackInfo> *out,
            std::string *err)
{
    out->clear();
    if (!Validate(db, err)) {
        return false;
    }
    uint32_t n = Get(db, kHdCount);
    for (uint32_t i = 0; i < n; i++) {
        int32_t id = (int32_t)Get(db, kHdIds + i * 4);
        if (id < 0 || id >= kStMaxSoundtracks) {
            continue;
        }
        size_t off = SoundtrackOff(id);
        if (Get(db, off) != kSoundtrackMagic) {
            continue;
        }
        StSoundtrackInfo si;
        si.id = id;
        si.songs = Get(db, off + kStSongs);
        si.total_ms = Get(db, off + kStTotal);
        si.name = StUtf16ToUtf8(GetName(db, off + kStName, kStNameUnits));
        out->push_back(si);
    }
    return true;
}

bool StAdd(std::vector<uint8_t> *dbp, int32_t soundtrack_id, const std::string &new_name,
           const std::vector<StNewSong> &songs, std::vector<StPlaced> *placed,
           std::string *err)
{
    std::vector<uint8_t> &db = *dbp;
    placed->clear();
    if (songs.empty()) {
        *err = "There are no songs to add";
        return false;
    }
    if (db.empty()) {
        db.assign(kGroupBase, 0);
        Put(db, 0, kHeaderMagic);
    }
    if (!Validate(db, err)) {
        return false;
    }
    // Only whole group records count
    size_t group_count = (db.size() - kGroupBase) / kStRecord;
    db.resize(kGroupBase + group_count * kStRecord);

    // The next song number: past everything already used
    uint32_t next_song = Get(db, kHdNextSong);
    for (size_t g = 0; g < group_count; g++) {
        size_t go = GroupOff(g);
        if (Get(db, go) != kGroupMagic) {
            continue;
        }
        for (int k = 0; k < kStSongsPerGroup; k++) {
            bool used = Get(db, go + kGrTimes + k * 4) != 0 ||
                        !GetName(db, go + kGrNames + k * kGrNameUnits * 2, kGrNameUnits)
                             .empty();
            if (used) {
                next_song = std::max(next_song, Get(db, go + kGrSongs + k * 4) + 1);
            }
        }
    }

    // The soundtrack the songs go into
    int32_t id = soundtrack_id;
    if (id < 0) {
        std::string name = StClipName(new_name, kStNameChars);
        if (name.empty()) {
            *err = "Give the new soundtrack a name";
            return false;
        }
        uint32_t count = Get(db, kHdCount);
        if (count >= (uint32_t)kStMaxSoundtracks) {
            *err = "The Xbox already has 100 soundtracks, the most it can keep";
            return false;
        }
        id = -1;
        for (int k = 0; k < kStMaxSoundtracks; k++) {
            if (Get(db, SoundtrackOff(k)) != kSoundtrackMagic && !InList(db, k)) {
                id = k;
                break;
            }
        }
        if (id < 0) {
            *err = "The Xbox already has 100 soundtracks, the most it can keep";
            return false;
        }
        size_t off = SoundtrackOff(id);
        memset(&db[off], 0, kStRecord);
        Put(db, off, kSoundtrackMagic);
        Put(db, off + kStId, (uint32_t)id);
        PutName(db, off + kStName, kStNameUnits, StUtf8ToUtf16(name));
        Put(db, kHdIds + count * 4, (uint32_t)id);
        Put(db, kHdCount, count + 1);
        Put(db, kHdNextSt, std::max(Get(db, kHdNextSt), (uint32_t)id + 1));
    } else if (id >= kStMaxSoundtracks || !InList(db, id) ||
               Get(db, SoundtrackOff(id)) != kSoundtrackMagic) {
        *err = "That soundtrack isn't on the Xbox any more";
        return false;
    }

    size_t soff = SoundtrackOff(id);
    uint32_t have = Get(db, soff + kStSongs);
    if (have + songs.size() > (size_t)kStMaxSongs) {
        *err = "A soundtrack can hold at most 500 songs";
        return false;
    }
    uint32_t total = Get(db, soff + kStTotal);
    for (const StNewSong &s : songs) {
        uint32_t n = Get(db, soff + kStSongs);
        size_t slot = n % kStSongsPerGroup;
        size_t gi = n / kStSongsPerGroup;
        size_t g;
        if (slot == 0) {
            g = group_count++;
            db.resize(kGroupBase + group_count * kStRecord, 0);
            size_t go = GroupOff(g);
            Put(db, go, kGroupMagic);
            Put(db, go + kGrSoundtrack, (uint32_t)id);
            Put(db, go + kGrId, (uint32_t)g);
            Put(db, go + kGrPad, 1);
            Put(db, soff + kStGroups + gi * 4, (uint32_t)g);
        } else {
            g = Get(db, soff + kStGroups + gi * 4);
            if (g >= group_count || Get(db, GroupOff(g)) != kGroupMagic) {
                *err = "The soundtrack database looks damaged";
                return false;
            }
        }
        size_t go = GroupOff(g);
        uint32_t song_id = next_song++;
        Put(db, go + kGrSongs + slot * 4, song_id);
        Put(db, go + kGrTimes + slot * 4, s.ms);
        PutName(db, go + kGrNames + slot * kGrNameUnits * 2, kGrNameUnits,
                StUtf8ToUtf16(StClipName(s.name, kStNameChars)));
        Put(db, soff + kStSongs, n + 1);
        total += s.ms;
        placed->push_back({ song_id, Get(db, go + kGrId) });
    }
    Put(db, soff + kStTotal, total);
    Put(db, kHdNextSong, next_song);
    return true;
}
