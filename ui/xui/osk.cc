//
// xemu User Interface - on-screen keyboard
//
// It doesn't take focus: the text box stays active and the keyboard feeds
// it typed characters and Backspace / arrow keys through ImGui's input
// queue, so the text, cursor and limits all behave as with a real
// keyboard. While it's up it owns the controller buttons, so A doesn't
// confirm the text box, B doesn't cancel it, and the D-pad doesn't move
// to other controls.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "osk.hh"
#include "common.hh"
#include "font-manager.hh"
#include "IconsFontAwesome6.h"
#include "game-library.hh"
#include "viewport-manager.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

namespace {

struct Osk {
    bool visible = false;
    ImGuiID target = 0;     // the text box it types into
    ImGuiID seen = 0;       // last active text box looked at
    int row = 1, col = 0;   // selected key
    int layer = 0;          // 0 letters, 1 capitals, 2 symbols
    bool want_close = false, want_enter = false;
    double shown_t = -10, hidden_t = -10; // for the fade/slide in and out
    ImVec2 hl_a, hl_b;                    // animated highlight
    bool hl_valid = false;
} g_osk;

const ImGuiID kOwner = 0x05C0FFEE; // key owner ID for the keyboard

// Text boxes drawn this frame and the last one
std::unordered_set<ImGuiID> g_boxes, g_boxes_prev;

// Keys per layer, 10 per row; the last row is special keys
const char *const kRows[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_" },
    { "1234567890", "!@#$%^&*()", "-_=+[]{}\\|", ";:'\"<>/?~`" },
};
enum Special { kShift, kSymbols, kSpace, kBack, kDone, kSpecialCount };
const int kSpecialSpan[kSpecialCount] = { 2, 2, 3, 2, 1 }; // in key widths
const int kRowsTotal = 5;

const ImGuiKey kOwned[] = {
    ImGuiKey_GamepadFaceDown,  ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft,
    ImGuiKey_GamepadFaceUp,    ImGuiKey_GamepadDpadUp,    ImGuiKey_GamepadDpadDown,
    ImGuiKey_GamepadDpadLeft,  ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickUp,
    ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
    ImGuiKey_GamepadL1,        ImGuiKey_GamepadR1,        ImGuiKey_GamepadL2,
    ImGuiKey_GamepadR2,        ImGuiKey_GamepadStart,     ImGuiKey_GamepadBack,
};

bool Pressed(ImGuiKey k, bool repeat = true)
{
    return ImGui::IsKeyPressed(k, repeat ? ImGuiInputFlags_Repeat : 0, kOwner);
}

// The layer in use: the Shift / #+= keys, or a trigger held down (RT for
// capitals, LT for symbols), like holding Shift on a real keyboard
int EffLayer()
{
    if (g_osk.layer != 0) {
        return g_osk.layer;
    }
    if (ImGui::IsKeyDown(ImGuiKey_GamepadR2, kOwner)) {
        return 1;
    }
    if (ImGui::IsKeyDown(ImGuiKey_GamepadL2, kOwner)) {
        return 2;
    }
    return 0;
}

// The active text box, or 0
ImGuiID ActiveTextBox()
{
    ImGuiContext &g = *GImGui;
    if (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) {
        return g.ActiveId;
    }
    return 0;
}

int SpecialAt(int col)
{
    int x = 0;
    for (int i = 0; i < kSpecialCount; i++) {
        x += kSpecialSpan[i];
        if (col < x) {
            return i;
        }
    }
    return kSpecialCount - 1;
}

int SpecialStart(int special)
{
    int x = 0;
    for (int i = 0; i < special; i++) {
        x += kSpecialSpan[i];
    }
    return x;
}

void Type(char c)
{
    ImGui::GetIO().AddInputCharacter((unsigned int)(unsigned char)c);
}

void TapKey(ImGuiKey k)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(k, true);
    io.AddKeyEvent(k, false);
}

