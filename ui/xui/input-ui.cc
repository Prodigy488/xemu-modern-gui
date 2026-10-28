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
#include "game-library.hh"
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

// While the test is running the controller is not allowed to drive the
// dashboard (scrolling, B to go back, LB/RB to change tab), or you could not
// try those buttons. Only counts while the test is on screen.
static bool g_test_on = false;
static int g_test_drawn_frame = -1000;
static double g_test_hold_start = -1;

static bool g_drift_running = false;
static int g_drift_drawn_frame = -1000;

bool InputTestCapturing(void)
{
    return (g_test_on && g_test_drawn_frame >= ImGui::GetFrameCount() - 1) ||
           (g_drift_running && g_drift_drawn_frame >= ImGui::GetFrameCount() - 1);
}

// Draws one controller's live state
void InputTestWidget(const char *id)
{
    // Coming back to the page later: the test is off again
    if (g_test_drawn_frame < ImGui::GetFrameCount() - 2) {
        g_test_on = false;
        g_test_hold_start = -1;
    }
    g_test_drawn_frame = ImGui::GetFrameCount();
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

    // Start / stop. Stop with the mouse, Esc, or by holding Back + Start
    // (a controller can't click it while the dashboard ignores the controller)
    bool pad_exit = false;
    if (g_test_on) {
        bool both = false;
        QTAILQ_FOREACH(iter, &available_controllers, entry) {
            const uint32_t m = CONTROLLER_BUTTON_BACK | CONTROLLER_BUTTON_START;
            if (iter->type == INPUT_DEVICE_SDL_GAMEPAD && (iter->buttons & m) == m) {
                both = true;
            }
        }
        if (both) {
            if (g_test_hold_start < 0) {
                g_test_hold_start = ImGui::GetTime();
            }
            pad_exit = ImGui::GetTime() - g_test_hold_start >= 2.0;
        } else {
            g_test_hold_start = -1;
        }
    }
    if (g_test_on) {
        if (ImGui::Button(tr("Stop test"), ImVec2(-FLT_MIN, 0)) || pad_exit ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                GameLibraryConsumeBack(); // Esc only ends the test
            }
            g_test_on = false;
            g_test_hold_start = -1;
        }
    } else if (ImGui::Button(tr("Start test"), ImVec2(-FLT_MIN, 0))) {
        g_test_on = true;
        g_test_hold_start = -1;
    }

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
    auto stick = [&](float cx, float cy, int ax, int ay, bool pressed, int raw, float dz) {
        const float r = 34;
        dl->AddCircleFilled(P(cx, cy), r * k, dim, 32);
        dl->AddCircle(P(cx, cy), r * k, line, 32, 1.5f);
        if (dz > 0.0f) {
            dl->AddCircle(P(cx, cy), r * k * 0.7f * std::min(dz, 1.0f), IM_COL32(245, 205, 90, 220), 24, 1.2f);
        }
        // The stick as it reports itself, so drift shows up (the dead zone
        // is the ring)
        float x = st.axis[ax] / 32767.0f, y = st.axis[ay] / 32767.0f;
        if (st.type == INPUT_DEVICE_SDL_GAMEPAD) {
            x = st.stick_raw[raw] / 32767.0f;
            y = st.stick_raw[raw + 1] / 32767.0f;
        }
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
    stick(70, 78, CONTROLLER_AXIS_LSTICK_X, CONTROLLER_AXIS_LSTICK_Y, lit(CONTROLLER_BUTTON_LSTICK), 0,
          g_config.input.stick_deadzone_left);
    stick(285, 142, CONTROLLER_AXIS_RSTICK_X, CONTROLLER_AXIS_RSTICK_Y, lit(CONTROLLER_BUTTON_RSTICK), 2,
          g_config.input.stick_deadzone_right);
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
    if (g_test_on) {
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 1.0f, 1.0f), "%s",
                           tr("Testing: the controller only moves the picture now, so it won't "
                              "scroll the page, go back or change tabs. To finish, click Stop "
                              "test, press Esc, or hold Back and Start for 2 seconds."));
    } else {
        ImGui::TextDisabled("%s", tr("Press Start test, then press buttons and move the sticks: "
                                     "they light up here. If nothing happens, the controller "
                                     "isn't reaching xemu."));
    }
    ImGui::PopID();
}

