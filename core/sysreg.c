/*
 * WindDroid - system registers (MRS/MSR) and SYS instructions.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "internal.h"

#define CNT_FREQ 62500000ULL

/* ID register values advertised to the guest. */
#define V_MIDR          0x000F0510ULL
#define V_MPIDR         0x80000000ULL
#define V_PFR0          0x01000011ULL   /* EL0/EL1 AArch64, FP + AdvSIMD, GICv3 sysregs */
#define V_DFR0          0x00000006ULL
#define V_ISAR0         0x00210000ULL   /* CRC32, LSE atomics */
#define V_ISAR1         0x00100000ULL   /* LRCPC */
#define V_MMFR0         0x0F000022ULL   /* 40-bit PA, 16-bit ASID, 4 KiB granule only */
#define V_MMFR1         0x00100000ULL   /* PAN */
#define V_CTR           0x84448004ULL
#define V_DCZID         0x4ULL          /* 64-byte DC ZVA blocks */
#define V_CLIDR         0x09200003ULL
#define V_CCSIDR        0x700FE01AULL

static uint64_t timer_ctl(wd_machine *m, uint64_t ctl, uint64_t cval)
{
    uint64_t v = ctl & 3;
    if ((ctl & 1) && wd_counter(m) >= cval) v |= 4;     /* ISTATUS */
    return v;
}

static int el0_allowed(wd_machine *m, uint32_t key)
{
    wd_cpu *c = &m->cpu;
    switch (key) {
    case SR(3, 3, 4, 2, 0):                              /* NZCV */
    case SR(3, 3, 13, 0, 2):                             /* TPIDR_EL0 */
    case SR(3, 3, 13, 0, 3):                             /* TPIDRRO_EL0 (read) */
    case SR(3, 3, 14, 0, 0):                             /* CNTFRQ_EL0 */
    case SR(3, 3, 0, 0, 7):                              /* DCZID_EL0 */
    case SR(3, 3, 4, 4, 0):                              /* FPCR */
    case SR(3, 3, 4, 4, 1):                              /* FPSR */
        return 1;
    case SR(3, 3, 4, 2, 1):                              /* DAIF */
        return (c->sctlr >> 9) & 1;
    case SR(3, 3, 0, 0, 1):                              /* CTR_EL0 */
        return (c->sctlr >> 15) & 1;
    case SR(3, 3, 14, 0, 2):                             /* CNTVCT_EL0 */
    case SR(3, 3, 14, 0, 6):
        return (c->cntkctl >> 1) & 1;
    case SR(3, 3, 14, 0, 1):                             /* CNTPCT_EL0 */
    case SR(3, 3, 14, 0, 5):
        return c->cntkctl & 1;
    case SR(3, 3, 14, 3, 0): case SR(3, 3, 14, 3, 1): case SR(3, 3, 14, 3, 2):
        return (c->cntkctl >> 8) & 1;                     /* EL0VTEN */
    case SR(3, 3, 14, 2, 0): case SR(3, 3, 14, 2, 1): case SR(3, 3, 14, 2, 2):
        return (c->cntkctl >> 9) & 1;                     /* EL0PTEN */
    default:
        return 0;
    }
}

static int is_id_space(uint32_t key)
{
    /* op0=3, op1=0, CRn=0, CRm=1..7: ID registers, unallocated ones are RAZ */
    return (key & ~0x7Fu) == SR(3, 0, 0, 0, 0) && ((key >> 3) & 15) >= 1;
}

