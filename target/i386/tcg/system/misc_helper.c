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
#include <glib/gstdio.h>

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

/* On the first dead halt, save the guest's low memory (the game image and
 * its heap, 0x10000..0x800000) next to the log, so code and data can be
 * studied offline. Unmapped pages are written as zeros. */
static void xemu_dump_guest_image(CPUState *cs)
{
    const uint32_t lo = 0x10000, hi = 0x800000;
    uint8_t page[4096];
    FILE *f = fopen("xemu-guest-memory.bin", "wb");
    uint32_t a, mapped = 0;

    if (!f) {
        fprintf(stderr, "guest: could not create xemu-guest-memory.bin\n");
        return;
    }
    for (a = lo; a < hi; a += sizeof(page)) {
        if (cpu_memory_rw_debug(cs, a, page, sizeof(page), false) < 0) {
            memset(page, 0, sizeof(page));
        } else {
            mapped++;
        }
        fwrite(page, 1, sizeof(page), f);
    }
    fclose(f);
    fprintf(stderr, "guest: saved %08x..%08x (%u pages mapped) to "
            "xemu-guest-memory.bin\n", lo, hi, mapped);
}


/* Debug: walk the game disc's file system the way the Xbox kernel does (a
 * binary search of each directory's name tree) and say what it finds for a
 * path, so a bad image shows up as a bad tree rather than a mystery. */
const char *xemu_debug_dvd_path(void);

typedef struct XdvdEnt {
    uint16_t left, right;
    uint32_t sector, size;
    uint8_t attr, nlen;
    char name[256];
} XdvdEnt;

static bool xdvd_read(FILE *f, int64_t base, uint32_t sector, uint32_t off,
                      void *buf, size_t n)
{
    return fseeko(f, base + (int64_t)sector * 2048 + off, SEEK_SET) == 0 &&
           fread(buf, 1, n, f) == n;
}

static bool xdvd_ent(FILE *f, int64_t base, uint32_t dsec, uint32_t dsize,
                     uint32_t woff, XdvdEnt *e)
{
    uint8_t h[14];

    if (woff * 4 + 14 > dsize || !xdvd_read(f, base, dsec, woff * 4, h, 14)) {
        return false;
    }
    e->left = h[0] | h[1] << 8;
    e->right = h[2] | h[3] << 8;
    e->sector = ldl_le_p(h + 4);
    e->size = ldl_le_p(h + 8);
    e->attr = h[12];
    e->nlen = h[13];
    if (!xdvd_read(f, base, dsec, woff * 4 + 14, e->name, e->nlen)) {
        return false;
    }
    e->name[e->nlen] = 0;
    return true;
}

/* kernel-style lookup: walk the tree comparing names without case */
static bool xdvd_find(FILE *f, int64_t base, uint32_t dsec, uint32_t dsize,
                      const char *name, XdvdEnt *out, int *steps)
{
    uint32_t woff = 0;
    XdvdEnt e;

    for (*steps = 0; *steps < 4096; (*steps)++) {
        int c;

        if (!xdvd_ent(f, base, dsec, dsize, woff, &e)) {
            return false;
        }
        c = g_ascii_strcasecmp(name, e.name);
        if (c == 0) {
            *out = e;
            return true;
        }
        woff = c < 0 ? e.left : e.right;
        if (!woff) {
            return false;
        }
    }
    return false;
}

/* every entry of one directory by scanning its sectors, not by the tree */
static bool xdvd_scan(FILE *f, int64_t base, uint32_t dsec, uint32_t dsize,
                      const char *name, XdvdEnt *out, bool list)
{
    uint8_t *d = g_malloc(dsize);
    uint32_t o = 0;
    bool found = false;

    if (!xdvd_read(f, base, dsec, 0, d, dsize)) {
        g_free(d);
        return false;
    }
    while (o + 14 <= dsize) {
        uint32_t nl = d[o + 13];
        char nm[256];

        if (ldl_le_p(d + o) == 0xffffffff) {
            o = (o + 2048) & ~2047u;     /* padding up to the next sector */
            continue;
        }
        memcpy(nm, d + o + 14, nl);
        nm[nl] = 0;
        if (list) {
            fprintf(stderr, "disc:     %-28s attr %02x sector %u size %u "
                    "(left %u right %u)\n", nm, d[o + 12],
                    ldl_le_p(d + o + 4), ldl_le_p(d + o + 8),
                    d[o] | d[o + 1] << 8, d[o + 2] | d[o + 3] << 8);
        }
        if (name && !g_ascii_strcasecmp(name, nm)) {
            out->sector = ldl_le_p(d + o + 4);
            out->size = ldl_le_p(d + o + 8);
            out->attr = d[o + 12];
            found = true;
        }
        o += (14 + nl + 3) & ~3u;
    }
    g_free(d);
    return found;
}

