//
// xemu User Interface - the "Original Xbox" dashboard style
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "xbox-dash.hh"
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace {

const ImU32 kGreen = IM_COL32(133, 204, 36, 255);
const float kPi = 3.14159265f;

ImU32 A(ImU32 c, int alpha)
{
    alpha = std::clamp(alpha, 0, 255);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

// A steady pseudo-random number in 0..1 for n
float Hash(int n)
{
    unsigned x = (unsigned)n * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28) + 4)) ^ x) * 277803737u;
    x = (x >> 22) ^ x;
    return (float)(x & 0xFFFFFF) / 16777215.0f;
}

float Approach(float cur, float target, float speed, bool animate)
{
    if (!animate) {
        return target;
    }
    float k = 1.0f - expf(-speed * ImGui::GetIO().DeltaTime);
    float v = cur + (target - cur) * k;
    return fabsf(target - v) < 0.002f ? target : v;
}

// Globe centre and size for a menu area
void GlobeSpot(ImVec2 a, ImVec2 b, ImVec2 *c, float *r)
{
    float w = b.x - a.x, h = b.y - a.y;
    *c = ImVec2(a.x + w * 0.72f, a.y + h * 0.50f);
    *r = std::min(h * 0.30f, w * 0.19f);
}

} // namespace

void XboxDashGlobe(ImDrawList *dl, ImVec2 c, float R, double t, float pulse, bool animate)
{
    // Glow around it
    for (int i = 0; i < 18; i++) {
        float r = R * (1.02f + 0.052f * i);
        int alpha = (int)((9.0f - i * 0.45f) * (1.0f + pulse * 1.6f));
        dl->AddCircleFilled(c, r, A(kGreen, alpha), 96);
    }
    // Glass body, lighter toward the lower left
    dl->AddCircleFilled(c, R, IM_COL32(3, 16, 4, 244), 96);
    for (int i = 0; i < 14; i++) {
        float rr = R * (0.96f - i * 0.058f);
        float off = R * 0.03f * i;
        dl->AddCircleFilled(ImVec2(c.x - off * 0.9f, c.y + off), rr,
                            A(kGreen, 5 + i + (int)(pulse * 6)), 96);
    }

    // Wireframe: meridians turning about a slightly tilted axis
    const float rot = animate ? (float)(t * 0.42) : 0.6f;
    const float tilt = 0.38f;
    const float ct = cosf(tilt), st = sinf(tilt);
    auto project = [&](float lat, float lon, float *z) {
        float x = cosf(lat) * sinf(lon), y = sinf(lat), zz = cosf(lat) * cosf(lon);
        float y2 = y * ct - zz * st, z2 = y * st + zz * ct;
        *z = z2;
        return ImVec2(c.x + x * R, c.y - y2 * R);
    };
    const int meridians = 16;
    for (int m = 0; m < meridians; m++) {
        float lon = rot + m * (2 * kPi / meridians);
        bool first = true;
        ImVec2 prev;
        float pz = 0;
        for (int k = 0; k <= 24; k++) {
            float lat = -kPi / 2 + k * (kPi / 24);
            float z;
            ImVec2 p = project(lat, lon, &z);
            if (!first) {
                float zm = (z + pz) * 0.5f;
                int alpha = zm > 0 ? (int)(120 + 110 * zm) : 34;
                dl->AddLine(prev, p, A(kGreen, alpha), zm > 0 ? std::max(1.0f, R * 0.012f) : 1.0f);
            }
            prev = p;
            pz = z;
            first = false;
        }
    }
    // Parallels
    static const float lats[] = { -60, -30, 0, 30, 60 };
    for (float deg : lats) {
        float lat = deg * kPi / 180;
        ImVec2 prev;
        float pz = 0;
        for (int k = 0; k <= 48; k++) {
            float lon = k * (2 * kPi / 48);
            float z;
            ImVec2 p = project(lat, lon, &z);
            if (k) {
                float zm = (z + pz) * 0.5f;
                int alpha = zm > 0 ? (int)(100 + 100 * zm) : 28;
                dl->AddLine(prev, p, A(kGreen, alpha), 1.0f);
            }
            prev = p;
            pz = z;
        }
    }
    // Rim and shine
    dl->AddCircle(c, R, A(kGreen, 190), 96, std::max(1.5f, R * 0.014f));
    dl->AddCircleFilled(ImVec2(c.x - R * 0.36f, c.y - R * 0.42f), R * 0.16f,
                        IM_COL32(255, 255, 255, 16), 40);
    dl->AddCircleFilled(ImVec2(c.x - R * 0.40f, c.y - R * 0.46f), R * 0.07f,
                        IM_COL32(255, 255, 255, 34), 32);
}