bool wd_sysreg_read(wd_machine *m, uint32_t key, uint64_t *val)
{
    wd_cpu *c = &m->cpu;
    if (c->el == 0 && !el0_allowed(m, key)) return false;

    switch (key) {
    case SR(3, 0, 0, 0, 0): *val = V_MIDR; return true;
    case SR(3, 0, 0, 0, 5): *val = V_MPIDR | (uint64_t)m->cur; return true;   /* Aff0 = CPU index */
    case SR(3, 0, 0, 0, 6): *val = 0; return true;      /* REVIDR */
    case SR(3, 0, 0, 4, 0): *val = V_PFR0; return true;
    case SR(3, 0, 0, 5, 0): *val = V_DFR0; return true;
    case SR(3, 0, 0, 6, 0): *val = V_ISAR0; return true;
    case SR(3, 0, 0, 6, 1): *val = V_ISAR1; return true;
    case SR(3, 0, 0, 7, 0): *val = V_MMFR0; return true;
    case SR(3, 0, 0, 7, 1): *val = V_MMFR1; return true;
    case SR(3, 1, 0, 0, 0): *val = V_CCSIDR; return true;
    case SR(3, 1, 0, 0, 1): *val = V_CLIDR; return true;
    case SR(3, 1, 0, 0, 7): *val = 0; return true;      /* AIDR */
    case SR(3, 2, 0, 0, 0): *val = c->csselr; return true;
    case SR(3, 3, 0, 0, 1): *val = V_CTR; return true;
    case SR(3, 3, 0, 0, 7): *val = V_DCZID | (((c->sctlr >> 14) & 1) || c->el ? 0 : 0x10); return true;

    case SR(3, 0, 1, 0, 0): *val = c->sctlr; return true;
    case SR(3, 0, 1, 0, 1): *val = c->actlr; return true;
    case SR(3, 0, 1, 0, 2): *val = c->cpacr; return true;
    case SR(3, 0, 2, 0, 0): *val = c->ttbr0; return true;
    case SR(3, 0, 2, 0, 1): *val = c->ttbr1; return true;
    case SR(3, 0, 2, 0, 2): *val = c->tcr; return true;
    case SR(3, 0, 4, 0, 0): *val = c->spsr; return true;
    case SR(3, 0, 4, 0, 1): *val = c->elr; return true;
    case SR(3, 0, 4, 1, 0):
        if (!c->spsel) return false;
        *val = c->sp_el0; return true;
    case SR(3, 0, 4, 2, 0): *val = (uint64_t)c->spsel; return true;
    case SR(3, 0, 4, 2, 2): *val = (uint64_t)c->el << 2; return true;
    case SR(3, 0, 4, 2, 3): *val = (uint64_t)c->pan << 22; return true;
    case SR(3, 0, 4, 2, 4): *val = 0; return true;      /* UAO */
    case SR(3, 0, 5, 1, 0): *val = c->afsr0; return true;
    case SR(3, 0, 5, 1, 1): *val = c->afsr1; return true;
    case SR(3, 0, 5, 2, 0): *val = c->esr; return true;
    case SR(3, 0, 5, 3, 0): *val = 0; return true;      /* ERRIDR */
    case SR(3, 0, 6, 0, 0): *val = c->far; return true;
    case SR(3, 0, 7, 4, 0): *val = c->par; return true;
    case SR(3, 0, 10, 2, 0): *val = c->mair; return true;
    case SR(3, 0, 10, 3, 0): *val = c->amair; return true;
    case SR(3, 0, 12, 0, 0): *val = c->vbar; return true;
    case SR(3, 0, 12, 0, 1): *val = 0; return true;     /* RVBAR */
    case SR(3, 0, 12, 1, 0):                             /* ISR_EL1 */
        *val = wd_gic_any_pending(m) ? 0x80 : 0;
        return true;
    case SR(3, 0, 12, 1, 1): *val = c->disr; return true;
    case SR(3, 0, 13, 0, 1): *val = c->contextidr; return true;
    case SR(3, 0, 13, 0, 4): *val = c->tpidr_el1; return true;
    case SR(3, 0, 14, 1, 0): *val = c->cntkctl; return true;

    case SR(3, 3, 4, 2, 0): *val = c->nzcv; return true;
    case SR(3, 3, 4, 2, 1): *val = c->daif; return true;
    case SR(3, 3, 13, 0, 2): *val = c->tpidr_el0; return true;
    case SR(3, 3, 13, 0, 3): *val = c->tpidrro_el0; return true;
    case SR(3, 3, 4, 4, 0): wd_fp_check(m); *val = c->fpcr; return true;
    case SR(3, 3, 4, 4, 1): wd_fp_check(m); *val = c->fpsr; return true;
    case SR(3, 0, 0, 3, 0): *val = 0x00000000ULL; return true;    /* MVFR0_EL1 (AArch32 only): RAZ */

    case SR(3, 3, 14, 0, 0): *val = CNT_FREQ; return true;
    case SR(3, 3, 14, 0, 1): case SR(3, 3, 14, 0, 2):
    case SR(3, 3, 14, 0, 5): case SR(3, 3, 14, 0, 6):
        *val = wd_counter(m); return true;
    case SR(3, 3, 14, 2, 0): *val = (uint32_t)(c->cntp_cval - wd_counter(m)); return true;
    case SR(3, 3, 14, 2, 1): *val = timer_ctl(m, c->cntp_ctl, c->cntp_cval); return true;
    case SR(3, 3, 14, 2, 2): *val = c->cntp_cval; return true;
    case SR(3, 3, 14, 3, 0): *val = (uint32_t)(c->cntv_cval - wd_counter(m)); return true;
    case SR(3, 3, 14, 3, 1): *val = timer_ctl(m, c->cntv_ctl, c->cntv_cval); return true;
    case SR(3, 3, 14, 3, 2): *val = c->cntv_cval; return true;

    /* GICv3 CPU interface */
    case SR(3, 0, 4, 6, 0): *val = m->gic.pmr; return true;
    case SR(3, 0, 12, 8, 0): case SR(3, 0, 12, 12, 0): *val = wd_gic_iar(m); return true;
    case SR(3, 0, 12, 8, 2): case SR(3, 0, 12, 12, 2): *val = wd_gic_hppir(m); return true;
    case SR(3, 0, 12, 8, 3): *val = m->gic.bpr0; return true;
    case SR(3, 0, 12, 12, 3): *val = m->gic.bpr1; return true;
    case SR(3, 0, 12, 11, 3): *val = wd_gic_rpr(m); return true;
    case SR(3, 0, 12, 12, 4): *val = m->gic.ctlr | (4u << 8) | (1u << 14); return true;
    case SR(3, 0, 12, 12, 5): *val = 7; return true;    /* ICC_SRE_EL1: SRE, DFB, DIB */
    case SR(3, 0, 12, 12, 6): *val = m->gic.igrpen0; return true;
    case SR(3, 0, 12, 12, 7): *val = m->gic.igrpen1; return true;
    case SR(3, 0, 12, 8, 4): case SR(3, 0, 12, 8, 5): case SR(3, 0, 12, 8, 6): case SR(3, 0, 12, 8, 7):
    case SR(3, 0, 12, 9, 0): case SR(3, 0, 12, 9, 1): case SR(3, 0, 12, 9, 2): case SR(3, 0, 12, 9, 3):
        *val = 0; return true;                           /* active priority registers */

    /* debug */
    case SR(2, 0, 0, 2, 2): *val = c->mdscr; return true;
    case SR(2, 0, 1, 1, 4): *val = c->oslsr | 8; return true;
    case SR(2, 0, 1, 3, 4): *val = c->osdlr; return true;
    default:
        break;
    }
    if (is_id_space(key)) { *val = 0; return true; }
    if ((key >> 14) == 0 && c->el == 1) { *val = 0; return true; }   /* other debug regs: RAZ */
    if ((key & 0x7F80u) == (SR(3, 3, 9, 0, 0) & 0x7F80u) && c->el == 1) {
        *val = 0; return true;                            /* PMU (op1=3, CRn=9): RAZ */
    }
    return false;
}

