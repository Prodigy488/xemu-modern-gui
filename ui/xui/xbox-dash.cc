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
                 bool animate)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = b.x - a.x, h = b.y - a.y;
    int picked = -1;
    sel = std::clamp(sel, 0, std::max(0, n - 1));

    // Brand
    dl->AddText(small_font, small_font->FontSize, ImVec2(a.x + w * 0.07f, a.y + 26 * s),
                A(kGreen, 235), brand);

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
    const float fs = font->FontSize;
    float row_h = std::min(fs * 1.75f, (h * 0.70f) / std::max(1, n));
    float list_h = row_h * n;
    float x0 = a.x + w * 0.07f;
    float y0 = a.y + (h - list_h) * 0.5f + 8 * s;
    float row_w = w * 0.40f;

    static float bar_y = -1;
    float target_y = y0 + sel * row_h;
    bar_y = bar_y < 0 ? target_y : Approach(bar_y, target_y, 16.0f, animate);
    // The highlight slides to the chosen row
    dl->AddRectFilledMultiColor(ImVec2(x0 - 14 * s, bar_y), ImVec2(x0 + row_w, bar_y + row_h),
                                A(kGreen, 96), A(kGreen, 0), A(kGreen, 0), A(kGreen, 96));
    dl->AddRectFilled(ImVec2(x0 - 14 * s, bar_y + 4 * s), ImVec2(x0 - 10 * s, bar_y + row_h - 4 * s),
                      kGreen);

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
        if (focus || (hov && (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0))) {
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
    return picked;
}

XboxDashRect XboxDashSection(const char *brand, const char *title, ImVec2 a, ImVec2 b, float s,
                             double t, ImFont *font, ImFont *small_font, float bottom_margin,
                             bool *back, bool animate)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = b.x - a.x;
    const float pad = std::max(28.0f * s, w * 0.04f);
    float y = a.y + 22 * s;

    // Crumb: brand and an arrow, clickable to go back
    std::string crumb = std::string("<  ") + brand;
    ImVec2 cs = small_font->CalcTextSizeA(small_font->FontSize, FLT_MAX, 0, crumb.c_str());
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    bool clicked = ImGui::InvisibleButton("##xbback", ImVec2(cs.x + 8 * s, cs.y + 6 * s));
    bool hov = ImGui::IsItemHovered();
    if (clicked && back) {
        *back = true;
    }
    dl->AddText(small_font, small_font->FontSize, ImVec2(a.x + pad, y),
                hov ? IM_COL32(255, 255, 255, 255) : A(kGreen, 235), crumb.c_str());

    float ty = y + cs.y + 8 * s;
    dl->AddText(font, font->FontSize, ImVec2(a.x + pad, ty), IM_COL32(255, 255, 255, 255), title);
    float ly = ty + font->FontSize + 10 * s;
    dl->AddRectFilledMultiColor(ImVec2(a.x + pad, ly), ImVec2(a.x + pad + w * 0.55f, ly + 2 * s),
                                kGreen, A(kGreen, 0), A(kGreen, 0), kGreen);

    // The panel
    ImVec2 p0(a.x + pad, ly + 16 * s), p1(b.x - pad, b.y - bottom_margin);
    dl->AddRectFilled(p0, p1, IM_COL32(4, 14, 5, 188), 6 * s);
    dl->AddRect(p0, p1, A(kGreen, 120), 6 * s, 0, 1.2f * s);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + 3 * s), A(kGreen, 200), A(kGreen, 60),
                                A(kGreen, 60), A(kGreen, 200));
    XboxDashRect r;
    r.min = ImVec2(p0.x + 16 * s, p0.y + 14 * s);
    r.max = ImVec2(p1.x - 16 * s, p1.y - 12 * s);
    return r;
}
