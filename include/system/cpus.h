#ifndef QEMU_CPUS_H
#define QEMU_CPUS_H

#include "qemu/thread.h"
#include "qemu/cpu-pace.h"

/* register accel-specific operations */
void cpus_register_accel(const AccelOpsClass *i);

/* return registers ops */
const AccelOpsClass *cpus_get_accel(void);

/* interface available for cpus accelerator threads */

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

void cpus_kick_thread(CPUState *cpu);
bool cpu_work_list_empty(CPUState *cpu);
bool cpu_thread_is_idle(CPUState *cpu);
bool all_cpu_threads_idle(void);
bool cpu_can_run(CPUState *cpu);
void qemu_process_cpu_events_common(CPUState *cpu);
void cpu_thread_signal_created(CPUState *cpu);
void cpu_thread_signal_destroyed(CPUState *cpu);
void cpu_handle_guest_debug(CPUState *cpu);

/*
 * Debug probe (XEMU_TBRATE=1): the vCPU thread's waits, for the big lock
 * held by another thread and halted until an interrupt (lock retake
 * included). Read and reset by the report in accel/tcg/cpu-exec.c. One
 * vCPU, so plain counters.
 */
typedef struct XemuVcpuWaits {
    int64_t bql_ns;
    uint64_t bql_waits;
    int64_t halt_ns;
    uint32_t irqs[256];     /* hardware interrupts taken, by vector */
    int64_t gpu_ns;         /* waiting for a lock the GPU thread holds */
    uint64_t gpu_waits;
    /* Where the guest was when the 1 kHz timer interrupt arrived (the
     * interrupted EIP, with its stack and frame pointer), busiest first
     * in the report. */
    struct { uint32_t eip, esp, ebp, n; } eips[48];
    uint32_t eip_samples;
    /* The DVD drive: reads started, sectors asked for, the range of
     * start sectors, the busiest start sectors, and failed commands. */
    uint32_t ide_reads, ide_sectors, ide_errors;
    int32_t ide_lba_min, ide_lba_max;
    struct { int32_t lba; uint32_t n; } ide_lba[8];
    uint8_t ide_err_sense, ide_err_asc;
    uint32_t ide_host_n;        /* host reads of the image, count */
    uint32_t ide_host_us, ide_host_max_us;  /* total, slowest */
} XemuVcpuWaits;
extern XemuVcpuWaits xemu_vcpu_waits;
extern int xemu_tbrate_state;   /* -1 until the environment is read */
bool xemu_tbrate_init(void);
/* The GPU thread's own account, added atomically, read by the report. */
extern int64_t xemu_tbrate_flipsvc_ns, xemu_tbrate_fence_ns;
void xemu_tbrate_lock_slow(QemuMutex *m);

static inline bool xemu_tbrate_enabled(void)
{
    return likely(xemu_tbrate_state >= 0) ? xemu_tbrate_state
                                          : xemu_tbrate_init();
}

/* XEMU_TBRATE: count one sample of where the guest is. A small
 * frequent-items table: a new address takes a free slot, or when full
 * every count drops by one. */
static inline void xemu_tbrate_sample_eip(uint32_t eip, uint32_t esp,
                                          uint32_t ebp)
{
    XemuVcpuWaits *w = &xemu_vcpu_waits;
    int free_slot = -1;

    w->eip_samples++;
    for (int i = 0; i < (int)ARRAY_SIZE(w->eips); i++) {
        if (w->eips[i].n && w->eips[i].eip == eip) {
            w->eips[i].n++;
            w->eips[i].esp = esp;
            w->eips[i].ebp = ebp;
            return;
        }
        if (!w->eips[i].n && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        w->eips[free_slot].eip = eip;
        w->eips[free_slot].esp = esp;
        w->eips[free_slot].ebp = ebp;
        w->eips[free_slot].n = 1;
        return;
    }
    for (int i = 0; i < (int)ARRAY_SIZE(w->eips); i++) {
        w->eips[i].n--;
    }
}

/* A DVD read command: start sector and length. */
static inline void xemu_tbrate_ide_read(int32_t lba, int nb_sectors)
{
    XemuVcpuWaits *w = &xemu_vcpu_waits;
    int free_slot = -1, victim = 0;

    if (!w->ide_reads++) {
        w->ide_lba_min = w->ide_lba_max = lba;
    }
    w->ide_sectors += nb_sectors;
    w->ide_lba_min = MIN(w->ide_lba_min, lba);
    w->ide_lba_max = MAX(w->ide_lba_max, lba);
    for (int i = 0; i < (int)ARRAY_SIZE(w->ide_lba); i++) {
        if (w->ide_lba[i].n && w->ide_lba[i].lba == lba) {
            w->ide_lba[i].n++;
            return;
        }
        if (!w->ide_lba[i].n && free_slot < 0) {
            free_slot = i;
        }
        if (w->ide_lba[i].n < w->ide_lba[victim].n) {
            victim = i;
        }
    }
    if (free_slot < 0) {
        free_slot = victim;
    }
    w->ide_lba[free_slot].lba = lba;
    w->ide_lba[free_slot].n = 1;
}

/* A GPU lock taken on the guest's behalf; with the probe on, or Real
 * hardware speed (its stall account), a wait for a lock the GPU thread
 * holds is timed. */
static inline void xemu_tbrate_lock(QemuMutex *m)
{
    if (likely(!xemu_tbrate_enabled() && !xemu_cpu_pace_on)) {
        qemu_mutex_lock(m);
    } else {
        xemu_tbrate_lock_slow(m);
    }
}

/* end interface for cpus accelerator threads */

bool qemu_in_vcpu_thread(void);
void qemu_init_cpu_loop(void);
void resume_all_vcpus(void);
void pause_all_vcpus(void);
void cpu_stop_current(void);

/* Unblock cpu */
void qemu_cpu_kick_self(void);

bool cpus_are_resettable(void);

void cpu_synchronize_all_states(void);
void cpu_synchronize_all_post_reset(void);
void cpu_synchronize_all_post_init(void);
void cpu_synchronize_all_pre_loadvm(void);

#endif
