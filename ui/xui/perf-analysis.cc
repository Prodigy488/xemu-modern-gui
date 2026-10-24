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
    auto add = [&](int sev, const std::string &t, const std::string &d,
                   const std::string &how = "") {
        r.findings.push_back({ sev, t, d, how });
    };
    // How a slow-frame comparison was made
    auto slow_how = [&](const PerfCounterDiff *d) {
        return Fmt("Took the %d frames that needed more than %d ms to render "
                   "(1.5 times the typical %d ms, and never under 17 ms) and "
                   "compared their average counters with the other %d frames. "
                   "%s averaged %.1f per slow frame and %.1f per other frame; "
                   "a counter only counts as different when it is at least "
                   "1.5 times higher and up by at least 1 per frame.",
                   r.slow_frames, r.slow_threshold, r.median_work,
                   r.frames - r.slow_frames, d->name.c_str(), d->slow_avg,
                   d->normal_avg);
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
                d->normal_avg),
            slow_how(d));
    }
    if (const PerfCounterDiff *d = diff({ "SURF_DOWNLOAD", "FINISH_SURFACE_DOWN" })) {
        add(2, "Rendered images read back from the GPU",
            Fmt("Slow frames copied %.1f images back from your GPU each (other "
                "frames %.1f). The game reads what it drew, which makes "
                "xemu wait for your GPU, and costs more at higher resolution "
                "scales. Compare at 1x.",
                d->slow_avg, d->normal_avg),
            slow_how(d));
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
                    d->slow_avg, d->normal_avg),
                slow_how(d));
        } else {
            add(2, "Waiting for the GPU",
                Fmt("Slow frames waited for your GPU to finish more often "
                    "(%s: %.1f per frame vs %.1f). Emulation stops while it "
                    "waits.",
                    d->name.c_str(), d->slow_avg, d->normal_avg),
                slow_how(d));
        }
    }
    if (const PerfCounterDiff *d = diff({ "TEX_UPLOAD", "SURF_UPLOAD", "SURF_SWIZZLE" })) {
        add(1, "Lots of data sent to the GPU",
            Fmt("%s: %.1f per slow frame vs %.1f (%s). Typical when a new area, "
                "video or streamed textures load.",
                d->name.c_str(), d->slow_avg, d->normal_avg,
                PerfCounterMeaning(d->name)),
            slow_how(d));
    }
    if (const PerfCounterDiff *d = diff({ "SURF_CREATE", "FINISH_SURFACE_CREATE" })) {
        add(1, "New render targets",
            Fmt("Slow frames created %.1f render targets each (other frames "
                "%.1f).",
                d->slow_avg, d->normal_avg),
            slow_how(d));
    }
    if (const PerfCounterDiff *d = diff({}, { "INLINE_" })) {
        add(1, "Vertices sent the slow way",
            Fmt("%s: %.1f per slow frame vs %.1f.", d->name.c_str(), d->slow_avg,
                d->normal_avg),
            slow_how(d));
    }
    if (const PerfCounterDiff *d =
            diff({ "BEGIN_ENDS", "DRAW_ARRAYS", "CLEAR", "PIPELINE_BIND",
                   "SHADER_BIND", "ATTR_BIND", "PIPELINE_RENDERPASSES" },
                 { "GEOM_BUFFER_UPDATE", "QUEUE_SUBMIT" })) {
        add(1, "More graphics work in the slow frames",
            Fmt("%s: %.1f per slow frame vs %.1f (%s). A busier scene.",
                d->name.c_str(), d->slow_avg, d->normal_avg,
                PerfCounterMeaning(d->name)),
            slow_how(d));
    }
    // Trend: the frame rate over time against each counter. Slowdown that
    // builds up over many ordinary frames (a heavier scene) never makes
    // single frames "slow", but shows here.
    bool trend_found = false;
    std::set<std::string> rising; // counters that rose with the slowdown
    {
        struct Win {
            int frames = 0;
            double work = 0;
            std::vector<double> c;
        };
        std::map<long, Win> wins;
        for (auto &f : cap.frames) {
            Win &w = wins[(long)(f.t_ms / 1000)];
            if (w.c.empty()) {
                w.c.assign(nc, 0);
            }
            w.frames++;
            w.work += f.work_ms;
            for (size_t c = 0; c < nc && c < f.counters.size(); c++) {
                w.c[c] += f.counters[c];
            }
        }
        // Whole seconds only: drop the first and last (partial) windows
        std::vector<Win> ws;
        long first = wins.empty() ? 0 : wins.begin()->first;
        long last = wins.empty() ? 0 : wins.rbegin()->first;
        for (auto &kv : wins) {
            if (kv.first != first && kv.first != last && kv.second.frames > 0) {
                Win w = kv.second;
                w.work /= w.frames;
                for (auto &v : w.c) {
                    v /= w.frames;
                }
                ws.push_back(std::move(w));
            }
        }
        r.windows = (int)ws.size();
        std::vector<double> fps;
        for (auto &w : ws) {
            fps.push_back(w.frames);
        }
        double fmax = fps.empty() ? 0 : *std::max_element(fps.begin(), fps.end());
        double fmin = fps.empty() ? 0 : *std::min_element(fps.begin(), fps.end());
        auto corr = [](const std::vector<double> &x, const std::vector<double> &y) {
            double mx = 0, my = 0;
            for (size_t i = 0; i < x.size(); i++) {
                mx += x[i];
                my += y[i];
            }
            mx /= x.size();
            my /= y.size();
            double sxy = 0, sx = 0, sy = 0;
            for (size_t i = 0; i < x.size(); i++) {
                sxy += (x[i] - mx) * (y[i] - my);
                sx += (x[i] - mx) * (x[i] - mx);
                sy += (y[i] - my) * (y[i] - my);
            }
            return sx > 0 && sy > 0 ? sxy / std::sqrt(sx * sy) : 0.0;
        };
        // Worth a look when the frame rate varies by over 12%
        if (ws.size() >= 8 && fmax - fmin >= std::max(4.0, fmax * 0.12)) {
            // "Smooth": within 5% of the best; "slow": 15% or more below it
            double smooth_at = fmax * 0.95, slow_at = fmax * 0.85;
            int n_slow = 0, n_smooth = 0;
            double work_slow = 0, work_smooth = 0;
            for (auto &w : ws) {
                if (w.frames <= slow_at) {
                    n_slow++;
                    work_slow += w.work;
                } else if (w.frames >= smooth_at) {
                    n_smooth++;
                    work_smooth += w.work;
                }
            }
            if (n_slow >= 2 && n_smooth >= 2) {
                work_slow /= n_slow;
                work_smooth /= n_smooth;
                struct Cand {
                    double r, slow, smooth, slope;
                    size_t c;
                };
                std::vector<Cand> cands;
                for (size_t c = 0; c < nc; c++) {
                    std::vector<double> v, work;
                    double vs = 0, vm = 0;
                    for (auto &w : ws) {
                        v.push_back(w.c[c]);
                        work.push_back(w.work);
                        if (w.frames <= slow_at) {
                            vs += w.c[c];
                        } else if (w.frames >= smooth_at) {
                            vm += w.c[c];
                        }
                    }
                    vs /= n_slow;
                    vm /= n_smooth;
                    double rc = corr(v, fps);
                    // Must fall as fps rises, clearly, and by a real amount
                    if (rc <= -0.6 && vs >= vm * 1.15 + 1.0) {
                        // ms of render time per extra item (least squares)
                        double mx = 0, my = 0, sxy = 0, sxx = 0;
                        for (size_t i = 0; i < v.size(); i++) {
                            mx += v[i];
                            my += work[i];
                        }
                        mx /= v.size();
                        my /= v.size();
                        for (size_t i = 0; i < v.size(); i++) {
                            sxy += (v[i] - mx) * (work[i] - my);
                            sxx += (v[i] - mx) * (v[i] - mx);
                        }
                        cands.push_back({ rc, vs, vm, sxx > 0 ? sxy / sxx : 0, c });
                    }
                }
                std::sort(cands.begin(), cands.end(),
                          [](const Cand &a, const Cand &b) { return a.r < b.r; });
                // Report the strongest, preferring the counter that names
                // the work over its side effects
                const Cand *best = nullptr;
                for (const char *pref : { "BEGIN_ENDS", "DRAW_ARRAYS" }) {
                    for (auto &cd : cands) {
                        if (!best && cap.counter_names[cd.c] == pref &&
                            cd.r <= cands[0].r + 0.1) {
                            best = &cd;
                        }
                    }
                }
                if (!best && !cands.empty()) {
                    best = &cands[0];
                }
                for (auto &cd : cands) {
                    rising.insert(cap.counter_names[cd.c]);
                }
                if (best) {
                    trend_found = true;
                    const std::string &name = cap.counter_names[best->c];
                    std::string also;
                    for (auto &cd : cands) {
                        if (&cd != best && also.size() < 160) {
                            also += Fmt("%s%s (%.0f vs %.0f, r %.2f)",
                                        also.empty() ? "" : ", ",
                                        cap.counter_names[cd.c].c_str(), cd.slow,
                                        cd.smooth, cd.r);
                        }
                    }
                    // Is nearly all of it inline vertex data?
                    std::string inline_note;
                    double inl = 0, batches = 0;
                    for (size_t c = 0; c < nc; c++) {
                        if (Starts(cap.counter_names[c], "INLINE_")) {
                            inl += all[c];
                        }
                        if (cap.counter_names[c] == "BEGIN_ENDS") {
                            batches = all[c];
                        }
                    }
                    if (batches > 100 && inl >= batches * 0.8) {
                        inline_note = Fmt(
                            " Nearly all of them (%.0f of %.0f per frame) send "
                            "their vertices inline, the slower path, rather than "
                            "from vertex buffers.",
                            inl, batches);
                    }
                    double per_item_us = best->slope * 1000;
                    std::string cost =
                        per_item_us > 0.05
                            ? Fmt(" Each extra one costs about %.1f microseconds of "
                                  "render time.",
                                  per_item_us)
                            : std::string();
                    add(2, "Frame rate drops when " +
                               std::string(PerfCounterMeaning(name)[0]
                                               ? PerfCounterMeaning(name)
                                               : name.c_str()) +
                               " go up",
                        Fmt("In the slower stretches the game did %.0f %s per "
                            "frame against %.0f when it ran smoothly, and the "
                            "frame rate followed it closely.%s%s The slowdown "
                            "comes from the amount of this work, not from one "
                            "expensive event.",
                            best->slow, name.c_str(), best->smooth, cost.c_str(),
                            inline_note.c_str()),
                        Fmt("Split the %.0f s recording into %d one-second "
                            "windows and counted the frames finished in each "
                            "(the frame rate), from %.0f to %.0f fps. Windows "
                            "within 5%% of the best (%.0f fps or more, %d of "
                            "them) count as smooth; windows 15%% or more below "
                            "it (%.0f fps or less, %d of them) as slow. %s "
                            "averaged %.0f per frame in the slow windows and "
                            "%.0f in the smooth ones. Across all windows it moves "
                            "against the frame rate with a correlation of %.2f "
                            "(-1 means they always move in opposite directions, "
                            "0 means unrelated; only -0.6 or stronger counts). "
                            "A straight-line fit of each window's average render "
                            "time against %s gives %.2f ms per 1000. Render time "
                            "averaged %.1f ms in slow windows and %.1f ms in "
                            "smooth ones.%s",
                            cap.duration_ms / 1000, r.windows, fmin, fmax,
                            smooth_at, n_smooth, slow_at, n_slow, name.c_str(),
                            best->slow, best->smooth, best->r, name.c_str(),
                            best->slope * 1000, work_slow, work_smooth,
                            also.empty() ? ""
                                         : (" Also rising with it: " + also + ".")
                                               .c_str()));
                } else if (work_slow < work_smooth * 1.15 + 1) {
                    trend_found = true;
                    add(2, "Frame rate drops but the graphics work doesn't change",
                        Fmt("The frame rate fell to %.0f fps in places while "
                            "render time stayed about the same (%.1f ms vs "
                            "%.1f ms), so the extra time is going to the "
                            "emulated Xbox CPU (game logic, physics, audio), "
                            "not graphics.",
                            fmin, work_slow, work_smooth),
                        Fmt("Split the recording into %d one-second windows and "
                            "compared the %d slow ones (%.0f fps or less) with "
                            "the %d smooth ones (%.0f fps or more). No graphics "
                            "counter rose with the slowdown (none moved against "
                            "the frame rate with a correlation of -0.6 or "
                            "stronger), and the average render time barely "
                            "changed.",
                            r.windows, n_slow, slow_at, n_smooth, smooth_at));
                }
            }
        }
    }
    if (r.slow_frames > 0 && r.slow_diffs.empty() && !trend_found) {
        add(2, "Graphics look the same in the slow frames",
            "None of the graphics counters went up when frames were slow, so "
            "the time is probably going to the emulated Xbox CPU (game logic, "
            "physics, audio) rather than graphics.",
            Fmt("Compared the %d frames over %d ms with the other %d: no counter "
                "was at least 1.5 times higher and up by at least 1 per frame.",
                r.slow_frames, r.slow_threshold, r.frames - r.slow_frames));
    }
    // What can be ruled out: big costs that stayed flat or never happened
    {
        std::string none, flat;
        for (const char *n : { "SHADER_GEN", "PIPELINE_GEN", "SURF_DOWNLOAD", "QUERY",
                               "SURF_TO_TEX_FALLBACK" }) {
            for (size_t c = 0; c < nc; c++) {
                if (cap.counter_names[c] != n) {
                    continue;
                }
                bool in_diffs = false;
                for (auto &d : r.slow_diffs) {
                    in_diffs |= d.name == n;
                }
                if (all[c] == 0) {
                    none += std::string(none.empty() ? "" : ", ") + n;
                } else if (!in_diffs && !rising.count(n)) {
                    flat += Fmt("%s%s (%.1f)", flat.empty() ? "" : ", ", n, all[c]);
                }
            }
        }
        if (!none.empty()) {
            add(0, "Ruled out",
                "These never happened during the recording, so they aren't the "
                "cause: " + none + ".",
                "Checked the per-frame counters of every recorded frame; each "
                "of these was zero throughout.");
        }
        if (!flat.empty()) {
            add(0, "Present but not behind the slowdown",
                "These happened (average per frame in brackets) but didn't rise "
                "enough with the slowdown to explain it: " + flat + ".",
                "Each was compared both ways: slow frames against the rest, and "
                "the one-second frame rate against the counter. Neither showed a "
                "clear rise (at least 1.15 times higher and up by at least 1 "
                "per frame).");
        }
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
                r.median_work, list.empty() ? "none recorded" : list.c_str()),
            Fmt("The median render time of all %d frames is %d ms; anything over "
                "34 ms counts as slow throughout. The list is each counter's "
                "average per frame across the whole recording.",
                r.frames, r.median_work));
    }
    if (r.host_fps_avg > 0 && r.avg_fps > 0 && r.host_fps_avg < 50 &&
        r.host_fps_avg < r.avg_fps * 0.9) {
        add(1, "xemu's own drawing is slow",
            Fmt("xemu drew %.0f frames a second on your PC while the game made "
                "%.0f. The upscaling filter, resolution scale or a large window "
                "may be too much for your GPU.",
                r.host_fps_avg, r.avg_fps),
            "Averaged the two frame rates sampled four times a second: the "
            "game's (frames the emulated Xbox finished) and xemu's (frames "
            "drawn on your PC). xemu's was under 50 and under 90% of the "
            "game's.");
    }
    if (r.slow_frames == 0 && r.median_work <= 34 && !trend_found) {
        add(0, "No slowdown in this recording",
            Fmt("No frame took longer than %d ms.", r.slow_threshold),
            Fmt("Render time of every frame compared with %d ms (1.5 times the "
                "typical %d ms, and never under 17 ms).",
                r.slow_threshold, r.median_work));
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
        mk.how = Fmt("Averaged the counters of the %d frames within half a "
                     "second of the mark and compared them with the whole "
                     "recording's averages. A counter is listed when it was at "
                     "least twice its usual level and up by at least 0.5 per "
                     "frame, or appeared only here.",
                     n);
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
        if (!f.how.empty()) {
            o += "    How this was worked out: " + f.how + "\n";
        }
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
        if (!m.how.empty()) {
            o += "  How this was worked out: " + m.how + "\n";
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
