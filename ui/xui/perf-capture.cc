//
// xemu User Interface - performance capture
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "perf-capture.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "actions.hh"
#include "common.hh"
#include "game-library.hh"
#include "game-shortcut.hh"
#include "IconsFontAwesome6.h"
#include "perf-analysis.hh"
#include "viewport-manager.hh"
#include "../xemu-notifications.h"
#include "../xemu-settings.h"

namespace fs = std::filesystem;

namespace
{

const int kLengths[] = { 15, 30, 60, 0 }; // seconds; 0 until stopped

struct State {
    bool window = false;
    bool recording = false;
    int length = 1; // index into kLengths
    uint64_t start_ticks = 0, last_sample = 0;
    unsigned int last_frame_count = 0;
    time_t start_time = 0;
    PerfCapture cap;
    bool have_report = false;
    PerfReport report;
    std::string saved_dir, save_err;
} g;

double NowMs()
{
    return (double)(SDL_GetTicks() - g.start_ticks);
}

const char *FilterName(int f)
{
    switch (f) {
    case CONFIG_DISPLAY_FILTERING_NEAREST: return "Nearest";
    case CONFIG_DISPLAY_FILTERING_FSR: return "FSR 1.0";
    case CONFIG_DISPLAY_FILTERING_BICUBIC: return "Bicubic";
    case CONFIG_DISPLAY_FILTERING_LANCZOS: return "Lanczos";
    case CONFIG_DISPLAY_FILTERING_SGSR: return "Snapdragon GSR";
    case CONFIG_DISPLAY_FILTERING_NIS: return "NVIDIA NIS";
    case CONFIG_DISPLAY_FILTERING_RTX: return "NVIDIA RTX Video";
    default: return "Linear";
    }
}

std::string SafeName(const std::string &s)
{
    std::string o;
    for (unsigned char c : s) {
        o += (isalnum(c) || c == '-' || c == '_') ? (char)c : (c == ' ' ? '_' : '\0');
    }
    o.erase(std::remove(o.begin(), o.end(), '\0'), o.end());
    if (o.size() > 40) {
        o.resize(40);
    }
    return o.empty() ? "capture" : o;
}

bool WriteText(const fs::path &p, const std::string &s)
{
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(s.data(), (std::streamsize)s.size());
    return (bool)o;
}

void Start()
{
    g = State{ g.window, false, g.length };
    g.recording = true;
    g.start_ticks = SDL_GetTicks();
    g.start_time = time(NULL);
    g.last_frame_count = g_nv2a_stats.frame_count;
    for (unsigned int i = 0; i < NV2A_PROF__COUNT; i++) {
        g.cap.counter_names.push_back(nv2a_profile_get_counter_name(i));
    }
    g.cap.game = GameLibraryCurrentTitle();
    g.cap.renderer =
        g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN ? "Vulkan"
        : g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL ? "OpenGL"
                                                                      : "Null";
    const char *gpu = (const char *)glGetString(GL_RENDERER);
    g.cap.gpu = gpu ? gpu : "";
    char b[160];
    snprintf(b, sizeof(b), "%ux resolution, %s upscaling, sharpness %.2f",
             nv2a_get_surface_scale_factor(), FilterName(g_config.display.filtering),
             g_config.display.fsr_sharpness);
    g.cap.settings = b;
    xemu_queue_notification("Recording performance. Shift+F12 marks a glitch, "
                            "Shift+F9 stops.");
}

// Write the report files, plus screenshots taken while recording
void Save()
{
    std::error_code ec;
    char stamp[32];
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &g.start_time);
#else
    localtime_r(&g.start_time, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tmv);
    fs::path dir = fs::u8path(xemu_settings_get_base_path()) / "captures" /
                   (SafeName(g.cap.game.empty() ? "xemu" : g.cap.game) + "_" + stamp);
    fs::create_directories(dir, ec);
    bool ok = !ec && WriteText(dir / "report.txt", PerfReportText(g.cap, g.report)) &&
              WriteText(dir / "frames.csv", PerfFramesCsv(g.cap)) &&
              WriteText(dir / "trace.json", PerfTraceJson(g.cap));
    if (!ok) {
        g.save_err = "Couldn't save the report to " + dir.u8string();
        return;
    }
    // Screenshots from the glitch marks (F12 / Shift+F12 while recording)
    const char *sdir = g_config.general.screenshot_dir;
    fs::path shots = fs::u8path(sdir && sdir[0] ? sdir : ".");
    auto start = fs::file_time_type::clock::now() -
                 std::chrono::milliseconds((long long)g.cap.duration_ms + 2000);
    for (fs::directory_iterator it(shots, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".png" &&
            fs::last_write_time(it->path(), ec) >= start) {
            fs::copy_file(it->path(), dir / it->path().filename(),
                          fs::copy_options::skip_existing, ec);
        }
    }
    g.saved_dir = dir.u8string();
}

