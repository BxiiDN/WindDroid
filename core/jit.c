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
#include <sys/syscall.h>
#include <unistd.h>

#define CACHE_SIZE  (32u << 20)          /* host code */
#define NBUCKET     65536
#define MAX_INSNS   48

typedef void (*block_fn)(wd_machine *m, wd_cpu *c);

typedef struct block {
    uint64_t pc;                         /* ~0: invalidated */
    const uint8_t *host;                 /* guest code in host memory */
    uint32_t mode;                       /* el | spsel << 1 | memory mode << 2 */
    uint32_t n;                          /* guest instructions */
    uint64_t page;                       /* RAM page index of the code, NO_PAGE outside RAM */
    uint32_t words[MAX_INSNS];           /* compared before each run when outside RAM */
    block_fn fn;                         /* NULL: first instruction is not translatable */
    struct block *next, *next_page;
} block;

#define NO_PAGE   (~0ULL)
#define NPAGEHASH 16384
#define POOL      65536
#define ENTRY_SKIP 4                     /* words of prologue before the body (chained entry) */

typedef struct {
    uint32_t *code;                      /* RX view */
    uint32_t *wcode;                     /* RW view (same pages) */
    size_t used, cap;                    /* in 32-bit words */
    block *bucket[NBUCKET];
    block *page_head[NPAGEHASH];
    block *pool;
    size_t npool;
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
    unsigned icount_target, icount_done;  /* guest instructions to have counted before a helper call */
    int mem_mode;                         /* fast TLB set: 0 EL0, 1 EL1, 2 EL1 with PAN */
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

/* count this instruction and the ones before it (a helper may raise a guest exception) */
static void count(emitter *e)
{
    if (e->icount_target > e->icount_done) {
        ldr_x(e, 17, HC, OFF_ICOUNT);
        emit(e, 0x91000000u | (e->icount_target - e->icount_done) << 10 | 17u << 5 | 17u);   /* add x17, x17, #d */
        str_x(e, 17, HC, OFF_ICOUNT);
        e->icount_done = e->icount_target;
    }
}

static void call(emitter *e, const void *fn)
{
    count(e);
    mov_imm(e, 16, (uint64_t)(uintptr_t)fn);
    emit(e, 0xD63F0200u);                                                         /* blr x16 */
}

static void set_pc(emitter *e, uint64_t pc) { mov_imm(e, T0, pc); str_x(e, T0, HC, OFF_PC); }

static void epilogue(emitter *e);

/* A block exit to a constant guest PC. The first word is a branch to the stub that
 * returns to the dispatcher; the dispatcher may later patch it into a direct branch to
 * the target block ("chaining"). The stub tells the dispatcher which word to patch. */
static void exit_to(emitter *e, uint64_t target)
{
    uint32_t *site = e->p;
    emit(e, 0x14000001u);                                     /* b stub (next word) */
    set_pc(e, target);
    /* m->jit_exit_site = address of the site in the executable view */
    uint64_t rx = (uint64_t)(uintptr_t)(e->j->code + (site - e->j->wcode));
    mov_imm(e, T0, rx);
    mov_imm(e, T1, (uint64_t)offsetof(wd_machine, jit_exit_site));
    emit(e, 0xF8206800u | (uint32_t)T1 << 16 | (uint32_t)HM << 5 | (uint32_t)T0);    /* str x9, [x20, x10] */
    epilogue(e);
}

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

/* After an access that may have touched a device: leave the block when the run loop
 * has something to do (interrupt state changed, stop requested), so interrupts are
 * taken at the same instruction boundary as in the interpreter. */
static void exit_check(emitter *e, uint64_t next_pc)
{
    mov_imm(e, T1, (uint64_t)offsetof(wd_machine, irq_dirty));
    emit(e, 0xB8606800u | (uint32_t)T1 << 16 | (uint32_t)HM << 5 | (uint32_t)T0);    /* ldr w9, [x20, x10] */
    mov_imm(e, T1, (uint64_t)offsetof(wd_machine, stop));
    emit(e, 0xB8606800u | (uint32_t)T1 << 16 | (uint32_t)HM << 5 | (uint32_t)T1);    /* ldr w10, [x20, x10] */
    emit(e, 0x2A2A03EAu);                                     /* mvn w10, w10 */
    emit(e, 0x2A4A7D29u);                                     /* orr w9, w9, w10, lsr #31 */
    uint32_t *br = e->p;
    emit(e, 0x34000000u | T0);                                /* cbz w9, continue (patched) */
    set_pc(e, next_pc);
    epilogue(e);
    if (!e->overflow) *br |= (uint32_t)(e->p - br) << 5;
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

/* One access with the address in x12 (store value in x3, load result in x0): an inline
 * lookup in the interpreter's fast TLB for aligned RAM accesses, the helper otherwise.
 * x12 and x14 survive (x14 holds the first value of a load pair). */
static void mem_access(emitter *e, int load, int size)
{
    static const uint32_t ld[4] = { 0x386969A0u, 0x786969A0u, 0xB86969A0u, 0xF86969A0u };   /* ldr* x0, [x13, x9] */
    static const uint32_t st[4] = { 0x382969A3u, 0x782969A3u, 0xB82969A3u, 0xF82969A3u };   /* str* x3, [x13, x9] */
    count(e);
    uint32_t *to_slow[2];
    int nslow = 0;
    if (size > 1) {
        emit(e, 0xF240019Fu | (uint32_t)(size == 2 ? 0 : size == 4 ? 1 : 2) << 10);         /* tst x12, #size-1 */
        to_slow[nslow++] = e->p;
        emit(e, 0x54000001u);                                                              /* b.ne slow */
    }
    emit(e, 0xD34CFD89u);                                                                  /* lsr x9, x12, #12 */
    emit(e, 0x92401D2Au);                                                                  /* and x10, x9, #0xff */
    size_t off = load ? offsetof(wd_machine, ftlb_r) : offsetof(wd_machine, ftlb_w);
    off += (size_t)e->mem_mode * FTLB_SIZE * 16;
    mov_imm(e, T2, (uint64_t)off);
    emit(e, 0x8B0B028Bu);                                                                  /* add x11, x20, x11 */
    emit(e, 0x8B0A116Bu);                                                                  /* add x11, x11, x10, lsl #4 */
    ldr_x(e, T4, T2, 0);
    emit(e, 0xEB0901BFu);                                                                  /* cmp x13, x9 */
    to_slow[nslow++] = e->p;
    emit(e, 0x54000001u);                                                                  /* b.ne slow */
    ldr_x(e, T4, T2, 8);
    emit(e, 0x92402D89u);                                                                  /* and x9, x12, #0xfff */
    int lg = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    emit(e, load ? ld[lg] : st[lg]);
    uint32_t *to_done = e->p;
    emit(e, 0x14000000u);                                                                  /* b done */
    for (int i = 0; i < nslow; i++) *to_slow[i] |= (uint32_t)((e->p - to_slow[i]) & 0x7FFFF) << 5;
    emit(e, 0xA9BF3BECu);                                                                  /* stp x12, x14, [sp, #-16]! */
    mov_reg(e, 1, T3);
    mov_reg(e, 0, HM);
    mov_imm(e, 2, (uint64_t)size);
    call(e, load ? (const void *)h_load : (const void *)h_store);
    emit(e, 0xA8C13BECu);                                                                  /* ldp x12, x14, [sp], #16 */
    *to_done |= (uint32_t)(e->p - to_done) & 0x3FFFFFF;
}

static void tr_access(emitter *e, int load, int size, int sign, int dst64, int rt, int addr_reg, uint32_t sp_off)
{
    (void)sp_off;
    if (addr_reg != T3) mov_reg(e, T3, addr_reg);
    if (load) {
        mem_access(e, 1, size);
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
        mem_access(e, 0, size);
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
        exit_check(e, pc + 4);
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
        if (addr != T3) mov_reg(e, T3, addr);
        if (L) {
            /* both loads first: a fault on the second access leaves the registers unchanged */
            mem_access(e, 1, b);
            emit(e, 0xAA0003EEu);                               /* mov x14, x0 */
            add_imm64(e, T3, T3, b);
            mem_access(e, 1, b);
            if (sign) {
                emit(e, 0x93407C00u | 14u << 5 | 14u);          /* sxtw x14, w14 */
                emit(e, 0x93407C00u);                           /* sxtw x0, w0 */
            }
            if (rt != 31) str_x(e, 14, HC, OFF_X(rt));
            if (rt2 != 31) str_x(e, 0, HC, OFF_X(rt2));
        } else {
            if (rt == 31) mov_imm(e, 3, 0); else ldr_x(e, 3, HC, OFF_X(rt));
            mem_access(e, 0, b);
            add_imm64(e, T3, T3, b);
            if (rt2 == 31) mov_imm(e, 3, 0); else ldr_x(e, 3, HC, OFF_X(rt2));
            mem_access(e, 0, b);
        }
        if (wbk) {
            ldr_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
            add_imm64(e, T0, T0, off);
            str_x(e, T0, HC, rn == 31 ? sp_off : OFF_X(rn));
        }
        exit_check(e, pc + 4);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- branches (end a block) */

static int is_branch(uint32_t insn)
{
    return (insn & 0x7C000000u) == 0x14000000u || (insn & 0xFF000010u) == 0x54000000u ||
           (insn & 0x7E000000u) == 0x34000000u || (insn & 0x7E000000u) == 0x36000000u ||
           ((insn & 0xFF9FFC1Fu) == 0xD61F0000u && ((insn >> 21) & 3) != 3);
}

static int tr_branch(emitter *e, uint32_t sp_off, uint32_t insn, uint64_t pc)
{
    (void)sp_off;
    if ((insn & 0x7C000000u) == 0x14000000u) {                 /* B, BL */
        int64_t off = (int64_t)((uint64_t)(insn & 0x03FFFFFFu) << 38) >> 36;
        if (insn >> 31) { mov_imm(e, T0, pc + 4); str_x(e, T0, HC, OFF_X(30)); }
        exit_to(e, pc + (uint64_t)off);
        return 1;
    }
    if ((insn & 0xFF000010u) == 0x54000000u) {                 /* B.cond */
        int64_t off = (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        int cond = insn & 15;
        if (cond >= 14) { exit_to(e, pc + (uint64_t)off); return 1; }
        load_nzcv(e);
        uint32_t *skip = e->p;
        emit(e, 0x54000000u | (uint32_t)(cond ^ 1));           /* b.<not cond> not_taken (patched) */
        exit_to(e, pc + (uint64_t)off);
        if (!e->overflow) *skip |= (uint32_t)((e->p - skip) & 0x7FFFF) << 5;
        exit_to(e, pc + 4);
        return 1;
    }
    if ((insn & 0x7E000000u) == 0x34000000u || (insn & 0x7E000000u) == 0x36000000u) {   /* CB(N)Z, TB(N)Z */
        int tb = (insn & 0x7E000000u) == 0x36000000u;
        int64_t off = tb ? (int64_t)((uint64_t)((insn >> 5) & 0x3FFF) << 50) >> 48
                         : (int64_t)((uint64_t)((insn >> 5) & 0x7FFFF) << 45) >> 43;
        int rt = insn & 31;
        if (rt == 31) mov_imm(e, T2, 0); else ldr_x(e, T2, HC, OFF_X(rt));
        uint32_t fieldmask = (tb ? 0x3FFFu : 0x7FFFFu) << 5;
        uint32_t *skip = e->p;
        emit(e, ((insn & ~fieldmask & ~31u) | (uint32_t)T2) ^ (1u << 24));   /* inverted: to not_taken */
        exit_to(e, pc + (uint64_t)off);
        if (!e->overflow) *skip |= (uint32_t)((e->p - skip) & (tb ? 0x3FFF : 0x7FFFF)) << 5;
        exit_to(e, pc + 4);
        return 1;
    }
    if ((insn & 0xFF9FFC1Fu) == 0xD61F0000u) {                 /* BR, BLR, RET */
        int opc = (insn >> 21) & 3, rn = (insn >> 5) & 31;
        if (opc == 3) return 0;
        if (rn == 31) mov_imm(e, T0, 0); else ldr_x(e, T0, HC, OFF_X(rn));
        if (opc == 1) { mov_imm(e, T1, pc + 4); str_x(e, T1, HC, OFF_X(30)); }
        str_x(e, T0, HC, OFF_PC);
        epilogue(e);
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
    j->pool = calloc(POOL, sizeof *j->pool);
    m->code_page = calloc((size_t)(m->ram_size >> 12) + 1, 1);
    if (!j->pool || !m->code_page) return NULL;
    /* one read-write-execute mapping where the system allows it; otherwise (W^X policies)
     * a memfd mapped twice, writable and executable */
    void *p = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        j->code = j->wcode = p;
    } else {
#ifdef SYS_memfd_create
        int fd = (int)syscall(SYS_memfd_create, "winddroid-jit", 0);
        if (fd < 0 || ftruncate(fd, CACHE_SIZE)) { if (fd >= 0) close(fd); return NULL; }
        void *w = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void *x = mmap(NULL, CACHE_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        close(fd);
        if (w == MAP_FAILED || x == MAP_FAILED) {
            if (w != MAP_FAILED) munmap(w, CACHE_SIZE);
            if (x != MAP_FAILED) munmap(x, CACHE_SIZE);
            return NULL;
        }
        j->wcode = w;
        j->code = x;
#else
        return NULL;
#endif
    }
    j->cap = CACHE_SIZE / 4;
    j->ok = 1;
    return j;
}

static void jit_reset(wd_machine *m, jit_state *j)
{
    memset(j->bucket, 0, sizeof j->bucket);
    memset(j->page_head, 0, sizeof j->page_head);
    j->npool = 0;
    j->used = 0;
    if (m->code_page) memset(m->code_page, 0, (size_t)(m->ram_size >> 12));
}

static block *new_block(wd_machine *m, jit_state *j)
{
    if (j->npool == POOL) jit_reset(m, j);
    return &j->pool[j->npool++];
}

/* A RAM page now holds translated code: its writes must leave the fast path. */
static void mark_code_page(wd_machine *m, uint64_t page)
{
    if (m->code_page[page]) return;
    m->code_page[page] = 1;
    uint8_t *base = m->ram + (page << 12);
    for (int k = 0; k < 3; k++)
        for (unsigned i = 0; i < FTLB_SIZE; i++)
            if (m->ftlb_w[k][i].host == base) m->ftlb_w[k][i].vpn = ~0ULL;
}

/* Guest memory of a code page was written: drop every block translated from it. */
void wd_jit_page_written(wd_machine *m, uint64_t page)
{
    jit_state *j = m->jit;
    if (!j || !m->code_page || page >= (m->ram_size >> 12)) return;
    block **pp = &j->page_head[page & (NPAGEHASH - 1)];
    while (*pp) {
        block *b = *pp;
        if (b->page == page) {
            b->pc = ~0ULL;                       /* never found again; chains into it come from */
            *pp = b->next_page;                  /* blocks of the same page, dropped as well */
        } else {
            pp = &b->next_page;
        }
    }
    m->code_page[page] = 0;
    m->irq_dirty = 1;                            /* leave translated code at the next check */
}

static block *translate(wd_machine *m, jit_state *j, uint64_t pc, const uint8_t *host, uint32_t mode)
{
    if (j->used + 4096 > j->cap) jit_reset(m, j);
    block *b = new_block(m, j);
    b->pc = pc;
    b->host = host;
    b->mode = mode;
    b->n = 0;
    b->fn = NULL;
    b->next = b->next_page = NULL;
    b->page = host >= m->ram && host < m->ram + m->ram_size ? (uint64_t)(host - m->ram) >> 12 : NO_PAGE;
    uint32_t sp_off = (mode & 1) && (mode & 2) ? OFF_SP1 : OFF_SP0;

    emitter e = { j, j->wcode + j->used, j->wcode + j->cap, 0, 0, 0, (int)(mode >> 2) };
    uint32_t *start = e.p;
    prologue(&e);
    /* body (also the entry of chained branches): stop when the slice is used up */
    mov_imm(&e, T1, (uint64_t)offsetof(wd_machine, jit_left));
    emit(&e, 0xF8606800u | (uint32_t)T1 << 16 | (uint32_t)HM << 5 | (uint32_t)T0);   /* ldr x9, [x20, x10] */
    uint32_t *nslot = e.p;
    emit(&e, 0xF1000000u | (uint32_t)T0 << 5 | (uint32_t)T0);                   /* subs x9, x9, #n (patched) */
    uint32_t *bail = e.p;
    emit(&e, 0x5400000Bu);                                                       /* b.lt bail (patched) */
    emit(&e, 0xF8206800u | (uint32_t)T1 << 16 | (uint32_t)HM << 5 | (uint32_t)T0);   /* str x9, [x20, x10] */
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
        unsigned saved_done = e.icount_done;
        e.icount_target = b->n + 1;
        if (is_branch(insn)) {
            count(&e);                               /* the whole block, branch included */
            tr_branch(&e, sp_off, insn, p);
            b->words[b->n++] = insn;
            ended = 1;
            break;
        }
        if (!tr_dp(&e, sp_off, insn, p) && !tr_ldst(&e, sp_off, insn, p)) {
            e.p = mark;
            e.icount_done = saved_done;
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
    if (!ended) {
        /* fell through: count, then a chainable exit to the next instruction */
        if (b->n > e.icount_done) {
            ldr_x(&e, T0, HC, OFF_ICOUNT);
            add_imm64(&e, T0, T0, (int64_t)(b->n - e.icount_done));
            str_x(&e, T0, HC, OFF_ICOUNT);
        }
        exit_to(&e, pc + 4ULL * b->n);
    }
    /* slice used up before the block ran: back to the dispatcher at its first instruction */
    if (!e.overflow) *bail |= (uint32_t)((e.p - bail) & 0x7FFFF) << 5;
    set_pc(&e, pc);
    epilogue(&e);
    if (!e.overflow) *nslot |= b->n << 10;
    if (e.overflow) { b->n = 0; return b; }
    size_t words = (size_t)(e.p - start);
    if (j->wcode != j->code)
        __builtin___clear_cache((char *)(j->wcode + j->used), (char *)(j->wcode + j->used + words));
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
        uint32_t mode = (uint32_t)c->el | (uint32_t)c->spsel << 1 | (uint32_t)wd_mem_mode(m) << 2;
        unsigned h = (unsigned)((pc >> 2) ^ (pc >> 18)) & (NBUCKET - 1);
        block *b;
        for (b = j->bucket[h]; b; b = b->next)
            if (b->pc == pc && b->host == host && b->mode == mode) break;
        /* code outside RAM is not write-tracked: compare it before running */
        if (b && b->n && b->page == NO_PAGE && memcmp(b->words, host, 4u * b->n)) b = NULL;
        if (!b) {
            b = translate(m, j, pc, host, mode);
            if (!b) break;
            b->next = j->bucket[h];
            j->bucket[h] = b;
            if (b->n && b->page != NO_PAGE) {
                block **ph = &j->page_head[b->page & (NPAGEHASH - 1)];
                b->next_page = *ph;
                *ph = b;
                mark_code_page(m, b->page);
            }
        }
        if (!b->n || b->n > max - done) break;
        uint64_t before = c->icount;
        m->jit_left = (int64_t)(max - done);
        m->jit_exit_site = NULL;
        b->fn(m, c);
        if (m->trace == 2) wd_state_trace(m);
        uint64_t ran = c->icount - before;             /* blocks chained, or an early exit */
        done += ran;
        m->budget -= ran;
        if (m->stop >= 0 || m->irq_dirty || m->wfi) break;
        /* chain: the exit just taken jumps straight to the next block from now on, when both
         * come from the same RAM page (same translation, invalidated together) */
        uint32_t *site = m->jit_exit_site;
        if (site && b->page != NO_PAGE && m->trace != 2) {
            uint64_t npc = c->pc;
            if ((npc >> 12) == (pc >> 12)) {
                const uint8_t *nhost = m->fetch_host + (npc & 0xFFF);
                unsigned nh = (unsigned)((npc >> 2) ^ (npc >> 18)) & (NBUCKET - 1);
                block *t;
                for (t = j->bucket[nh]; t; t = t->next)
                    if (t->pc == npc && t->host == nhost && t->mode == mode) break;
                /* the source block found by the exit may have been invalidated meanwhile */
                if (t && t->n && t->page == b->page && b->pc != ~0ULL && (npc >> 12) == m->fetch_vpn) {
                    uint32_t *target = (uint32_t *)(void *)t->fn + ENTRY_SKIP;
                    int64_t d = target - site;
                    uint32_t *wsite = j->wcode + (site - j->code);
                    *wsite = 0x14000000u | ((uint32_t)d & 0x3FFFFFF);
                    if (j->wcode != j->code) __builtin___clear_cache((char *)wsite, (char *)(wsite + 1));
                    __builtin___clear_cache((char *)site, (char *)(site + 1));
                }
            }
        }
    }
    return done;
}

void wd_jit_free(wd_machine *m)
{
    jit_state *j = m->jit;
    if (!j) return;
    if (j->code) munmap(j->code, CACHE_SIZE);
    if (j->wcode && j->wcode != j->code) munmap(j->wcode, CACHE_SIZE);
    free(j->pool);
    free(j);
    free(m->code_page);
    m->code_page = NULL;
    m->jit = NULL;
}

int wd_jit_available(void) { return 1; }

int wd_jit_active(wd_machine *m)
{
    jit_state *j = m->use_jit ? jit_get(m) : NULL;
    return j != NULL;
}

#else

uint64_t wd_jit_run(wd_machine *m, uint64_t max) { (void)m; (void)max; return 0; }
void wd_jit_page_written(wd_machine *m, uint64_t page) { (void)m; (void)page; }
void wd_jit_free(wd_machine *m) { (void)m; }
int wd_jit_available(void) { return 0; }
int wd_jit_active(wd_machine *m) { (void)m; return 0; }

#endif