void Press()
{
    if (g_osk.row < 4) {
        Type(kRows[EffLayer()][g_osk.row][g_osk.col]);
        if (g_osk.layer == 1) {
            g_osk.layer = 0; // capitals for one letter, like a phone
        }
        return;
    }
    switch (SpecialAt(g_osk.col)) {
    case kShift:
        g_osk.layer = EffLayer() == 1 ? 0 : 1;
        break;
    case kSymbols:
        g_osk.layer = EffLayer() == 2 ? 0 : 2;
        break;
    case kSpace:
        Type(' ');
        break;
    case kBack:
        TapKey(ImGuiKey_Backspace);
        break;
    default:
        g_osk.want_close = true;
        break;
    }
}

} // namespace

void OskTextBox()
{
    g_boxes.insert(ImGui::GetItemID());
}

void OskBeginFrame()
{
    ImGuiContext &g = *GImGui;
    g_boxes_prev.swap(g_boxes);
    g_boxes.clear();
    // ImGui opens text boxes with Y on a controller; A only "tweaks". Let
    // A open them too, for the keyboard.
    if (g_config.display.ui.onscreen_keyboard && g.NavActivateId != 0 &&
        g.NavInputSource == ImGuiInputSource_Gamepad &&
        !(g.NavActivateFlags & ImGuiActivateFlags_PreferInput) &&
        g_boxes_prev.count(g.NavActivateId)) {
        g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
    }
    ImGuiID active = ActiveTextBox();
    // A text box already active from the keyboard (some focus themselves
    // when they appear): A on a controller brings the keyboard up for it
    // instead of confirming it
    if (!g_osk.visible && active && g_osk.seen == active &&
        g_config.display.ui.onscreen_keyboard && g_boxes_prev.count(active) &&
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false, ImGuiKeyOwner_NoOwner)) {
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceDown, kOwner, ImGuiInputFlags_LockThisFrame);
        g_osk.visible = true;
        g_osk.shown_t = ImGui::GetTime();
        g_osk.hl_valid = false;
        g_osk.target = active;
        g_osk.row = 1;
        g_osk.col = 0;
        g_osk.layer = 0;
        return;
    }
    if (active != g_osk.seen) {
        g_osk.seen = active;
        // Opened with a controller: show the keyboard for it
        bool by_pad = active && g.ActiveIdSource == ImGuiInputSource_Gamepad;
        if (active && by_pad && g_config.display.ui.onscreen_keyboard) {
            g_osk.visible = true;
            g_osk.shown_t = ImGui::GetTime();
            g_osk.hl_valid = false;
            g_osk.target = active;
            g_osk.row = 1;
            g_osk.col = 0;
            g_osk.layer = 0;
        }
    }
    if (g_osk.visible &&
        (!g_config.display.ui.onscreen_keyboard || active != g_osk.target)) {
        g_osk.visible = false; // closed some other way (mouse, Esc...)
        g_osk.hidden_t = ImGui::GetTime();
    }
    if (!g_osk.visible) {
        return;
    }

    // Own the controller buttons while it's up
    for (ImGuiKey k : kOwned) {
        ImGui::SetKeyOwner(k, kOwner, ImGuiInputFlags_LockThisFrame);
    }

    if (Pressed(ImGuiKey_GamepadDpadUp) || Pressed(ImGuiKey_GamepadLStickUp)) {
        if (g_osk.row == 4) {
            g_osk.col = std::min(9, SpecialStart(SpecialAt(g_osk.col)));
        }
        g_osk.row = (g_osk.row + kRowsTotal - 1) % kRowsTotal;
    }
    if (Pressed(ImGuiKey_GamepadDpadDown) || Pressed(ImGuiKey_GamepadLStickDown)) {
        if (g_osk.row == 4) {
            g_osk.col = std::min(9, SpecialStart(SpecialAt(g_osk.col)));
        }
        g_osk.row = (g_osk.row + 1) % kRowsTotal;
    }
    if (Pressed(ImGuiKey_GamepadDpadLeft) || Pressed(ImGuiKey_GamepadLStickLeft)) {
        if (g_osk.row == 4) {
            int sp = SpecialAt(g_osk.col);
            g_osk.col = SpecialStart((sp + kSpecialCount - 1) % kSpecialCount);
        } else {
            g_osk.col = (g_osk.col + 9) % 10;
        }
    }
    if (Pressed(ImGuiKey_GamepadDpadRight) || Pressed(ImGuiKey_GamepadLStickRight)) {
        if (g_osk.row == 4) {
            int sp = SpecialAt(g_osk.col);
            g_osk.col = SpecialStart((sp + 1) % kSpecialCount);
        } else {
            g_osk.col = (g_osk.col + 1) % 10;
        }
    }
    if (Pressed(ImGuiKey_GamepadFaceDown)) {
        Press();
    }
    if (Pressed(ImGuiKey_GamepadFaceLeft)) {
        TapKey(ImGuiKey_Backspace);
    }
    if (Pressed(ImGuiKey_GamepadFaceUp)) {
        Type(' ');
    }
    if (Pressed(ImGuiKey_GamepadL1)) {
        TapKey(ImGuiKey_LeftArrow);
    }
    if (Pressed(ImGuiKey_GamepadR1)) {
        TapKey(ImGuiKey_RightArrow);
    }
    if (Pressed(ImGuiKey_GamepadFaceRight, false) || Pressed(ImGuiKey_GamepadBack, false)) {
        g_osk.want_close = true;
    }
    if (Pressed(ImGuiKey_GamepadStart, false)) {
        g_osk.want_enter = true;
    }
    if (g_osk.want_enter) {
        // Confirm like Enter: the text box finishes and reports it
        g_osk.want_enter = false;
        g_osk.visible = false;
        g_osk.hidden_t = ImGui::GetTime();
        TapKey(ImGuiKey_Enter);
    } else if (g_osk.want_close) {
        // Close and keep the text (edits are already in the text box)
        g_osk.want_close = false;
        g_osk.visible = false;
        g_osk.hidden_t = ImGui::GetTime();
        ImGui::ClearActiveID();
    }
}

