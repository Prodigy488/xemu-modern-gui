/*
 *  x86 misc helpers - system code
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "accel/tcg/cpu-ldst.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "exec/cputlb.h"
#include "tcg/helper-tcg.h"
#include "hw/i386/apic.h"

void helper_outb(CPUX86State *env, uint32_t port, uint32_t data)
{
    address_space_stb(&address_space_io, port, data,
                      cpu_get_mem_attrs(env), NULL);
}

target_ulong helper_inb(CPUX86State *env, uint32_t port)
{
    return address_space_ldub(&address_space_io, port,
                              cpu_get_mem_attrs(env), NULL);
}

void helper_outw(CPUX86State *env, uint32_t port, uint32_t data)
{
    address_space_stw(&address_space_io, port, data,
                      cpu_get_mem_attrs(env), NULL);
}

target_ulong helper_inw(CPUX86State *env, uint32_t port)
{
    return address_space_lduw(&address_space_io, port,
                              cpu_get_mem_attrs(env), NULL);
}

void helper_outl(CPUX86State *env, uint32_t port, uint32_t data)
{
    address_space_stl(&address_space_io, port, data,
                      cpu_get_mem_attrs(env), NULL);
}

target_ulong helper_inl(CPUX86State *env, uint32_t port)
{
    return address_space_ldl(&address_space_io, port,
                             cpu_get_mem_attrs(env), NULL);
}

target_ulong helper_read_cr8(CPUX86State *env)
{
    if (!(env->hflags2 & HF2_VINTR_MASK)) {
        return cpu_get_apic_tpr(env_archcpu(env)->apic_state);
    } else {
        return env->int_ctl & V_TPR_MASK;
    }
}

void helper_write_crN(CPUX86State *env, int reg, target_ulong t0)
{
    switch (reg) {
    case 0:
        /*
        * If we reach this point, the CR0 write intercept is disabled.
        * But we could still exit if the hypervisor has requested the selective
        * intercept for bits other than TS and MP
        */
        if (cpu_svm_has_intercept(env, SVM_EXIT_CR0_SEL_WRITE) &&
            ((env->cr[0] ^ t0) & ~(CR0_TS_MASK | CR0_MP_MASK))) {
            cpu_vmexit(env, SVM_EXIT_CR0_SEL_WRITE, 0, GETPC());
        }
        cpu_x86_update_cr0(env, t0);
        break;
    case 3:
        if ((env->efer & MSR_EFER_LMA) &&
                (t0 & ((~0ULL) << env_archcpu(env)->phys_bits))) {
            cpu_vmexit(env, SVM_EXIT_ERR, 0, GETPC());
        }
        if (!(env->efer & MSR_EFER_LMA)) {
            t0 &= 0xffffffffUL;
        }
        cpu_x86_update_cr3(env, t0);
        break;
    case 4:
        if (t0 & cr4_reserved_bits(env)) {
            cpu_vmexit(env, SVM_EXIT_ERR, 0, GETPC());
        }
        if (((t0 ^ env->cr[4]) & CR4_LA57_MASK) &&
            (env->hflags & HF_CS64_MASK)) {
            raise_exception_ra(env, EXCP0D_GPF, GETPC());
        }
        cpu_x86_update_cr4(env, t0);
        break;
    case 8:
        if (!(env->hflags2 & HF2_VINTR_MASK)) {
            bql_lock();
            cpu_set_apic_tpr(env_archcpu(env)->apic_state, t0);
            bql_unlock();
        }
        env->int_ctl = (env->int_ctl & ~V_TPR_MASK) | (t0 & V_TPR_MASK);

        CPUState *cs = env_cpu(env);
        if (ctl_has_irq(env)) {
            cpu_interrupt(cs, CPU_INTERRUPT_VIRQ);
        } else {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_VIRQ);
        }
        break;
    default:
        env->cr[reg] = t0;
        break;
    }
}

