/*
 * CPU boost: accelerations of the CPU emulation.
 *
 * Ideas and code adapted from Tovarichtch's xemu fork
 * (https://github.com/Tovarichtch/xemu, branch perf-experimental),
 * Copyright (c) 2026 Réda Chérif-Touil, LGPL-2.1-or-later.
 *
 * The "CPU boost" setting (perf.native_sse) is read once at start-up by
 * tcg_x86_init(). Each acceleration sits next to the stock path and runs only
 * when it is set.
 */

#ifndef QEMU_CPU_BOOST_H
#define QEMU_CPU_BOOST_H

extern int xemu_cpu_boost;
extern int xemu_x87_exact;       /* the mode in use */
extern int xemu_x87_title_exact; /* the game asks for exact loads */

#endif