bool OskVisible()
{
    return g_osk.visible;
}

namespace {

float EaseOut(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

ImU32 WithAlpha(ImU32 c, float a)
{
    int base = (int)((c >> IM_COL32_A_SHIFT) & 0xFF);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(base * a) << IM_COL32_A_SHIFT);
}

// The dashboard's shape: cut corners for Neon, rounded for Green
void Shape(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float r, bool neon,
           float stroke = 0)
{
    if (!neon) {
        if (stroke > 0) {
            dl->AddRect(a, b, col, r, 0, stroke);
        } else {
            dl->AddRectFilled(a, b, col, r);
        }
        return;
    }
    float c = std::min(r * 0.9f, std::min(b.x - a.x, b.y - a.y) * 0.3f);
    ImVec2 pts[6] = { ImVec2(a.x + c, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - c),
                      ImVec2(b.x - c, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + c) };
    if (stroke > 0) {
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, stroke);
    } else {
        dl->AddConvexPolyFilled(pts, 6, col);
    }
}

// A controller button badge like the dashboard's hint bar
float Badge(ImDrawList *dl, ImFont *font, float size, ImVec2 p, float h, const char *key,
            ImU32 col, const char *what, ImU32 text_col, float alpha)
{
    float r = h * 0.5f;
    ImVec2 c(p.x + r, p.y + r);
    dl->AddCircleFilled(c, r, WithAlpha(col, alpha), 20);
    ImVec2 ks = font->CalcTextSizeA(size, FLT_MAX, 0, key);
    dl->AddText(font, size, ImVec2(c.x - ks.x / 2, c.y - ks.y / 2),
                WithAlpha(IM_COL32(255, 255, 255, 255), alpha), key);
    ImVec2 ws = font->CalcTextSizeA(size, FLT_MAX, 0, what);
    dl->AddText(font, size, ImVec2(p.x + h + 6 * size / 16, c.y - ws.y / 2),
                WithAlpha(text_col, alpha), what);
    return h + 6 * size / 16 + ws.x;
}

} // namespace