void helper_wrmsr(CPUX86State *env)
{
    uint64_t val;
    CPUState *cs = env_cpu(env);

    cpu_svm_check_intercept_param(env, SVM_EXIT_MSR, 1, GETPC());

    val = ((uint32_t)env->regs[R_EAX]) |
        ((uint64_t)((uint32_t)env->regs[R_EDX]) << 32);

    switch ((uint32_t)env->regs[R_ECX]) {
    case MSR_IA32_SYSENTER_CS:
        env->sysenter_cs = val & 0xffff;
        break;
    case MSR_IA32_SYSENTER_ESP:
        env->sysenter_esp = val;
        break;
    case MSR_IA32_SYSENTER_EIP:
        env->sysenter_eip = val;
        break;
    case MSR_IA32_APICBASE: {
        int ret;

        if (val & MSR_IA32_APICBASE_RESERVED) {
            goto error;
        }

        ret = cpu_set_apic_base(env_archcpu(env)->apic_state, val);
        if (ret < 0) {
            goto error;
        }
        break;
    }
    case MSR_EFER:
        {
            uint64_t update_mask;

            update_mask = 0;
            if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_SYSCALL) {
                update_mask |= MSR_EFER_SCE;
            }
            if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_LM) {
                update_mask |= MSR_EFER_LME;
            }
            if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_FFXSR) {
                update_mask |= MSR_EFER_FFXSR;
            }
            if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_NX) {
                update_mask |= MSR_EFER_NXE;
            }
            if (env->features[FEAT_8000_0001_ECX] & CPUID_EXT3_SVM) {
                update_mask |= MSR_EFER_SVME;
            }
            if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_FFXSR) {
                update_mask |= MSR_EFER_FFXSR;
            }
            cpu_load_efer(env, (env->efer & ~update_mask) |
                          (val & update_mask));
        }
        break;
    case MSR_STAR:
        env->star = val;
        break;
    case MSR_PAT:
        env->pat = val;
        break;
    case MSR_IA32_PKRS:
        if (val & 0xFFFFFFFF00000000ull) {
            goto error;
        }
        env->pkrs = val;
        tlb_flush(cs);
        break;
    case MSR_VM_HSAVE_PA:
        if (val & (0xfff | ((~0ULL) << env_archcpu(env)->phys_bits))) {
            goto error;
        }
        env->vm_hsave = val;
        break;
#ifdef TARGET_X86_64
    case MSR_LSTAR:
        env->lstar = val;
        break;
    case MSR_CSTAR:
        env->cstar = val;
        break;
    case MSR_FMASK:
        env->fmask = val;
        break;
    case MSR_FSBASE:
        env->segs[R_FS].base = val;
        break;
    case MSR_GSBASE:
        env->segs[R_GS].base = val;
        break;
    case MSR_KERNELGSBASE:
        env->kernelgsbase = val;
        break;
