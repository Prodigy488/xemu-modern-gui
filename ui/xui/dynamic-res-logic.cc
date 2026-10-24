//
// xemu User Interface - dynamic resolution (decision logic)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "dynamic-res-logic.hh"

#include <algorithm>
#include <cstdio>

namespace
{
const double kKeepMs = 120000;    // frame rate history kept (for the target)
const double kSlowFor = 3000;     // how long it must be slow before lowering
const double kSettle = 1500;      // ignore this long after a change (the stall)
const double kCheckAfter = 4500;  // judge a change after this long
const double kRaiseAfter = 15000; // smooth this long before raising
const double kMinGap = 6000;      // between any two changes

std::string ScaleMsg(int from, int to, const char *why)
{
    char b[128];
    snprintf(b, sizeof(b), "Dynamic resolution: %dx -> %dx (%s)", from, to, why);
    return b;
}
} // namespace

double DynResLogic::Avg(double now, double span, double skip_before) const
{
    double sum = 0;
    int n = 0;
    for (auto &s : m_samples) {
        if (s.t >= now - span && s.t > skip_before) {
            sum += s.fps;
            n++;
        }
    }
    return n ? sum / n : -1;
}

// The game's usual frame rate: 60 or 30, from the best second recently
int DynResLogic::Target(double now) const
{
    double best = 0;
    for (auto &s : m_samples) {
        if (s.t >= now - kKeepMs) {
            best = std::max(best, (double)s.fps);
        }
    }
    if (best >= 50) {
        return 60;
    }
    if (best >= 25) {
        return 30;
    }
    return 0; // loading screens, menus at odd rates: leave alone
}

int DynResLogic::Release()
{
    int restore = Lowered() ? m_user : 0;
    m_user = m_cur = 0;
    m_check = None;
    m_samples.clear();
    return restore;
}

DynResLogic::Result DynResLogic::Update(double now, int game_fps, int scale,
                                        bool running, const std::string &game)
{
    Result r;
    if (game != m_game) {
        // Another game (or none): give the old one's resolution back, unless
        // something else (its own settings) already changed it
        int restore = scale == m_cur ? Release() : (Release(), 0);
        m_game = game;
        m_no_drop_until = m_no_raise_until = -1e9;
        if (restore) {
            r.set_scale = restore;
            return r;
        }
    }
    if (!running || game.empty()) {
        return r;
    }
    if (!m_user || scale != m_cur) {
        // First look, or the user (or a per-game setting) changed it
        m_user = m_cur = scale;
        m_check = None;
        m_last_change = now;
    }
    m_samples.push_back({ now, game_fps });
    while (!m_samples.empty() && m_samples.front().t < now - kKeepMs) {
        m_samples.pop_front();
    }
    int target = Target(now);
    double since = now - m_last_change;

    // Judge the last change
    if (m_check != None && since >= kCheckAfter) {
        double after = Avg(now, kCheckAfter - kSettle, m_last_change + kSettle);
        if (m_check == AfterDrop) {
            m_check = None;
            if (after < m_fps_before * 1.08 + 1) {
                // Lower resolution didn't help: put it back, try later
                m_no_drop_until = now + 60000;
                r.set_scale = m_cur + 1;
                r.message = ScaleMsg(m_cur, m_cur + 1, "didn't help, restored");
                m_cur++;
                m_last_change = now;
                return r;
            }
        } else {
            m_check = None;
            if (target && after < target * 0.85) {
                // Too much at this resolution: back down, wait longer
                m_no_raise_until = now + 120000;
                r.set_scale = m_cur - 1;
                r.message = ScaleMsg(m_cur, m_cur - 1, "too slow");
                m_cur--;
                m_last_change = now;
                m_check = None;
                return r;
            }
        }
    }
    if (!target || m_check != None || since < kMinGap) {
        return r;
    }
    // Lower: nearly every sample of the last few seconds slow, and lowering
    // hasn't just failed
    int n = 0, slow = 0;
    for (auto &smp : m_samples) {
        if (smp.t >= now - kSlowFor && smp.t > m_last_change + kSettle) {
            n++;
            slow += smp.fps < target * 0.85;
        }
    }
    double recent = Avg(now, kSlowFor, m_last_change + kSettle);
    bool slow_long_enough = n >= (int)(kSlowFor / 250) - 1 && slow >= n * 0.8;
    if (m_cur > 1 && now >= m_no_drop_until && slow_long_enough &&
        recent < target * 0.85) {
        m_fps_before = recent;
        r.set_scale = m_cur - 1;
        r.message = ScaleMsg(m_cur, m_cur - 1, "slowdown");
        m_cur--;
        m_last_change = now;
        m_check = AfterDrop;
        return r;
    }
    // Raise back towards the user's choice when smooth for a while
    double smooth = Avg(now, kRaiseAfter, m_last_change + kSettle);
    if (m_cur < m_user && now >= m_no_raise_until && since >= kRaiseAfter &&
        smooth >= target * 0.97) {
        r.set_scale = m_cur + 1;
        r.message = ScaleMsg(m_cur, m_cur + 1, "running smoothly");
        m_cur++;
        m_last_change = now;
        m_check = AfterRaise;
        return r;
    }
    return r;
}
