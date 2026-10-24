//
// xemu User Interface - performance capture analysis
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "perf-analysis.hh"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>

namespace
{

std::string Fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

int Percentile(std::vector<int> v, double p)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5));
    return v[i];
}

bool Starts(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool InGroup(const std::string &n, std::initializer_list<const char *> names,
             std::initializer_list<const char *> prefixes = {})
{
    for (const char *x : names) {
        if (n == x) {
            return true;
        }
    }
    for (const char *p : prefixes) {
        if (Starts(n, p)) {
            return true;
        }
    }
    return false;
}

std::string JsonEscape(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            o += Fmt("\\u%04x", c);
        } else {
            o += (char)c;
        }
    }
    return o;
}

std::string Clock(double ms)
{
    int s = (int)(ms / 1000);
    return Fmt("%d:%02d.%d", s / 60, s % 60, (int)(std::fmod(ms, 1000) / 100));
}

} // namespace

const char *PerfCounterMeaning(const std::string &n)
{
    static const std::map<std::string, const char *> m = {
        { "SHADER_GEN", "new shaders built (stutter the first time an effect appears)" },
        { "PIPELINE_GEN", "new Vulkan pipelines built (stutter the first time an effect appears)" },
        { "SURF_DOWNLOAD", "rendered images copied back from your GPU for the Xbox to read (slow)" },
        { "SURF_UPLOAD", "images the Xbox drew itself copied to your GPU" },
        { "SURF_CREATE", "new render targets created" },
        { "SURF_SWIZZLE", "image layout conversions" },
        { "SURF_TO_TEX", "rendered images reused as textures" },
        { "SURF_TO_TEX_FALLBACK", "rendered image reused as a texture by the slower fallback path" },
        { "TEX_UPLOAD", "textures uploaded to your GPU" },
        { "QUERY", "occlusion queries (visibility tests, e.g. for lens flares)" },
        { "CLEAR", "screen or buffer clears" },
        { "BEGIN_ENDS", "draw batches" },
        { "DRAW_ARRAYS", "draw calls" },
        { "INLINE_BUFFERS", "vertices sent inline (slower path)" },
        { "INLINE_ARRAYS", "vertices sent inline (slower path)" },
        { "INLINE_ELEMENTS", "vertices sent inline (slower path)" },
        { "PIPELINE_BIND", "pipeline changes" },
        { "PIPELINE_RENDERPASSES", "render passes" },
        { "PIPELINE_NOTDIRTY", "pipeline reused" },
        { "SHADER_BIND", "shader changes" },
        { "SHADER_BIND_NOTDIRTY", "shader reused" },
        { "SHADER_UBO_DIRTY", "shader constants updated" },
        { "SHADER_UBO_NOTDIRTY", "shader constants reused" },
        { "ATTR_BIND", "vertex format changes" },
        { "QUEUE_SUBMIT", "GPU work submitted" },
        { "QUEUE_SUBMIT_AUX", "extra GPU work submitted" },
        { "FINISH_VERTEX_BUFFER_DIRTY", "waited for the GPU: vertex data changed" },
        { "FINISH_SURFACE_CREATE", "waited for the GPU: new render target" },
        { "FINISH_SURFACE_DOWN", "waited for the GPU: image read back" },
        { "FINISH_NEED_BUFFER_SPACE", "waited for the GPU: ran out of buffer space" },
        { "FINISH_FRAMEBUFFER_DIRTY", "waited for the GPU: framebuffer changed" },
        { "FINISH_PRESENTING", "waited for the GPU: showing the frame" },
        { "FINISH_FLIP_STALL", "waited for the GPU: end of frame" },
        { "FINISH_FLUSH", "waited for the GPU: flush" },
        { "FINISH_STALLED", "waited for the GPU: the Xbox waited on it" },
    };
    auto it = m.find(n);
    if (it != m.end()) {
        return it->second;
    }
    if (Starts(n, "GEOM_BUFFER_UPDATE")) {
        return "vertex buffer updates";
    }
    if (Starts(n, "QUEUE_SUBMIT")) {
        return "GPU work submitted";
    }
    return "";
}