#endif
    case MSR_MTRRphysBase(0):
    case MSR_MTRRphysBase(1):
    case MSR_MTRRphysBase(2):
    case MSR_MTRRphysBase(3):
    case MSR_MTRRphysBase(4):
    case MSR_MTRRphysBase(5):
    case MSR_MTRRphysBase(6):
    case MSR_MTRRphysBase(7):
        env->mtrr_var[((uint32_t)env->regs[R_ECX] -
                       MSR_MTRRphysBase(0)) / 2].base = val;
        break;
    case MSR_MTRRphysMask(0):
    case MSR_MTRRphysMask(1):
    case MSR_MTRRphysMask(2):
    case MSR_MTRRphysMask(3):
    case MSR_MTRRphysMask(4):
    case MSR_MTRRphysMask(5):
    case MSR_MTRRphysMask(6):
    case MSR_MTRRphysMask(7):
        env->mtrr_var[((uint32_t)env->regs[R_ECX] -
                       MSR_MTRRphysMask(0)) / 2].mask = val;
        break;
    case MSR_MTRRfix64K_00000:
        env->mtrr_fixed[(uint32_t)env->regs[R_ECX] -
                        MSR_MTRRfix64K_00000] = val;
        break;
    case MSR_MTRRfix16K_80000:
    case MSR_MTRRfix16K_A0000:
        env->mtrr_fixed[(uint32_t)env->regs[R_ECX] -
                        MSR_MTRRfix16K_80000 + 1] = val;
        break;
    case MSR_MTRRfix4K_C0000:
    case MSR_MTRRfix4K_C8000:
    case MSR_MTRRfix4K_D0000:
    case MSR_MTRRfix4K_D8000:
    case MSR_MTRRfix4K_E0000:
    case MSR_MTRRfix4K_E8000:
    case MSR_MTRRfix4K_F0000:
    case MSR_MTRRfix4K_F8000:
        env->mtrr_fixed[(uint32_t)env->regs[R_ECX] -
                        MSR_MTRRfix4K_C0000 + 3] = val;
        break;
    case MSR_MTRRdefType:
        env->mtrr_deftype = val;
        break;
    case MSR_MCG_STATUS:
        env->mcg_status = val;
        break;
    case MSR_MCG_CTL:
        if ((env->mcg_cap & MCG_CTL_P)
            && (val == 0 || val == ~(uint64_t)0)) {
            env->mcg_ctl = val;
        }
        break;
    case MSR_TSC_AUX:
        env->tsc_aux = val;
        break;
    case MSR_IA32_MISC_ENABLE:
        env->msr_ia32_misc_enable = val;
        break;
    case MSR_IA32_BNDCFGS:
        /* FIXME: #GP if reserved bits are set.  */
        /* FIXME: Extend highest implemented bit of linear address.  */
        env->msr_bndcfgs = val;
        cpu_sync_bndcs_hflags(env);
        break;
    case MSR_APIC_START ... MSR_APIC_END: {
        int ret;
        int index = (uint32_t)env->regs[R_ECX] - MSR_APIC_START;

        bql_lock();
        ret = apic_msr_write(env_archcpu(env)->apic_state, index, val);
        bql_unlock();
        if (ret < 0) {
            goto error;
        }

        break;
    }
    default:
        if ((uint32_t)env->regs[R_ECX] >= MSR_MC0_CTL
            && (uint32_t)env->regs[R_ECX] < MSR_MC0_CTL +
            (4 * env->mcg_cap & 0xff)) {
            uint32_t offset = (uint32_t)env->regs[R_ECX] - MSR_MC0_CTL;
            if ((offset & 0x3) != 0
                || (val == 0 || val == ~(uint64_t)0)) {
                env->mce_banks[offset] = val;
            }
            break;
        }
        /* XXX: exception? */
        break;
    }
    return;
error:
    raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
}

