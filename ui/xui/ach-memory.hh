//
// xemu User Interface - achievement triggers and the address finder's search
//
// An achievement can have a trigger: a few conditions about values in the
// running game's memory ("the value at this address is at least 5"). xemu
// checks them about ten times a second while that game runs and unlocks the
// achievement by itself when they hold. The finder searches the game's
// memory for the addresses worth watching.
//
// This file has no ImGui or emulator code in it, so it can be tested alone:
// memory is read through a function you pass in.
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
#include <nlohmann/json_fwd.hpp>

enum AchSize { kAchU8, kAchU16, kAchU32, kAchI8, kAchI16, kAchI32, kAchF32 };
enum AchOp { kAchEq, kAchNe, kAchLt, kAchLe, kAchGt, kAchGe };

struct AchCond {
    uint32_t addr = 0;           // the game's own (virtual) address
    AchSize size = kAchU32;
    uint32_t mask = 0xFFFFFFFFu; // bits to look at (for flags)
    AchOp op = kAchEq;
    bool vs_prev = false;        // compare with the value at the last check
    double value = 0;
    // while running
    double prev = 0;
    bool have_prev = false;
    bool latched = false;        // a "vs previous" condition that has been true
};

struct AchTrigger {
    std::vector<AchCond> all;       // every one must hold
    std::vector<AchCond> any;       // if there are any, at least one must hold
    int hold = 3;                   // checks in a row (0.1 s each)
    std::vector<uint32_t> builds;   // XBE build stamps it was made for (empty = any)
    // while running
    int streak = 0;
};

// Reads len bytes at a game address. False if they can't be read.
typedef bool (*AchReadFn)(uint32_t addr, void *buf, uint32_t len, void *user);

uint32_t AchSizeBytes(AchSize s);
const char *AchSizeName(AchSize s);                // "u8" ... "f32"
bool AchSizeFromName(const std::string &n, AchSize *out);
const char *AchOpName(AchOp o);                    // "==" ...
bool AchOpFromName(const std::string &n, AchOp *out);

// Raw bits (zero-extended) <-> number, for a size
double AchRawToValue(AchSize s, uint32_t raw);
uint32_t AchValueToRaw(AchSize s, double v);

// Pack files come from other people: these never throw, and refuse anything
// out of range. Parse returns false for a trigger that can't be used.
bool AchCondParse(const nlohmann::json &j, AchCond &out);
bool AchTriggerParse(const nlohmann::json &j, AchTrigger &out);
nlohmann::json AchCondToJson(const AchCond &c);
nlohmann::json AchTriggerToJson(const AchTrigger &t);
// "4 bytes at 0x0045A3C0 is at least 5"
std::string AchCondText(const AchCond &c);

// Reads one condition's number. False if it couldn't be read.
bool AchReadCond(const AchCond &c, AchReadFn fn, void *user, double *out);
// Forget what was seen (a new game, or the game started over)
void AchTriggerReset(AchTrigger &t);
// One check (call about every 0.1 s). True once the conditions have held
// for `hold` checks in a row.
bool AchTriggerTick(AchTrigger &t, AchReadFn fn, void *user);

// ---------------------------------------------------------------------------
// Searching memory
// ---------------------------------------------------------------------------

enum AchScanKind { kScanEqual, kScanChanged, kScanUnchanged, kScanIncreased, kScanDecreased };

struct AchHit {
    uint32_t addr;
    uint32_t last; // raw value when last looked at
};

class AchSearch {
public:
    static const size_t kMaxHits = 2000000;
    // Starts a search over these pages (4 KB each). With `unknown` no value
    // is needed: it remembers everything to compare against later.
    // Returns how many addresses are left, -1 for "too many to list" or -2 if
    // nothing could be read.
    long First(AchSize size, bool unknown, double value,
               const std::vector<uint32_t> &pages, AchReadFn fn, void *user);
    // Keeps the addresses whose value now fits
    long Next(AchScanKind kind, double value, AchReadFn fn, void *user);
    void Reset();
    bool Active() const { return m_active; }
    // Still holding every address in memory (no list yet)
    bool Remembering() const { return m_active && m_snap_mode; }
    AchSize Size() const { return m_size; }
    const std::vector<AchHit> &Hits() const { return m_hits; }

private:
    bool m_active = false;
    bool m_snap_mode = false;
    AchSize m_size = kAchU32;
    std::vector<uint32_t> m_snap_pages;
    std::vector<uint8_t> m_snap; // pages * 4096
    std::vector<AchHit> m_hits;
};