PerfReport AnalyzePerf(const PerfCapture &cap)
{
    PerfReport r;
    const size_t nc = cap.counter_names.size();
    r.frames = (int)cap.frames.size();

    // Frame rate from the periodic samples (skip the first, it may straddle
    // the start)
    double fps_sum = 0, host_sum = 0;
    int fps_n = 0;
    r.low1_fps = 1e9;
    for (size_t i = 0; i < cap.samples.size(); i++) {
        if (i == 0 && cap.samples.size() > 2) {
            continue;
        }
        fps_sum += cap.samples[i].game_fps;
        host_sum += cap.samples[i].host_fps;
        r.low1_fps = std::min<double>(r.low1_fps, cap.samples[i].game_fps);
        fps_n++;
    }
    r.avg_fps = fps_n ? fps_sum / fps_n : 0;
    r.host_fps_avg = fps_n ? (float)(host_sum / fps_n) : 0;
    if (!fps_n) {
        r.low1_fps = 0;
    }

    std::vector<int> work;
    for (auto &f : cap.frames) {
        work.push_back(f.work_ms);
    }
    r.median_work = Percentile(work, 0.5);
    r.p99_work = Percentile(work, 0.99);
    r.worst_work = work.empty() ? 0 : *std::max_element(work.begin(), work.end());
    r.slow_threshold = std::max(17, (int)std::ceil(r.median_work * 1.5));

    // Slow frames against the rest
    std::vector<double> slow(nc, 0), normal(nc, 0), all(nc, 0);
    int ns = 0, nn = 0;
    for (auto &f : cap.frames) {
        bool is_slow = f.work_ms > r.slow_threshold;
        (is_slow ? ns : nn)++;
        for (size_t c = 0; c < nc && c < f.counters.size(); c++) {
            (is_slow ? slow : normal)[c] += f.counters[c];
            all[c] += f.counters[c];
        }
    }
    r.slow_frames = ns;
    for (size_t c = 0; c < nc; c++) {
        double sa = ns ? slow[c] / ns : 0, na = nn ? normal[c] / nn : 0;
        all[c] = r.frames ? all[c] / r.frames : 0;
        if (ns && sa >= na * 1.5 + 1.0) {
            r.slow_diffs.push_back({ cap.counter_names[c], sa, na });
        }
    }
    std::sort(r.slow_diffs.begin(), r.slow_diffs.end(),
              [](const PerfCounterDiff &a, const PerfCounterDiff &b) {
                  return (a.slow_avg - a.normal_avg) / (a.normal_avg + 1) >
                         (b.slow_avg - b.normal_avg) / (b.normal_avg + 1);
              });
    if (r.slow_diffs.size() > 8) {
        r.slow_diffs.resize(8);
    }

    // Findings
    auto diff = [&](std::initializer_list<const char *> names,
                    std::initializer_list<const char *> prefixes = {}) {
        const PerfCounterDiff *best = nullptr;
        for (auto &d : r.slow_diffs) {
            if (InGroup(d.name, names, prefixes) &&
                (!best || d.slow_avg - d.normal_avg > best->slow_avg - best->normal_avg)) {
                best = &d;
            }
        }
        return best;
    };
    auto add = [&](int sev, const std::string &t, const std::string &d) {
        r.findings.push_back({ sev, t, d });
    };
    if (r.frames < 10) {
        add(0, "Not enough frames recorded",
            "Start a game, then record while it's running.");
        return r;
    }
    if (const PerfCounterDiff *d = diff({ "SHADER_GEN", "PIPELINE_GEN" })) {
        add(2, "Shaders being built while playing",
            Fmt("Slow frames built %.1f new %s each (other frames %.1f). This is "
                "stutter the first time an effect is seen; with the shader cache "
                "on, playing the same part again should be smoother. If it "
                "is, this was the cause.",
                d->slow_avg, d->name == "SHADER_GEN" ? "shaders" : "pipelines",
                d->normal_avg));
    }
    if (const PerfCounterDiff *d = diff({ "SURF_DOWNLOAD", "FINISH_SURFACE_DOWN" })) {
        add(2, "Rendered images read back from the GPU",
            Fmt("Slow frames copied %.1f images back from your GPU each (other "
                "frames %.1f). The game reads what it drew, which makes "
                "xemu wait for your GPU, and costs more at higher resolution "
                "scales. Compare at 1x.",
                d->slow_avg, d->normal_avg));
    }
    // (FINISH_SURFACE_DOWN/CREATE are covered by their own findings)
    if (const PerfCounterDiff *d = diff(
            { "QUERY", "FINISH_VERTEX_BUFFER_DIRTY", "FINISH_NEED_BUFFER_SPACE",
              "FINISH_FRAMEBUFFER_DIRTY", "FINISH_PRESENTING", "FINISH_FLIP_STALL",
              "FINISH_FLUSH", "FINISH_STALLED" })) {
        if (d->name == "QUERY") {
            add(2, "Occlusion queries",
                Fmt("Slow frames ran %.1f visibility queries each (other frames "
                    "%.1f). Each one waits for your GPU's answer, a known "
                    "source of slowdown in xemu.",
                    d->slow_avg, d->normal_avg));
        } else {
            add(2, "Waiting for the GPU",
                Fmt("Slow frames waited for your GPU to finish more often "
                    "(%s: %.1f per frame vs %.1f). Emulation stops while it "
                    "waits.",
                    d->name.c_str(), d->slow_avg, d->normal_avg));
        }
    }
    if (const PerfCounterDiff *d = diff({ "TEX_UPLOAD", "SURF_UPLOAD", "SURF_SWIZZLE" })) {
        add(1, "Lots of data sent to the GPU",
            Fmt("%s: %.1f per slow frame vs %.1f (%s). Typical when a new area, "
                "video or streamed textures load.",
                d->name.c_str(), d->slow_avg, d->normal_avg,
                PerfCounterMeaning(d->name)));
    }
    if (const PerfCounterDiff *d = diff({ "SURF_CREATE", "FINISH_SURFACE_CREATE" })) {
        add(1, "New render targets",
            Fmt("Slow frames created %.1f render targets each (other frames "
                "%.1f).",
                d->slow_avg, d->normal_avg));
    }
    if (const PerfCounterDiff *d = diff({}, { "INLINE_" })) {
        add(1, "Vertices sent the slow way",
            Fmt("%s: %.1f per slow frame vs %.1f.", d->name.c_str(), d->slow_avg,
                d->normal_avg));
    }
    if (const PerfCounterDiff *d =
            diff({ "BEGIN_ENDS", "DRAW_ARRAYS", "CLEAR", "PIPELINE_BIND",
                   "SHADER_BIND", "ATTR_BIND", "PIPELINE_RENDERPASSES" },
                 { "GEOM_BUFFER_UPDATE", "QUEUE_SUBMIT" })) {
        add(1, "More graphics work in the slow frames",
            Fmt("%s: %.1f per slow frame vs %.1f (%s). A busier scene.",
                d->name.c_str(), d->slow_avg, d->normal_avg,
                PerfCounterMeaning(d->name)));
    }
    if (r.slow_frames > 0 && r.slow_diffs.empty()) {
        add(2, "Graphics look the same in the slow frames",
            "None of the graphics counters went up when frames were slow, so "
            "the time is probably going to the emulated Xbox CPU (game logic, "
            "physics, audio) rather than graphics.");
    }
    if (r.median_work > 34) {
        // Everything is slow: what each frame does on average
        std::vector<std::pair<double, std::string>> top;
        for (size_t c = 0; c < nc; c++) {
            if (all[c] >= 1) {
                top.push_back({ all[c], cap.counter_names[c] });
            }
        }
        std::sort(top.rbegin(), top.rend());
        std::string list;
        for (size_t i = 0; i < top.size() && i < 4; i++) {
            list += Fmt("%s%s %.0f", i ? ", " : "", top[i].second.c_str(),
                        top[i].first);
        }
        add(2, "Every frame is slow",
            Fmt("A typical frame takes %d ms (over 33 ms means under 30 fps), "
                "so this isn't a spike. Most frequent work per frame: %s.",
                r.median_work, list.empty() ? "none recorded" : list.c_str()));
    }
    if (r.host_fps_avg > 0 && r.avg_fps > 0 && r.host_fps_avg < 50 &&
        r.host_fps_avg < r.avg_fps * 0.9) {
        add(1, "xemu's own drawing is slow",
            Fmt("xemu drew %.0f frames a second on your PC while the game made "
                "%.0f. The upscaling filter, resolution scale or a large window "
                "may be too much for your GPU.",
                r.host_fps_avg, r.avg_fps));
    }
    if (r.slow_frames == 0 && r.median_work <= 34) {
        add(0, "No slowdown in this recording",
            Fmt("No frame took longer than %d ms.", r.slow_threshold));
    }
    std::stable_sort(r.findings.begin(), r.findings.end(),
                     [](const PerfFinding &a, const PerfFinding &b) {
                         return a.severity > b.severity;
                     });

    // Around each glitch mark (half a second either side)
    for (double m : cap.marks_ms) {
        PerfReport::Mark mk;
        mk.t_ms = m;
        std::vector<double> near(nc, 0);
        int n = 0;
        for (auto &f : cap.frames) {
            if (std::fabs(f.t_ms - m) <= 500) {
                n++;
                for (size_t c = 0; c < nc && c < f.counters.size(); c++) {
                    near[c] += f.counters[c];
                }
            }
        }
        for (size_t c = 0; c < nc && n; c++) {
            double na = near[c] / n;
            if (na >= all[c] * 2 + 0.5 || (all[c] == 0 && na > 0)) {
                mk.unusual.push_back({ cap.counter_names[c], na, all[c] });
            }
        }
        std::sort(mk.unusual.begin(), mk.unusual.end(),
                  [](const PerfCounterDiff &a, const PerfCounterDiff &b) {
                      return (a.slow_avg - a.normal_avg) / (a.normal_avg + 1) >
                             (b.slow_avg - b.normal_avg) / (b.normal_avg + 1);
                  });
        if (mk.unusual.size() > 6) {
            mk.unusual.resize(6);
        }
        std::set<std::string> u;
        for (auto &d : mk.unusual) {
            u.insert(d.name);
        }
        if (!n) {
            mk.notes.push_back("No frames recorded around this mark.");
        }
        if (u.count("SURF_TO_TEX_FALLBACK")) {
            mk.notes.push_back("A rendered image was turned into a texture by the "
                               "fallback path. Effects drawn this way can come "
                               "out wrong; compare with the Vulkan renderer.");
        }
        if (u.count("SHADER_GEN") || u.count("PIPELINE_GEN")) {
            mk.notes.push_back("New shaders appeared right then: the effect may be "
                               "new on screen, and drawn by a shader that "
                               "doesn't translate correctly.");
        }
        if (u.count("QUERY")) {
            mk.notes.push_back("Occlusion queries ran: these decide whether things "
                               "like lens flares and coronas are drawn, a common "
                               "source of missing or flickering effects.");
        }
        if (u.count("SURF_DOWNLOAD") || u.count("SURF_UPLOAD") ||
            u.count("SURF_SWIZZLE")) {
            mk.notes.push_back("Images moved between the Xbox's memory and your "
                               "GPU: render-to-texture effects (reflections, "
                               "shadows, motion blur, screen filters) live here. "
                               "Compare at 1x resolution.");
        }
        if (u.count("SURF_CREATE")) {
            mk.notes.push_back("New render targets were created: a new effect or "
                               "screen mode started.");
        }
        if (mk.unusual.empty() && n) {
            mk.notes.push_back("Nothing unusual in the counters here, so the "
                               "glitch is probably in how something is drawn "
                               "rather than what. A RenderDoc capture (Debug "
                               "menu, where available) shows the draw calls.");
        }
        r.marks.push_back(std::move(mk));
    }
    return r;
}

