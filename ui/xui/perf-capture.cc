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

#include <fpng.h>

#include "common.hh" // first: sets up imgui (and its OpenGL backend)
#include "actions.hh"
#include "font-manager.hh"
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
    bool export_pending = false;
    std::string export_msg;
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
    xemu_queue_notification(tr("Recording performance. Shift+F12 marks a glitch, "
                            "Shift+F9 stops."));
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
        g.save_err = string_format(tr("Couldn't save the report to %s"),
                                   dir.u8string().c_str());
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

void PollFrames();

void Stop()
{
    if (!g.recording) {
        return;
    }
    PollFrames(); // the last frames (not PerfCaptureUpdate: it calls Stop)
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
    ImGui::Text("%s", g.cap.game.empty() ? tr("No game detected") : g.cap.game.c_str());
    ImGui::TextDisabled(tr("%s  -  %s  -  %.0f s, %d frames"), g.cap.renderer.c_str(),
                        g.cap.settings.c_str(), g.cap.duration_ms / 1000, r.frames);
    ImGui::Text(tr("Frame rate %.0f average, %.0f lowest.  Render time %d ms typical, "
                "%d ms worst.  %d slow frames."),
                r.avg_fps, r.low1_fps, r.median_work, r.worst_work, r.slow_frames);
    DrawGraph(ImGui::GetContentRegionAvail().x, 110 * g_viewport_mgr.m_scale);
    ImGui::TextDisabled(tr("Bars: each frame's render time (red = slow). Yellow lines: "
                        "glitch marks. Hover for that frame's counters."));
    ImGui::Spacing();
    ImGui::SeparatorText(tr("What stands out"));
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
        if (!f.how.empty()) {
            ImGui::PushID(&f);
            if (ImGui::TreeNode(tr("How this was worked out"))) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", f.how.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::Unindent();
    }
    if (!r.slow_diffs.empty() &&
        ImGui::BeginTable("##diffs", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Per frame"));
        ImGui::TableSetupColumn(tr("Slow"));
        ImGui::TableSetupColumn(tr("Others"));
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
        ImGui::SeparatorText(string_format(tr("Glitch mark %d at %d:%02d"), (int)i + 1,
                                           sec / 60, sec % 60).c_str());
        for (auto &d : m.unusual) {
            ImGui::BulletText(tr("%s: %.1f per frame here, %.1f usually"), d.name.c_str(),
                              d.slow_avg, d.normal_avg);
            ImGui::SetItemTooltip("%s", PerfCounterMeaning(d.name));
        }
        for (auto &n : m.notes) {
            ImGui::TextWrapped("%s", n.c_str());
        }
        if (!m.how.empty()) {
            ImGui::PushID((int)i);
            if (ImGui::TreeNode(tr("How this was worked out"))) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", m.how.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (!r.marks.empty()) {
        ImGui::SeparatorText(tr("Narrow it down"));
        ImGui::TextWrapped(tr("Change one thing, play the same part and see if the "
                           "glitch stays:"));
        if (ImGui::Button(tr("Resolution 1x"))) {
            nv2a_set_surface_scale_factor(1);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Upscaling: Linear"))) {
            g_config.display.filtering = CONFIG_DISPLAY_FILTERING_LINEAR;
        }
        ImGui::TextDisabled(tr("Also try the other renderer (Settings, needs a "
                            "restart), the game's patches off, and official xemu."));
    }
}

} // namespace

namespace
{
// A picture of the report for posting: the frame-time graph and the main
// findings. Drawn with ImGui into an offscreen framebuffer, after the UI
// has rendered (see PerfCaptureAfterRender).
bool RenderSummaryPng(const std::string &path, std::string *err)
{
    const int W = 1200, H = 675;
    const PerfReport &r = g.report;
    ImFont *font = ImGui::GetFont();
    ImFont *big = g_font_mgr.m_menu_font_medium ? g_font_mgr.m_menu_font_medium : font;
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl._ResetForNewFrame();
    dl.PushTextureID(ImGui::GetIO().Fonts->TexID);
    dl.PushClipRect(ImVec2(0, 0), ImVec2(W, H));
    const ImU32 text = IM_COL32(232, 236, 245, 255), dim = IM_COL32(140, 150, 175, 255),
                accent = IM_COL32(0, 229, 255, 255), warn = IM_COL32(255, 140, 110, 255),
                info = IM_COL32(150, 175, 220, 255);
    dl.AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(14, 16, 24, 255));
    dl.AddRectFilled(ImVec2(0, 0), ImVec2(W, 6), accent);
    float x = 40, y = 30;
    std::string game = g.cap.game.empty() ? std::string("Performance capture") : g.cap.game;
    dl.AddText(big, 34, ImVec2(x, y), text, game.c_str());
    y += 44;
    char line[256];
    snprintf(line, sizeof(line), "%s  -  %s  -  %.0f s, %d frames",
             g.cap.renderer.c_str(), g.cap.settings.c_str(), g.cap.duration_ms / 1000,
             r.frames);
    dl.AddText(font, 17, ImVec2(x, y), dim, line);
    y += 26;
    snprintf(line, sizeof(line),
             "%.0f fps average, %.0f lowest    Render time %d ms typical, %d ms "
             "worst    %d slow frames",
             r.avg_fps, r.low1_fps, r.median_work, r.worst_work, r.slow_frames);
    dl.AddText(font, 19, ImVec2(x, y), text, line);
    y += 38;
    // Frame-time graph
    ImVec2 ga(x, y), gb(W - 40, y + 170);
    dl.AddRectFilled(ga, gb, IM_COL32(0, 0, 0, 120), 6);
    int top = std::max(50, r.worst_work + 5);
    auto gy = [&](float ms) { return gb.y - (gb.y - ga.y) * std::min(1.0f, ms / top); };
    for (float ms : { 16.7f, 33.3f }) {
        dl.AddLine(ImVec2(ga.x, gy(ms)), ImVec2(gb.x, gy(ms)), IM_COL32(255, 255, 255, 40));
        snprintf(line, sizeof(line), "%.0f fps", 1000 / ms);
        dl.AddText(font, 14, ImVec2(gb.x - 64, gy(ms) - 17), dim, line);
    }
    double dur = std::max(1.0, g.cap.duration_ms);
    float bw = std::max(1.0f, (gb.x - ga.x) / std::max<size_t>(1, g.cap.frames.size()));
    for (auto &f : g.cap.frames) {
        float fx = ga.x + (float)(f.t_ms / dur) * (gb.x - ga.x);
        bool slow = f.work_ms > r.slow_threshold;
        dl.AddLine(ImVec2(fx, gb.y), ImVec2(fx, gy((float)f.work_ms)),
                   slow ? IM_COL32(255, 80, 70, 230) : IM_COL32(90, 200, 120, 170), bw);
    }
    for (double m : g.cap.marks_ms) {
        float fx = ga.x + (float)(m / dur) * (gb.x - ga.x);
        dl.AddLine(ImVec2(fx, ga.y), ImVec2(fx, gb.y), IM_COL32(255, 210, 0, 255), 2);
    }
    dl.AddText(font, 14, ImVec2(ga.x, gb.y + 6), dim,
               "Each frame's render time (red = slow, yellow = glitch marked)");
    y = gb.y + 34;
    // The main findings
    int shown = 0;
    for (auto &f : r.findings) {
        if (shown >= 3 || y > H - 90) {
            break;
        }
        ImU32 col = f.severity == 2 ? warn : f.severity == 1 ? IM_COL32(240, 200, 80, 255) : info;
        dl.AddText(font, 21, ImVec2(x, y), col, f.title.c_str());
        y += 28;
        ImVec2 ts = font->CalcTextSizeA(17, FLT_MAX, W - 2 * x, f.detail.c_str());
        // The lead finding in full, the rest briefly
        float h = std::min(ts.y, 17.0f * 1.2f * (shown == 0 ? 4 : 2));
        dl.PushClipRect(ImVec2(x, y), ImVec2(W - x, y + h), true);
        dl.AddText(font, 17, ImVec2(x, y), text, f.detail.c_str(), nullptr, W - 2 * x);
        dl.PopClipRect();
        y += h + 16;
        shown++;
    }
    dl.AddText(font, 14, ImVec2(x, H - 30), dim,
               "xemu Modern GUI Performance Capture  -  experimental, unofficial xemu "
               "fork, not supported by the xemu project");
    dl.PopClipRect();
    dl.PopTextureID();

    ImDrawData dd;
    dd.Valid = true;
    dd.DisplayPos = ImVec2(0, 0);
    dd.DisplaySize = ImVec2(W, H);
    dd.FramebufferScale = ImVec2(1, 1);
    dd.OwnerViewport = ImGui::GetMainViewport();
    dd.AddDrawList(&dl);

    GLint prev_fb = 0, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    GLuint fb = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    bool ok_fb = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    std::vector<uint8_t> px((size_t)W * H * 4), flip((size_t)W * H * 4);
    if (ok_fb) {
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(&dd);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fb);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &tex);
    if (!ok_fb) {
        *err = tr("Couldn't draw the summary picture");
        return false;
    }
    for (int row = 0; row < H; row++) {
        memcpy(&flip[(size_t)row * W * 4], &px[(size_t)(H - 1 - row) * W * 4], W * 4);
    }
    for (size_t i = 3; i < flip.size(); i += 4) {
        flip[i] = 255;
    }
    std::vector<uint8_t> png;
    if (!fpng::fpng_encode_image_to_memory(flip.data(), W, H, 4, png)) {
        *err = tr("Couldn't encode the picture");
        return false;
    }
    if (!WriteText(fs::u8path(path), std::string((const char *)png.data(), png.size()))) {
        *err = string_format(tr("Couldn't save %s"), path.c_str());
        return false;
    }
    return true;
}
} // namespace