void XboxDashBackground(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, double t, bool menu_view,
                        bool animate)
{
    float w = b.x - a.x, h = b.y - a.y;
    // Black, with green rising from the bottom
    dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 2, 0, 244), IM_COL32(0, 2, 0, 244),
                                menu_view ? IM_COL32(6, 28, 6, 246) : IM_COL32(3, 14, 3, 246),
                                menu_view ? IM_COL32(6, 28, 6, 246) : IM_COL32(3, 14, 3, 246));
    // A soft pool of light behind the globe
    if (menu_view) {
        ImVec2 c;
        float r;
        GlobeSpot(a, b, &c, &r);
        for (int i = 0; i < 12; i++) {
            dl->AddCircleFilled(c, r * (1.6f + i * 0.2f), A(kGreen, 3), 96);
        }
    }
    // Slow ribbons of light
    float tt = animate ? (float)t : 0.0f;
    const int layers = 3;
    for (int l = 0; l < layers; l++) {
        float base = a.y + h * (menu_view ? 0.88f : 0.90f) + l * h * 0.03f;
        float amp = h * (0.035f + 0.008f * l);
        float k = (2 * kPi) / (w * (0.55f + 0.2f * l));
        float sp = 0.22f + 0.07f * l;
        ImVec2 pts[96];
        int n = 96;
        for (int i = 0; i < n; i++) {
            float x = a.x + w * i / (n - 1);
            float y = base + amp * sinf(x * k + tt * sp + l * 1.7f) +
                      amp * 0.35f * sinf(x * k * 2.3f - tt * sp * 1.4f);
            pts[i] = ImVec2(x, y);
        }
        for (int pass = 0; pass < 3; pass++) {
            float width = (7.0f - pass * 2.6f) * s;
            int alpha = 7 + pass * 12 + (menu_view ? 5 : 0);
            dl->AddPolyline(pts, n, A(kGreen, alpha), 0, width);
        }
    }
    // Drifting specks
    for (int i = 0; i < 46; i++) {
        float fx = Hash(i * 3 + 1), fy = Hash(i * 3 + 2), fz = Hash(i * 3 + 3);
        float drift = animate ? (float)fmod(t * (0.004f + 0.006f * fz) + fy, 1.0) : fy;
        float x = a.x + w * fx + sinf(tt * 0.3f + i) * 8 * s;
        float y = b.y - h * drift;
        int alpha = (int)((22 + 50 * fz) * (menu_view ? 1.0f : 0.5f) * sinf(drift * kPi));
        dl->AddCircleFilled(ImVec2(x, y), (0.8f + 1.6f * fz) * s, A(kGreen, alpha), 8);
    }
    // Faint frame lines at top and bottom
    dl->AddLine(ImVec2(a.x, a.y + 1), ImVec2(b.x, a.y + 1), A(kGreen, 60), 1.0f);
}

