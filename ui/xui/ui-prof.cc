/*
 * xemu User Interface - slow frame profiler (see ui-prof.hh)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include "ui-prof.hh"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kMaxSections = 48;
constexpr double kSlowMs = 12.0;
constexpr int kMaxLogged = 60;

struct Section {
    const char *name;
    double ms;
};

Section g_sections[kMaxSections];
int g_count;
uint64_t g_last;
uint64_t g_start;
int g_logged;

double ToMs(uint64_t ticks)
{
    return (double)ticks * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

} // namespace

void UiProfBegin()
{
    g_count = 0;
    g_start = g_last = SDL_GetPerformanceCounter();
}

void UiProfMark(const char *name)
{
    const uint64_t now = SDL_GetPerformanceCounter();
    const double ms = ToMs(now - g_last);
    g_last = now;
    for (int i = 0; i < g_count; i++) {
        if (g_sections[i].name == name || !strcmp(g_sections[i].name, name)) {
            g_sections[i].ms += ms;
            return;
        }
    }
    if (g_count < kMaxSections) {
        g_sections[g_count++] = { name, ms };
    }
}

void UiProfEnd()
{
    UiProfMark("rest");
    const double total = ToMs(g_last - g_start);
    if (total < kSlowMs || g_logged >= kMaxLogged) {
        return;
    }
    g_logged++;
    char line[600];
    int n = snprintf(line, sizeof(line), "ui: slow frame, %.0f ms:", total);
    for (int i = 0; i < g_count && n < (int)sizeof(line) - 60; i++) {
        if (g_sections[i].ms >= 1.0) {
            n += snprintf(line + n, sizeof(line) - n, " %s %.0f,", g_sections[i].name,
                          g_sections[i].ms);
        }
    }
    fprintf(stderr, "%s\n", line);
}
