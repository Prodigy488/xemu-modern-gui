//
// xemu User Interface - performance capture analysis
//
// Turns a recording of per-frame graphics counters (from the nv2a profiler)
// into a plain-language report: how smooth it was, what was different in
// the slow frames, and what was going on around frames the user marked as
// glitched.
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

struct PerfFrame {
    double t_ms = 0;              // when it was seen, from the start
    int work_ms = 0;              // the frame's render time (nv2a "mspf")
    std::vector<int> counters;    // per counter, this frame
};

struct PerfSample {               // a few times a second
    double t_ms = 0;
    int game_fps = 0;             // frames the Xbox finished per second
    float host_fps = 0;           // frames xemu drew on the PC per second
};

struct PerfCapture {
    std::vector<std::string> counter_names; // "SHADER_GEN"...
    std::vector<PerfFrame> frames;
    std::vector<PerfSample> samples;
    std::vector<double> marks_ms;  // glitches marked by the user
    double duration_ms = 0;
    // Context for the report
    std::string game, renderer, gpu, settings;
};

struct PerfCounterDiff {
    std::string name;
    double slow_avg = 0, normal_avg = 0;
};

struct PerfFinding {
    int severity = 0;     // 2 likely cause, 1 worth a look, 0 info
    std::string title;
    std::string detail;
    std::string how; // how it was worked out, with the numbers
};

struct PerfReport {
    int frames = 0;
    double avg_fps = 0, low1_fps = 0; // game fps: average, 1% low
    int median_work = 0, p99_work = 0, worst_work = 0;
    int slow_frames = 0, slow_threshold = 0;
    int windows = 0; // one-second windows used for the trend check
    float host_fps_avg = 0;
    std::vector<PerfCounterDiff> slow_diffs;     // biggest differences first
    std::vector<PerfFinding> findings;           // most important first
    struct Mark {
        double t_ms = 0;
        std::vector<PerfCounterDiff> unusual;    // near the mark vs overall
        std::vector<std::string> notes;
        std::string how;
    };
    std::vector<Mark> marks;
};

PerfReport AnalyzePerf(const PerfCapture &cap);

// Plain-text report, for the report file and the clipboard
std::string PerfReportText(const PerfCapture &cap, const PerfReport &r);
// One line per frame, one column per counter
std::string PerfFramesCsv(const PerfCapture &cap);
// Chrome / Perfetto trace (JSON): frame time and counters as tracks, marks
// as instant events
std::string PerfTraceJson(const PerfCapture &cap);

// What a counter means, in a few words ("" if unknown)
const char *PerfCounterMeaning(const std::string &name);
