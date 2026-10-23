/*
 * os-win32.c
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2010 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <windows.h>
#include <mmsystem.h>
#include "system/runstate.h"

static BOOL WINAPI qemu_ctrl_handler(DWORD type)
{
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
    /* Windows 7 kills application when the function returns.
       Sleep here to give QEMU a try for closing.
       Sleep period is 10000ms because Windows kills the program
       after 10 seconds anyway. */
    Sleep(10000);

    return TRUE;
}

static TIMECAPS mm_tc;

static void os_undo_timer_resolution(void)
{
    timeEndPeriod(mm_tc.wPeriodMin);
}

/*
 * Opt out of Windows power throttling (EcoQoS). Without this, Windows may
 * run the emulator on efficiency cores at reduced clocks, or stop honoring
 * the timer resolution above while the window is not in focus, which hurts
 * emulation speed and frame pacing, especially on laptops. Looked up at
 * runtime: older Windows versions simply skip it.
 */
static void os_disable_power_throttling(void)
{
    typedef struct {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    } xemu_power_throttling_state;
    typedef BOOL(WINAPI * set_process_information_fn)(HANDLE, int, LPVOID,
                                                       DWORD);
    const int process_power_throttling = 4; /* ProcessPowerThrottling */
    const ULONG execution_speed = 0x1;
    const ULONG ignore_timer_resolution = 0x4;

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    set_process_information_fn set_info =
        kernel32 ? (set_process_information_fn)(void (*)(void))GetProcAddress(
                       kernel32, "SetProcessInformation")
                 : NULL;
    if (!set_info) {
        return;
    }
    xemu_power_throttling_state state = { 1, /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */
                                          execution_speed |
                                              ignore_timer_resolution,
                                          0 };
    if (!set_info(GetCurrentProcess(), process_power_throttling, &state,
                  sizeof(state))) {
        /* Windows 10 does not know IGNORE_TIMER_RESOLUTION; retry without */
        state.ControlMask = execution_speed;
        set_info(GetCurrentProcess(), process_power_throttling, &state,
                 sizeof(state));
    }
}

void os_setup_early_signal_handling(void)
{
    SetConsoleCtrlHandler(qemu_ctrl_handler, TRUE);
    timeGetDevCaps(&mm_tc, sizeof(mm_tc));
    timeBeginPeriod(mm_tc.wPeriodMin);
    atexit(os_undo_timer_resolution);
    os_disable_power_throttling();
}

void os_set_line_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