bool wd_sysreg_write(wd_machine *m, uint32_t key, uint64_t v)
{
    wd_cpu *c = &m->cpu;
    if (c->el == 0) {
        if (key == SR(3, 3, 13, 0, 3)) return false;     /* TPIDRRO_EL0 is read-only at EL0 */
        if (!el0_allowed(m, key)) return false;
    }

    switch (key) {
    case SR(3, 0, 1, 0, 0): c->sctlr = v; wd_tlb_flush(m); return true;
    case SR(3, 0, 1, 0, 1): c->actlr = v; return true;
    case SR(3, 0, 1, 0, 2): c->cpacr = v; return true;
    case SR(3, 0, 2, 0, 0): c->ttbr0 = v; wd_tlb_flush(m); return true;
    case SR(3, 0, 2, 0, 1): c->ttbr1 = v; wd_tlb_flush(m); return true;
    case SR(3, 0, 2, 0, 2): c->tcr = v; wd_tlb_flush(m); return true;
    case SR(3, 0, 4, 0, 0): c->spsr = v; return true;
    case SR(3, 0, 4, 0, 1): c->elr = v; return true;
    case SR(3, 0, 4, 1, 0):
        if (!c->spsel) return false;
        c->sp_el0 = v; return true;
    case SR(3, 0, 4, 2, 0): c->spsel = v & 1; return true;
    case SR(3, 0, 4, 2, 3): c->pan = (v >> 22) & 1; return true;
    case SR(3, 0, 4, 2, 4): return true;                 /* UAO */
    case SR(3, 0, 5, 1, 0): c->afsr0 = v; return true;
    case SR(3, 0, 5, 1, 1): c->afsr1 = v; return true;
    case SR(3, 0, 5, 2, 0): c->esr = v; return true;
    case SR(3, 0, 6, 0, 0): c->far = v; return true;
    case SR(3, 0, 7, 4, 0): c->par = v; return true;
    case SR(3, 0, 10, 2, 0): c->mair = v; return true;
    case SR(3, 0, 10, 3, 0): c->amair = v; return true;
    case SR(3, 0, 12, 0, 0): c->vbar = v & ~0x7FFULL; return true;
    case SR(3, 0, 12, 1, 1): c->disr = v; return true;
    case SR(3, 0, 13, 0, 1): c->contextidr = v; return true;
    case SR(3, 0, 13, 0, 4): c->tpidr_el1 = v; return true;
    case SR(3, 0, 14, 1, 0): c->cntkctl = v; return true;
    case SR(3, 2, 0, 0, 0): c->csselr = v; return true;

    case SR(3, 3, 4, 2, 0): c->nzcv = (uint32_t)v & 0xF0000000u; return true;
    case SR(3, 3, 4, 2, 1): c->daif = (uint32_t)v & 0x3C0u; m->irq_dirty = 1; return true;
    case SR(3, 3, 13, 0, 2): c->tpidr_el0 = v; return true;
    case SR(3, 3, 13, 0, 3): c->tpidrro_el0 = v; return true;
    case SR(3, 3, 4, 4, 0): wd_fp_check(m); c->fpcr = (uint32_t)v & 0x07C80000u; return true;
    case SR(3, 3, 4, 4, 1): wd_fp_check(m); c->fpsr = (uint32_t)v & 0xF800009Fu; return true;

    case SR(3, 3, 14, 0, 0): return true;                /* CNTFRQ_EL0: fixed */
    case SR(3, 3, 14, 2, 0): c->cntp_cval = wd_counter(m) + wd_sext(v, 32); m->irq_dirty = 1; return true;
    case SR(3, 3, 14, 2, 1): c->cntp_ctl = v & 3; m->irq_dirty = 1; return true;
    case SR(3, 3, 14, 2, 2): c->cntp_cval = v; m->irq_dirty = 1; return true;
    case SR(3, 3, 14, 3, 0): c->cntv_cval = wd_counter(m) + wd_sext(v, 32); m->irq_dirty = 1; return true;
    case SR(3, 3, 14, 3, 1): c->cntv_ctl = v & 3; m->irq_dirty = 1; return true;
    case SR(3, 3, 14, 3, 2): c->cntv_cval = v; m->irq_dirty = 1; return true;

    case SR(3, 0, 4, 6, 0): m->gic.pmr = (uint8_t)(v & 0xF8); m->irq_dirty = 1; return true;
    case SR(3, 0, 12, 8, 1): case SR(3, 0, 12, 12, 1): wd_gic_eoi(m, (uint32_t)v & 0xFFFFFF); return true;
    case SR(3, 0, 12, 11, 1): wd_gic_dir(m, (uint32_t)v & 0xFFFFFF); return true;
    case SR(3, 0, 12, 11, 5): case SR(3, 0, 12, 11, 6): case SR(3, 0, 12, 11, 7):
        wd_gic_sgi(m, v); return true;
    case SR(3, 0, 12, 8, 3): m->gic.bpr0 = v & 7; return true;
    case SR(3, 0, 12, 12, 3): m->gic.bpr1 = v & 7; return true;
    case SR(3, 0, 12, 12, 4): m->gic.ctlr = (uint32_t)v & 3; return true;
    case SR(3, 0, 12, 12, 5): return true;
    case SR(3, 0, 12, 12, 6): m->gic.igrpen0 = v & 1; m->irq_dirty = 1; return true;
    case SR(3, 0, 12, 12, 7): m->gic.igrpen1 = v & 1; m->irq_dirty = 1; return true;
    case SR(3, 0, 12, 8, 4): case SR(3, 0, 12, 8, 5): case SR(3, 0, 12, 8, 6): case SR(3, 0, 12, 8, 7):
    case SR(3, 0, 12, 9, 0): case SR(3, 0, 12, 9, 1): case SR(3, 0, 12, 9, 2): case SR(3, 0, 12, 9, 3):
        return true;

    case SR(2, 0, 0, 2, 2): c->mdscr = v; return true;
    case SR(2, 0, 1, 0, 4): c->oslsr = (v & 1) << 1; return true;   /* OSLAR -> OSLSR.OSLK */
    case SR(2, 0, 1, 3, 4): c->osdlr = v & 1; return true;
    default:
        break;
    }
    if ((key >> 14) == 0 && c->el == 1) return true;                  /* other debug regs: WI */
    if ((key & 0x7F80u) == (SR(3, 3, 9, 0, 0) & 0x7F80u) && c->el == 1) return true;
    return false;
}