void helper_rdmsr(CPUX86State *env)
{
    X86CPU *x86_cpu = env_archcpu(env);
    uint64_t val;

    cpu_svm_check_intercept_param(env, SVM_EXIT_MSR, 0, GETPC());

    switch ((uint32_t)env->regs[R_ECX]) {
    case MSR_IA32_SYSENTER_CS:
        val = env->sysenter_cs;
        break;
    case MSR_IA32_SYSENTER_ESP:
        val = env->sysenter_esp;
        break;
    case MSR_IA32_SYSENTER_EIP:
        val = env->sysenter_eip;
        break;
    case MSR_IA32_APICBASE:
        val = cpu_get_apic_base(env_archcpu(env)->apic_state);
        break;
    case MSR_EFER:
        val = env->efer;
        break;
    case MSR_STAR:
        val = env->star;
        break;
    case MSR_PAT:
        val = env->pat;
        break;
    case MSR_IA32_PKRS:
        val = env->pkrs;
        break;
    case MSR_VM_HSAVE_PA:
        val = env->vm_hsave;
        break;
    case MSR_IA32_PERF_STATUS:
        /* tsc_increment_by_tick */
        val = 1000ULL;
        /* CPU multiplier */
        val |= (((uint64_t)4ULL) << 40);
        break;
#ifdef TARGET_X86_64
    case MSR_LSTAR:
        val = env->lstar;
        break;
    case MSR_CSTAR:
        val = env->cstar;
        break;
    case MSR_FMASK:
        val = env->fmask;
        break;
    case MSR_FSBASE:
        val = env->segs[R_FS].base;
        break;
    case MSR_GSBASE:
        val = env->segs[R_GS].base;
        break;
    case MSR_KERNELGSBASE:
        val = env->kernelgsbase;
        break;
    case MSR_TSC_AUX:
        val = env->tsc_aux;
        break;
#endif
    case MSR_SMI_COUNT:
        val = env->msr_smi_count;
        break;
    case MSR_MTRRphysBase(0):
    case MSR_MTRRphysBase(1):
    case MSR_MTRRphysBase(2):
    case MSR_MTRRphysBase(3):
    case MSR_MTRRphysBase(4):
    case MSR_MTRRphysBase(5):
    case MSR_MTRRphysBase(6):
    case MSR_MTRRphysBase(7):
        val = env->mtrr_var[((uint32_t)env->regs[R_ECX] -
                             MSR_MTRRphysBase(0)) / 2].base;
        break;
    case MSR_MTRRphysMask(0):
    case MSR_MTRRphysMask(1):
    case MSR_MTRRphysMask(2):
    case MSR_MTRRphysMask(3):
    case MSR_MTRRphysMask(4):
    case MSR_MTRRphysMask(5):
    case MSR_MTRRphysMask(6):
    case MSR_MTRRphysMask(7):
        val = env->mtrr_var[((uint32_t)env->regs[R_ECX] -
                             MSR_MTRRphysMask(0)) / 2].mask;
        break;
    case MSR_MTRRfix64K_00000:
        val = env->mtrr_fixed[0];
        break;
    case MSR_MTRRfix16K_80000:
    case MSR_MTRRfix16K_A0000:
        val = env->mtrr_fixed[(uint32_t)env->regs[R_ECX] -
                              MSR_MTRRfix16K_80000 + 1];
        break;
    case MSR_MTRRfix4K_C0000:
    case MSR_MTRRfix4K_C8000:
    case MSR_MTRRfix4K_D0000:
    case MSR_MTRRfix4K_D8000:
    case MSR_MTRRfix4K_E0000:
    case MSR_MTRRfix4K_E8000:
    case MSR_MTRRfix4K_F0000:
    case MSR_MTRRfix4K_F8000:
        val = env->mtrr_fixed[(uint32_t)env->regs[R_ECX] -
                              MSR_MTRRfix4K_C0000 + 3];
        break;
    case MSR_MTRRdefType:
        val = env->mtrr_deftype;
        break;
    case MSR_MTRRcap:
        if (env->features[FEAT_1_EDX] & CPUID_MTRR) {
            val = MSR_MTRRcap_VCNT | MSR_MTRRcap_FIXRANGE_SUPPORT |
                MSR_MTRRcap_WC_SUPPORTED;
        } else {
            /* XXX: exception? */
            val = 0;
        }
        break;
#ifdef XBOX
    case MSR_IA32_EBL_CR_POWERON:
        val = 0xc5040000;
        break;
#endif
    case MSR_MCG_CAP:
        val = env->mcg_cap;
        break;
    case MSR_MCG_CTL:
        if (env->mcg_cap & MCG_CTL_P) {
            val = env->mcg_ctl;
        } else {
            val = 0;
        }
        break;
    case MSR_MCG_STATUS:
        val = env->mcg_status;
        break;
    case MSR_IA32_MISC_ENABLE:
        val = env->msr_ia32_misc_enable;
        break;
    case MSR_IA32_BNDCFGS:
        val = env->msr_bndcfgs;
        break;
     case MSR_IA32_UCODE_REV:
        val = x86_cpu->ucode_rev;
        break;
    case MSR_CORE_THREAD_COUNT: {
        val = cpu_x86_get_msr_core_thread_count(x86_cpu);
        break;
    }
    case MSR_APIC_START ... MSR_APIC_END: {
        int ret;
        int index = (uint32_t)env->regs[R_ECX] - MSR_APIC_START;

        bql_lock();
        ret = apic_msr_read(x86_cpu->apic_state, index, &val);
        bql_unlock();
        if (ret < 0) {
            raise_exception_err_ra(env, EXCP0D_GPF, 0, GETPC());
        }

        break;
    }
    default:
        if ((uint32_t)env->regs[R_ECX] >= MSR_MC0_CTL
            && (uint32_t)env->regs[R_ECX] < MSR_MC0_CTL +
            (4 * env->mcg_cap & 0xff)) {
            uint32_t offset = (uint32_t)env->regs[R_ECX] - MSR_MC0_CTL;
            val = env->mce_banks[offset];
            break;
        }
        /* XXX: exception? */
        val = 0;
        break;
    }
    env->regs[R_EAX] = (uint32_t)(val);
    env->regs[R_EDX] = (uint32_t)(val >> 32);
}

