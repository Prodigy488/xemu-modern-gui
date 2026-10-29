/*
 * CPU boost: accelerations of the CPU emulation (see qemu/cpu-boost.h)
 */

#include "qemu/osdep.h"
#include "qemu/cpu-boost.h"

int xemu_cpu_boost;
int xemu_x87_exact = 1;