int XboxDashMenu(const char *brand, const char *const *labels, int n, int &sel, ImVec2 a,
                 ImVec2 b, float s, double t, ImFont *font, ImFont *small_font, bool focus_sel,
                 float open_t, float *list_right, bool animate)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = b.x - a.x, h = b.y - a.y;
    int picked = -1;
    sel = std::clamp(sel, 0, std::max(0, n - 1));

    const bool open = open_t > 0.5f; // a section's window is showing
    const float fs = font->FontSize;
    float max_w = 0;
    for (int i = 0; i < n; i++) {
        max_w = std::max(max_w, font->CalcTextSizeA(fs, FLT_MAX, 0, labels[i]).x);
    }
    float x0 = a.x + w * (0.07f + (0.045f - 0.07f) * open_t);
    float row_w = w * 0.40f + (max_w + 34 * s - w * 0.40f) * open_t;
    if (list_right) {
        *list_right = x0 + row_w;
    }

    // Brand
    dl->AddText(small_font, small_font->FontSize, ImVec2(x0, a.y + 26 * s), A(kGreen, 235),
                brand);

    // Globe
    static int last_sel = -1;
    static float pulse = 0;
    if (last_sel != sel) {
        if (last_sel >= 0) {
            pulse = 1.0f;
        }
        last_sel = sel;
    }
    pulse = Approach(pulse, 0.0f, 5.0f, animate);
    ImVec2 gc;
    float gr;
    GlobeSpot(a, b, &gc, &gr);
    XboxDashGlobe(dl, gc, gr, t, pulse, animate);

    // The list
    float row_h = std::min(fs * 1.75f, (h * 0.70f) / std::max(1, n));
    float list_h = row_h * n;
    float y0 = a.y + (h - list_h) * 0.5f + 8 * s;

    static float bar_y = -1;
    float target_y = y0 + sel * row_h;
    bar_y = bar_y < 0 ? target_y : Approach(bar_y, target_y, 16.0f, animate);
    // The highlight slides to the chosen row
    dl->AddRectFilledMultiColor(ImVec2(x0 - 14 * s, bar_y), ImVec2(x0 + row_w, bar_y + row_h),
                                A(kGreen, 96), A(kGreen, 0), A(kGreen, 0), A(kGreen, 96));
    dl->AddRectFilled(ImVec2(x0 - 14 * s, bar_y + 4 * s), ImVec2(x0 - 10 * s, bar_y + row_h - 4 * s),
                      kGreen);

    static int focus_prev = -1;
    int focused_row = -1;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0)); // the bar is the cursor
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        ImVec2 p(x0, y0 + i * row_h);
        ImGui::SetCursorScreenPos(ImVec2(x0 - 14 * s, p.y));
        if (focus_sel && i == sel) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        bool clicked = ImGui::InvisibleButton("##row", ImVec2(row_w + 14 * s, row_h),
                                              ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered();
        bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (focus) {
            focused_row = i;
        }
        bool moved = hov && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0);
        // With a window open the highlight only follows the focus when it
        // moves from one row to another, and a click picks (so passing the
        // mouse over the list on the way to the window changes nothing)
        if (open ? (focus && focus_prev >= 0 && focus_prev != i) : (focus || moved)) {
            sel = i;
        }
        if (clicked) {
            sel = i;
            picked = i;
        }
        bool on = i == sel;
        std::string text = labels[i];
        dl->AddText(font, fs, ImVec2(p.x, p.y + (row_h - fs) / 2),
                    on ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 225, 205, hov ? 190 : 130),
                    text.c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    focus_prev = focused_row;
    return picked;
}

XboxDashRect XboxDashWindow(const char *title, ImVec2 a, ImVec2 b, float s, ImFont *font,
                            float left, float bottom_margin, float open_t)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = b.x - a.x;
    const float pad = std::max(20.0f * s, w * 0.03f);
    int fade = (int)(255 * std::clamp(open_t, 0.0f, 1.0f));
    ImVec2 p0(left + (1.0f - open_t) * 60 * s, a.y + 58 * s), p1(b.x - pad, b.y - bottom_margin);

    // Glass over the globe
    dl->AddRectFilled(p0, p1, IM_COL32(3, 12, 4, 214 * fade / 255), 8 * s);
    dl->AddRect(p0, p1, A(kGreen, 130 * fade / 255), 8 * s, 0, 1.4f * s);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + 3 * s), A(kGreen, 210 * fade / 255),
                                A(kGreen, 60 * fade / 255), A(kGreen, 60 * fade / 255),
                                A(kGreen, 210 * fade / 255));
    float tx = p0.x + 22 * s, ty = p0.y + 14 * s;
    dl->AddText(font, font->FontSize, ImVec2(tx, ty), IM_COL32(255, 255, 255, fade), title);
    float ly = ty + font->FontSize + 8 * s;
    dl->AddRectFilledMultiColor(ImVec2(tx, ly), ImVec2(tx + (p1.x - p0.x) * 0.55f, ly + 2 * s),
                                A(kGreen, fade), A(kGreen, 0), A(kGreen, 0), A(kGreen, fade));
    XboxDashRect r;
    r.min = ImVec2(p0.x + 16 * s, ly + 12 * s);
    r.max = ImVec2(p1.x - 16 * s, p1.y - 12 * s);
    return r;
}

