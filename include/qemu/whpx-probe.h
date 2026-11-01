/*
 * Hardware virtualization (WHPX) probe: what the Windows Hypervisor Platform
 * CPU path is doing, so a test run can say which games work on it.
 *
 * The counters are written by the CPU thread and read by the UI; they are
 * plain integers, good enough for a report. Nothing here depends on the
 * Windows headers, so every build has it (it just stays empty elsewhere).
 */

#ifndef QEMU_WHPX_PROBE_H
#define QEMU_WHPX_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Why the virtual processor stopped running guest code */
typedef enum {
    XEMU_WHPX_EXIT_MMIO,        /* memory access (device registers) */
    XEMU_WHPX_EXIT_PORTIO,      /* port input/output */
    XEMU_WHPX_EXIT_HALT,
    XEMU_WHPX_EXIT_INTWINDOW,   /* interrupt window */
    XEMU_WHPX_EXIT_EOI,
    XEMU_WHPX_EXIT_MSR,
    XEMU_WHPX_EXIT_CPUID,
    XEMU_WHPX_EXIT_EXCEPTION,
    XEMU_WHPX_EXIT_CANCELED,    /* kicked out to handle an event */
    XEMU_WHPX_EXIT_SIPI,
    XEMU_WHPX_EXIT_OTHER,
    XEMU_WHPX_EXIT_KINDS
} XemuWhpxExit;

/* The setting asked for it (set once, before the accelerator starts) */
void xemu_whpx_probe_requested(void);

/* The accelerator started (ok) or could not (why says so) */
void xemu_whpx_probe_started(int ok, const char *why);

/* One exit; the CPU thread calls this for every one */
void xemu_whpx_probe_exit(XemuWhpxExit kind);

/* Something went wrong that stops or may stop the game. what is a short
 * description; rip and extra are the guest address and a detail (a device
 * address, an exit code, an error code). Remembered, logged, and the flush
 * function is called so the report reaches the disk before a crash. */
void xemu_whpx_probe_failure(const char *what, uint64_t rip, uint64_t extra);

/* Memory change tracking: one sync of a memory range. tracked says the
 * hypervisor answered (pages changed since the last sync); otherwise the
 * whole range was reported as changed. */
void xemu_whpx_probe_dirty_sync(int tracked, uint64_t pages_total,
                                uint64_t pages_dirty);

/* A function the UI gives to write the report when a failure happens */
void xemu_whpx_probe_set_flush(void (*flush)(void));

int xemu_whpx_probe_requested_flag(void);
int xemu_whpx_probe_active(void);
unsigned xemu_whpx_probe_failures(void);
uint64_t xemu_whpx_probe_total_exits(void);
uint64_t xemu_whpx_probe_exits(XemuWhpxExit kind);

/* Text for the user: the reason it did not start, or the last failure.
 * Both empty when there is none. */
const char *xemu_whpx_probe_start_note(void);
const char *xemu_whpx_probe_last_failure(void);

/* The counters as text lines, for the report */
void xemu_whpx_probe_describe(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
