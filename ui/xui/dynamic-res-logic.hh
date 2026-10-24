//
// xemu User Interface - dynamic resolution (decision logic)
//
// Lowers the internal resolution while a game runs below its usual frame
// rate and raises it again when there's room. Each change is checked: if
// lowering didn't raise the frame rate (the slowdown isn't about
// resolution), it's undone and not tried again for a while; if raising
// brings the slowdown back, it goes back down and waits longer before the
// next try. Changing the resolution briefly stalls the emulated GPU, so
// changes are rare.
//
// Assumes it's called about four times a second.
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
#include <deque>
#include <string>

class DynResLogic
{
public:
    struct Result {
        int set_scale = 0;   // non-zero: change the resolution to this
        std::string message; // shown briefly when it changes
    };

    // Call a few times a second while enabled. scale: the resolution in
    // effect; game: changes when another game starts (resets everything)
    Result Update(double now_ms, int game_fps, int scale, bool running,
                  const std::string &game);
    // Stop managing: what to set to give the user their resolution back
    // (0 if nothing was lowered)
    int Release();

    int UserScale() const { return m_user; } // 0 when not managing
    bool SameGame(const std::string &game) const { return game == m_game; }
    bool Lowered() const { return m_user && m_cur < m_user; }

private:
    double Avg(double now, double span, double skip_before = -1) const;
    int Target(double now) const;

    struct Sample {
        double t;
        int fps;
    };
    std::deque<Sample> m_samples;
    std::string m_game;
    int m_user = 0;          // the user's resolution
    int m_cur = 0;           // what we last set (or saw)
    double m_last_change = -1e9;
    // Checking a change
    enum { None, AfterDrop, AfterRaise } m_check = None;
    double m_fps_before = 0;
    double m_no_drop_until = -1e9;  // lowering didn't help recently
    double m_no_raise_until = -1e9; // raising brought the slowdown back
};