void helper_flush_page(CPUX86State *env, target_ulong addr)
{
    tlb_flush_page(env_cpu(env), addr);
}

/* Print `len` bytes of guest memory from `addr` (virtual), `??` if unmapped */
static void xemu_dump_guest_bytes(CPUState *cs, const char *label,
                                  uint32_t addr, uint32_t len)
{
    fprintf(stderr, "guest: %s @%08x:", label, addr);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b;
        if (cpu_memory_rw_debug(cs, addr + i, &b, 1, false)) {
            fprintf(stderr, " ??");
        } else {
            fprintf(stderr, " %02x", b);
        }
    }
    fprintf(stderr, "\n");
}

/* A stop in the guest kernel leaves a trap frame and the call chain on the
 * stack. Dump the code and data they point at, so the failing kernel call and
 * the object it was given can be worked out from the log. */
static void xemu_dump_stop_context(CPUState *cs, uint32_t esp)
{
    enum { N = 192 };
    uint32_t st[N];
    int n = 0;
    for (; n < N; n++) {
        if (cpu_memory_rw_debug(cs, esp + 4 * n, (uint8_t *)&st[n], 4, false)) {
            break;
        }
    }

    /* trap frame: error code, EIP (kernel), CS 8, EFLAGS */
    for (int i = 0; i + 3 < n; i++) {
        if (st[i + 2] == 8 && st[i + 1] >= 0x80010000 && st[i + 1] < 0x80100000 &&
            (st[i + 3] & 2) && st[i] < 0x20) {
            xemu_dump_guest_bytes(cs, "kernel code around the fault",
                                  st[i + 1] - 0x60, 0x80);
            break;
        }
    }

    uint32_t seen[16];
    int ns = 0;
    for (int i = 0; i < n && ns < 10; i++) {
        uint32_t a = st[i];
        if (a < 0x80010000 || a >= 0x80100000) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < ns; j++) {
            dup |= seen[j] == a;
        }
        if (!dup) {
            seen[ns++] = a;
            xemu_dump_guest_bytes(cs, "kernel code before a return address",
                                  a - 0x30, 0x40);
        }
    }

    ns = 0;
    for (int i = 0; i < n && ns < 8; i++) {
        uint32_t a = st[i];
        uint8_t b[8];
        if (a < 0x00010000 || a >= 0x00400000 ||
            cpu_memory_rw_debug(cs, a - 8, b, 8, false)) {
            continue;
        }
        /* b[3] is the byte at a-5 (call rel32), b[2..3] at a-6 (call [mem]) */
        bool call = b[3] == 0xe8 || (b[2] == 0xff && b[3] == 0x15) ||
                    b[6] == 0xff;
        bool dup = false;
        for (int j = 0; j < ns; j++) {
            dup |= seen[j] == a;
        }
        if (call && !dup) {
            seen[ns++] = a;
            xemu_dump_guest_bytes(cs, "game code before a return address",
                                  a - 0x20, 0x28);
        }
    }

    ns = 0;
    for (int i = 0; i < n && ns < 4; i++) {
        uint32_t a = st[i];
        if (a < 0x00010000 || a >= 0x04000000 || (a & 3)) {
            continue;
        }
        int count = 0;
        for (int j = 0; j < n; j++) {
            count += st[j] == a;
        }
        bool dup = false;
        for (int j = 0; j < ns; j++) {
            dup |= seen[j] == a;
        }
        if (count >= 2 && !dup) {
            seen[ns++] = a;
            xemu_dump_guest_bytes(cs, "memory a call kept passing", a - 0x10,
                                  0x50);
        }
    }
}

