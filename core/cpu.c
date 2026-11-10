/*
 * WindDroid - AArch64 (A64) instruction interpreter.
 *
 * Implements the base integer instruction set of ARMv8.0 plus
 * FEAT_LSE (atomics), FEAT_CRC32, FEAT_PAN and FEAT_LRCPC. FP/AdvSIMD
 * are reported as not implemented in ID_AA64PFR0_EL1 and trap as
 * undefined instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

/* ------------------------------------------------------------ helpers */

static inline uint64_t X(wd_cpu *c, int r) { return r == 31 ? 0 : c->x[r]; }

uint64_t *wd_sp_ptr(wd_cpu *c)
{
    return (c->el && c->spsel) ? &c->sp_el1 : &c->sp_el0;
}

static inline uint64_t XS(wd_cpu *c, int r) { return r == 31 ? *wd_sp_ptr(c) : c->x[r]; }

static inline void setX(wd_cpu *c, int r, uint64_t v)
{
    if (r != 31) c->x[r] = v;
}

static inline void setXS(wd_cpu *c, int r, uint64_t v)
{
    if (r == 31) *wd_sp_ptr(c) = v;
    else c->x[r] = v;
}

static inline uint64_t trunc_sf(int sf, uint64_t v) { return sf ? v : (uint32_t)v; }

uint32_t wd_get_pstate(wd_cpu *c)
{
    uint32_t m = c->el == 0 ? 0 : (c->spsel ? 5 : 4);
    return c->nzcv | c->daif | (c->pan ? (1u << 22) : 0) | m;
}

_Noreturn void wd_fatal(wd_machine *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof m->err, fmt, ap);
    va_end(ap);
    m->stop = WD_RUN_ERROR;
    longjmp(m->jb, 1);
}

static void enter_exception(wd_machine *m, uint64_t off, uint64_t ret_pc)
{
    wd_cpu *c = &m->cpu;
    uint64_t vec = c->vbar & ~0x7FFULL;
    if (c->el == 0) vec += 0x400;
    else if (c->spsel) vec += 0x200;
    c->spsr = wd_get_pstate(c);
    c->elr = ret_pc;
    c->el = 1;
    c->spsel = 1;
    c->daif = DAIF_D | DAIF_A | DAIF_I | DAIF_F;
    if (!(c->sctlr & (1ULL << 23))) c->pan = 1;   /* SCTLR_EL1.SPAN == 0 */
    c->excl_valid = 0;
    c->pc = vec + off;
    m->fetch_vpn = ~0ULL;
    m->irq_dirty = 1;
}

_Noreturn void wd_exception(wd_machine *m, uint32_t ec, uint32_t iss, uint64_t far,
                            int has_far, uint64_t ret_pc)
{
    wd_cpu *c = &m->cpu;
    c->esr = ((uint64_t)ec << 26) | (1u << 25) | (iss & 0x1FFFFFF);
    if (has_far) c->far = far;
    enter_exception(m, 0x000, ret_pc);
    longjmp(m->jb, 1);
}

void wd_take_irq(wd_machine *m)
{
    m->wfi = 0;
    enter_exception(m, 0x080, m->cpu.pc);
}

/* An undefined instruction in the OS or firmware usually means something the
 * emulator lacks: say so on the serial console (a few times), then raise it. */
static void log_undef(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    if (c->el == 0 || m->undef_logged >= 8 || !m->uart_tx) return;
    m->undef_logged++;
    uint32_t insn = wd_fetch(m, c->pc);
    char line[128];
    int n = snprintf(line, sizeof line, "\r\n[WindDroid] undefined instruction %08x at pc=%016llx EL%d\r\n",
                     insn, (unsigned long long)c->pc, c->el);
    for (int i = 0; i < n; i++) m->uart_tx(m->uart_opaque, (uint8_t)line[i]);
}

void wd_undef(wd_machine *m)
{
    log_undef(m);
    wd_exception(m, EC_UNKNOWN, 0, 0, 0, m->cpu.pc);
}

void wd_cpu_reset(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    memset(c, 0, sizeof *c);
    c->el = 1;
    c->spsel = 1;
    c->daif = DAIF_D | DAIF_A | DAIF_I | DAIF_F;
    c->sctlr = 0x30D00800ULL;   /* RES1 bits, MMU and caches off */
    c->pc = WD_RAM_BASE;
    wd_tlb_flush(m);
}

/* ------------------------------------------------------------ flags */

static uint64_t add_with_carry(wd_cpu *c, int sf, uint64_t a, uint64_t b, int carry, int setflags)
{
    uint64_t res;
    uint32_t n, z, cf, v;
    if (sf) {
        res = a + b + (uint64_t)carry;
        n = res >> 63;
        z = res == 0;
        cf = (res < a) || (carry && res == a);
        v = ((~(a ^ b) & (a ^ res)) >> 63) & 1;
    } else {
        uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
        uint64_t wide = (uint64_t)a32 + b32 + (uint64_t)carry;
        uint32_t r32 = (uint32_t)wide;
        res = r32;
        n = r32 >> 31;
        z = r32 == 0;
        cf = (wide >> 32) & 1;
        v = ((~(a32 ^ b32) & (a32 ^ r32)) >> 31) & 1;
    }
    if (setflags) c->nzcv = (n << 31) | (z << 30) | (cf << 29) | (v << 28);
    return res;
}

static void set_nz(wd_cpu *c, int sf, uint64_t res)
{
    uint32_t n = sf ? (res >> 63) : ((res >> 31) & 1);
    uint32_t z = sf ? res == 0 : (uint32_t)res == 0;
    c->nzcv = (n << 31) | (z << 30);
}