static void dc_zva(wd_machine *m, uint64_t va)
{
    va &= ~63ULL;
    wd_translate(m, va, ACC_WRITE, 0);
    for (int i = 0; i < 64; i += 8) wd_write(m, va + (uint64_t)i, 8, 0, 0);
}

void wd_sys_insn(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int op1 = (insn >> 16) & 7, crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
    int rt = insn & 31;
    uint64_t v = rt == 31 ? 0 : c->x[rt];

    if (crn == 7) {
        if (op1 == 3 && crm == 4 && op2 == 1) {         /* DC ZVA */
            if (c->el == 0 && !((c->sctlr >> 14) & 1)) wd_undef(m);
            dc_zva(m, v);
            return;
        }
        if (crm == 8 || crm == 9) {                     /* AT */
            if (c->el == 0 || op1 != 0) wd_undef(m);
            int acc = (op2 & 1) ? ACC_WRITE : ACC_READ;
            int unpriv = (crm == 8 && (op2 == 2 || op2 == 3));
            int saved_pan = c->pan;
            if (crm == 8) c->pan = 0;                   /* S1E1R/W ignore PAN, S1E1RP/WP honour it */
            wd_at(m, v, acc, unpriv);
            c->pan = saved_pan;
            return;
        }
        if (op1 == 3) {                                 /* EL0-accessible cache maintenance */
            if (c->el == 0 && !((c->sctlr >> 26) & 1)) wd_undef(m);
            return;
        }
        if (c->el == 0) wd_undef(m);
        return;                                         /* other cache/prediction maintenance */
    }
    if (crn == 8) {                                     /* TLBI */
        if (c->el == 0) wd_undef(m);
        wd_tlb_flush(m);
        return;
    }
    wd_undef(m);
}