// ---- The sphere-and-fins look ----------------------------------------------
//
// The other face of the original Xbox's menu: a bright green web over a deep
// green field, a big glowing sphere on the left wrapped in translucent fins,
// and an arc of glass orbs, one per section, each with a notched bar beside it.
// The orbs turn round the sphere as the selection moves. All drawn from
// scratch.

namespace {

const ImU32 kLime = IM_COL32(150, 225, 40, 255);

ImU32 Mix(ImU32 c0, ImU32 c1, float f)
{
    f = std::clamp(f, 0.0f, 1.0f);
    auto ch = [&](int sh) {
        float a0 = (float)((c0 >> sh) & 0xFF), a1 = (float)((c1 >> sh) & 0xFF);
        return (ImU32)(a0 + (a1 - a0) * f) << sh;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) |
           ch(IM_COL32_A_SHIFT);
}

// One translucent fin: a curved blade that swells in the middle and tapers
// to points, following an arc around the sphere
void DrawFin(ImDrawList *dl, ImVec2 c, float r, float thick, float ang0, float span,
             float squash, ImU32 col, ImU32 edge, float line)
{
    const int n = 28;
    ImVec2 outer[n + 1], inner[n + 1];
    for (int i = 0; i <= n; i++) {
        float u = (float)i / n;
        float ang = ang0 + u * span;
        float th = thick * powf(sinf(u * kPi), 0.8f);
        float ro = r + th * 0.35f, ri = r - th * 0.65f;
        outer[i] = ImVec2(c.x + cosf(ang) * ro * squash, c.y + sinf(ang) * ro);
        inner[i] = ImVec2(c.x + cosf(ang) * ri * squash, c.y + sinf(ang) * ri);
    }
    for (int i = 0; i < n; i++) {
        ImVec2 q[4] = { outer[i], outer[i + 1], inner[i + 1], inner[i] };
        dl->AddConvexPolyFilled(q, 4, col);
    }
    dl->AddPolyline(outer, n + 1, edge, 0, line);
    dl->AddPolyline(inner, n + 1, A(edge, (edge >> IM_COL32_A_SHIFT & 0xFF) / 2), 0, line);
}

void DrawSphere(ImDrawList *dl, ImVec2 c, float R, double t, float spin, float pulse,
                float s, bool animate)
{
    float tt = animate ? spin : 0.0f; // the fins' turn: speeds up when the selection moves
    float tm = animate ? (float)t : 0.0f;
    // Glow
    for (int i = 0; i < 16; i++) {
        float r = R * (1.02f + 0.07f * i);
        dl->AddCircleFilled(c, r, A(kLime, (int)((11.0f - i * 0.6f) * (1.0f + pulse * 1.2f))), 96);
    }
    // Body: dark at the rim, lime toward the upper left
    const ImU32 rim = IM_COL32(52, 104, 10, 255), mid = IM_COL32(150, 190, 22, 255),
                core = IM_COL32(214, 226, 52, 255);
    for (int i = 0; i < 20; i++) {
        float f = (float)i / 19;
        float rr = R * (1.0f - f * 0.82f);
        float off = R * 0.16f * f;
        ImU32 col = f < 0.5f ? Mix(rim, mid, f * 2) : Mix(mid, core, (f - 0.5f) * 2);
        dl->AddCircleFilled(ImVec2(c.x - off * 0.8f, c.y - off * 0.7f), rr, col, 96);
    }
    // Soft shine
    dl->AddCircleFilled(ImVec2(c.x - R * 0.38f, c.y - R * 0.5f), R * 0.12f,
                        IM_COL32(255, 255, 220, 70), 32);
    dl->AddCircleFilled(ImVec2(c.x - R * 0.40f, c.y - R * 0.54f), R * 0.05f,
                        IM_COL32(255, 255, 240, 140), 24);
    dl->AddCircle(c, R, IM_COL32(190, 235, 80, 150), 96, std::max(1.2f, R * 0.012f));

    // Fins turning round it
    const ImU32 fin = IM_COL32(170, 235, 130, 62), edge = IM_COL32(200, 250, 160, 150);
    float ln = std::max(1.0f, 1.3f * s);
    DrawFin(dl, c, R * 1.30f, R * 0.28f, tt * 0.30f + 2.2f, 2.5f, 1.0f, fin, edge, ln);
    DrawFin(dl, c, R * 1.46f, R * 0.22f, -tt * 0.21f + 0.4f, 2.1f, 1.0f, fin, edge, ln);
    DrawFin(dl, c, R * 1.18f, R * 0.18f, tt * 0.40f + 4.4f, 1.7f, 1.0f, fin, edge, ln);
    DrawFin(dl, c, R * 1.62f, R * 0.16f, tt * 0.14f + 1.2f, 1.5f, 1.0f, fin, edge, ln);
    // Thin rings
    dl->AddCircle(c, R * 1.24f, A(kLime, 70), 96, ln);
    dl->AddCircle(c, R * 1.52f, A(kLime, 44), 96, ln);
    // A streak through it
    float sa = -0.30f + 0.06f * sinf(tm * 0.5f);
    ImVec2 d(cosf(sa), sinf(sa));
    ImVec2 s0(c.x - d.x * R * 2.0f, c.y - d.y * R * 2.0f), s1(c.x + d.x * R * 0.1f, c.y + d.y * R * 0.1f);
    dl->AddLine(s0, s1, IM_COL32(190, 245, 150, 150), std::max(1.5f, R * 0.02f));
    dl->AddLine(s0, s1, IM_COL32(255, 255, 220, 70), 1.0f);
}

// A glass orb
void DrawOrb(ImDrawList *dl, ImVec2 c, float r, float lit, int fade, float beat)
{
    auto F = [&](int a) { return a * fade / 255; };
    // The lit orb's glow breathes
    dl->AddCircleFilled(c, r * (1.35f + 0.25f * beat * lit), A(kLime, F((int)(10 + 40 * lit * (0.8f + 0.2f * beat)))), 40);
    if (lit > 0.01f) {
        dl->AddCircle(c, r * (1.5f + 0.5f * beat), A(kLime, F((int)(70 * lit * (1.0f - beat)))), 40, 1.5f);
    }
    dl->AddCircleFilled(c, r, A(IM_COL32(18, 70, 8, 255), F(235)), 40);
    for (int i = 0; i < 5; i++) {
        float f = (float)i / 4;
        dl->AddCircleFilled(ImVec2(c.x - r * 0.12f * f, c.y - r * 0.14f * f), r * (0.92f - 0.16f * i),
                            A(Mix(IM_COL32(40, 120, 12, 255), IM_COL32(200, 235, 60, 255),
                                  0.15f + 0.8f * lit * f),
                              F(60 + (int)(60 * lit))),
                            32);
    }
    dl->AddCircle(c, r, A(Mix(IM_COL32(120, 200, 40, 255), IM_COL32(230, 250, 120, 255), lit),
                         F((int)(170 + 85 * lit))),
                  40, std::max(1.2f, r * 0.08f));
    dl->AddCircleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.42f), r * 0.2f,
                        IM_COL32(255, 255, 230, F(60)), 16);
}

} // namespace

