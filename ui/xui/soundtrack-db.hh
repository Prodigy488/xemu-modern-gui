//
// xemu User Interface - the Xbox dashboard's custom soundtrack database
//
// The dashboard keeps its custom soundtracks in E:\TDATA\fffe0000\music: an
// index file, ST.DB, and the songs as WMA files. This reads the index and
// adds songs to it (a new soundtrack, or more songs in an existing one),
// leaving everything already in it untouched. No file or disk access here.
//
// Layout (all little-endian, every record is 512 bytes):
//   0x0000          header: magic 1, soundtrack count, next soundtrack id,
//                   100 soundtrack ids, next song id
//   0x0200 + 512*n  soundtrack n (100 slots): magic 0x00021371, id, song
//                   count, 84 song group indexes, total ms (at 0x15C), name
//                   (80 UTF-16 chars at 0x160)
//   0xCA00 + 512*g  song group g: magic 0x00031073, soundtrack id, group id,
//                   1, 6 song ids, 6 song lengths (ms), 6 names (32 UTF-16
//                   chars each)
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

constexpr size_t kStRecord = 512;
constexpr int kStMaxSoundtracks = 100;
constexpr int kStMaxSongs = 500;       // per soundtrack
constexpr int kStSongsPerGroup = 6;
constexpr int kStNameChars = 31;       // the dashboard shows 32

struct StSoundtrackInfo {
    int32_t id = 0;
    std::string name;        // UTF-8
    uint32_t songs = 0;
    uint32_t total_ms = 0;
};

// The soundtracks in an ST.DB. False (with *err) if it isn't one.
bool StList(const std::vector<uint8_t> &db, std::vector<StSoundtrackInfo> *out,
            std::string *err);

struct StNewSong {
    std::string name;        // UTF-8, cut to kStNameChars
    uint32_t ms = 0;
};

// Where a song added by StAdd belongs on the disk
struct StPlaced {
    uint32_t song_id = 0;    // file name: %08x.wma
    uint32_t group_id = 0;   // folder name: %04u
};

// Add songs to the database. db is an existing ST.DB, or empty for none.
// soundtrack_id < 0 makes a new soundtrack called new_name; otherwise the
// songs are appended to that soundtrack. *db is rewritten; *placed has one
// entry per song, in order. False (with *err) if it doesn't fit or the file
// isn't a soundtrack database.
bool StAdd(std::vector<uint8_t> *db, int32_t soundtrack_id,
           const std::string &new_name, const std::vector<StNewSong> &songs,
           std::vector<StPlaced> *placed, std::string *err);

// Helpers shared with the converter
std::u16string StUtf8ToUtf16(const std::string &s);
std::string StUtf16ToUtf8(const std::u16string &s);
// Cut to at most n UTF-16 units without splitting a character
std::string StClipName(const std::string &s, size_t n);