static int cond_holds(uint32_t nzcv, int cond)
{
    int n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, cf = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    int r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cf; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cf && !z; break;
    case 5: r = n == v; break;
    case 6: r = (n == v) && !z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

/* ------------------------------------------------------------ bit helpers */

int wd_decode_bitmask(int sf, int N, int imms, int immr, uint64_t *out)
{
    uint32_t v = ((uint32_t)N << 6) | (~(uint32_t)imms & 0x3F);
    if (v == 0) return 0;
    int len = 31 - __builtin_clz(v);
    if (len < 1) return 0;
    if (!sf && N) return 0;
    int esize = 1 << len;
    int levels = esize - 1;
    int S = imms & levels, R = immr & levels;
    if (S == levels) return 0;
    uint64_t emask = wd_mask(esize);
    uint64_t welem = wd_mask(S + 1);
    if (R) welem = ((welem >> R) | (welem << (esize - R))) & emask;
    uint64_t r = 0;
    for (int i = 0; i < 64; i += esize) r |= welem << i;
    if (!sf) r &= 0xFFFFFFFFULL;
    *out = r;
    return 1;
}

/* All 2^13 (N, immr, imms) combinations decoded once; bit 0 of valid64/valid32 per entry. */
static uint64_t bm_val[8192];
static uint8_t bm_ok[8192];       /* bit0: valid for sf=1, bit1: valid for sf=0 */
static int bm_ready;

static inline int bitmask_fast(int sf, uint32_t key, uint64_t *out)
{
    if (__builtin_expect(!bm_ready, 0)) {
        for (uint32_t k = 0; k < 8192; k++) {
            uint64_t v = 0, v32 = 0;
            int ok64 = wd_decode_bitmask(1, (int)(k >> 12), (int)(k & 63), (int)((k >> 6) & 63), &v);
            int ok32 = wd_decode_bitmask(0, (int)(k >> 12), (int)(k & 63), (int)((k >> 6) & 63), &v32);
            bm_val[k] = v;
            bm_ok[k] = (uint8_t)(ok64 | (ok32 << 1));
        }
        bm_ready = 1;
    }
    if (!(bm_ok[key] & (sf ? 1 : 2))) return 0;
    *out = sf ? bm_val[key] : bm_val[key] & 0xFFFFFFFFULL;
    return 1;
}

static uint64_t shift_reg(uint64_t v, int type, int amt, int sf)
{
    int size = sf ? 64 : 32;
    v = trunc_sf(sf, v);
    if (amt == 0) return v;
    switch (type) {
    case 0: return trunc_sf(sf, v << amt);
    case 1: return v >> amt;
    case 2: return trunc_sf(sf, sf ? (uint64_t)((int64_t)v >> amt) : (uint64_t)(uint32_t)((int32_t)v >> amt));
    default: return trunc_sf(sf, (v >> amt) | (v << (size - amt)));
    }
}

static uint64_t extend_reg(uint64_t v, int option, int shift)
{
    switch (option) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
    default: break;
    }
    return v << shift;
}

static uint64_t rbit64(uint64_t v)
{
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static uint32_t crc32_update(uint32_t crc, uint64_t data, int bytes, int castagnoli)
{
    uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        crc ^= (uint8_t)(data >> (8 * i));
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
    }
    return crc;
}

static void mul_128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    unsigned __int128 r = (unsigned __int128)a * b;
    *lo = (uint64_t)r;
    *hi = (uint64_t)(r >> 64);
}

static uint64_t smulh(uint64_t a, uint64_t b)
{
    __int128 r = (__int128)(int64_t)a * (__int128)(int64_t)b;
    return (uint64_t)((unsigned __int128)r >> 64);
}

/* ------------------------------------------------------------ data processing - immediate */

