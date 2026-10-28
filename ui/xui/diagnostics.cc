//
// xemu User Interface: diagnostics for bug reports
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "diagnostics.hh"

#include <cstdio>
#include <cinttypes>

#include "common.hh"
#include "i18n.hh"
#include "modern-version.hh"
#include "hw/xbox/nv2a/nv2a.h"
#include "qemu/units.h"
#include "ui/xemu-notifications.h"
#include "ui/xemu-os-utils.h"
#include "ui/xemu-settings.h"
#include "xemu-version.h"
#include "xemu-xbe.h"

extern "C" {
#include "qapi/qapi-commands-machine.h"
#include "qom/object.h"
#include "system/runstate.h"
}

namespace {

const char *Pick(const char *const *names, int n, int i)
{
    return i >= 0 && i < n ? names[i] : "?";
}

const char *OnOff(bool b)
{
    return b ? "on" : "off";
}

// Only whether a file is set, never where it is
const char *IsSet(const char *path)
{
    return path && *path ? "set" : "not set";
}

const char *GlStr(GLenum name)
{
    const GLubyte *s = glGetString(name);
    return s ? (const char *)s : "unknown";
}

} // namespace

std::string DiagnosticsText()
{
    static const char *const renderers[] = { "none", "OpenGL", "Vulkan" };
    static const char *const filters[] = { "linear", "nearest", "fsr",  "bicubic",  "lanczos",
                                           "sgsr",   "nis",     "rtx",  "crt",      "scanlines" };
    static const char *const styles[] = { "dashboard", "classic",         "blades",        "metro",
                                          "xbox",      "original-blades", "glossy-blades", "modern" };
    static const char *const backends[] = { "nat", "udp", "pcap" };

    std::string t;
    char buf[512];
    auto add = [&](const char *fmt, auto... args) {
        snprintf(buf, sizeof(buf), fmt, args...);
        t += buf;
    };

    t += "xemu Modern GUI diagnostics\n";
    add("Fork version: %s (based on xemu %s, commit %s, built %s)\n", XEMU_MODERN_GUI_VERSION,
        xemu_version, xemu_commit, xemu_date);
    add("OS: %s %s\n", SDL_GetPlatform(), xemu_get_os_info());
    add("CPU: %s, %d logical cores, %d MB RAM\n", xemu_get_cpu_info(), SDL_GetNumLogicalCPUCores(),
        SDL_GetSystemRAM());

    if (g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN) {
        const char *device = g_config.display.vulkan.preferred_physical_device;
        add("GPU: Vulkan, preferred device %s\n", (device && *device) ? device : "automatic");
    } else {
        add("GPU: %s / %s / OpenGL %s\n", GlStr(GL_VENDOR), GlStr(GL_RENDERER), GlStr(GL_VERSION));
    }

    add("Renderer: %s, scale %dx, dynamic resolution %s, filter %s, vsync %s\n",
        Pick(renderers, 3, g_config.display.renderer), nv2a_get_surface_scale_factor(),
        OnOff(g_config.display.quality.dynamic_resolution),
        Pick(filters, 10, g_config.display.filtering), OnOff(g_config.display.window.vsync));
    add("Performance: hard FPU %s, shader cache %s, high priority %s, fast cores %s, realtime DSP %s\n",
        OnOff(g_config.perf.hard_fpu), OnOff(g_config.perf.cache_shaders),
        OnOff(g_config.perf.boost_threads), OnOff(g_config.perf.prefer_fast_cores),
        OnOff(g_config.audio.use_dsp));

    // Memory and AV pack apply at restart, so read them from the machine
    {
        MemoryInfo *mem_info = qmp_query_memory_size_summary(nullptr);
        Object *machine = object_resolve_path_component(object_get_root(), "machine");
        char *avpack = machine ? object_property_get_str(machine, "avpack", nullptr) : nullptr;
        add("Machine: %" PRIu64 " MiB, AV pack %s, %s\n", mem_info ? mem_info->base_memory / MiB : 0,
            avpack ? avpack : "unknown", runstate_is_running() ? "running" : "not running");
        if (mem_info) {
            qapi_free_MemoryInfo(mem_info);
        }
        g_free(avpack);
    }

    add("Files: boot ROM %s, BIOS %s, hard disk %s, disc %s\n", IsSet(g_config.sys.files.bootrom_path),
        IsSet(g_config.sys.files.flashrom_path), IsSet(g_config.sys.files.hdd_path),
        IsSet(g_config.sys.files.dvd_path));
    add("Network: %s, backend %s\n", OnOff(g_config.net.enable),
        Pick(backends, 3, g_config.net.backend));
    const char *lang = XuiLanguageCode();
    add("Dashboard: style %s, theme %s, animations %s, language %s\n",
        Pick(styles, 8, g_config.general.library.style),
        g_config.general.library.theme == 0 ? "neon" : "green", OnOff(g_config.display.ui.use_animations),
        (lang && *lang) ? lang : "auto");
    add("Input: stick dead zone left %d%% right %d%%, pause on focus loss %s\n",
        (int)(g_config.input.stick_deadzone_left * 100 + 0.5),
        (int)(g_config.input.stick_deadzone_right * 100 + 0.5),
        OnOff(g_config.general.pause_on_focus_loss));

    struct xbe *xbe = xemu_get_xbe_info();
    if (xbe && xbe->cert && xbe->header) {
        add("Game: title ID %08X, build stamp 0x%08X, version %u\n", xbe->cert->m_titleid,
            xbe->header->m_timedate, xbe->cert->m_version);
    } else {
        t += "Game: none identified\n";
    }
    return t;
}

void DiagnosticsCopy()
{
    ImGui::SetClipboardText(DiagnosticsText().c_str());
    xemu_queue_notification(tr("Diagnostics copied. Paste them into your bug report."));
}