std::string PerfReportText(const PerfCapture &cap, const PerfReport &r)
{
    std::string o;
    o += "xemu Modern GUI - performance capture\n";
    o += "======================================\n";
    o += "Experimental, unofficial xemu fork. Not supported by the xemu project.\n\n";
    o += "Game:      " + (cap.game.empty() ? std::string("(unknown)") : cap.game) + "\n";
    o += "Renderer:  " + cap.renderer + "\n";
    if (!cap.gpu.empty()) {
        o += "GPU:       " + cap.gpu + "\n";
    }
    if (!cap.settings.empty()) {
        o += "Settings:  " + cap.settings + "\n";
    }
    o += Fmt("Length:    %.1f s, %d frames\n\n", cap.duration_ms / 1000, r.frames);
    o += Fmt("Game frame rate: %.0f average, %.0f lowest\n", r.avg_fps, r.low1_fps);
    o += Fmt("Frame render time: %d ms typical, %d ms 99th percentile, %d ms worst\n",
             r.median_work, r.p99_work, r.worst_work);
    o += Fmt("Slow frames (over %d ms): %d (%.1f%%)\n", r.slow_threshold,
             r.slow_frames, r.frames ? 100.0 * r.slow_frames / r.frames : 0);
    if (r.host_fps_avg > 0) {
        o += Fmt("xemu drawing on the PC: %.0f fps\n", r.host_fps_avg);
    }
    o += "\nWhat stands out\n---------------\n";
    for (auto &f : r.findings) {
        o += std::string(f.severity == 2 ? "[likely] " : f.severity == 1 ? "[maybe]  " : "[info]   ") +
             f.title + "\n    " + f.detail + "\n";
    }
    if (!r.slow_diffs.empty()) {
        o += "\nCounters per frame: slow frames vs the rest\n";
        o += "-------------------------------------------\n";
        for (auto &d : r.slow_diffs) {
            o += Fmt("  %-28s %8.1f %8.1f   %s\n", d.name.c_str(), d.slow_avg,
                     d.normal_avg, PerfCounterMeaning(d.name));
        }
    }
    for (auto &m : r.marks) {
        o += "\nGlitch marked at " + Clock(m.t_ms) + "\n";
        o += "-----------------------\n";
        for (auto &d : m.unusual) {
            o += Fmt("  %-28s %8.1f near the mark, %.1f usually   %s\n",
                     d.name.c_str(), d.slow_avg, d.normal_avg,
                     PerfCounterMeaning(d.name));
        }
        for (auto &n : m.notes) {
            o += "  - " + n + "\n";
        }
    }
    if (!r.marks.empty()) {
        o += "\nTo narrow a glitch down, change one thing and check again:\n"
             "  - Resolution scale 1x (many glitches only show when upscaled)\n"
             "  - The other renderer, Vulkan or OpenGL (restart needed)\n"
             "  - Patches for this game off\n"
             "  - Upscaling filter Linear\n"
             "  - Official xemu, to see if this fork is involved\n";
    }
    return o;
}