void OskDraw()
{
    double now = ImGui::GetTime();
    bool anim = g_config.display.ui.use_animations;
    const float in_dur = 0.22f, out_dur = 0.16f;
    float k; // 0 hidden .. 1 fully shown
    if (g_osk.visible) {
        k = anim ? EaseOut((float)(now - g_osk.shown_t) / in_dur) : 1.0f;
    } else {
        if (!anim || now - g_osk.hidden_t > out_dur) {
            return;
        }
        k = 1 - EaseOut((float)(now - g_osk.hidden_t) / out_dur);
    }
    const GameLibraryColors th = GameLibraryThemeColors();
    const int layer = EffLayer();
    const float s = g_viewport_mgr.m_scale;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImDrawList *dl = ImGui::GetForegroundDrawList(vp);
    ImFont *font = g_font_mgr.m_menu_font_small ? g_font_mgr.m_menu_font_small
                                                : ImGui::GetFont();
    float fs = font->FontSize;
    float key = std::min(58.0f * s, (vp->WorkSize.x - 80 * s) / 10.6f);
    float gap = 7 * s;
    float pad = 18 * s;
    float preview_h = fs + 18 * s;
    float legend_h = ImGui::GetFontSize() + 16 * s;
    float w = key * 10 + gap * 9 + pad * 2;
    float keys_h = key * kRowsTotal + gap * (kRowsTotal - 1);
    float h = pad + preview_h + 12 * s + keys_h + 12 * s + legend_h + pad * 0.5f;
    float slide = (1 - k) * 48 * s; // rises into place
    // Above the dashboard's button hints
    ImVec2 a(vp->WorkPos.x + (vp->WorkSize.x - w) / 2,
             vp->WorkPos.y + vp->WorkSize.y - h - 48 * s + slide);
    ImVec2 b(a.x + w, a.y + h);
    float r = 14 * s;

    // Dim what's behind, so the keyboard is the one thing in focus
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      IM_COL32(0, 0, 0, (int)(110 * k)));
    // Panel: dark glass with the theme's accent edge, like the dashboard cards
    Shape(dl, ImVec2(a.x, a.y + 6 * s), ImVec2(b.x, b.y + 6 * s),
          WithAlpha(IM_COL32(0, 0, 0, 110), k), r, th.neon); // soft shadow
    Shape(dl, a, b, WithAlpha(th.neon ? IM_COL32(10, 11, 24, 252) : IM_COL32(20, 24, 20, 252), k),
          r, th.neon);
    Shape(dl, a, b, WithAlpha(th.neon ? WithAlpha(th.accent, 0.45f) : IM_COL32(255, 255, 255, 30), k),
          r, th.neon, 1.5f * s);
    dl->AddRectFilled(ImVec2(a.x + r, a.y), ImVec2(a.x + r + 70 * s, a.y + 3 * s),
                      WithAlpha(th.accent, k));

    // What's been typed, with the cursor
    {
        ImGuiContext &g = *GImGui;
        ImVec2 pa(a.x + pad, a.y + pad), pb(b.x - pad, a.y + pad + preview_h);
        Shape(dl, pa, pb, WithAlpha(IM_COL32(255, 255, 255, 14), k), 8 * s, th.neon);
        std::string text;
        int cursor = 0;
        if (g.InputTextState.ID == g_osk.target && g_osk.target) {
            text.assign(g.InputTextState.TextA.Data,
                        (size_t)std::max(0, g.InputTextState.TextLen));
            cursor = std::clamp(g.InputTextState.GetCursorPos(), 0, (int)text.size());
            if (g.InputTextState.Flags & ImGuiInputTextFlags_Password) {
                // Show dots, keeping the cursor at the same character
                int chars = 0, before = 0;
                for (int i = 0; i < (int)text.size(); i++) {
                    if ((text[i] & 0xC0) != 0x80) {
                        chars++;
                        if (i < cursor) {
                            before++;
                        }
                    }
                }
                text.assign(chars, '*');
                cursor = before;
            }
        }
        float tx = pa.x + 12 * s, ty = pa.y + (preview_h - fs) / 2;
        float max_w = pb.x - tx - 12 * s;
        // Keep the cursor in view: drop text from the left if needed
        size_t start = 0;
        while (start < (size_t)cursor &&
               font->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str() + start,
                                   text.c_str() + cursor).x > max_w) {
            start++;
            while (start < text.size() && (text[start] & 0xC0) == 0x80) {
                start++;
            }
        }
        ImVec4 clip(pa.x, pa.y, pb.x - 8 * s, pb.y);
        dl->AddText(font, fs, ImVec2(tx, ty), WithAlpha(th.text, k), text.c_str() + start,
                    nullptr, 0, &clip);
        float cx = tx + font->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str() + start,
                                            text.c_str() + std::max<size_t>(start, cursor)).x;
        if (fmod(now, 1.0) < 0.6) {
            dl->AddRectFilled(ImVec2(cx, ty + 2 * s), ImVec2(cx + 2 * s, ty + fs - 2 * s),
                              WithAlpha(th.accent, k));
        }
    }

    // Keys
    float keys_y = a.y + pad + preview_h + 12 * s;
    auto key_rect = [&](int row, int col, int span, ImVec2 *ka, ImVec2 *kb) {
        float kw = key * span + gap * (span - 1);
        *ka = ImVec2(a.x + pad + col * (key + gap), keys_y + row * (key + gap));
        *kb = ImVec2(ka->x + kw, ka->y + key);
    };
    // Where the highlight should be
    ImVec2 sa, sb;
    if (g_osk.row < 4) {
        key_rect(g_osk.row, g_osk.col, 1, &sa, &sb);
    } else {
        int sp = SpecialAt(g_osk.col);
        key_rect(4, SpecialStart(sp), kSpecialSpan[sp], &sa, &sb);
    }
    if (!g_osk.hl_valid || !anim) {
        g_osk.hl_a = sa;
        g_osk.hl_b = sb;
        g_osk.hl_valid = true;
    } else {
        // Glide toward the selected key
        float t = 1 - powf(0.0005f, ImGui::GetIO().DeltaTime);
        g_osk.hl_a = ImLerp(g_osk.hl_a, sa, t);
        g_osk.hl_b = ImLerp(g_osk.hl_b, sb, t);
    }
    ImU32 key_bg = th.neon ? WithAlpha(th.accent, 0.07f) : IM_COL32(255, 255, 255, 18);
    ImU32 key_edge = th.neon ? WithAlpha(th.accent, 0.22f) : 0;
    auto draw_key = [&](ImVec2 ka, ImVec2 kb, bool on) {
        Shape(dl, ka, kb, WithAlpha(on ? WithAlpha(th.accent, 0.30f) : key_bg, k), 8 * s,
              th.neon);
        if (key_edge) {
            Shape(dl, ka, kb, WithAlpha(key_edge, k), 8 * s, th.neon, 1.0f * s);
        }
    };
    // Icons (Font Awesome, U+F000 and up: 0xEF lead byte) are only in the
    // default font
    ImFont *icon_font = ImGui::GetIO().Fonts->Fonts[0];
    auto draw_label = [&](ImVec2 ka, ImVec2 kb, const char *label, bool sel) {
        bool icon = (unsigned char)label[0] == 0xEF;
        ImFont *f = icon ? icon_font : font;
        float size = icon ? icon_font->FontSize * 1.15f : fs;
        ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, label);
        dl->AddText(f, size, ImVec2((ka.x + kb.x - ts.x) / 2, (ka.y + kb.y - ts.y) / 2),
                    WithAlpha(sel ? th.on_accent : th.text, k), label);
    };
    for (int rr = 0; rr < 4; rr++) {
        for (int c = 0; c < 10; c++) {
            ImVec2 ka, kb;
            key_rect(rr, c, 1, &ka, &kb);
            draw_key(ka, kb, false);
        }
    }
    std::string space_label = tr("Space");
    const char *special[kSpecialCount] = {
        ICON_FA_ARROW_UP,
        layer == 2 ? "abc" : "#+=",
        space_label.c_str(),
        ICON_FA_DELETE_LEFT,
        ICON_FA_CHECK,
    };
    ImVec2 sp_a[kSpecialCount], sp_b[kSpecialCount];
    for (int i = 0, col = 0; i < kSpecialCount; col += kSpecialSpan[i], i++) {
        key_rect(4, col, kSpecialSpan[i], &sp_a[i], &sp_b[i]);
        bool on = (i == kShift && layer == 1) || (i == kSymbols && layer == 2) ||
                  i == kDone;
        draw_key(sp_a[i], sp_b[i], on);
    }
    // The highlight, under the labels
    Shape(dl, ImVec2(g_osk.hl_a.x - 2 * s, g_osk.hl_a.y - 2 * s),
          ImVec2(g_osk.hl_b.x + 2 * s, g_osk.hl_b.y + 2 * s),
          WithAlpha(WithAlpha(th.accent, 0.25f), k), 10 * s, th.neon); // glow
    Shape(dl, g_osk.hl_a, g_osk.hl_b, WithAlpha(th.accent, k), 8 * s, th.neon);
    for (int rr = 0; rr < 4; rr++) {
        for (int c = 0; c < 10; c++) {
            ImVec2 ka, kb;
            key_rect(rr, c, 1, &ka, &kb);
            char label[2] = { kRows[layer][rr][c], 0 };
            draw_label(ka, kb, label, g_osk.row == rr && g_osk.col == c);
        }
    }
    int sel_special = g_osk.row == 4 ? SpecialAt(g_osk.col) : -1;
    for (int i = 0; i < kSpecialCount; i++) {
        draw_label(sp_a[i], sp_b[i], special[i], sel_special == i);
    }

    // Controller legend, same colors as the dashboard's hint bar
    struct Hint { const char *key; ImU32 col; std::string what; };
    const Hint hints[] = {
        { "A", IM_COL32(63, 163, 27, 255), tr("Press key") },
        { "X", IM_COL32(40, 110, 220, 255), tr("Delete") },
        { "Y", IM_COL32(226, 180, 23, 255), tr("Space") },
        { "LB/RB", 0, tr("Move") },
        { "LT", 0, tr("Hold: symbols") },
        { "RT", 0, tr("Hold: capitals") },
        { "B", IM_COL32(200, 50, 42, 255), tr("Done") },
        { "Start", 0, tr("Enter") },
    };
    // Measured at full size, then shrunk to fit the panel (long languages)
    ImFont *lf = ImGui::GetFont();
    float base = ImGui::GetFontSize();
    auto measure = [&](float size) {
        float u = size / 16, total = 0, hh = size + 4 * u;
        for (const Hint &hnt : hints) {
            float key_w = hnt.col ? hh : lf->CalcTextSizeA(size, FLT_MAX, 0, hnt.key).x;
            total += key_w + 6 * u + lf->CalcTextSizeA(size, FLT_MAX, 0, hnt.what.c_str()).x +
                     18 * u;
        }
        return total - 18 * u;
    };
    float lsize = base;
    float avail = w - pad * 2;
    float total = measure(lsize);
    if (total > avail) {
        lsize = std::max(base * 0.7f, base * avail / total);
        total = measure(lsize);
    }
    float u = lsize / 16, hh = lsize + 4 * u;
    float hx = a.x + (w - total) / 2;
    float hy = keys_y + keys_h + 12 * s + (legend_h - hh) / 2;
    for (const Hint &hnt : hints) {
        if (hnt.col) {
            hx += Badge(dl, lf, lsize, ImVec2(hx, hy), hh, hnt.key, hnt.col, hnt.what.c_str(),
                        th.dim, k);
        } else {
            ImVec2 ks = lf->CalcTextSizeA(lsize, FLT_MAX, 0, hnt.key);
            dl->AddText(lf, lsize, ImVec2(hx, hy + (hh - ks.y) / 2), WithAlpha(th.accent, k),
                        hnt.key);
            ImVec2 ws = lf->CalcTextSizeA(lsize, FLT_MAX, 0, hnt.what.c_str());
            dl->AddText(lf, lsize, ImVec2(hx + ks.x + 6 * u, hy + (hh - ws.y) / 2),
                        WithAlpha(th.dim, k), hnt.what.c_str());
            hx += ks.x + 6 * u + ws.x;
        }
        hx += 18 * u;
    }
}