// Stick drift test: watch both sticks for a few seconds while they are left
// alone, and say how far each one rests from the centre.
namespace {
struct DriftStick {
    float max_mag = 0;               // furthest from the centre (0..1)
    float min_x = 9, max_x = -9;     // how much it moved, to spot a touched stick
    float min_y = 9, max_y = -9;
    void Reset() { *this = DriftStick(); }
    void Add(float x, float y)
    {
        max_mag = std::max(max_mag, std::sqrt(x * x + y * y));
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    float Range() const { return std::max(max_x - min_x, max_y - min_y); }
    // Dead zone that would cover it, in whole percent (0 if it's fine)
    int Suggested() const
    {
        int pct = (int)std::ceil(max_mag * 100.0f) + 2;
        return max_mag < 0.02f ? 0 : std::min(pct, 40);
    }
};
} // namespace

void InputDriftTest(const char *id)
{
    constexpr double kSeconds = 3.0;
    static int phase = 0; // 0 not run yet, 1 running, 2 done
    static double started = 0;
    static DriftStick res[2];
    static ControllerState *sel = nullptr;

    // Coming back to the page later: nothing is running
    if (g_drift_drawn_frame < ImGui::GetFrameCount() - 2 && phase == 1) {
        phase = 0;
        g_drift_running = false;
    }
    g_drift_drawn_frame = ImGui::GetFrameCount();

    ControllerState *iter, *first = nullptr;
    bool found = false;
    int pads = 0;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_SDL_GAMEPAD) {
            first = first ? first : iter;
            found |= iter == sel;
            pads++;
        }
    }
    if (!found) {
        sel = first;
    }
    if (!sel) {
        ImGui::TextDisabled("%s", tr("No controller connected."));
        g_drift_running = false;
        return;
    }
    ImGui::PushID(id);

    if (pads > 1) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##driftdev", sel->name)) {
            QTAILQ_FOREACH(iter, &available_controllers, entry) {
                if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) {
                    continue;
                }
                ImGui::PushID(iter);
                if (ImGui::Selectable(iter->name, iter == sel)) {
                    sel = iter;
                    phase = 0;
                    g_drift_running = false;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    const float fs = ImGui::GetFontSize();
    const double now = ImGui::GetTime();
    float raw[4];
    for (int i = 0; i < 4; i++) {
        raw[i] = sel->stick_raw[i] / 32767.0f;
    }
    if (phase == 1) {
        res[0].Add(raw[0], raw[1]);
        res[1].Add(raw[2], raw[3]);
        if (now - started >= kSeconds) {
            phase = 2;
            g_drift_running = false;
        }
    }

    // Both sticks, drawn as live as the controller test draws them: the dot is
    // the stick, the yellow ring the dead zone now, the red ring how far it
    // wandered during the check
    const float r = fs * 3.0f;
    const float gap = fs * 1.5f;
    const float W = std::min(ImGui::GetContentRegionAvail().x, 4 * r + gap);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, 2 * r + fs * 0.5f));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float dz[2] = { g_config.input.stick_deadzone_left, g_config.input.stick_deadzone_right };
    const float x0 = o.x + (W - (4 * r + gap)) / 2;
    for (int i = 0; i < 2; i++) {
        const ImVec2 c(x0 + r + i * (2 * r + gap), o.y + r);
        dl->AddCircleFilled(c, r, IM_COL32(60, 66, 80, 160), 40);
        dl->AddCircle(c, r, IM_COL32(150, 160, 180, 200), 40, 1.5f);
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(150, 160, 180, 70));
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(150, 160, 180, 70));
        if (phase != 0 && res[i].max_mag > 0.0f) {
            dl->AddCircle(c, r * std::min(res[i].max_mag, 1.0f), IM_COL32(235, 100, 100, 230), 32, 1.5f);
        }
        if (dz[i] > 0.0f) {
            dl->AddCircle(c, r * std::min(dz[i], 1.0f), IM_COL32(245, 205, 90, 230), 32, 1.5f);
        }
        float x = raw[i * 2], y = raw[i * 2 + 1];
        const float m = std::sqrt(x * x + y * y);
        if (m > 1.0f) {
            x /= m;
            y /= m;
        }
        dl->AddCircleFilled(ImVec2(c.x + x * r, c.y - y * r), fs * 0.45f,
                            IM_COL32(120, 230, 255, 255), 16);
    }

    const char *names[2] = { tr("Left stick"), tr("Right stick") };
    if (phase == 1) {
        const int left = (int)std::ceil(kSeconds - (now - started));
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 1.0f, 1.0f), "%s",
                           string_format(tr("Checking for %d s. Take your thumbs off the sticks."),
                                         std::max(left, 1)).c_str());
    } else if (ImGui::Button(phase == 2 ? tr("Check again") : tr("Check for stick drift"),
                             ImVec2(-FLT_MIN, 0))) {
        phase = 1;
        started = now;
        res[0].Reset();
        res[1].Reset();
        g_drift_running = true;
    }

    if (phase == 2) {
        int use[2] = { 0, 0 };
        bool any = false;
        for (int i = 0; i < 2; i++) {
            const DriftStick &d = res[i];
            const int pct = (int)std::ceil(d.max_mag * 100.0f);
            std::string msg;
            ImVec4 col(0.55f, 0.95f, 0.6f, 1.0f);
            if (d.Range() > 0.12f) {
                msg = string_format(tr("%s: it moved during the check. Leave it alone and check again."),
                                    names[i]);
                col = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
            } else if (d.Suggested() == 0) {
                msg = string_format(tr("%s: no drift found."), names[i]);
            } else if (d.max_mag > 0.38f) {
                msg = string_format(tr("%s: rests %d%% off centre, too far for a dead zone to fix well."),
                                    names[i], pct);
                col = ImVec4(1.0f, 0.55f, 0.5f, 1.0f);
            } else {
                msg = string_format(tr("%s: rests up to %d%% off centre. A dead zone of %d%% should fix it."),
                                    names[i], pct, d.Suggested());
                col = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
                use[i] = d.Suggested();
                any = true;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
        }
        if (any && ImGui::Button(tr("Use these dead zones"), ImVec2(-FLT_MIN, 0))) {
            if (use[0]) {
                g_config.input.stick_deadzone_left = use[0] / 100.0f;
            }
            if (use[1]) {
                g_config.input.stick_deadzone_right = use[1] / 100.0f;
            }
            xemu_queue_notification(tr("Stick dead zones set."));
        }
    } else if (phase == 0) {
        ImGui::TextDisabled("%s", tr("Leave both sticks alone while it checks. The dot is where each "
                                     "stick sits; the yellow ring is your dead zone."));
    }
    ImGui::PopID();
}
