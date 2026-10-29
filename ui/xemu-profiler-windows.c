/*
 * xemu CPU sampling profiler (Windows; see xemu-profiler.h)
 *
 * While a thread is suspended nothing here may allocate, take a lock or call
 * into the loader: the thread could be holding it. Samples are only copied
 * into memory set aside beforehand, and looked at once every thread is
 * running again.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/atomic.h"
#include "xemu-profiler.h"
#include "xemu-settings.h"
#include <tlhelp32.h>

#define MAX_THREADS 8
#define INTERVAL_MS 4
#define TOP_N 160
#define MAX_MODULES 24

typedef HRESULT (WINAPI *pGetThreadDescription)(HANDLE, PWSTR *);

typedef struct {
    DWORD tid;
    HANDLE h;
    char name[64];
    uint64_t cpu_us;
    uint64_t *rip;      /* one slot per sweep; 0 when not sampled */
} Sampled;

static QemuThread g_thread;
static int g_running;           /* atomic */
static int g_done;              /* atomic */
static int g_seconds;
static char g_result[MAX_PATH];

static uint64_t thread_cpu_us(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) {
        return 0;
    }
    return (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) +
            ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10;
}

static void thread_name(HANDLE h, char *out, size_t len)
{
    out[0] = 0;
    pGetThreadDescription fn = (pGetThreadDescription)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    PWSTR w = NULL;
    if (fn && SUCCEEDED(fn(h, &w)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)len, NULL, NULL);
        LocalFree(w);
    }
}

/* Open every thread of this process except the caller's */
static int open_threads(Sampled *list, int max)
{
    int n = 0;
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 te = { .dwSize = sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok && n < max; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        list[n].tid = te.th32ThreadID;
        list[n].h = h;
        n++;
    }
    CloseHandle(snap);
    return n;
}

typedef struct { uint32_t rva, count; } Bin;

static int cmp_bin(const void *a, const void *b)
{
    const Bin *x = a, *y = b;
    return (y->count > x->count) - (y->count < x->count);
}

static void write_report(Sampled *t, int nt, int sweeps, uint64_t window_us)
{
    snprintf(g_result, sizeof(g_result), "%sprofile.txt", xemu_settings_get_base_path());
    FILE *f = fopen(g_result, "w");
    if (!f) {
        fprintf(stderr, "profiler: could not create %s (error %d)\n", g_result, errno);
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(sizeof(tmp), tmp);
        if (n > 0 && n < sizeof(tmp)) {
            snprintf(g_result, sizeof(g_result), "%sxemu-profile.txt", tmp);
            f = fopen(g_result, "w");
        }
        if (!f) {
            fprintf(stderr, "profiler: could not create %s either (error %d)\n", g_result, errno);
            snprintf(g_result, sizeof(g_result), "FAILED: could not create profile.txt");
            return;
        }
    }
    const char *base = (const char *)GetModuleHandleA(NULL);
    fprintf(f, "xemu profile: %d sweeps of %d ms over %.1f s; image base %p\n",
            sweeps, INTERVAL_MS, window_us / 1e6, (const void *)base);
    char mods[MAX_MODULES][64];
    for (int ti = 0; ti < nt; ti++) {
        Sampled *s = &t[ti];
        uint32_t nbins = 0, cap = 1 << 14;
        Bin *bins = calloc(cap, sizeof(Bin));
        uint64_t total = 0, in_exe = 0, jit = 0;
        uint64_t modcount[MAX_MODULES] = { 0 };
        int nmods = 0;
        for (int i = 0; i < sweeps; i++) {
            uint64_t rip = s->rip[i];
            if (!rip) {
                continue;
            }
            total++;
            HMODULE m = NULL;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)rip, &m) || !m) {
                jit++;
                continue;
            }
            if ((const char *)m == base) {
                in_exe++;
                uint32_t rva = (uint32_t)(rip - (uint64_t)(uintptr_t)base);
                uint32_t h = (rva * 2654435761u) >> 18;
                for (;;) {
                    h &= cap - 1;
                    if (bins[h].count == 0 || bins[h].rva == rva) {
                        break;
                    }
                    h++;
                }
                if (bins[h].count == 0) {
                    nbins++;
                }
                bins[h].rva = rva;
                bins[h].count++;
                continue;
            }
            char name[64] = "?";
            GetModuleFileNameA(m, name, sizeof(name));
            const char *slash = strrchr(name, '\\');
            const char *bn = slash ? slash + 1 : name;
            int k;
            for (k = 0; k < nmods; k++) {
                if (!strcmp(mods[k], bn)) {
                    break;
                }
            }
            if (k == nmods && nmods < MAX_MODULES) {
                snprintf(mods[nmods], sizeof(mods[0]), "%s", bn);
                nmods++;
            }
            if (k < nmods) {
                modcount[k]++;
            }
        }
        fprintf(f, "\nthread %lu \"%s\": %.1f s of CPU, %llu samples\n", (unsigned long)s->tid,
                s->name, s->cpu_us / 1e6, (unsigned long long)total);
        fprintf(f, "  in xemu.exe: %llu   in generated code: %llu", (unsigned long long)in_exe,
                (unsigned long long)jit);
        for (int k = 0; k < nmods; k++) {
            fprintf(f, "   in %s: %llu", mods[k], (unsigned long long)modcount[k]);
        }
        fprintf(f, "\n");
        Bin *flat = malloc(sizeof(Bin) * (nbins ? nbins : 1));
        uint32_t m = 0;
        for (uint32_t i = 0; i < cap; i++) {
            if (bins[i].count) {
                flat[m++] = bins[i];
            }
        }
        qsort(flat, m, sizeof(Bin), cmp_bin);
        for (uint32_t i = 0; i < m && i < TOP_N; i++) {
            fprintf(f, "  %x %u\n", flat[i].rva, flat[i].count);
        }
        free(flat);
        free(bins);
    }
    fclose(f);
    fprintf(stderr, "profiler: wrote %s\n", g_result);
}

