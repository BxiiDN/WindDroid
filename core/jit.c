/*
 * WindDroid - AArch64 -> AArch64 block translator ("JIT"), used when the host is
 * itself AArch64 (Android tablets, ARM64 servers).
 *
 * Guest and host share the instruction set, so most integer data-processing
 * instructions are copied with their register fields rewritten to host scratch
 * registers: the host computes exactly what the guest instruction defines,
 * including NZCV. Guest registers stay in wd_cpu (memory); every translated
 * instruction loads its operands and stores its result, so the guest state is
 * complete at any point where a helper can raise a guest exception. Loads and
 * stores call the interpreter's memory functions. Branches end a block; any
 * instruction the translator does not handle ends the block before it and is
 * left to the interpreter.
 *
 * Blocks never cross a 4 KiB page and are looked up by guest PC, host address
 * of the code and translation regime (EL, SPSel). Before a block runs, its
 * guest instruction words are compared with memory, so self-modifying code and
 * remapped pages are always seen.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "internal.h"

int wd_decode_bitmask(int sf, int N, int imms, int immr, uint64_t *out);

#if defined(__aarch64__) && !defined(WD_NO_JIT)

#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#define CACHE_SIZE  (32u << 20)          /* host code */
#define NBUCKET     65536
#define MAX_INSNS   48

typedef void (*block_fn)(wd_machine *m, wd_cpu *c);

typedef struct block {
    uint64_t pc;
    const uint8_t *host;                 /* guest code in host memory */
    uint32_t mode;                       /* el | spsel << 1 */
    uint32_t n;                          /* guest instructions */
    uint32_t words[MAX_INSNS];
    block_fn fn;                         /* NULL: first instruction is not translatable */
    struct block *next;
} block;

typedef struct {
    uint32_t *code;                      /* RX view */
    uint32_t *wcode;                     /* RW view (same pages) */
    size_t used, cap;                    /* in 32-bit words */
    block *bucket[NBUCKET];
    block *pool;
    size_t npool, cappool;
    int ok;
} jit_state;

/* ---------------------------------------------------------------- helpers called from code */

static uint64_t h_load(wd_machine *m, uint64_t va, uint64_t size) { return wd_read(m, va, (int)size, 0); }
static void h_store(wd_machine *m, uint64_t va, uint64_t size, uint64_t v) { wd_write(m, va, (int)size, v, 0); }

/* ---------------------------------------------------------------- emitter */

enum { HC = 19, HM = 20 };               /* x19 = cpu, x20 = machine */
enum { T0 = 9, T1 = 10, T2 = 11, T3 = 12, T4 = 13 };   /* scratch */

typedef struct {
    jit_state *j;
    uint32_t *p;                          /* write pointer (RW view) */
    uint32_t *end;
    int overflow;
} emitter;

static void emit(emitter *e, uint32_t w)
{
    if (e->p >= e->end) { e->overflow = 1; return; }
    *e->p++ = w;
}

#define OFF_X(r)    ((uint32_t)offsetof(wd_cpu, x) + 8u * (uint32_t)(r))
#define OFF_SP0     ((uint32_t)offsetof(wd_cpu, sp_el0))
#define OFF_SP1     ((uint32_t)offsetof(wd_cpu, sp_el1))
#define OFF_PC      ((uint32_t)offsetof(wd_cpu, pc))
#define OFF_NZCV    ((uint32_t)offsetof(wd_cpu, nzcv))
#define OFF_ICOUNT  ((uint32_t)offsetof(wd_cpu, icount))