/* A guest that halts with interrupts off never wakes: say where, once per
 * halt, for the first few (a console that dies shows a black screen). */
static void xemu_report_dead_halt(CPUX86State *env)
{
    static int reports;
    CPUState *cs = env_cpu(env);
    uint32_t v;

    if (reports++ >= 6) {
        return;
    }
    fprintf(stderr, "guest: HLT with interrupts disabled at %08x (eflags "
            "%08x, cr0 %08x, cr3 %08x, cr4 %08x); eax %08x ebx %08x ecx %08x "
            "edx %08x esi %08x edi %08x ebp %08x esp %08x\n",
            (uint32_t)env->eip, (uint32_t)env->eflags, (uint32_t)env->cr[0],
            (uint32_t)env->cr[3], (uint32_t)env->cr[4],
            (uint32_t)env->regs[R_EAX], (uint32_t)env->regs[R_EBX],
            (uint32_t)env->regs[R_ECX], (uint32_t)env->regs[R_EDX],
            (uint32_t)env->regs[R_ESI], (uint32_t)env->regs[R_EDI],
            (uint32_t)env->regs[R_EBP], (uint32_t)env->regs[R_ESP]);
    fprintf(stderr, "guest: stack at the halt:");
    for (int i = 0; i < 24; i++) {
        if (cpu_memory_rw_debug(cs, (uint32_t)env->regs[R_ESP] + 4 * i,
                                (uint8_t *)&v, 4, false)) {
            break;
        }
        fprintf(stderr, " %08x", v);
    }
    if (reports <= 2) {
        /* The fault that led here left its frames further up this stack. */
        fprintf(stderr, "\nguest: the stack above that, from %08x:",
                (uint32_t)env->regs[R_ESP]);
        for (int i = 0; i < 640; i++) {
            if (cpu_memory_rw_debug(cs, (uint32_t)env->regs[R_ESP] + 4 * i,
                                    (uint8_t *)&v, 4, false)) {
                break;
            }
            if (i % 16 == 0) {
                fprintf(stderr, "\nguest:   +%03x:", 4 * i);
            }
            fprintf(stderr, " %08x", v);
        }
    }
    fprintf(stderr, "\nguest: code before the halt:");
    for (int i = 0; i < 24; i++) {
        uint8_t b;
        if (cpu_memory_rw_debug(cs, (uint32_t)env->eip - 24 + i, &b, 1,
                                false)) {
            fprintf(stderr, " ??");
            continue;
        }
        fprintf(stderr, " %02x", b);
    }
    fprintf(stderr, "\n");
    if (reports <= 2) {
        xemu_dump_stop_context(cs, (uint32_t)env->regs[R_ESP]);
    }
    fflush(stderr);
}

G_NORETURN void helper_hlt(CPUX86State *env)
{
    CPUState *cs = env_cpu(env);

    do_end_instruction(env);
    if (!(env->eflags & IF_MASK)) {
        xemu_report_dead_halt(env);
    }
    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}

void helper_monitor(CPUX86State *env, target_ulong ptr)
{
    if ((uint32_t)env->regs[R_ECX] != 0) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    /* XXX: store address? */
    cpu_svm_check_intercept_param(env, SVM_EXIT_MONITOR, 0, GETPC());
}

G_NORETURN void helper_mwait(CPUX86State *env, int next_eip_addend)
{
    CPUState *cs = env_cpu(env);

    if ((uint32_t)env->regs[R_ECX] != 0) {
        raise_exception_ra(env, EXCP0D_GPF, GETPC());
    }
    cpu_svm_check_intercept_param(env, SVM_EXIT_MWAIT, 0, GETPC());
    env->eip += next_eip_addend;

    /* XXX: not complete but not completely erroneous */
    if (cs->cpu_index != 0 || CPU_NEXT(cs) != NULL) {
        helper_pause(env);
    } else {
        helper_hlt(env);
    }
}
