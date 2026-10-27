//
// xemu User Interface - controller port pickers and the controller test
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "input-ui.hh"
#include "common.hh"
#include "i18n.hh"
#include "misc.hh"
#include "../xemu-notifications.h"
#include "../xemu-input.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

void InputPortCombo(const char *id, int port)
{
    ControllerState *bound = xemu_input_get_bound(port);
    const char *name = tr("Not Connected");
    if (bound) {
        name = bound->type == INPUT_DEVICE_SDL_KEYBOARD
                   ? tr("Keyboard, mouse and controller") : bound->name;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo(id, name)) {
        return;
    }
    if (ImGui::Selectable(tr("Not Connected"), bound == nullptr)) {
        xemu_input_bind(port, nullptr, 1);
        bound = nullptr;
    }
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        const bool is_keyboard = iter->type == INPUT_DEVICE_SDL_KEYBOARD;
        const bool here = bound == iter;
        std::string label = is_keyboard ? std::string(tr("Keyboard, mouse and controller"))
                                        : std::string(iter->name);
        if (iter->bound >= 0 && !here) {
            label += string_format(" (%s)", string_format(tr("Port %d"), iter->bound + 1).c_str());
        }
        ImGui::PushID(iter);
        if (ImGui::Selectable(label.c_str(), here)) {
            if (!here) {
                xemu_input_bind(port, iter, 1);
                if (bound == nullptr) {
                    xemu_input_rebind_xmu(port);
                }
                if (is_keyboard) {
                    // The mouse comes on with the keyboard; it can be turned
                    // off in the mouse settings
                    g_config.input.mouse.enabled = true;
                    g_config.input.mouse.defaulted = true;
                    xemu_queue_notification(tr(
                        "Mouse is on for the keyboard. You can turn it off under Mouse and keyboard."));
                }
                bound = iter;
            }
        }
        if (here) {
            ImGui::SetItemDefaultFocus();
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

// Draws one controller's live state
void InputTestWidget(const char *id)
{
    ControllerState *first = nullptr, *first_pad = nullptr;
    static ControllerState *sel = nullptr;
    bool found = false;
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        first = first ? first : iter;
        if (iter->type == INPUT_DEVICE_SDL_GAMEPAD && !first_pad) {
            first_pad = iter;
        }
        found |= iter == sel;
    }
    // Until you pick one yourself, show the first controller (the keyboard
    // if there is none), and follow it when one is plugged in
    static bool chosen = false;
    if (!found || !chosen) {
        sel = first_pad ? first_pad : first;
        chosen = chosen && found;
    }
    if (!sel) {
        ImGui::TextDisabled("%s", tr("No controller connected."));
        return;
    }
    ImGui::PushID(id);
    const char *name = sel->type == INPUT_DEVICE_SDL_KEYBOARD
                           ? tr("Keyboard, mouse and controller") : sel->name;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##testdev", name)) {
        QTAILQ_FOREACH(iter, &available_controllers, entry) {
            ImGui::PushID(iter);
            const char *n = iter->type == INPUT_DEVICE_SDL_KEYBOARD
                                ? tr("Keyboard, mouse and controller") : iter->name;
            if (ImGui::Selectable(n, iter == sel)) {
                sel = iter;
                chosen = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    const float fs = ImGui::GetFontSize();
    float W = std::min(ImGui::GetContentRegionAvail().x, 30.0f * fs * 1.6f);
    W = std::max(W, 10.0f * fs);
    const float k = W / 420.0f;
    const float H = 190.0f * k;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, H + 4 * k));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ControllerState &st = *sel;

    const ImU32 line = IM_COL32(150, 160, 180, 200);
    const ImU32 dim = IM_COL32(60, 66, 80, 160);
    const ImU32 on_col = IM_COL32(120, 230, 255, 255);
    auto P = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
    auto label = [&](float x, float y, const char *t, ImU32 c) {
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(o.x + x * k - ts.x / 2, o.y + y * k - ts.y / 2), c, t);
    };
    auto lit = [&](uint32_t mask) { return (st.buttons & mask) != 0; };
    auto dot = [&](float x, float y, float r, bool on, ImU32 col, const char *t) {
        dl->AddCircleFilled(P(x, y), r * k, on ? col : dim, 24);
        dl->AddCircle(P(x, y), r * k, line, 24, 1.5f);
        if (t) {
            label(x, y, t, on ? IM_COL32(10, 10, 14, 255) : line);
        }
    };
    auto bar = [&](float x0, float y0, float w, float h, float v, const char *t) {
        v = std::min(1.0f, std::max(0.0f, v));
        dl->AddRectFilled(P(x0, y0), P(x0 + w, y0 + h), dim, 3 * k);
        dl->AddRectFilled(P(x0, y0), P(x0 + w * v, y0 + h), on_col, 3 * k);
        dl->AddRect(P(x0, y0), P(x0 + w, y0 + h), line, 3 * k);
        label(x0 + w / 2, y0 + h + 9, t, line);
    };
    auto stick = [&](float cx, float cy, int ax, int ay, bool pressed) {
        const float r = 34;
        dl->AddCircleFilled(P(cx, cy), r * k, dim, 32);
        dl->AddCircle(P(cx, cy), r * k, line, 32, 1.5f);
        float x = st.axis[ax] / 32767.0f, y = st.axis[ay] / 32767.0f;
        float m = std::sqrt(x * x + y * y);
        if (m > 1.0f) {
            x /= m;
            y /= m;
        }
        dl->AddCircleFilled(P(cx + x * r * 0.7f, cy - y * r * 0.7f), 11 * k,
                            pressed ? on_col : IM_COL32(190, 200, 220, 255), 20);
    };

    // Triggers and the White / Black buttons across the top
    bar(10, 6, 110, 12, st.axis[CONTROLLER_AXIS_LTRIG] / 32767.0f, "LT");
    bar(300, 6, 110, 12, st.axis[CONTROLLER_AXIS_RTRIG] / 32767.0f, "RT");
    dot(160, 16, 11, lit(CONTROLLER_BUTTON_WHITE), IM_COL32(235, 235, 235, 255), "W");
    dot(260, 16, 11, lit(CONTROLLER_BUTTON_BLACK), IM_COL32(150, 150, 150, 255), "B");
    // Sticks, D-pad
    stick(70, 78, CONTROLLER_AXIS_LSTICK_X, CONTROLLER_AXIS_LSTICK_Y, lit(CONTROLLER_BUTTON_LSTICK));
    stick(285, 142, CONTROLLER_AXIS_RSTICK_X, CONTROLLER_AXIS_RSTICK_Y, lit(CONTROLLER_BUTTON_RSTICK));
    dot(135, 128, 10, lit(CONTROLLER_BUTTON_DPAD_UP) , on_col, nullptr);
    dot(135, 168, 10, lit(CONTROLLER_BUTTON_DPAD_DOWN), on_col, nullptr);
    dot(115, 148, 10, lit(CONTROLLER_BUTTON_DPAD_LEFT), on_col, nullptr);
    dot(155, 148, 10, lit(CONTROLLER_BUTTON_DPAD_RIGHT), on_col, nullptr);
    label(135, 148, "+", line);
    // Back and Start
    dot(190, 80, 9, lit(CONTROLLER_BUTTON_BACK), on_col, nullptr);
    dot(230, 80, 9, lit(CONTROLLER_BUTTON_START), on_col, nullptr);
    label(190, 98, tr("Back"), line);
    label(230, 98, tr("Start"), line);
    // Face buttons
    dot(352, 82, 12, lit(CONTROLLER_BUTTON_A), IM_COL32(110, 220, 120, 255), "A");
    dot(378, 58, 12, lit(CONTROLLER_BUTTON_B), IM_COL32(235, 100, 100, 255), "B");
    dot(326, 58, 12, lit(CONTROLLER_BUTTON_X), IM_COL32(110, 160, 255, 255), "X");
    dot(352, 34, 12, lit(CONTROLLER_BUTTON_Y), IM_COL32(245, 205, 90, 255), "Y");

    std::string where = sel->bound >= 0 ? string_format(tr("Port %d"), sel->bound + 1)
                                        : std::string(tr("Not on a port"));
    ImGui::TextDisabled("%s", where.c_str());
    ImGui::TextDisabled("%s", tr("Press buttons and move the sticks: they light up here. If "
                                 "nothing happens, the controller isn't reaching xemu."));
    ImGui::PopID();
}