static void dp_imm(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    int sf = insn >> 31;
    uint32_t op = (insn >> 23) & 7;

    switch (op) {
    case 0: case 1: {                                   /* ADR / ADRP */
        uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
        imm = wd_sext(imm, 21);
        if (insn >> 31) setX(c, rd, (c->pc & ~0xFFFULL) + (imm << 12));
        else setX(c, rd, c->pc + imm);
        return;
    }
    case 2: {                                           /* ADD/SUB immediate */
        int opn = (insn >> 30) & 1, S = (insn >> 29) & 1;
        uint64_t imm = (insn >> 10) & 0xFFF;
        if ((insn >> 22) & 1) imm <<= 12;
        uint64_t a = XS(c, rn), r;
        if (opn) r = add_with_carry(c, sf, a, ~imm, 1, S);
        else r = add_with_carry(c, sf, a, imm, 0, S);
        r = trunc_sf(sf, r);
        if (S) setX(c, rd, r); else setXS(c, rd, r);
        return;
    }
    case 4: {                                           /* logical immediate */
        int opc = (insn >> 29) & 3;
        uint64_t imm;
        if (!bitmask_fast(sf, (insn >> 10) & 0x1FFF, &imm)) wd_undef(m);   /* N:immr:imms */
        uint64_t a = trunc_sf(sf, X(c, rn)), r;
        switch (opc) {
        case 0: r = a & imm; break;
        case 1: r = a | imm; break;
        case 2: r = a ^ imm; break;
        default: r = a & imm; set_nz(c, sf, r); setX(c, rd, r); return;
        }
        setXS(c, rd, trunc_sf(sf, r));
        return;
    }
    case 5: {                                           /* move wide */
        int opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        if (opc == 1 || (!sf && hw > 1)) wd_undef(m);
        uint64_t imm = ((insn >> 5) & 0xFFFFULL) << (hw * 16);
        uint64_t r;
        if (opc == 0) r = ~imm;
        else if (opc == 2) r = imm;
        else r = (X(c, rd) & ~(0xFFFFULL << (hw * 16))) | imm;
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    case 6: {                                           /* bitfield */
        int opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        int immr = (insn >> 16) & 63, imms = (insn >> 10) & 63;
        int size = sf ? 64 : 32;
        if (opc == 3 || N != sf || (!sf && (immr > 31 || imms > 31))) wd_undef(m);
        uint64_t src = trunc_sf(sf, X(c, rn)), r;
        if (opc == 1) {                                 /* BFM */
            uint64_t dst = trunc_sf(sf, X(c, rd));
            int w, pos;
            uint64_t field;
            if (imms >= immr) { w = imms - immr + 1; pos = 0; field = (src >> immr) & wd_mask(w); }
            else { w = imms + 1; pos = size - immr; field = src & wd_mask(w); }
            uint64_t fm = wd_mask(w) << pos;
            r = (dst & ~fm) | ((field << pos) & fm);
        } else {
            int w, pos;
            uint64_t field;
            if (imms >= immr) { w = imms - immr + 1; pos = 0; field = (src >> immr) & wd_mask(w); }
            else { w = imms + 1; pos = size - immr; field = src & wd_mask(w); }
            if (opc == 0) field = wd_sext(field, w);     /* SBFM */
            r = field << pos;
        }
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    case 7: {                                           /* EXTR */
        int N = (insn >> 22) & 1, imms = (insn >> 10) & 63, rm = (insn >> 16) & 31;
        if (((insn >> 29) & 3) || ((insn >> 21) & 1) || N != sf || (!sf && imms > 31)) wd_undef(m);
        uint64_t hi = trunc_sf(sf, X(c, rn)), lo = trunc_sf(sf, X(c, rm)), r;
        int size = sf ? 64 : 32;
        if (imms == 0) r = lo;
        else r = (lo >> imms) | (hi << (size - imms));
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    default:
        wd_undef(m);
    }
}

/* ------------------------------------------------------------ branches, exceptions, system */

static void do_eret(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    uint64_t spsr = c->spsr;
    uint32_t mode = spsr & 0x1F;
    c->excl_valid = 0;
    if (mode == 0) { c->el = 0; c->spsel = 0; }
    else if (mode == 4) { c->el = 1; c->spsel = 0; }
    else if (mode == 5) { c->el = 1; c->spsel = 1; }
    else {
        /* illegal exception return: stay at EL1 and report */
        wd_exception(m, EC_ILLEGAL, 0, 0, 0, c->elr);
    }
    c->nzcv = spsr & 0xF0000000u;
    c->daif = spsr & 0x3C0u;
    c->pan = (spsr >> 22) & 1;
    c->pc = c->elr;
    if (c->el == 0) c->pc &= ~3ULL;
    m->fetch_vpn = ~0ULL;
    m->irq_dirty = 1;
}

static void msr_pstate(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int op1 = (insn >> 16) & 7, op2 = (insn >> 5) & 7, crm = (insn >> 8) & 15;
    if (c->el == 0 && !(op1 == 3 && (op2 == 6 || op2 == 7) && (c->sctlr & (1u << 9))))
        wd_undef(m);
    if (op1 == 0 && op2 == 5) { c->spsel = crm & 1; return; }
    if (op1 == 3 && op2 == 6) { c->daif |= (crm & 15u) << 6; m->irq_dirty = 1; return; }
    if (op1 == 3 && op2 == 7) { c->daif &= ~((crm & 15u) << 6); m->irq_dirty = 1; return; }
    if (op1 == 0 && op2 == 4) { c->pan = crm & 1; wd_tlb_flush(m); return; }
    if (op1 == 0 && op2 == 3) return;                   /* UAO: accept, no effect */
    if (op1 == 3 && op2 == 1) return;                   /* SSBS */
    if (op1 == 3 && op2 == 2) return;                   /* DIT */
    wd_undef(m);
}

static void branch_sys(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    uint64_t pc = c->pc;

    if ((insn & 0x7C000000u) == 0x14000000u) {          /* B / BL */
        uint64_t off = wd_sext(insn & 0x3FFFFFF, 26) << 2;
        if (insn >> 31) c->x[30] = pc + 4;
        c->pc = pc + off;
        return;
    }
    if ((insn & 0x7E000000u) == 0x34000000u) {          /* CBZ / CBNZ */
        int sf = insn >> 31, nz = (insn >> 24) & 1;
        uint64_t v = trunc_sf(sf, X(c, insn & 31));
        if ((v != 0) == nz) c->pc = pc + (wd_sext((insn >> 5) & 0x7FFFF, 19) << 2);
        else c->pc = pc + 4;
        return;
    }
    if ((insn & 0x7E000000u) == 0x36000000u) {          /* TBZ / TBNZ */
        int bit = ((insn >> 31) << 5) | ((insn >> 19) & 31), nz = (insn >> 24) & 1;
        int set = (X(c, insn & 31) >> bit) & 1;
        if (set == nz) c->pc = pc + (wd_sext((insn >> 5) & 0x3FFF, 14) << 2);
        else c->pc = pc + 4;
        return;
    }
    if ((insn & 0xFF000010u) == 0x54000000u) {          /* B.cond */
        if (cond_holds(c->nzcv, insn & 15)) c->pc = pc + (wd_sext((insn >> 5) & 0x7FFFF, 19) << 2);
        else c->pc = pc + 4;
        return;
    }
    if ((insn & 0xFF000000u) == 0xD4000000u) {          /* exception generation */
        int opc = (insn >> 21) & 7, ll = insn & 3;
        uint32_t imm16 = (insn >> 5) & 0xFFFF;
        if ((insn >> 2) & 7) wd_undef(m);
        if (opc == 0 && ll == 1) wd_exception(m, EC_SVC64, imm16, 0, 0, pc + 4);
        if (opc == 0 && (ll == 2 || ll == 3)) {
            if (c->el == 0) wd_undef(m);
            /* PSCI / SMCCC is handled by the emulator (QEMU-style conduit) */
            wd_psci(m);
            c->pc = pc + 4;
            return;
        }
        if (opc == 1 && ll == 0) wd_exception(m, EC_BRK, imm16, 0, 0, pc);
        if (opc == 2 && ll == 0) {                      /* HLT: semihosting is not supported */
            wd_undef(m);
        }
        wd_undef(m);
    }
    if ((insn & 0xFFC00000u) == 0xD5000000u) {          /* system */
        int L = (insn >> 21) & 1, op0 = (insn >> 19) & 3;
        int crn = (insn >> 12) & 15, crm = (insn >> 8) & 15, op2 = (insn >> 5) & 7;
        int rt = insn & 31;
        if (op0 == 0 && !L && crn == 4) {
            if (rt != 31) wd_undef(m);
            msr_pstate(m, insn);
            c->pc = pc + 4;
            return;
        }
        if (op0 == 0 && !L && crn == 2) {               /* hints */
            int h = (crm << 3) | op2;
            if (h == 3) {                               /* WFI */
                if (c->el == 0 && !(c->sctlr & (1u << 16)))
                    wd_exception(m, EC_WFX, 1 << 24 | 0, 0, 0, pc);
                m->wfi = 1;
            } else if ((h == 1 || h == 2) && m->ncpu > 1) {   /* YIELD, WFE: let another CPU run */
                m->yield = 1;
                m->irq_dirty = 1;
            }
            c->pc = pc + 4;
            return;
        }
        if (op0 == 0 && !L && crn == 3) {               /* barriers, CLREX */
            if (op2 == 2) c->excl_valid = 0;
            c->pc = pc + 4;
            return;
        }
        if (op0 == 1) {                                 /* SYS / SYSL */
            if (L) wd_undef(m);
            wd_sys_insn(m, insn);
            c->pc = pc + 4;
            return;
        }
        if (op0 >= 2) {                                 /* MRS / MSR */
            uint32_t key = (insn >> 5) & 0x7FFF;
            if (L) {
                uint64_t v;
                if (!wd_sysreg_read(m, key, &v)) wd_undef(m);
                setX(c, rt, v);
            } else {
                if (!wd_sysreg_write(m, key, X(c, rt))) wd_undef(m);
            }
            c->pc = pc + 4;
            return;
        }
        wd_undef(m);
    }
    if ((insn & 0xFE000000u) == 0xD6000000u) {          /* branch to register */
        int opc = (insn >> 21) & 15, op2 = (insn >> 16) & 31, op3 = (insn >> 10) & 63, op4 = insn & 31;
        int rn = (insn >> 5) & 31;
        if (op2 != 31 || op3 != 0 || op4 != 0) wd_undef(m);
        uint64_t target = X(c, rn);
        switch (opc) {
        case 0: c->pc = target; return;                 /* BR */
        case 1: c->x[30] = pc + 4; c->pc = target; return; /* BLR */
        case 2: c->pc = target; return;                 /* RET */
        case 4:                                         /* ERET */
            if (rn != 31 || c->el == 0) wd_undef(m);
            do_eret(m);
            return;
        default: wd_undef(m);
        }
    }
    wd_undef(m);
}

/* ------------------------------------------------------------ loads and stores */

static uint64_t load_ext(uint64_t v, int size, int sign, int dst64)
{
    if (sign) {
        v = wd_sext(v, size * 8);
        if (!dst64) v = (uint32_t)v;
    }
    return v;
}

static void check_sp_align(wd_machine *m, int rn, uint64_t addr)
{
    WD_UNUSED(m); WD_UNUSED(rn); WD_UNUSED(addr);
    /* SCTLR_EL1.SA/SA0 stack alignment checking is not enforced. */
}

static void ldst_exclusive(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int size = insn >> 30, o2 = (insn >> 23) & 1, L = (insn >> 22) & 1, o1 = (insn >> 21) & 1;
    int rs = (insn >> 16) & 31, rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    int bytes = 1 << size;
    uint64_t addr = XS(c, rn);

    if (o2 && o1) {                                     /* CAS{A}{L}{B,H} */
        if (rt2 != 31) wd_undef(m);
        wd_check_align(m, addr, bytes, 1);
        uint64_t old = wd_read(m, addr, bytes, 0);
        uint64_t cmp = X(c, rs) & wd_mask(bytes * 8);
        if (old == cmp) wd_write(m, addr, bytes, X(c, rt), 0);
        else wd_translate(m, addr, ACC_WRITE, 0);       /* CAS needs write permission */
        setX(c, rs, old);
        c->excl_valid = 0;
        return;
    }
    if (!o2 && o1 && !(insn >> 31)) {                   /* CASP{A}{L} */
        int sf = (insn >> 30) & 1;
        if ((rs & 1) || (rt & 1) || rt2 != 31) wd_undef(m);
        int b = sf ? 8 : 4;
        wd_check_align(m, addr, 2 * b, 1);
        uint64_t o_lo = wd_read(m, addr, b, 0), o_hi = wd_read(m, addr + b, b, 0);
        uint64_t mask = wd_mask(b * 8);
        if (o_lo == (X(c, rs) & mask) && o_hi == (X(c, rs + 1) & mask)) {
            wd_write(m, addr, b, X(c, rt), 0);
            wd_write(m, addr + b, b, X(c, rt + 1), 0);
        }
        setX(c, rs, o_lo);
        setX(c, rs + 1, o_hi);
        c->excl_valid = 0;
        return;
    }
    if (!o2) {                                          /* exclusive (pair) */
        int pair = o1;
        int b = pair ? (4 << (size & 1)) : bytes;
        int total = pair ? 2 * b : b;
        wd_check_align(m, addr, total, !L);
        if (L) {
            uint64_t v1 = wd_read(m, addr, b, 0);
            uint64_t v2 = pair ? wd_read(m, addr + b, b, 0) : 0;
            c->excl_addr = addr;
            c->excl_valid = total;
            setX(c, rt, v1);
            if (pair) setX(c, rt2, v2);
        } else {
            int ok = c->excl_valid == total && c->excl_addr == addr;
            if (ok) {
                wd_write(m, addr, b, X(c, rt), 0);
                if (pair) wd_write(m, addr + b, b, X(c, rt2), 0);
            } else {
                wd_translate(m, addr, ACC_WRITE, 0);
            }
            c->excl_valid = 0;
            setX(c, rs, ok ? 0 : 1);
        }
        return;
    }
    /* o2 && !o1: load-acquire / store-release */
    wd_check_align(m, addr, bytes, !L);
    if (L) setX(c, rt, wd_read(m, addr, bytes, 0));
    else wd_write(m, addr, bytes, X(c, rt), 0);
}

static void ldst_atomic(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int size = insn >> 30, rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
    int o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
    int bytes = 1 << size, bits = bytes * 8;
    uint64_t addr = XS(c, rn);
    wd_check_align(m, addr, bytes, 1);

    if (o3 && opc == 4) {                               /* LDAPR */
        if (rs != 31) wd_undef(m);
        setX(c, rt, wd_read(m, addr, bytes, 0));
        return;
    }
    if (o3 && opc != 0) wd_undef(m);

    uint64_t mask = wd_mask(bits);
    uint64_t old = wd_read(m, addr, bytes, 0);
    uint64_t s = X(c, rs) & mask, r;
    if (o3) r = s;                                      /* SWP */
    else switch (opc) {
    case 0: r = old + s; break;
    case 1: r = old & ~s; break;
    case 2: r = old ^ s; break;
    case 3: r = old | s; break;
    case 4: r = (int64_t)wd_sext(old, bits) > (int64_t)wd_sext(s, bits) ? old : s; break;
    case 5: r = (int64_t)wd_sext(old, bits) < (int64_t)wd_sext(s, bits) ? old : s; break;
    case 6: r = old > s ? old : s; break;
    default: r = old < s ? old : s; break;
    }
    wd_write(m, addr, bytes, r & mask, 0);
    c->excl_valid = 0;
    setX(c, rt, old);
}

static void ldst(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int rt = insn & 31, rn = (insn >> 5) & 31;

    if ((insn >> 26) & 1) { wd_simd_ldst(m, insn); return; }

    /* exclusive / ordered / CAS: size 001000 ... */
    if ((insn & 0x3F000000u) == 0x08000000u) { ldst_exclusive(m, insn); return; }

    /* load literal: opc 011 V 00 */
    if ((insn & 0x3B000000u) == 0x18000000u) {
        if ((insn >> 26) & 1) wd_undef(m);              /* FP/SIMD */
        int opc = insn >> 30;
        uint64_t addr = c->pc + (wd_sext((insn >> 5) & 0x7FFFF, 19) << 2);
        if (opc == 3) return;                           /* PRFM literal */
        if (opc == 0) setX(c, rt, wd_read(m, addr, 4, 0));
        else if (opc == 1) setX(c, rt, wd_read(m, addr, 8, 0));
        else setX(c, rt, wd_sext(wd_read(m, addr, 4, 0), 32));
        return;
    }

    /* load/store pair: opc 101 V xx L imm7 Rt2 Rn Rt */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        if ((insn >> 26) & 1) wd_undef(m);
        int opc = insn >> 30, mode = (insn >> 23) & 3, L = (insn >> 22) & 1;
        int rt2 = (insn >> 10) & 31;
        if (opc == 3 || (opc == 1 && !L)) wd_undef(m);  /* STGP not supported */
        int sf = opc == 2, sign = opc == 1;
        int b = sf ? 8 : 4;
        int64_t off = (int64_t)wd_sext((insn >> 15) & 0x7F, 7) * b;
        uint64_t base = XS(c, rn);
        uint64_t addr = mode == 1 ? base : base + off;
        int wb = mode == 1 || mode == 3;
        if (rn == 31) check_sp_align(m, rn, base);
        if (L) {
            uint64_t v1 = wd_read(m, addr, b, 0);
            uint64_t v2 = wd_read(m, addr + b, b, 0);
            if (sign) { v1 = wd_sext(v1, 32); v2 = wd_sext(v2, 32); }
            if (wb) setXS(c, rn, base + off);
            setX(c, rt, v1);
            setX(c, rt2, v2);
        } else {
            uint64_t v1 = X(c, rt), v2 = X(c, rt2);
            wd_write(m, addr, b, v1, 0);
            wd_write(m, addr + b, b, v2, 0);
            if (wb) setXS(c, rn, base + off);
        }
        return;
    }

    /* load/store register: size 111 V xx opc ... */
    if ((insn & 0x3A000000u) == 0x38000000u) {
        if ((insn >> 26) & 1) wd_undef(m);
        int size = insn >> 30, opc = (insn >> 22) & 3;
        int bytes = 1 << size;
        uint64_t base = XS(c, rn), addr;
        int wb = 0, post = 0, unpriv = 0;
        int64_t off = 0;

        if ((insn >> 24) & 1) {                         /* unsigned offset */
            addr = base + (((insn >> 10) & 0xFFFULL) << size);
        } else if ((insn >> 21) & 1) {
            int k = (insn >> 10) & 3;
            if (k == 0) { ldst_atomic(m, insn); return; }
            if (k != 2) wd_undef(m);                    /* PAC loads */
            int rm = (insn >> 16) & 31, option = (insn >> 13) & 7, S = (insn >> 12) & 1;
            if (!(option & 2)) wd_undef(m);
            addr = base + extend_reg(X(c, rm), option, S ? size : 0);
        } else {
            int k = (insn >> 10) & 3;
            off = wd_sext((insn >> 12) & 0x1FF, 9);
            switch (k) {
            case 0: addr = base + off; break;                       /* unscaled */
            case 1: addr = base; wb = 1; post = 1; break;           /* post-index */
            case 2: addr = base + off; unpriv = c->el == 1; break;  /* LDTR/STTR */
            default: addr = base + off; wb = 1; break;              /* pre-index */
            }
        }
        WD_UNUSED(post);

        if (opc == 0) {                                 /* store */
            uint64_t v = X(c, rt);
            wd_write(m, addr, bytes, v, unpriv);
            if (wb) setXS(c, rn, base + off);
            return;
        }
        if (opc == 1) {                                 /* load, zero extend */
            uint64_t v = wd_read(m, addr, bytes, unpriv);
            if (wb) setXS(c, rn, base + off);
            setX(c, rt, v);
            return;
        }
        if (size == 3) {
            if (opc == 2) return;                       /* PRFM / PRFUM */
            wd_undef(m);
        }
        if (size == 2 && opc == 3) wd_undef(m);
        uint64_t v = wd_read(m, addr, bytes, unpriv);
        v = load_ext(v, bytes, 1, opc == 2);
        if (wb) setXS(c, rn, base + off);
        setX(c, rt, v);
        return;
    }
    wd_undef(m);
}

/* ------------------------------------------------------------ data processing - register */

static void dp_reg(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int sf = insn >> 31, rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    int op1 = (insn >> 28) & 1, op2 = (insn >> 21) & 15;

    if (!op1) {
        if (!(op2 & 8)) {                               /* logical (shifted register) */
            int opc = (insn >> 29) & 3, shift = (insn >> 22) & 3, N = (insn >> 21) & 1;
            int imm6 = (insn >> 10) & 63;
            if (!sf && imm6 > 31) wd_undef(m);
            uint64_t a = trunc_sf(sf, X(c, rn));
            uint64_t b = shift_reg(X(c, rm), shift, imm6, sf);
            if (N) b = trunc_sf(sf, ~b);
            uint64_t r;
            switch (opc) {
            case 0: r = a & b; break;
            case 1: r = a | b; break;
            case 2: r = a ^ b; break;
            default: r = a & b; set_nz(c, sf, r); break;
            }
            setX(c, rd, trunc_sf(sf, r));
            return;
        }
        int opn = (insn >> 30) & 1, S = (insn >> 29) & 1;
        if (!(op2 & 1)) {                               /* add/sub (shifted register) */
            int shift = (insn >> 22) & 3, imm6 = (insn >> 10) & 63;
            if (shift == 3 || (!sf && imm6 > 31)) wd_undef(m);
            uint64_t a = X(c, rn), b = shift_reg(X(c, rm), shift, imm6, sf), r;
            if (opn) r = add_with_carry(c, sf, a, ~b, 1, S);
            else r = add_with_carry(c, sf, a, b, 0, S);
            setX(c, rd, trunc_sf(sf, r));
            return;
        }
        /* add/sub (extended register) */
        int opt = (insn >> 22) & 3, option = (insn >> 13) & 7, imm3 = (insn >> 10) & 7;
        if (opt || imm3 > 4) wd_undef(m);
        uint64_t a = XS(c, rn), b = extend_reg(X(c, rm), option, imm3), r;
        if (opn) r = add_with_carry(c, sf, a, ~b, 1, S);
        else r = add_with_carry(c, sf, a, b, 0, S);
        r = trunc_sf(sf, r);
        if (S) setX(c, rd, r); else setXS(c, rd, r);
        return;
    }

    if (op2 & 8) {                                      /* data processing (3 source) */
        int op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
        int ra = (insn >> 10) & 31;
        uint64_t a = X(c, rn), b = X(c, rm), acc = X(c, ra), r, hi, lo;
        if (op54) wd_undef(m);
        switch (op31) {
        case 0: r = o0 ? acc - a * b : acc + a * b; r = trunc_sf(sf, r); break;
        case 1:
            if (!sf) wd_undef(m);
            r = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
            r = o0 ? acc - r : acc + r;
            break;
        case 2:
            if (!sf || o0) wd_undef(m);
            r = smulh(a, b);
            break;
        case 5:
            if (!sf) wd_undef(m);
            r = (uint64_t)(uint32_t)a * (uint32_t)b;
            r = o0 ? acc - r : acc + r;
            break;
        case 6:
            if (!sf || o0) wd_undef(m);
            mul_128(a, b, &hi, &lo);
            r = hi;
            break;
        default: wd_undef(m);
        }
        setX(c, rd, r);
        return;
    }

    switch (op2) {
    case 0: {                                           /* ADC/SBC */
        if ((insn >> 10) & 63) wd_undef(m);
        int opn = (insn >> 30) & 1, S = (insn >> 29) & 1;
        uint64_t b = X(c, rm);
        if (opn) b = ~b;
        uint64_t r = add_with_carry(c, sf, X(c, rn), b, (c->nzcv >> 29) & 1, S);
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    case 2: {                                           /* CCMN / CCMP */
        if (!((insn >> 29) & 1) || ((insn >> 10) & 1) || ((insn >> 4) & 1)) wd_undef(m);
        int opn = (insn >> 30) & 1, cond = (insn >> 12) & 15, nzcv = insn & 15;
        uint64_t b = ((insn >> 11) & 1) ? (uint64_t)rm : X(c, rm);
        if (cond_holds(c->nzcv, cond)) {
            if (opn) add_with_carry(c, sf, X(c, rn), ~b, 1, 1);
            else add_with_carry(c, sf, X(c, rn), b, 0, 1);
        } else {
            c->nzcv = (uint32_t)nzcv << 28;
        }
        return;
    }
    case 4: {                                           /* conditional select */
        if ((insn >> 29) & 1 || ((insn >> 11) & 1)) wd_undef(m);
        int opn = (insn >> 30) & 1, o2 = (insn >> 10) & 1, cond = (insn >> 12) & 15;
        uint64_t r;
        if (cond_holds(c->nzcv, cond)) r = X(c, rn);
        else {
            r = X(c, rm);
            if (opn) r = ~r;
            if (o2) r += 1;
        }
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    case 6: {
        int opcode = (insn >> 10) & 63;
        if ((insn >> 29) & 1) wd_undef(m);
        if ((insn >> 30) & 1) {                         /* 1 source */
            if (rm != 0) wd_undef(m);
            uint64_t a = X(c, rn), r;
            switch (opcode) {
            case 0: r = sf ? rbit64(a) : rbit64(a) >> 32; break;
            case 1: {                                   /* REV16 */
                r = ((a & 0x00FF00FF00FF00FFULL) << 8) | ((a >> 8) & 0x00FF00FF00FF00FFULL);
                break;
            }
            case 2:                                     /* REV (32) / REV32 */
                if (sf) {
                    uint64_t lo = __builtin_bswap32((uint32_t)a), hi = __builtin_bswap32((uint32_t)(a >> 32));
                    r = lo | (hi << 32);
                } else r = __builtin_bswap32((uint32_t)a);
                break;
            case 3:
                if (!sf) wd_undef(m);
                r = __builtin_bswap64(a);
                break;
            case 4:                                     /* CLZ */
                if (sf) r = a ? (uint64_t)__builtin_clzll(a) : 64;
                else r = (uint32_t)a ? (uint64_t)__builtin_clz((uint32_t)a) : 32;
                break;
            case 5: {                                   /* CLS */
                int size = sf ? 64 : 32;
                uint64_t v = trunc_sf(sf, a);
                uint64_t t = ((v >> 1) ^ v) & wd_mask(size - 1);
                int n = 0;
                for (int i = size - 2; i >= 0 && !((t >> i) & 1); i--) n++;
                r = (uint64_t)n;
                break;
            }
            default: wd_undef(m);
            }
            setX(c, rd, trunc_sf(sf, r));
            return;
        }
        /* 2 source */
        uint64_t a = X(c, rn), b = X(c, rm), r;
        int size = sf ? 64 : 32;
        switch (opcode) {
        case 2:                                         /* UDIV */
            a = trunc_sf(sf, a); b = trunc_sf(sf, b);
            r = b ? a / b : 0;
            break;
        case 3:                                         /* SDIV */
            if (sf) {
                int64_t x = (int64_t)a, y = (int64_t)b;
                if (y == 0) r = 0;
                else if (x == INT64_MIN && y == -1) r = (uint64_t)INT64_MIN;
                else r = (uint64_t)(x / y);
            } else {
                int32_t x = (int32_t)a, y = (int32_t)b;
                if (y == 0) r = 0;
                else if (x == INT32_MIN && y == -1) r = (uint32_t)INT32_MIN;
                else r = (uint32_t)(x / y);
            }
            break;
        case 8: r = shift_reg(a, 0, (int)(b & (size - 1)), sf); break;
        case 9: r = shift_reg(a, 1, (int)(b & (size - 1)), sf); break;
        case 10: r = shift_reg(a, 2, (int)(b & (size - 1)), sf); break;
        case 11: r = shift_reg(a, 3, (int)(b & (size - 1)), sf); break;
        case 16: case 17: case 18: case 19:
        case 20: case 21: case 22: case 23: {           /* CRC32 / CRC32C */
            int sz = opcode & 3, cc = (opcode >> 2) & 1;
            if ((sz == 3) != sf) wd_undef(m);
            r = crc32_update((uint32_t)a, b, 1 << sz, cc);
            sf = 0;
            break;
        }
        default: wd_undef(m);
        }
        setX(c, rd, trunc_sf(sf, r));
        return;
    }
    default:
        wd_undef(m);
    }
}

/* ------------------------------------------------------------ dispatcher */

/* ---------------------------------------------------------------- fast paths
 * The most frequent integer forms executed without the general decoder. Every
 * case falls back to the general path (return 0) for anything unusual; the
 * results are identical (checked by the random ALU/memory tests). */

static inline uint32_t flags_add64(uint64_t a, uint64_t b, uint64_t r)
{
    return (uint32_t)((r >> 63) << 31) | (uint32_t)(r == 0) << 30 | (uint32_t)(r < a) << 29 |
           (uint32_t)(((~(a ^ b) & (a ^ r)) >> 63) << 28);
}
static inline uint32_t flags_sub64(uint64_t a, uint64_t b, uint64_t r)
{
    return (uint32_t)((r >> 63) << 31) | (uint32_t)(r == 0) << 30 | (uint32_t)(a >= b) << 29 |
           (uint32_t)((((a ^ b) & (a ^ r)) >> 63) << 28);
}
static inline uint32_t flags_add32(uint32_t a, uint32_t b, uint32_t r)
{
    return (r >> 31) << 31 | (uint32_t)(r == 0) << 30 | (uint32_t)(r < a) << 29 | ((~(a ^ b) & (a ^ r)) >> 31) << 28;
}
static inline uint32_t flags_sub32(uint32_t a, uint32_t b, uint32_t r)
{
    return (r >> 31) << 31 | (uint32_t)(r == 0) << 30 | (uint32_t)(a >= b) << 29 | (((a ^ b) & (a ^ r)) >> 31) << 28;
}

static inline uint64_t fast_shift(uint64_t v, int type, int amt, int sf)
{
    if (!amt) return v;
    if (sf) {
        switch (type) {
        case 0: return v << amt;
        case 1: return v >> amt;
        case 2: return (uint64_t)((int64_t)v >> amt);
        default: return (v >> amt) | (v << (64 - amt));
        }
    }
    uint32_t w = (uint32_t)v;
    switch (type) {
    case 0: return (uint32_t)(w << amt);
    case 1: return w >> amt;
    case 2: return (uint32_t)((int32_t)w >> amt);
    default: return (uint32_t)((w >> amt) | (w << (32 - amt)));
    }
}

static inline int fast_exec(wd_machine *m, wd_cpu *c, uint32_t insn, uint64_t pc)
{
    int rd = insn & 31, rn = (insn >> 5) & 31;
    int sf = insn >> 31;

    if ((insn & 0x1F000000u) == 0x0A000000u) {               /* logical (shifted register) */
        int shift = (insn >> 22) & 3, amt = (insn >> 10) & 63, opc = (insn >> 29) & 3;
        if (!sf && amt > 31) return 0;
        uint64_t a = rn == 31 ? 0 : c->x[rn];
        int rm = (insn >> 16) & 31;
        uint64_t b = fast_shift(rm == 31 ? 0 : c->x[rm], shift, amt, sf);
        if (!sf) { a = (uint32_t)a; b = (uint32_t)b; }
        if ((insn >> 21) & 1) b = sf ? ~b : (uint32_t)~b;
        uint64_t r;
        switch (opc) {
        case 0: r = a & b; break;
        case 1: r = a | b; break;
        case 2: r = a ^ b; break;
        default:
            r = a & b;
            c->nzcv = sf ? (uint32_t)((r >> 63) << 31) | (uint32_t)(r == 0) << 30
                         : (uint32_t)(((uint32_t)r >> 31) << 31) | (uint32_t)((uint32_t)r == 0) << 30;
            break;
        }
        if (rd != 31) c->x[rd] = r;
        c->pc = pc + 4;
        return 1;
    }
    if ((insn & 0x1F200000u) == 0x0B000000u) {               /* add/sub (shifted register) */
        int shift = (insn >> 22) & 3, amt = (insn >> 10) & 63;
        if (shift == 3 || (!sf && amt > 31)) return 0;
        int sub = (insn >> 30) & 1, S = (insn >> 29) & 1, rm = (insn >> 16) & 31;
        uint64_t a = rn == 31 ? 0 : c->x[rn];
        uint64_t b = fast_shift(rm == 31 ? 0 : c->x[rm], shift, amt, sf);
        uint64_t r;
        if (sf) {
            r = sub ? a - b : a + b;
            if (S) c->nzcv = sub ? flags_sub64(a, b, r) : flags_add64(a, b, r);
        } else {
            uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b, r32 = sub ? a32 - b32 : a32 + b32;
            if (S) c->nzcv = sub ? flags_sub32(a32, b32, r32) : flags_add32(a32, b32, r32);
            r = r32;
        }
        if (rd != 31) c->x[rd] = r;
        c->pc = pc + 4;
        return 1;
    }
    if ((insn & 0x1F000000u) == 0x11000000u) {               /* add/sub (immediate) */
        if ((insn >> 23) & 1) return 0;                      /* tag variants */
        int sub = (insn >> 30) & 1, S = (insn >> 29) & 1;
        uint64_t imm = (insn >> 10) & 0xFFF;
        if ((insn >> 22) & 1) imm <<= 12;
        uint64_t a = rn == 31 ? *wd_sp_ptr(c) : c->x[rn];
        uint64_t r;
        if (sf) {
            r = sub ? a - imm : a + imm;
            if (S) c->nzcv = sub ? flags_sub64(a, imm, r) : flags_add64(a, imm, r);
        } else {
            uint32_t a32 = (uint32_t)a, b32 = (uint32_t)imm, r32 = sub ? a32 - b32 : a32 + b32;
            if (S) c->nzcv = sub ? flags_sub32(a32, b32, r32) : flags_add32(a32, b32, r32);
            r = r32;
        }
        if (rd == 31) { if (!S) *wd_sp_ptr(c) = r; }
        else c->x[rd] = r;
        c->pc = pc + 4;
        return 1;
    }
    if ((insn & 0x7C000000u) == 0x14000000u) {               /* B, BL */
        int64_t off = (int64_t)((uint64_t)(insn & 0x03FFFFFFu) << 38) >> 36;
        if (insn >> 31) c->x[30] = pc + 4;
        c->pc = pc + (uint64_t)off;
        return 1;
    }
    if ((insn & 0xFF000010u) == 0x54000000u) {               /* B.cond */
        int64_t off = (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        c->pc = cond_holds(c->nzcv, insn & 15) ? pc + (uint64_t)off : pc + 4;
        return 1;
    }
    if ((insn & 0x7E000000u) == 0x34000000u) {               /* CBZ, CBNZ */
        int64_t off = (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        uint64_t v = rd == 31 ? 0 : c->x[rd];
        if (!sf) v = (uint32_t)v;
        int take = ((insn >> 24) & 1) ? v != 0 : v == 0;
        c->pc = take ? pc + (uint64_t)off : pc + 4;
        return 1;
    }
    if ((insn & 0x7E000000u) == 0x36000000u) {               /* TBZ, TBNZ */
        int64_t off = (int64_t)((uint64_t)((insn >> 5) & 0x3FFF) << 50) >> 48;
        int bit = (int)((insn >> 31) << 5 | ((insn >> 19) & 31));
        uint64_t v = rd == 31 ? 0 : c->x[rd];
        int set = (v >> bit) & 1;
        int take = ((insn >> 24) & 1) ? set : !set;
        c->pc = take ? pc + (uint64_t)off : pc + 4;
        return 1;
    }
    if ((insn & 0xFFFFFC1Fu) == 0xD65F0000u) {               /* RET */
        c->pc = rn == 31 ? 0 : c->x[rn];
        return 1;
    }
    if ((insn & 0x1FE00800u) == 0x1A800000u && !((insn >> 29) & 1)) {   /* CSEL, CSINC, CSINV, CSNEG */
        int rm = (insn >> 16) & 31, op = (insn >> 30) & 1, o2 = (insn >> 10) & 1;
        uint64_t r;
        if (cond_holds(c->nzcv, (insn >> 12) & 15)) r = rn == 31 ? 0 : c->x[rn];
        else {
            r = rm == 31 ? 0 : c->x[rm];
            if (op) r = ~r;
            if (o2) r = r + 1;
        }
        if (!sf) r = (uint32_t)r;
        if (rd != 31) c->x[rd] = r;
        c->pc = pc + 4;
        return 1;
    }
    if ((insn & 0x3F000000u) == 0x39000000u) {               /* LDR/STR (unsigned offset), integer */
        int size = insn >> 30, opc = (insn >> 22) & 3;
        if (opc > 1) return 0;
        uint64_t addr = (rn == 31 ? *wd_sp_ptr(c) : c->x[rn]) + (((insn >> 10) & 0xFFFULL) << size);
        int bytes = 1 << size;
        if (opc == 0) wd_write(m, addr, bytes, rd == 31 ? 0 : c->x[rd], 0);
        else {
            uint64_t v = wd_read(m, addr, bytes, 0);
            if (rd != 31) c->x[rd] = v;
        }
        c->pc = pc + 4;
        return 1;
    }
    if ((insn & 0x1F800000u) == 0x12800000u) {               /* MOVN, MOVZ, MOVK */
        int opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        if (opc == 1 || (!sf && hw > 1)) return 0;
        uint64_t imm = (uint64_t)((insn >> 5) & 0xFFFF) << (16 * hw), r;
        if (opc == 0) r = ~imm;
        else if (opc == 2) r = imm;
        else r = ((rd == 31 ? 0 : c->x[rd]) & ~(0xFFFFULL << (16 * hw))) | imm;
        if (!sf) r = (uint32_t)r;
        if (rd != 31) c->x[rd] = r;
        c->pc = pc + 4;
        return 1;
    }
    return 0;
}

/* General decoder for one instruction (pc already checked and fetched). */
static __attribute__((noinline)) void step_general(wd_machine *m, uint32_t insn, uint64_t pc)
{
    wd_cpu *c = &m->cpu;
    uint32_t op0 = (insn >> 25) & 15;
    switch (op0) {
    case 8: case 9:
        dp_imm(m, insn);
        c->pc = pc + 4;
        break;
    case 10: case 11:
        branch_sys(m, insn);
        break;
    case 4: case 6: case 12: case 14:
        ldst(m, insn);
        c->pc = pc + 4;
        break;
    case 5: case 13:
        dp_reg(m, insn);
        c->pc = pc + 4;
        break;
    case 7: case 15:                                    /* SIMD & FP data processing */
        wd_simd_dp(m, insn);
        c->pc = pc + 4;
        break;
    default:
        wd_undef(m);
    }
}

static inline uint32_t fetch_inline(wd_machine *m, uint64_t pc)
{
    if (__builtin_expect((pc >> 12) == m->fetch_vpn && m->fetch_el == m->cpu.el, 1)) {
        uint32_t insn;
        memcpy(&insn, m->fetch_host + (pc & 0xFFF), 4);
        return insn;
    }
    return wd_fetch(m, pc);
}

void wd_cpu_step(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    uint64_t pc = c->pc;
    if (pc & 3) wd_exception(m, EC_PC_ALIGN, 0, pc, 1, pc);
    uint32_t insn = fetch_inline(m, pc);
    if (m->trace) {
        fprintf(stderr, "[%llu] EL%d pc=%016llx insn=%08x\n", (unsigned long long)c->icount,
                c->el, (unsigned long long)pc, insn);
    }
    c->icount++;
    if (!m->no_fast && fast_exec(m, c, insn, pc)) return;
    step_general(m, insn, pc);
}

/* Run up to n instructions; returns early when the run loop has something to do
 * (stop request, interrupt state change, WFI). Exceptions longjmp out as usual. */
void wd_state_trace(wd_machine *m)
{
    const wd_cpu *c = &m->cpu;
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 31; i++) h = (h ^ c->x[i]) * 1099511628211ULL;
    h = (h ^ c->sp_el0) * 1099511628211ULL;
    h = (h ^ c->sp_el1) * 1099511628211ULL;
    h = (h ^ c->nzcv) * 1099511628211ULL;
    fprintf(stderr, "%llu %llx %016llx\n", (unsigned long long)c->icount, (unsigned long long)c->pc, (unsigned long long)h);
}

void wd_cpu_run(wd_machine *m, uint64_t n)
{
    wd_cpu *c = &m->cpu;
    if (m->trace == 1 || m->no_fast) {
        while (n--) {
            wd_cpu_step(m);
            m->budget--;
            if (m->stop >= 0 || m->irq_dirty || m->wfi) return;
        }
        return;
    }
    while (n) {
        if (m->use_jit) {
            uint64_t k = wd_jit_run(m, n);
            n -= k;
            (void)k;
            if (m->stop >= 0 || m->irq_dirty || m->wfi) return;
            if (!n) return;
        }
        n--;
        uint64_t pc = c->pc;
        if (__builtin_expect(pc & 3, 0)) wd_exception(m, EC_PC_ALIGN, 0, pc, 1, pc);
        uint32_t insn = fetch_inline(m, pc);
        c->icount++;
        if (!fast_exec(m, c, insn, pc)) step_general(m, insn, pc);
        m->budget--;
        if (m->trace == 2) wd_state_trace(m);
        if (__builtin_expect(m->stop >= 0 || m->irq_dirty || m->wfi, 0)) return;
    }
}