static void xemu_dump_disc_tree(const char *path)
{
    static const int64_t bases[] = { 0, 0x18300000LL, 0x0FD90000LL,
                                     0x30600000LL };
    static const char *const parts[] = { "Media", "Sounds", NULL };
    uint8_t vd[32];
    FILE *f = g_fopen(path, "rb");
    int64_t base = -1;
    uint32_t dsec, dsize;

    if (!f) {
        fprintf(stderr, "disc: cannot open %s\n", path);
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(bases); i++) {
        if (xdvd_read(f, bases[i], 32, 0, vd, 20) &&
            !memcmp(vd, "MICROSOFT*XBOX*MEDIA*DATA", 20)) {
            base = bases[i];
            break;
        }
    }
    if (base < 0 || !xdvd_read(f, base, 32, 20, vd, 8)) {
        fprintf(stderr, "disc: no Xbox file system found in %s\n", path);
        fclose(f);
        return;
    }
    dsec = ldl_le_p(vd);
    dsize = ldl_le_p(vd + 4);
    fprintf(stderr, "disc: image offset %lld, root directory sector %u, "
            "%u bytes\n", (long long)base, dsec, dsize);
    for (int i = 0; i < 3; i++) {
        XdvdEnt e = { 0 }, s = { 0 };
        int steps = 0;
        bool tree, scan;

        if (dsize > 1 << 20) {
            break;
        }
        fprintf(stderr, "disc: entries of %s directory (sector %u, %u bytes):"
                "\n", i ? (i == 1 ? "\\Media" : "\\Media\\Sounds") : "root",
                dsec, dsize);
        if (!parts[i]) {
            xdvd_scan(f, base, dsec, dsize, NULL, &s, true);
            break;
        }
        tree = xdvd_find(f, base, dsec, dsize, parts[i], &e, &steps);
        scan = xdvd_scan(f, base, dsec, dsize, parts[i], &s, true);
        fprintf(stderr, "disc: kernel-style lookup of \"%s\": %s (%d steps)"
                "%s; scan of the directory: %s\n", parts[i],
                tree ? "found" : "NOT FOUND", steps,
                tree ? "" : "", scan ? "found" : "not found");
        if (tree) {
            fprintf(stderr, "disc:   -> attr %02x sector %u size %u\n",
                    e.attr, e.sector, e.size);
        }
        if (scan && (!tree || s.sector != e.sector)) {
            fprintf(stderr, "disc:   scan says attr %02x sector %u size %u\n",
                    s.attr, s.sector, s.size);
        }
        if (!(tree ? e.attr & 0x10 : scan && s.attr & 0x10)) {
            fprintf(stderr, "disc:   \"%s\" is not marked as a directory\n",
                    parts[i]);
            break;
        }
        dsec = tree ? e.sector : s.sector;
        dsize = tree ? e.size : s.size;
    }
    fclose(f);
}

/* The thread's FS block and its thread-local-storage array: games keep
 * their "last error" there, which says why their last failed call failed. */
static void xemu_dump_tls(CPUState *cs, CPUX86State *env)
{
    uint32_t fsb = (uint32_t)env->segs[R_FS].base, tls = 0, v, p;
    uint8_t buf[16];

    fprintf(stderr, "guest: fs base %08x:", fsb);
    for (int i = 0; i < 16; i++) {
        if (cpu_memory_rw_debug(cs, fsb + 4 * i, (uint8_t *)&v, 4, false)) {
            break;
        }
        if (i == 1) {
            tls = v;
        }
        fprintf(stderr, " %08x", v);
    }
    fprintf(stderr, "\nguest: tls array at %08x:", tls);
    for (int i = 0; i < 48; i++) {
        if (cpu_memory_rw_debug(cs, tls + 4 * i, (uint8_t *)&p, 4, false)) {
            break;
        }
        fprintf(stderr, " %08x", p);
    }
    for (int i = 0; i < 48; i++) {
        if (cpu_memory_rw_debug(cs, tls + 4 * i, (uint8_t *)&p, 4, false) ||
            p < 0x10000 || cpu_memory_rw_debug(cs, p, buf, 16, false)) {
            continue;
        }
        fprintf(stderr, "\nguest:   tls[%d] -> %08x:", i, p);
        for (int j = 0; j < 16; j++) {
            fprintf(stderr, " %02x", buf[j]);
        }
    }
    fprintf(stderr, "\n");
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
    if (reports <= 2) {
        xemu_dump_tls(cs, env);
    }
    if (reports == 1) {
        const char *dvd = xemu_debug_dvd_path();
        if (dvd && *dvd) {
            xemu_dump_disc_tree(dvd);
        }
        xemu_dump_guest_image(cs);
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