void XboxDashSphereBackground(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, double t,
                              bool animate)
{
    float w = b.x - a.x, h = b.y - a.y;
    dl->AddRectFilledMultiColor(a, b, IM_COL32(18, 76, 16, 250), IM_COL32(12, 58, 12, 250),
                                IM_COL32(4, 34, 7, 252), IM_COL32(6, 42, 8, 252));
    // The web: a jittered lattice, joined into triangles, slowly breathing
    const int cols = 22, rows = 13;
    float dx = w / (cols - 1), dy = h / (rows - 1);
    float tt = animate ? (float)t : 0.0f;
    static ImVec2 pts[(22 + 1) * (13 + 1)];
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            float jx = (Hash(j * 101 + i * 7 + 1) - 0.5f) * 0.9f;
            float jy = (Hash(j * 131 + i * 13 + 2) - 0.5f) * 0.9f;
            float x = a.x + (i + jx) * dx + sinf(tt * 0.25f + j * 0.9f + i * 0.3f) * 5 * s;
            float y = a.y + (j + jy) * dy + cosf(tt * 0.21f + i * 0.8f + j * 0.4f) * 5 * s;
            pts[j * cols + i] = ImVec2(x, y);
        }
    }
    const ImU32 web = IM_COL32(120, 205, 44, 78);
    const float lw = std::max(1.0f, 1.2f * s);
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            ImVec2 p = pts[j * cols + i];
            if (i + 1 < cols) {
                dl->AddLine(p, pts[j * cols + i + 1], web, lw);
            }
            if (j + 1 < rows) {
                dl->AddLine(p, pts[(j + 1) * cols + i], web, lw);
                if (((i + j) & 1) == 0 && i + 1 < cols) {
                    dl->AddLine(p, pts[(j + 1) * cols + i + 1], web, lw);
                } else if (((i + j) & 1) == 1 && i > 0) {
                    dl->AddLine(p, pts[(j + 1) * cols + i - 1], web, lw);
                }
            }
        }
    }
    // Big curved lines across the web, as if it wrapped a sphere
    for (int i = 0; i < 4; i++) {
        float cx = a.x + w * (0.2f + 0.2f * i), cy = a.y + h * (i & 1 ? 1.5f : -0.5f);
        dl->AddCircle(ImVec2(cx, cy), h * (1.1f + 0.15f * i), A(kLime, 40), 128, lw);
    }
    // Dim the edges so the middle reads clearly
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + h * 0.14f), IM_COL32(0, 14, 2, 120),
                                IM_COL32(0, 14, 2, 120), IM_COL32(0, 14, 2, 0),
                                IM_COL32(0, 14, 2, 0));
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - h * 0.2f), b, IM_COL32(0, 14, 2, 0),
                                IM_COL32(0, 14, 2, 0), IM_COL32(0, 14, 2, 150),
                                IM_COL32(0, 14, 2, 150));
}