static void ldr_x(emitter *e, int t, int n, uint32_t off) { emit(e, 0xF9400000u | (off / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void str_x(emitter *e, int t, int n, uint32_t off) { emit(e, 0xF9000000u | (off / 8) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void ldr_w(emitter *e, int t, int n, uint32_t off) { emit(e, 0xB9400000u | (off / 4) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void str_w(emitter *e, int t, int n, uint32_t off) { emit(e, 0xB9000000u | (off / 4) << 10 | (uint32_t)n << 5 | (uint32_t)t); }
static void mov_reg(emitter *e, int d, int s) { emit(e, 0xAA0003E0u | (uint32_t)s << 16 | (uint32_t)d); }   /* orr xd, xzr, xs */

static void mov_imm(emitter *e, int d, uint64_t v)
{
    emit(e, 0xD2800000u | (uint32_t)(v & 0xFFFF) << 5 | (uint32_t)d);                /* movz */
    for (int hw = 1; hw < 4; hw++) {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        if (part) emit(e, 0xF2800000u | (uint32_t)hw << 21 | part << 5 | (uint32_t)d);   /* movk */
    }
}

static void load_nzcv(emitter *e)  { ldr_w(e, T4, HC, OFF_NZCV); emit(e, 0xD51B4200u | T4); }   /* msr nzcv, x13 */
static void store_nzcv(emitter *e) { emit(e, 0xD53B4200u | T4); str_w(e, T4, HC, OFF_NZCV); }  /* mrs x13, nzcv */

static void call(emitter *e, const void *fn)
{
    mov_imm(e, 16, (uint64_t)(uintptr_t)fn);
    emit(e, 0xD63F0200u);                                                         /* blr x16 */
}

static void set_pc(emitter *e, uint64_t pc) { mov_imm(e, T0, pc); str_x(e, T0, HC, OFF_PC); }

static void prologue(emitter *e)
{
    emit(e, 0xA9BE7BFDu);              /* stp x29, x30, [sp, #-32]! */
    emit(e, 0xA90153F3u);              /* stp x19, x20, [sp, #16] */
    mov_reg(e, HM, 0);
    mov_reg(e, HC, 1);
}

static void epilogue(emitter *e)
{
    emit(e, 0xA94153F3u);              /* ldp x19, x20, [sp, #16] */
    emit(e, 0xA8C27BFDu);              /* ldp x29, x30, [sp], #32 */
    emit(e, 0xD65F03C0u);              /* ret */
}

/* ---------------------------------------------------------------- register mapping */

typedef struct {
    emitter *e;
    uint32_t sp_off;
    int guest[5], host[5], n;
} regmap;

/* host register holding guest register r for reading: ZR context -> xzr, SP context -> SP value */
static int src(regmap *rm, int r, int sp_ctx)
{
    if (r == 31 && !sp_ctx) return 31;
    int key = r == 31 ? 32 : r;
    for (int i = 0; i < rm->n; i++) if (rm->guest[i] == key) return rm->host[i];
    int h = T0 + rm->n;
    rm->guest[rm->n] = key;
    rm->host[rm->n++] = h;
    if (key == 32) ldr_x(rm->e, h, HC, rm->sp_off);
    else ldr_x(rm->e, h, HC, OFF_X(r));
    return h;
}

/* host register to write guest register r into (stored afterwards by wb) */
static int dst(regmap *rm, int r, int sp_ctx)
{
    if (r == 31 && !sp_ctx) return 31;
    int key = r == 31 ? 32 : r;
    for (int i = 0; i < rm->n; i++) if (rm->guest[i] == key) return rm->host[i];
    int h = T0 + rm->n;
    rm->guest[rm->n] = key;
    rm->host[rm->n++] = h;
    return h;
}

static void wb(regmap *rm, int r, int sp_ctx, int h)
{
    if (h == 31) return;
    if (r == 31) { if (sp_ctx) str_x(rm->e, h, HC, rm->sp_off); return; }
    str_x(rm->e, h, HC, OFF_X(r));
}

/* ---------------------------------------------------------------- data processing */

/* Copy a data-processing instruction with rewritten registers. Fields: d = Rd, n = Rn,
 * m = Rm (-1 unused), a = Ra (-1 unused); *_sp: register 31 means SP. */
static void dp_copy(emitter *e, uint32_t sp_off, uint32_t insn, int has_n, int has_m, int has_a,
                    int d_sp, int n_sp, int reads_flags, int writes_flags, int writes_d)
{
    regmap rm = { e, sp_off, { 0 }, { 0 }, 0 };
    int rd = insn & 31, rn = (insn >> 5) & 31, rmr = (insn >> 16) & 31, ra = (insn >> 10) & 31;
    int hn = has_n ? src(&rm, rn, n_sp) : 0;
    int hm = has_m ? src(&rm, rmr, 0) : 0;
    int ha = has_a ? src(&rm, ra, 0) : 0;
    int hd = writes_d ? dst(&rm, rd, d_sp) : 31;
    uint32_t w = insn;
    if (writes_d) w = (w & ~31u) | (uint32_t)hd;
    if (has_n) w = (w & ~(31u << 5)) | (uint32_t)hn << 5;
    if (has_m) w = (w & ~(31u << 16)) | (uint32_t)hm << 16;
    if (has_a) w = (w & ~(31u << 10)) | (uint32_t)ha << 10;
    if (reads_flags) load_nzcv(e);
    emit(e, w);
    if (writes_flags) store_nzcv(e);
    if (writes_d) wb(&rm, rd, d_sp, hd);
}

/* returns 1 if translated */
static int tr_dp(emitter *e, uint32_t sp_off, uint32_t insn, uint64_t pc)
{
    int sf = insn >> 31;
    /* PC-relative addressing: the address is a translation-time constant */
    if ((insn & 0x1F000000u) == 0x10000000u) {
        int rd = insn & 31;
        int64_t imm = (int64_t)(((uint64_t)((insn >> 5) & 0x7FFFF) << 2 | ((insn >> 29) & 3)) << 43) >> 43;
        uint64_t v = (insn >> 31) ? (pc & ~0xFFFULL) + (uint64_t)(imm << 12) : pc + (uint64_t)imm;
        if (rd != 31) { mov_imm(e, T0, v); str_x(e, T0, HC, OFF_X(rd)); }
        return 1;
    }
    if ((insn & 0x1F000000u) == 0x11000000u) {                 /* add/sub immediate */
        if ((insn >> 23) & 1) return 0;
        int S = (insn >> 29) & 1;
        dp_copy(e, sp_off, insn, 1, 0, 0, !S, 1, 0, S, 1);
        return 1;
    }
    if ((insn & 0x1F800000u) == 0x12000000u) {                 /* logical immediate */
        int opc = (insn >> 29) & 3;
        uint64_t imm;
        if (!wd_decode_bitmask(sf, (insn >> 22) & 1, (insn >> 10) & 63, (insn >> 16) & 63, &imm)) return 0;
        dp_copy(e, sp_off, insn, 1, 0, 0, opc != 3, 0, 0, opc == 3, 1);
        return 1;
    }
    if ((insn & 0x1F800000u) == 0x12800000u) {                 /* move wide */
        int opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        if (opc == 1 || (!sf && hw > 1)) return 0;
        /* MOVK reads Rd: use the n slot trick by loading Rd first */
        regmap rm = { e, sp_off, { 0 }, { 0 }, 0 };
        int rd = insn & 31;
        int h = opc == 3 ? src(&rm, rd, 0) : dst(&rm, rd, 0);
        if (h == 31) { if (rd == 31) return 1; }
        emit(e, (insn & ~31u) | (uint32_t)h);
        wb(&rm, rd, 0, h);
        return 1;
    }
    if ((insn & 0x1F800000u) == 0x13000000u) {                 /* bitfield (SBFM, BFM, UBFM) */
        int opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        if (opc == 3 || N != sf) return 0;
        if (!sf && (((insn >> 21) & 1) || ((insn >> 15) & 1))) return 0;   /* immr/imms >= 32 */
        if (opc == 1) {                                         /* BFM reads Rd too */
            regmap rm = { e, sp_off, { 0 }, { 0 }, 0 };
            int rd = insn & 31, rn = (insn >> 5) & 31;
            int hd = src(&rm, rd, 0);
            int hn = src(&rm, rn, 0);
            if (hd == 31) return 1;
            emit(e, (insn & ~0x3FFu) | (uint32_t)hn << 5 | (uint32_t)hd);
            wb(&rm, rd, 0, hd);
            return 1;
        }
        dp_copy(e, sp_off, insn, 1, 0, 0, 0, 0, 0, 0, 1);
        return 1;
    }
    if ((insn & 0x1F800000u) == 0x13800000u) {                 /* EXTR */
        if (((insn >> 29) & 3) || ((insn >> 22) & 1) != sf || ((insn >> 21) & 1)) return 0;
        if (!sf && ((insn >> 15) & 1)) return 0;
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 0, 0, 1);
        return 1;
    }
    if ((insn & 0x1F000000u) == 0x0A000000u) {                 /* logical shifted register */
        int opc = (insn >> 29) & 3;
        if (!sf && ((insn >> 15) & 1)) return 0;
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 0, opc == 3, 1);
        return 1;
    }
    if ((insn & 0x1F200000u) == 0x0B000000u) {                 /* add/sub shifted register */
        if (((insn >> 22) & 3) == 3 || (!sf && ((insn >> 15) & 1))) return 0;
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 0, (insn >> 29) & 1, 1);
        return 1;
    }
    if ((insn & 0x1F200000u) == 0x0B200000u) {                 /* add/sub extended register */
        if (((insn >> 22) & 3) || ((insn >> 10) & 7) > 4) return 0;
        int S = (insn >> 29) & 1;
        dp_copy(e, sp_off, insn, 1, 1, 0, !S, 1, 0, S, 1);
        return 1;
    }
    if ((insn & 0x1FE0FC00u) == 0x1A000000u) {                 /* ADC, ADCS, SBC, SBCS */
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 1, (insn >> 29) & 1, 1);
        return 1;
    }
    if ((insn & 0x1FE00410u) == 0x1A400000u && ((insn >> 29) & 1)) {   /* CCMN, CCMP (register/imm) */
        int imm = (insn >> 11) & 1;
        dp_copy(e, sp_off, insn, 1, !imm, 0, 0, 0, 1, 1, 0);
        return 1;
    }
    if ((insn & 0x1FE00800u) == 0x1A800000u && !((insn >> 29) & 1)) {  /* conditional select */
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 1, 0, 1);
        return 1;
    }
    if ((insn & 0x5FE00000u) == 0x5AC00000u && !((insn >> 29) & 1)) {  /* 1 source: RBIT, REV*, CLZ, CLS */
        int op2 = (insn >> 16) & 31, op = (insn >> 10) & 63;
        if (op2 || op > 5 || (!sf && op == 3)) return 0;
        dp_copy(e, sp_off, insn, 1, 0, 0, 0, 0, 0, 0, 1);
        return 1;
    }
    if ((insn & 0x5FE00000u) == 0x1AC00000u && !((insn >> 29) & 1)) {  /* 2 source: DIV, shifts by register */
        int op = (insn >> 10) & 63;
        if (op != 2 && op != 3 && (op < 8 || op > 11)) return 0;
        dp_copy(e, sp_off, insn, 1, 1, 0, 0, 0, 0, 0, 1);
        return 1;
    }
    if ((insn & 0x1F000000u) == 0x1B000000u) {                 /* 3 source */
        int op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
        if (op54) return 0;
        int ok = op31 == 0 || ((op31 == 1 || op31 == 5) && sf) || ((op31 == 2 || op31 == 6) && sf && !o0);
        if (!ok) return 0;
        int has_a = !(op31 == 2 || op31 == 6);                  /* SMULH/UMULH ignore Ra */
        dp_copy(e, sp_off, insn, 1, 1, has_a, 0, 0, 0, 0, 1);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- loads and stores */

/* x0 = m, x1 = address, x2 = size, x3 = value */
static void tr_access(emitter *e, int load, int size, int sign, int dst64, int rt, int addr_reg, uint32_t sp_off)
{
    (void)sp_off;
    mov_reg(e, 1, addr_reg);
    mov_reg(e, 0, HM);
    mov_imm(e, 2, (uint64_t)size);
    if (load) {
        call(e, (const void *)h_load);
        if (rt != 31) {
            if (sign) {
                /* sxtb/sxth/sxtw into x0 (64) or w0 (32) */
                uint32_t imms = size == 1 ? 7 : size == 2 ? 15 : 31;
                if (dst64) emit(e, 0x93400000u | imms << 10);                  /* sbfm x0, x0, #0, #imms */
                else emit(e, 0x13000000u | imms << 10);                        /* sbfm w0, w0, #0, #imms */
            }
            str_x(e, 0, HC, OFF_X(rt));
        }
    } else {
        if (rt == 31) mov_imm(e, 3, 0);
        else ldr_x(e, 3, HC, OFF_X(rt));
        call(e, (const void *)h_store);
    }
}

static void add_imm64(emitter *e, int d, int n, int64_t v)
{
    if (v >= 0 && v < 4096) { emit(e, 0x91000000u | (uint32_t)v << 10 | (uint32_t)n << 5 | (uint32_t)d); return; }
    if (v < 0 && v > -4096) { emit(e, 0xD1000000u | (uint32_t)(-v) << 10 | (uint32_t)n << 5 | (uint32_t)d); return; }
    mov_imm(e, T4, (uint64_t)v);
    emit(e, 0x8B000000u | (uint32_t)T4 << 16 | (uint32_t)n << 5 | (uint32_t)d);   /* add xd, xn, x13 */
}

static int tr_ldst(emitter *e, uint32_t sp_off, uint32_t insn, uint64_t pc)
{
    int rt = insn & 31, rn = (insn >> 5) & 31;
    /* load/store register (integer): size 111 0 xx opc */
    if ((insn & 0x3F000000u) == 0x39000000u || (insn & 0x3F200000u) == 0x38000000u ||
        (insn & 0x3F200C00u) == 0x38200800u) {
        int size = insn >> 30, opc = (insn >> 22) & 3;
        int bytes = 1 << size, load, sign = 0, dst64 = 1;
        if (opc == 0) load = 0;
        else if (opc == 1) load = 1;
        else {
            if (size == 3) return 0;                            /* PRFM */
            if (size == 2 && opc == 3) return 0;
            load = 1; sign = 1; dst64 = opc == 2;
        }
        regmap rm = { e, sp_off, { 0 }, { 0 }, 0 };
        int hb = src(&rm, rn, 1);
        int wbk = 0;
        int64_t off = 0;
        int addr = T3;
        if ((insn >> 24) & 1) {                                 /* unsigned offset */
            off = (int64_t)(((insn >> 10) & 0xFFF) << size);
            add_imm64(e, addr, hb, off);
        } else if ((insn >> 21) & 1) {                          /* register offset */
            int rmr = (insn >> 16) & 31, option = (insn >> 13) & 7, S = (insn >> 12) & 1;
            if (!(option & 2)) return 0;
            int hm = src(&rm, rmr, 0);
            /* add x12, xb, wm/xm, <extend> #amount (extended register form) */
            emit(e, 0x8B200000u | (uint32_t)hm << 16 | (uint32_t)option << 13 | (uint32_t)(S ? size : 0) << 10 |
                    (uint32_t)hb << 5 | (uint32_t)addr);
        } else {
            int k = (insn >> 10) & 3;
            off = (int64_t)((uint64_t)((insn >> 12) & 0x1FF) << 55) >> 55;
            if (k == 2) return 0;                               /* LDTR/STTR */
            if (k == 1) { mov_reg(e, addr, hb); wbk = 1; }      /* post-index */
            else { add_imm64(e, addr, hb, off); wbk = k == 3; }
        }
        if (wbk && rn == rt && rn != 31) return 0;              /* unpredictable writeback forms */
        set_pc(e, pc);
        tr_access(e, load, bytes, sign, dst64, rt, addr, sp_off);
        if (wbk) {
            /* base register again from memory (the access may not change it), then add */
            ldr_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
            add_imm64(e, T0, T0, off);
            str_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
        }
        return 1;
    }
    /* load/store pair (integer): opc 101 0 mode L imm7 */
    if ((insn & 0x3E000000u) == 0x28000000u && !((insn >> 26) & 1)) {
        int opc = insn >> 30, mode = (insn >> 23) & 3, L = (insn >> 22) & 1;
        int rt2 = (insn >> 10) & 31;
        if (opc == 3 || (opc == 1 && !L) || mode == 0) return 0;
        if (L && rt == rt2) return 0;
        int wbk = mode == 1 || mode == 3;
        if (wbk && (rn == rt || rn == rt2) && rn != 31) return 0;
        int b = opc == 2 ? 8 : 4, sign = opc == 1;
        int64_t off = ((int64_t)((uint64_t)((insn >> 15) & 0x7F) << 57) >> 57) * b;
        regmap rm = { e, sp_off, { 0 }, { 0 }, 0 };
        int hb = src(&rm, rn, 1);
        int addr = T3;
        if (mode == 1) mov_reg(e, addr, hb);
        else add_imm64(e, addr, hb, off);
        set_pc(e, pc);
        if (L) {
            /* both loads first (into the stack slot pair of x19/x20 frame), then the registers:
             * a fault on the second access must leave the registers unchanged */
            mov_reg(e, 1, addr); mov_reg(e, 0, HM); mov_imm(e, 2, (uint64_t)b);
            emit(e, 0xA9BF33EBu);                               /* stp x11, x12, [sp, #-16]! */
            call(e, (const void *)h_load);
            emit(e, 0xA8C133EBu);                               /* ldp x11, x12, [sp], #16 */
            mov_reg(e, T2, 0);
            add_imm64(e, 1, addr, b); mov_reg(e, 0, HM); mov_imm(e, 2, (uint64_t)b);
            emit(e, 0xA9BF33EBu);
            call(e, (const void *)h_load);
            emit(e, 0xA8C133EBu);
            if (sign) {
                emit(e, 0x93407C00u | (uint32_t)T2 << 5 | (uint32_t)T2);       /* sxtw x11, w11 */
                emit(e, 0x93407C00u);                                            /* sxtw x0, w0 */
            }
            if (rt != 31) str_x(e, T2, HC, OFF_X(rt));
            if (rt2 != 31) str_x(e, 0, HC, OFF_X(rt2));
        } else {
            mov_reg(e, 1, addr); mov_reg(e, 0, HM); mov_imm(e, 2, (uint64_t)b);
            if (rt == 31) mov_imm(e, 3, 0); else ldr_x(e, 3, HC, OFF_X(rt));
            emit(e, 0xA9BF33EBu);
            call(e, (const void *)h_store);
            emit(e, 0xA8C133EBu);
            add_imm64(e, 1, addr, b); mov_reg(e, 0, HM); mov_imm(e, 2, (uint64_t)b);
            if (rt2 == 31) mov_imm(e, 3, 0); else ldr_x(e, 3, HC, OFF_X(rt2));
            call(e, (const void *)h_store);
        }
        if (wbk) {
            ldr_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
            add_imm64(e, T0, T0, off);
            str_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
        }
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- branches (end a block) */

static int tr_branch(emitter *e, uint32_t sp_off, uint32_t insn, uint64_t pc)
{
    (void)sp_off;
    if ((insn & 0x7C000000u) == 0x14000000u) {                 /* B, BL */
        int64_t off = (int64_t)((uint64_t)(insn & 0x03FFFFFFu) << 38) >> 36;
        if (insn >> 31) { mov_imm(e, T0, pc + 4); str_x(e, T0, HC, OFF_X(30)); }
        set_pc(e, pc + (uint64_t)off);
        return 1;
    }
    if ((insn & 0xFF000010u) == 0x54000000u) {                 /* B.cond */
        int64_t off = (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        mov_imm(e, T0, pc + (uint64_t)off);
        mov_imm(e, T1, pc + 4);
        load_nzcv(e);
        emit(e, 0x9A800000u | (uint32_t)T1 << 16 | (insn & 15) << 12 | (uint32_t)T0 << 5 | (uint32_t)T0);  /* csel */
        str_x(e, T0, HC, OFF_PC);
        return 1;
    }
    if ((insn & 0x7E000000u) == 0x34000000u || (insn & 0x7E000000u) == 0x36000000u) {   /* CB(N)Z, TB(N)Z */
        int tb = (insn & 0x7E000000u) == 0x36000000u;
        int64_t off = tb ? (int64_t)((uint64_t)((insn >> 5) & 0x3FFF) << 50) >> 48
                         : (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        int rt = insn & 31;
        mov_imm(e, T0, pc + 4);
        mov_imm(e, T1, pc + (uint64_t)off);
        if (rt == 31) mov_imm(e, T2, 0); else ldr_x(e, T2, HC, OFF_X(rt));
        /* same branch on the host register, skipping one instruction: mov x9, x10 */
        uint32_t w = (insn & ~((tb ? 0x3FFFu : 0x7FFFFu) << 5) & ~31u) | (uint32_t)T2 | 2u << 5;
        emit(e, w ^ (1u << 24));                                /* inverted: skip the move when not taken */
        mov_reg(e, T0, T1);
        str_x(e, T0, HC, OFF_PC);
        return 1;
    }
    if ((insn & 0xFF9FFC1Fu) == 0xD61F0000u) {                 /* BR, BLR, RET */
        int opc = (insn >> 21) & 3, rn = (insn >> 5) & 31;
        if (opc == 3) return 0;
        if (rn == 31) mov_imm(e, T0, 0); else ldr_x(e, T0, HC, OFF_X(rn));
        if (opc == 1) { mov_imm(e, T1, pc + 4); str_x(e, T1, HC, OFF_X(30)); }
        str_x(e, T0, HC, OFF_PC);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- cache */

static jit_state *jit_get(wd_machine *m)
{
    jit_state *j = m->jit;
    if (j) return j->ok ? j : NULL;
    j = calloc(1, sizeof *j);
    if (!j) return NULL;
    m->jit = j;
    /* two views of one memfd-like region: anonymous shared mapping mapped twice is not
     * portable, so use a single RWX mapping where allowed, else give up */
    void *p = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    j->code = j->wcode = p;
    j->cap = CACHE_SIZE / 4;
    j->ok = 1;
    return j;
}

static void jit_reset(jit_state *j)
{
    memset(j->bucket, 0, sizeof j->bucket);
    j->npool = 0;
    j->used = 0;
}

static block *new_block(jit_state *j)
{
    if (j->npool == j->cappool) {
        size_t cap = j->cappool ? j->cappool * 2 : 4096;
        if (cap > (1u << 20)) return NULL;
        block *n = realloc(j->pool, cap * sizeof *n);
        if (!n) return NULL;
        if (n != j->pool) {
            /* pointers into the pool changed: rebuild the buckets */
            j->pool = n;
            j->cappool = cap;
            memset(j->bucket, 0, sizeof j->bucket);
            for (size_t i = 0; i < j->npool; i++) {
                block *b = &j->pool[i];
                unsigned h = (unsigned)((b->pc >> 2) ^ (b->pc >> 18)) & (NBUCKET - 1);
                b->next = j->bucket[h];
                j->bucket[h] = b;
            }
        } else {
            j->cappool = cap;
        }
    }
    return &j->pool[j->npool++];
}

static block *translate(wd_machine *m, jit_state *j, uint64_t pc, const uint8_t *host, uint32_t mode)
{
    if (j->used + 4096 > j->cap) jit_reset(j);
    block *b = new_block(j);
    if (!b) { jit_reset(j); b = new_block(j); if (!b) return NULL; }
    b->pc = pc;
    b->host = host;
    b->mode = mode;
    b->n = 0;
    b->fn = NULL;
    uint32_t sp_off = (mode & 1) && (mode & 2) ? OFF_SP1 : OFF_SP0;

    emitter e = { j, j->wcode + j->used, j->wcode + j->cap, 0 };
    uint32_t *start = e.p;
    prologue(&e);
    int ended = 0;
    uint64_t p = pc;
    unsigned room = (unsigned)((0x1000 - (pc & 0xFFF)) / 4);
    static int limit = -1;
    if (limit < 0) {
        const char *env = getenv("WD_JIT_MAX");
        limit = env ? atoi(env) : MAX_INSNS;
        if (limit < 1 || limit > MAX_INSNS) limit = MAX_INSNS;
    }
    unsigned max = room < (unsigned)limit ? room : (unsigned)limit;
    while (b->n < max) {
        uint32_t insn;
        memcpy(&insn, host + 4 * b->n, 4);
        uint32_t *mark = e.p;
        if (tr_branch(&e, sp_off, insn, p)) {
            b->words[b->n++] = insn;
            ended = 1;
            break;
        }
        if (!tr_dp(&e, sp_off, insn, p) && !tr_ldst(&e, sp_off, insn, p)) {
            e.p = mark;
            break;
        }
        b->words[b->n++] = insn;
        p += 4;
    }
    (void)m;
    if (!b->n || e.overflow) {
        b->n = 0;
        return b;                                    /* not translatable: interpreter */
    }
    if (!ended) set_pc(&e, pc + 4ULL * b->n);
    /* icount once per block */
    ldr_x(&e, T0, HC, OFF_ICOUNT);
    add_imm64(&e, T0, T0, b->n);
    str_x(&e, T0, HC, OFF_ICOUNT);
    epilogue(&e);
    if (e.overflow) { b->n = 0; return b; }
    size_t words = (size_t)(e.p - start);
    __builtin___clear_cache((char *)(j->code + j->used), (char *)(j->code + j->used + words));
    b->fn = (block_fn)(void *)(j->code + j->used);
    j->used += words;
    return b;
}

/* Execute translated blocks while possible; returns instructions run (0: interpret one). */
uint64_t wd_jit_run(wd_machine *m, uint64_t max)
{
    jit_state *j = jit_get(m);
    if (!j) return 0;
    wd_cpu *c = &m->cpu;
    uint64_t done = 0;
    while (done < max) {
        uint64_t pc = c->pc;
        if (pc & 3) break;
        if ((pc >> 12) != m->fetch_vpn || m->fetch_el != c->el) {
            if (done) break;
            (void)wd_fetch(m, pc);                       /* fills the fetch cache or raises the abort */
        }
        const uint8_t *host = m->fetch_host + (pc & 0xFFF);
        uint32_t mode = (uint32_t)c->el | (uint32_t)c->spsel << 1;
        unsigned h = (unsigned)((pc >> 2) ^ (pc >> 18)) & (NBUCKET - 1);
        block *b;
        for (b = j->bucket[h]; b; b = b->next)
            if (b->pc == pc && b->host == host && b->mode == mode) break;
        if (b && b->n && memcmp(b->words, host, 4u * b->n)) b = NULL;   /* code changed */
        if (!b) {
            b = translate(m, j, pc, host, mode);
            if (!b) break;
            b->next = j->bucket[h];
            j->bucket[h] = b;
        }
        if (!b->n || b->n > max - done) break;
        b->fn(m, c);
        done += b->n;
        m->budget -= b->n;
        if (m->stop >= 0 || m->irq_dirty || m->wfi) break;
    }
    return done;
}

void wd_jit_free(wd_machine *m)
{
    jit_state *j = m->jit;
    if (!j) return;
    if (j->code) munmap(j->code, CACHE_SIZE);
    free(j->pool);
    free(j);
    m->jit = NULL;
}

int wd_jit_available(void) { return 1; }

#else

uint64_t wd_jit_run(wd_machine *m, uint64_t max) { (void)m; (void)max; return 0; }
void wd_jit_free(wd_machine *m) { (void)m; }
int wd_jit_available(void) { return 0; }

#endif