std::string PerfFramesCsv(const PerfCapture &cap)
{
    std::string o = "frame,time_ms,render_ms";
    for (auto &n : cap.counter_names) {
        o += "," + n;
    }
    o += ",glitch_mark\n";
    size_t mi = 0;
    for (size_t i = 0; i < cap.frames.size(); i++) {
        const PerfFrame &f = cap.frames[i];
        o += Fmt("%zu,%.1f,%d", i, f.t_ms, f.work_ms);
        for (size_t c = 0; c < cap.counter_names.size(); c++) {
            o += Fmt(",%d", c < f.counters.size() ? f.counters[c] : 0);
        }
        bool mark = false;
        while (mi < cap.marks_ms.size() && cap.marks_ms[mi] <= f.t_ms) {
            mark = true;
            mi++;
        }
        o += mark ? ",1\n" : ",0\n";
    }
    return o;
}

std::string PerfTraceJson(const PerfCapture &cap)
{
    std::string o = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
    bool first = true;
    auto ev = [&](const std::string &e) {
        o += (first ? "" : ",\n") + e;
        first = false;
    };
    ev("{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"" +
       JsonEscape(cap.game.empty() ? "xemu" : cap.game) + "\"}}");
    std::vector<bool> used(cap.counter_names.size(), false);
    for (auto &f : cap.frames) {
        for (size_t c = 0; c < f.counters.size() && c < used.size(); c++) {
            used[c] = used[c] || f.counters[c];
        }
    }
    for (auto &f : cap.frames) {
        long long ts = (long long)(f.t_ms * 1000);
        ev(Fmt("{\"name\":\"Frame render time (ms)\",\"ph\":\"C\",\"pid\":1,"
               "\"ts\":%lld,\"args\":{\"ms\":%d}}",
               ts, f.work_ms));
        for (size_t c = 0; c < used.size(); c++) {
            if (used[c]) {
                ev(Fmt("{\"name\":\"%s\",\"ph\":\"C\",\"pid\":1,\"ts\":%lld,"
                       "\"args\":{\"count\":%d}}",
                       JsonEscape(cap.counter_names[c]).c_str(), ts,
                       c < f.counters.size() ? f.counters[c] : 0));
            }
        }
    }
    for (auto &s : cap.samples) {
        ev(Fmt("{\"name\":\"Frame rate\",\"ph\":\"C\",\"pid\":1,\"ts\":%lld,"
               "\"args\":{\"game\":%d,\"xemu\":%.0f}}",
               (long long)(s.t_ms * 1000), s.game_fps, s.host_fps));
    }
    for (double m : cap.marks_ms) {
        ev(Fmt("{\"name\":\"Glitch marked\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,"
               "\"tid\":1,\"ts\":%lld}",
               (long long)(m * 1000)));
    }
    o += "\n]}\n";
    return o;
}