static void *profiler_thread(void *opaque)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    Sampled all[256];
    memset(all, 0, sizeof(all));
    int n = open_threads(all, 256);
    fprintf(stderr, "profiler: started, %d threads, %d s\n", n, g_seconds);
    for (int i = 0; i < n; i++) {
        all[i].cpu_us = thread_cpu_us(all[i].h);
    }
    Sleep(1000);

    /* The busiest threads over that second */
    Sampled pick[MAX_THREADS];
    int np = 0;
    for (int i = 0; i < n; i++) {
        all[i].cpu_us = thread_cpu_us(all[i].h) - all[i].cpu_us;
    }
    while (np < MAX_THREADS) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (all[i].h && (best < 0 || all[i].cpu_us > all[best].cpu_us)) {
                best = i;
            }
        }
        if (best < 0 || all[best].cpu_us < 5000) {
            break;
        }
        pick[np++] = all[best];
        all[best].h = NULL;     /* taken (closed below with the rest) */
        pick[np - 1].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                    THREAD_QUERY_INFORMATION, FALSE, pick[np - 1].tid);
    }
    for (int i = 0; i < n; i++) {
        if (all[i].h) {
            CloseHandle(all[i].h);
        }
    }

    fprintf(stderr, "profiler: sampling %d busy threads\n", np);
    const int sweeps_max = g_seconds * 1000 / INTERVAL_MS;
    for (int i = 0; i < np; i++) {
        thread_name(pick[i].h, pick[i].name, sizeof(pick[i].name));
        pick[i].rip = calloc(sweeps_max, sizeof(uint64_t));
    }

    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int sweeps = 0;
    while (sweeps < sweeps_max) {
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - t0.QuadPart) / freq.QuadPart >= g_seconds) {
            break;      /* Sleep() may take longer than asked */
        }
        for (int i = 0; i < np; i++) {
            if (SuspendThread(pick[i].h) == (DWORD)-1) {
                continue;
            }
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(pick[i].h, &ctx)) {
                pick[i].rip[sweeps] = ctx.Rip;
            }
            ResumeThread(pick[i].h);
        }
        sweeps++;
        Sleep(INTERVAL_MS);
    }
    QueryPerformanceCounter(&now);
    const uint64_t window_us = (now.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;

    for (int i = 0; i < np; i++) {
        pick[i].cpu_us = thread_cpu_us(pick[i].h);
    }
    fprintf(stderr, "profiler: %d sweeps done, writing the report\n", sweeps);
    write_report(pick, np, sweeps, window_us);
    for (int i = 0; i < np; i++) {
        CloseHandle(pick[i].h);
        free(pick[i].rip);
    }
    qatomic_set(&g_done, 1);
    qatomic_set(&g_running, 0);
    return NULL;
}

bool xemu_profiler_start(int seconds)
{
    if (qatomic_cmpxchg(&g_running, 0, 1) != 0) {
        return false;
    }
    fprintf(stderr, "profiler: requested\n");
    g_seconds = seconds < 5 ? 5 : (seconds > 120 ? 120 : seconds);
    qemu_thread_create(&g_thread, "xemu.profiler", profiler_thread, NULL,
                       QEMU_THREAD_DETACHED);
    return true;
}

bool xemu_profiler_running(void)
{
    return qatomic_read(&g_running);
}

bool xemu_profiler_take_result(char *out, int len)
{
    if (!qatomic_cmpxchg(&g_done, 1, 0)) {
        return false;
    }
    snprintf(out, len, "%s", g_result);
    return true;
}