void Stop()
{
    if (!g.recording) {
        return;
    }
    PerfCaptureUpdate(); // pick up the last frames
    g.recording = false;
    g.cap.duration_ms = NowMs();
    g.report = AnalyzePerf(g.cap);
    g.have_report = true;
    g.window = true;
    Save();
}

// Frame render times as bars; slow ones in red, glitch marks as lines
void DrawGraph(float w, float h)
{
    const float s = g_viewport_mgr.m_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::Dummy(ImVec2(w, h));
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 90), 4 * s);
    const auto &fr = g.cap.frames;
    if (fr.empty() || g.cap.duration_ms <= 0) {
        return;
    }
    int top = std::max(50, g.report.worst_work + 5);
    auto y_of = [&](float ms) { return b.y - (b.y - a.y) * std::min(1.0f, ms / top); };
    // 16.7 and 33.3 ms guides (60 and 30 fps)
    for (float g_ms : { 16.7f, 33.3f }) {
        float y = y_of(g_ms);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 40), 1);
        char t[16];
        snprintf(t, sizeof(t), "%.0f fps", 1000 / g_ms);
        dl->AddText(ImVec2(b.x - 50 * s, y - ImGui::GetFontSize()),
                    IM_COL32(255, 255, 255, 90), t);
    }
    double dur = g.cap.duration_ms;
    for (auto &f : fr) {
        float x = a.x + (float)(f.t_ms / dur) * w;
        bool slow = f.work_ms > g.report.slow_threshold;
        dl->AddLine(ImVec2(x, b.y), ImVec2(x, y_of((float)f.work_ms)),
                    slow ? IM_COL32(255, 80, 70, 230) : IM_COL32(90, 200, 120, 160),
                    std::max(1.0f, w / fr.size()));
    }
    for (double m : g.cap.marks_ms) {
        float x = a.x + (float)(m / dur) * w;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 210, 0, 255), 2 * s);
    }
    if (ImGui::IsItemHovered()) {
        float mx = ImGui::GetIO().MousePos.x;
        double t = (mx - a.x) / w * dur;
        const PerfFrame *best = nullptr;
        for (auto &f : fr) {
            if (!best || std::fabs(f.t_ms - t) < std::fabs(best->t_ms - t)) {
                best = &f;
            }
        }
        if (best) {
            std::string tip = std::to_string(best->work_ms) + " ms";
            for (size_t c = 0; c < best->counters.size(); c++) {
                if (best->counters[c]) {
                    tip += "\n" + g.cap.counter_names[c] + ": " +
                           std::to_string(best->counters[c]);
                }
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
}

void DrawReport()
{
    const PerfReport &r = g.report;
    ImGui::Text("%s", g.cap.game.empty() ? "No game detected" : g.cap.game.c_str());
    ImGui::TextDisabled("%s  -  %s  -  %.0f s, %d frames", g.cap.renderer.c_str(),
                        g.cap.settings.c_str(), g.cap.duration_ms / 1000, r.frames);
    ImGui::Text("Frame rate %.0f average, %.0f lowest.  Render time %d ms typical, "
                "%d ms worst.  %d slow frames.",
                r.avg_fps, r.low1_fps, r.median_work, r.worst_work, r.slow_frames);
    DrawGraph(ImGui::GetContentRegionAvail().x, 110 * g_viewport_mgr.m_scale);
    ImGui::TextDisabled("Bars: each frame's render time (red = slow). Yellow lines: "
                        "glitch marks. Hover for that frame's counters.");
    ImGui::Spacing();
    ImGui::SeparatorText("What stands out");
    for (auto &f : r.findings) {
        ImVec4 col = f.severity == 2   ? ImVec4(1.0f, 0.55f, 0.45f, 1)
                     : f.severity == 1 ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                                       : ImVec4(0.6f, 0.8f, 1.0f, 1);
        ImGui::TextColored(col, "%s %s",
                           f.severity == 2   ? ICON_FA_TRIANGLE_EXCLAMATION
                           : f.severity == 1 ? ICON_FA_MAGNIFYING_GLASS
                                             : ICON_FA_CHECK,
                           f.title.c_str());
        ImGui::Indent();
        ImGui::TextWrapped("%s", f.detail.c_str());
        ImGui::Unindent();
    }
    if (!r.slow_diffs.empty() &&
        ImGui::BeginTable("##diffs", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Per frame");
        ImGui::TableSetupColumn("Slow");
        ImGui::TableSetupColumn("Others");
        ImGui::TableHeadersRow();
        for (auto &d : r.slow_diffs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::SetItemTooltip("%s", PerfCounterMeaning(d.name));
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", d.slow_avg);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", d.normal_avg);
        }
        ImGui::EndTable();
    }
    for (size_t i = 0; i < r.marks.size(); i++) {
        auto &m = r.marks[i];
        int sec = (int)(m.t_ms / 1000);
        ImGui::SeparatorText(("Glitch mark " + std::to_string(i + 1) + " at " +
                              std::to_string(sec / 60) + ":" +
                              (sec % 60 < 10 ? "0" : "") + std::to_string(sec % 60))
                                 .c_str());
        for (auto &d : m.unusual) {
            ImGui::BulletText("%s: %.1f per frame here, %.1f usually", d.name.c_str(),
                              d.slow_avg, d.normal_avg);
            ImGui::SetItemTooltip("%s", PerfCounterMeaning(d.name));
        }
        for (auto &n : m.notes) {
            ImGui::TextWrapped("%s", n.c_str());
        }
    }
    if (!r.marks.empty()) {
        ImGui::SeparatorText("Narrow it down");
        ImGui::TextWrapped("Change one thing, play the same part and see if the "
                           "glitch stays:");
        if (ImGui::Button("Resolution 1x")) {
            nv2a_set_surface_scale_factor(1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Upscaling: Linear")) {
            g_config.display.filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
        }
        ImGui::TextDisabled("Also try the other renderer (Settings, needs a "
                            "restart), the game's patches off, and official xemu.");
    }
}

} // namespace

bool PerfCaptureRecording(void)
{
    return g.recording;
}

void PerfCaptureOpenWindow(void)
{
    g.window = true;
}

void PerfCaptureToggleRecording(void)
{
    if (g.recording) {
        Stop();
    } else {
        Start();
    }
}

void PerfCaptureMarkGlitch(void)
{
    if (!g.recording) {
        xemu_queue_notification("Start a performance recording first (Shift+F9)");
        return;
    }
    g.cap.marks_ms.push_back(NowMs());
    ActionScreenshot();
    xemu_queue_notification("Glitch marked");
}

void PerfCaptureUpdate(void)
{
    if (!g.recording) {
        return;
    }
    double now = NowMs();
    // New frames since the last look (the profiler keeps the last 300)
    unsigned int count = g_nv2a_stats.frame_count;
    unsigned int n = count - g.last_frame_count;
    if (n > NV2A_PROF_NUM_FRAMES - 1) {
        n = NV2A_PROF_NUM_FRAMES - 1;
    }
    unsigned int ptr = g_nv2a_stats.frame_ptr;
    for (unsigned int k = 0; k < n; k++) {
        unsigned int idx = (ptr + NV2A_PROF_NUM_FRAMES - n + k) % NV2A_PROF_NUM_FRAMES;
        PerfFrame f;
        f.t_ms = now - (n - 1 - k) * 1.0; // ordered within this poll
        f.work_ms = g_nv2a_stats.frame_history[idx].mspf;
        f.counters.assign(g_nv2a_stats.frame_history[idx].counters,
                          g_nv2a_stats.frame_history[idx].counters + NV2A_PROF__COUNT);
        g.cap.frames.push_back(std::move(f));
    }
    g.last_frame_count = count;
    if (SDL_GetTicks() - g.last_sample >= 250) {
        g.last_sample = SDL_GetTicks();
        g.cap.samples.push_back({ now, (int)g_nv2a_stats.increment_fps,
                                  ImGui::GetIO().Framerate });
    }
    int len = kLengths[g.length];
    if ((len && now >= len * 1000.0) || g.cap.frames.size() > 200000) {
        Stop();
    }
}

void PerfCaptureDraw(void)
{
    const float s = g_viewport_mgr.m_scale;
    if (g.recording) {
        // REC badge, top right
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        ImGuiViewport *vp = ImGui::GetMainViewport();
        int sec = (int)(NowMs() / 1000);
        char t[96];
        snprintf(t, sizeof(t), "REC %d:%02d   Shift+F12 mark glitch   Shift+F9 stop",
                 sec / 60, sec % 60);
        ImVec2 ts = ImGui::CalcTextSize(t);
        ImVec2 p(vp->WorkPos.x + vp->WorkSize.x - ts.x - 40 * s, vp->WorkPos.y + 30 * s);
        dl->AddRectFilled(ImVec2(p.x - 26 * s, p.y - 6 * s),
                          ImVec2(p.x + ts.x + 10 * s, p.y + ts.y + 6 * s),
                          IM_COL32(0, 0, 0, 170), 6 * s);
        bool blink = fmod(ImGui::GetTime(), 1.0) < 0.6;
        dl->AddCircleFilled(ImVec2(p.x - 13 * s, p.y + ts.y / 2), 5 * s,
                            blink ? IM_COL32(255, 60, 60, 255) : IM_COL32(120, 30, 30, 255));
        dl->AddText(p, IM_COL32_WHITE, t);
    }
    if (!g.window) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720 * s, 640 * s), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Performance Capture", &g.window)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Record a slow or glitchy part of a game to see what the "
                       "emulated graphics chip was doing. Start recording, play "
                       "the part, and press Shift+F12 the moment a glitch shows.");
    ImGui::Spacing();
    if (g.recording) {
        if (ImGui::Button(ICON_FA_STOP " Stop (Shift+F9)")) {
            Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FLAG " Mark glitch (Shift+F12)")) {
            PerfCaptureMarkGlitch();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d frames, %d marks", (int)g.cap.frames.size(),
                            (int)g.cap.marks_ms.size());
    } else {
        ImGui::SetNextItemWidth(160 * s);
        ImGui::Combo("##len", &g.length, "15 seconds\0" "30 seconds\0" "60 seconds\0"
                                         "Until stopped\0");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CIRCLE " Record (Shift+F9)")) {
            Start();
        }
        if (!runstate_is_running()) {
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1),
                               "The Xbox is paused or not started: start a game "
                               "first.");
        }
    }
    if (g.have_report && !g.recording) {
        ImGui::Separator();
        if (!g.saved_dir.empty()) {
            ImGui::TextDisabled("Saved to %s", g.saved_dir.c_str());
            if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open folder")) {
                if (!RevealFileInFolder((fs::u8path(g.saved_dir) / "report.txt").u8string())) {
                    SDL_OpenURL(("file:///" + g.saved_dir).c_str());
                }
            }
            ImGui::SameLine();
        } else if (!g.save_err.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", g.save_err.c_str());
        }
        if (ImGui::Button(ICON_FA_COPY " Copy report")) {
            ImGui::SetClipboardText(PerfReportText(g.cap, g.report).c_str());
            xemu_queue_notification("Report copied");
        }
        ImGui::SetItemTooltip("Paste it into an issue or a message");
        ImGui::SameLine();
        ImGui::TextDisabled("trace.json opens in ui.perfetto.dev");
        ImGui::Spacing();
        ImGui::BeginChild("##report", ImVec2(0, 0));
        DrawReport();
        ImGui::EndChild();
    }
    ImGui::End();
}