int XboxDashSphereMenu(const char *const *labels, int n, int &sel, ImVec2 a, ImVec2 b, float s,
                       double t, ImFont *font, ImFont *small_font, bool focus_sel,
                       float open_t, float *stage_right, bool animate)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float fw = b.x - a.x, h = b.y - a.y;
    sel = std::clamp(sel, 0, std::max(0, n - 1));
    const bool open = open_t > 0.5f;
    // Everything is laid out inside a stage that narrows when a window opens
    float w = fw + (fw * 0.40f - fw) * open_t;
    if (stage_right) {
        *stage_right = a.x + w;
    }

    static int last_sel = -1;
    static float pulse = 0, pos = -1;
    if (last_sel != sel) {
        if (last_sel >= 0) {
            pulse = 1.0f;
        }
        last_sel = sel;
    }
    pulse = Approach(pulse, 0.0f, 5.0f, animate);
    pos = pos < 0 ? (float)sel : Approach(pos, (float)sel, 11.0f, animate);

    // The fins turn at their own pace, faster for a moment when the selection moves
    static float spin = 0;
    if (animate) {
        spin += ImGui::GetIO().DeltaTime * (1.0f + pulse * 5.0f);
    }
    // Rows slide in one after another when the menu appears
    static int last_frame = -100;
    static double enter_t0 = 0;
    int frame = ImGui::GetFrameCount();
    if (frame - last_frame > 2) {
        enter_t0 = t;
    }
    last_frame = frame;
    const float beat = animate ? 0.5f + 0.5f * sinf((float)t * 3.2f) : 0.5f;

    const float R = std::min(w * 0.115f, h * 0.20f);
    ImVec2 c(a.x + w * 0.27f, a.y + h * 0.47f + (animate ? sinf((float)t * 0.8f) * R * 0.012f : 0.0f));
    DrawSphere(dl, c, R, t, spin, pulse, s, animate);

    const float ro = R * 1.50f, orb_r = R * 0.25f;
    const float row_h = R * 0.74f;            // between rows
    const float bar_h = row_h * 0.72f;
    const float vis = 2.2f + 2.4f * open_t;   // how many rows show each side of the lit one
    const float right = a.x + w - std::max(16.0f * s, w * 0.03f);
    
    // The arc the orbs ride on
    dl->PathArcTo(c, ro, -1.25f, 1.25f, 40);
    dl->PathStroke(A(kLime, 95), 0, std::max(1.5f, 2.2f * s));

    ImFont *f = bar_h < font->FontSize * 1.35f ? small_font : font;
    int picked = -1;
    static int focus_prev = -1;
    int focused_row = -1;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    for (int i = 0; i < n; i++) {
        float d = i - pos;
        // Rows are evenly spaced; each orb sits on the arc at its row's height
        float dy = d * row_h;
        float ox = c.x + sqrtf(std::max(0.0f, ro * ro - std::min(dy * dy, ro * ro * 0.96f)));
        float oy = c.y + dy;
        ox = std::max(ox, c.x + R * 0.55f); // far rows stay out of the middle of the sphere
        float ad = fabsf(d);
        float enter = animate ? std::clamp((float)(t - enter_t0) * 2.8f - (float)i * 0.16f, 0.0f, 1.0f) : 1.0f;
        enter = 1.0f - (1.0f - enter) * (1.0f - enter) * (1.0f - enter);
        int fade = (int)(255 * enter * std::clamp(1.0f - (ad - vis) / 0.9f, 0.0f, 1.0f));
        float lit = std::clamp(1.0f - ad, 0.0f, 1.0f);
        float x0 = ox + orb_r * 1.35f + 6 * s - lit * 4 * s + (1.0f - enter) * 70 * s;
        ox -= (1.0f - enter) * 20 * s;
        float y0 = oy - bar_h / 2, y1 = oy + bar_h / 2;
        float notch = bar_h * 0.28f;
        if (fade > 0) {
            DrawOrb(dl, ImVec2(ox, oy), orb_r * (1.0f + 0.22f * lit), lit, fade, beat);
        }
        // Bar
        ImVec2 q[5] = { ImVec2(x0 + notch, y0), ImVec2(right, y0), ImVec2(right, y1),
                        ImVec2(x0, y1), ImVec2(x0, y0 + notch) };
        auto F = [&](int al) { return al * fade / 255; };
        if (fade > 0) {
            // A little stub of circuit line above the bar
            ImVec2 stub[3] = { ImVec2(x0 - 6 * s, y0 + bar_h * 0.1f),
                               ImVec2(x0 + bar_h * 0.35f, y0 - bar_h * 0.16f),
                               ImVec2(x0 + bar_h * 1.4f, y0 - bar_h * 0.16f) };
            dl->AddPolyline(stub, 3, A(kLime, F(110)), 0, std::max(1.0f, 1.2f * s));
            ImU32 fill = Mix(IM_COL32(40, 112, 16, 255), IM_COL32(168, 172, 14, 255), lit);
            dl->AddConvexPolyFilled(q, 5, A(fill, F((int)(150 + 80 * lit))));
            dl->AddRectFilledMultiColor(ImVec2(x0 + notch, y0), ImVec2(right, y0 + bar_h * 0.45f),
                                        IM_COL32(255, 255, 255, F((int)(20 + 14 * lit))),
                                        IM_COL32(255, 255, 255, F(4)),
                                        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
            dl->AddPolyline(q, 5, A(Mix(IM_COL32(110, 200, 40, 255), IM_COL32(225, 240, 90, 255), lit),
                                    F((int)(130 + 120 * lit))),
                            ImDrawFlags_Closed, std::max(1.2f, 1.5f * s));
            if (lit > 0.5f && animate) {
                // A band of light crossing the lit bar now and then
                float cyc = fmodf((float)t * 0.55f, 2.2f) - 0.35f;
                float bw = (right - x0) * 0.16f, bx = x0 + (right - x0) * cyc;
                dl->PushClipRect(ImVec2(x0, y0), ImVec2(right, y1), true);
                dl->AddRectFilledMultiColor(ImVec2(bx, y0), ImVec2(bx + bw, y1),
                                            IM_COL32(255, 255, 220, 0), IM_COL32(255, 255, 220, F((int)(46 * lit))),
                                            IM_COL32(255, 255, 220, F((int)(46 * lit))), IM_COL32(255, 255, 220, 0));
                dl->PopClipRect();
            }
        }
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        if (focus_sel && i == sel) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        bool clicked = ImGui::InvisibleButton("##orbrow", ImVec2(right - x0, bar_h),
                                              ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered() && fade > 0;
        bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (focus) {
            focused_row = i;
        }
        bool moved = hov && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0);
        if (open ? (focus && focus_prev >= 0 && focus_prev != i) : (focus || moved)) {
            sel = i;
        }
        if (clicked && fade > 0) {
            sel = i;
            picked = i;
        }
        ImGui::PopID();
        if (fade > 0) {
            ImU32 txt = Mix(IM_COL32(205, 240, 130, 255), IM_COL32(22, 40, 4, 255), lit);
            dl->AddText(f, f->FontSize, ImVec2(x0 + notch + 12 * s, oy - f->FontSize / 2),
                        A(txt, F(hov ? 255 : 235)), labels[i]);
        }
    }
    ImGui::PopStyleColor();
    focus_prev = focused_row;
    return picked;
}