void PerfCaptureAfterRender(void)
{
    if (!g.export_pending) {
        return;
    }
    g.export_pending = false;
    if (g.saved_dir.empty()) {
        g.export_msg = tr("Record and save a capture first");
        return;
    }
    std::string path = (fs::u8path(g.saved_dir) / "summary.png").u8string();
    std::string err;
    if (RenderSummaryPng(path, &err)) {
        g.export_msg = tr("Saved summary.png in the capture folder");
        RevealFileInFolder(path);
    } else {
        g.export_msg = err;
    }
}

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
        xemu_queue_notification(tr("Start a performance recording first (Shift+F9)"));
        return;
    }
    g.cap.marks_ms.push_back(NowMs());
    ActionScreenshot();
    xemu_queue_notification(tr("Glitch marked"));
}

namespace
{
// Copy frames the profiler finished since the last look
void PollFrames()
{
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
}
} // namespace

void PerfCaptureUpdate(void)
{
    if (!g.recording) {
        return;
    }
    PollFrames();
    int len = kLengths[g.length];
    if ((len && NowMs() >= len * 1000.0) || g.cap.frames.size() > 200000) {
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
        char t[256];
        snprintf(t, sizeof(t), tr("REC %d:%02d   Shift+F12 mark glitch   Shift+F9 stop"),
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
    if (!ImGui::Begin(trid("Performance Capture"), &g.window)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped(tr("Record a slow or glitchy part of a game to see what the "
                       "emulated graphics chip was doing. Start recording, play "
                       "the part, and press Shift+F12 the moment a glitch shows."));
    ImGui::Spacing();
    if (g.recording) {
        if (ImGui::Button(tr(ICON_FA_STOP " Stop (Shift+F9)"))) {
            Stop();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_FLAG " Mark glitch (Shift+F12)"))) {
            PerfCaptureMarkGlitch();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%d frames, %d marks"), (int)g.cap.frames.size(),
                            (int)g.cap.marks_ms.size());
    } else {
        ImGui::SetNextItemWidth(160 * s);
        ImGui::Combo("##len", &g.length, tr_items("15 seconds\0" "30 seconds\0" "60 seconds\0"
                                         "Until stopped\0"));
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_CIRCLE " Record (Shift+F9)"))) {
            Start();
        }
        if (!runstate_is_running()) {
            ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.25f, 1),
                               tr("The Xbox is paused or not started: start a game "
                               "first."));
        }
    }
    if (g.have_report && !g.recording) {
        ImGui::Separator();
        if (!g.saved_dir.empty()) {
            ImGui::TextDisabled(tr("Saved to %s"), g.saved_dir.c_str());
            if (ImGui::Button(tr(ICON_FA_FOLDER_OPEN " Open folder"))) {
                if (!RevealFileInFolder((fs::u8path(g.saved_dir) / "report.txt").u8string())) {
                    SDL_OpenURL(("file:///" + g.saved_dir).c_str());
                }
            }
            ImGui::SameLine();
        } else if (!g.save_err.empty()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", g.save_err.c_str());
        }
        if (ImGui::Button(tr(ICON_FA_COPY " Copy report"))) {
            ImGui::SetClipboardText(PerfReportText(g.cap, g.report).c_str());
            xemu_queue_notification(tr("Report copied"));
        }
        ImGui::SetItemTooltip(tr("Paste it into an issue or a message"));
        ImGui::SameLine();
        if (ImGui::Button(tr(ICON_FA_IMAGE " Save picture"))) {
            g.export_pending = true; // drawn after this frame's UI
        }
        ImGui::SetItemTooltip(tr("summary.png: the graph and main findings, for "
                              "posting"));
        if (!g.export_msg.empty()) {
            ImGui::TextDisabled("%s", g.export_msg.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled(tr("trace.json opens in ui.perfetto.dev"));
        ImGui::Spacing();
        ImGui::BeginChild("##report", ImVec2(0, 0));
        DrawReport();
        ImGui::EndChild();
    }
    ImGui::End();
}
