/*
 * Hardware virtualization (WHPX) probe (see qemu/whpx-probe.h)
 */

#include "qemu/osdep.h"
#include "qemu/whpx-probe.h"

static const char *const exit_names[XEMU_WHPX_EXIT_KINDS] = {
    "memory access (device registers)", "port input/output", "halt",
    "interrupt window", "end of interrupt", "model-specific register",
    "cpu identification", "exception", "kicked to handle an event",
    "processor start", "other",
};

static struct {
    int requested;
    int active;
    unsigned failures;
    uint64_t total;
    uint64_t exits[XEMU_WHPX_EXIT_KINDS];
    char start_note[200];
    char last_failure[260];
    void (*flush)(void);
} g_probe;

void xemu_whpx_probe_requested(void)
{
    g_probe.requested = 1;
}

void xemu_whpx_probe_started(int ok, const char *why)
{
    g_probe.active = ok ? 1 : 0;
    snprintf(g_probe.start_note, sizeof(g_probe.start_note), "%s",
             ok || !why ? "" : why);
    if (ok) {
        fprintf(stderr, "whpx probe: the hardware CPU is running\n");
    } else {
        fprintf(stderr, "whpx probe: the hardware CPU did not start: %s\n",
                why ? why : "unknown reason");
    }
}

void xemu_whpx_probe_exit(XemuWhpxExit kind)
{
    g_probe.total++;
    g_probe.exits[kind < XEMU_WHPX_EXIT_KINDS ? kind : XEMU_WHPX_EXIT_OTHER]++;
}

void xemu_whpx_probe_set_flush(void (*flush)(void))
{
    g_probe.flush = flush;
}

void xemu_whpx_probe_failure(const char *what, uint64_t rip, uint64_t extra)
{
    g_probe.failures++;
    snprintf(g_probe.last_failure, sizeof(g_probe.last_failure),
             "%s (guest address 0x%08llx, detail 0x%llx)", what ? what : "failure",
             (unsigned long long)rip, (unsigned long long)extra);
    fprintf(stderr, "whpx probe: failure #%u: %s\n", g_probe.failures,
            g_probe.last_failure);
    if (g_probe.flush) {
        g_probe.flush();
    }
}

int xemu_whpx_probe_requested_flag(void)
{
    return g_probe.requested;
}

int xemu_whpx_probe_active(void)
{
    return g_probe.active;
}

unsigned xemu_whpx_probe_failures(void)
{
    return g_probe.failures;
}

uint64_t xemu_whpx_probe_total_exits(void)
{
    return g_probe.total;
}

uint64_t xemu_whpx_probe_exits(XemuWhpxExit kind)
{
    return kind < XEMU_WHPX_EXIT_KINDS ? g_probe.exits[kind] : 0;
}

const char *xemu_whpx_probe_start_note(void)
{
    return g_probe.start_note;
}

const char *xemu_whpx_probe_last_failure(void)
{
    return g_probe.last_failure;
}

void xemu_whpx_probe_describe(char *buf, size_t size)
{
    size_t n = 0;
    int i;

    if (!size) {
        return;
    }
    buf[0] = 0;
    n += snprintf(buf + n, size - n, "requested: %s\nrunning: %s\n",
                  g_probe.requested ? "yes" : "no",
                  g_probe.active ? "yes" : "no");
    if (g_probe.start_note[0] && n < size) {
        n += snprintf(buf + n, size - n, "did not start: %s\n",
                      g_probe.start_note);
    }
    if (n < size) {
        n += snprintf(buf + n, size - n, "exits: %llu, failures: %u\n",
                      (unsigned long long)g_probe.total, g_probe.failures);
    }
    for (i = 0; i < XEMU_WHPX_EXIT_KINDS && n < size; i++) {
        if (g_probe.exits[i]) {
            n += snprintf(buf + n, size - n, "  %-34s %llu\n", exit_names[i],
                          (unsigned long long)g_probe.exits[i]);
        }
    }
    if (g_probe.last_failure[0] && n < size) {
        n += snprintf(buf + n, size - n, "last failure: %s\n",
                      g_probe.last_failure);
    }
}
