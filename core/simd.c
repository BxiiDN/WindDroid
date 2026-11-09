/*
 * WindDroid - Advanced SIMD and floating-point instructions (A64).
 *
 * Covers scalar FP (arithmetic, fused multiply-add, compares, conversions,
 * rounding), AdvSIMD integer and FP vector/scalar instructions, permutes,
 * table lookups and all SIMD&FP load/store forms. Half-precision is
 * supported for conversions only (FEAT_FP16 is not advertised).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "fpu.h"

#define VB(c, r) ((uint8_t *)(c)->v[r])

static inline uint64_t ge(const uint8_t *v, int i, int esz)
{
    uint64_t x = 0;
    memcpy(&x, v + i * esz, (size_t)esz);
    return x;
}

static inline void se(uint8_t *v, int i, int esz, uint64_t x) { memcpy(v + i * esz, &x, (size_t)esz); }

static inline int64_t sx(uint64_t v, int bits) { return (int64_t)wd_sext(v, bits); }

static inline uint64_t mk(int bits) { return wd_mask(bits); }

static inline uint64_t Xr(wd_cpu *c, int r) { return r == 31 ? 0 : c->x[r]; }
static inline void setXr(wd_cpu *c, int r, uint64_t v) { if (r != 31) c->x[r] = v; }

static void wr_vec(wd_cpu *c, int rd, const uint8_t *res, int q)
{
    memcpy(VB(c, rd), res, q ? 16 : 8);
    if (!q) memset(VB(c, rd) + 8, 0, 8);
}

static void wr_scalar(wd_cpu *c, int rd, uint64_t val, int esz)
{
    uint8_t t[16] = { 0 };
    memcpy(t, &val, (size_t)esz);
    memcpy(VB(c, rd), t, 16);
}

static inline uint64_t rd_scalar(wd_cpu *c, int r, int esz) { return ge(VB(c, r), 0, esz); }

void wd_fp_check(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    unsigned fpen = (unsigned)(c->cpacr >> 20) & 3;
    int trap = c->el == 0 ? fpen != 3 : (fpen == 0 || fpen == 2);
    if (trap) wd_exception(m, EC_FP, 0x1E00000, 0, 0, c->pc);
}

/* ================================================================ saturation helpers */

static int64_t ssat(__int128 v, int bits, int *q)
{
    __int128 max = ((__int128)1 << (bits - 1)) - 1, min = -max - 1;
    if (v > max) { *q = 1; return (int64_t)max; }
    if (v < min) { *q = 1; return (int64_t)min; }
    return (int64_t)v;
}

static uint64_t usat(__int128 v, int bits, int *q)
{
    __int128 max = ((__int128)1 << bits) - 1;
    if (v > max) { *q = 1; return (uint64_t)max; }
    if (v < 0) { *q = 1; return 0; }
    return (uint64_t)v;
}

/* shift by signed register amount (SSHL family) */
static uint64_t shl_reg(uint64_t a, uint64_t b, int bits, int is_unsigned, int round, int sat, int *q)
{
    int sh = (int)(int8_t)(b & 0xFF);
    uint64_t M = mk(bits);
    __int128 v = is_unsigned ? (__int128)(a & M) : (__int128)sx(a, bits);
    __int128 r;
    if (sh >= 0) {
        if (!sat) return sh >= bits ? 0 : ((a & M) << sh) & M;
        if (v == 0) r = 0;
        else if (sh >= bits) r = v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100);
        else r = v * ((__int128)1 << sh);
    } else {
        int rs = -sh > 100 ? 100 : -sh;
        r = round ? (v + ((__int128)1 << (rs - 1))) >> rs : v >> rs;
        if (!sat) return (uint64_t)r & M;
    }
    return is_unsigned ? usat(r, bits, q) : (uint64_t)ssat(r, bits, q) & M;
}

static uint64_t sqdmulh(uint64_t a, uint64_t b, int bits, int round, int *q)
{
    __int128 p = (__int128)sx(a, bits) * sx(b, bits) * 2;
    if (round) p += (__int128)1 << (bits - 1);
    return (uint64_t)ssat(p >> bits, bits, q) & mk(bits);
}

/* ================================================================ FP immediate */

static uint64_t vfp_expand_imm(uint32_t imm8, int n)
{
    int E = n == 16 ? 5 : (n == 32 ? 8 : 11), F = n - E - 1;
    uint64_t sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    uint64_t exp = ((b6 ^ 1) << (E - 1)) | ((b6 ? mk(E - 3) : 0) << 2) | ((imm8 >> 4) & 3);
    uint64_t frac = (uint64_t)(imm8 & 15) << (F - 4);
    return (sign << (n - 1)) | (exp << F) | frac;
}

/* ================================================================ scalar FP */

static int ftype_bits(int type)
{
    switch (type) {
    case 0: return 32;
    case 1: return 64;
    case 3: return 16;
    default: return 0;
    }
}

static int cond_holds(uint32_t nzcv, int cond)
{
    int n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, cf = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cf; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cf && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static uint64_t fp_2src(wd_cpu *c, int opc, uint64_t a, uint64_t b, int n, int *ok)
{
    *ok = 1;
    switch (opc) {
    case 0: return fpu_mul(c, a, b, n);
    case 1: return fpu_div(c, a, b, n);
    case 2: return fpu_add(c, a, b, n);
    case 3: return fpu_sub(c, a, b, n);
    case 4: return fpu_max(c, a, b, n);
    case 5: return fpu_min(c, a, b, n);
    case 6: return fpu_maxnm(c, a, b, n);
    case 7: return fpu_minnm(c, a, b, n);
    case 8: return fpu_neg(fpu_mul(c, a, b, n), n);
    default: *ok = 0; return 0;
    }
}

static int fp_scalar(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    int type = (insn >> 22) & 3, n = ftype_bits(type);

    if ((insn & 0x5F000000u) == 0x1F000000u) {                 /* 3-source */
        if (n != 32 && n != 64) return 0;
        int o1 = (insn >> 21) & 1, o0 = (insn >> 15) & 1, ra = (insn >> 10) & 31;
        uint64_t A = rd_scalar(c, ra, n / 8), N = rd_scalar(c, rn, n / 8), M = rd_scalar(c, rm, n / 8), r;
        if (!o1 && !o0) r = fpu_muladd(c, A, N, M, n);
        else if (!o1) r = fpu_muladd(c, A, fpu_neg(N, n), M, n);
        else if (!o0) r = fpu_muladd(c, fpu_neg(A, n), fpu_neg(N, n), M, n);
        else r = fpu_muladd(c, fpu_neg(A, n), N, M, n);
        wr_scalar(c, rd, r, n / 8);
        return 1;
    }
    if ((insn & 0x5F200000u) == 0x1E000000u) {                 /* FP <-> fixed point */
        int sf = insn >> 31, rmode = (insn >> 19) & 3, opcode = (insn >> 16) & 7, scale = (insn >> 10) & 63;
        int fbits = 64 - scale;
        if ((n != 32 && n != 64) || (!sf && scale < 32)) return 0;
        int ib = sf ? 64 : 32;
        if (rmode == 3 && (opcode == 0 || opcode == 1)) {      /* FCVTZS/FCVTZU fixed */
            uint64_t v = fpu_to_fixed(c, rd_scalar(c, rn, n / 8), n, fbits, opcode, RM_RZ, ib);
            setXr(c, rd, v);
            return 1;
        }
        if (rmode == 0 && (opcode == 2 || opcode == 3)) {      /* SCVTF/UCVTF fixed */
            uint64_t v = fpu_from_fixed(c, Xr(c, rn), opcode == 2, ib, fbits, n, fpcr_rmode(c));
            wr_scalar(c, rd, v, n / 8);
            return 1;
        }
        return 0;
    }
    if ((insn & 0x5F20FC00u) == 0x1E200000u) {                 /* FP <-> integer */
        int sf = insn >> 31, rmode = (insn >> 19) & 3, opcode = (insn >> 16) & 7;
        int ib = sf ? 64 : 32;
        if (opcode == 6 || opcode == 7) {                      /* FMOV */
            if (rmode == 0 && ((!sf && type == 0) || (sf && type == 1))) {
                if (opcode == 6) setXr(c, rd, rd_scalar(c, rn, ib / 8));
                else wr_scalar(c, rd, Xr(c, rn) & mk(ib), ib / 8);
                return 1;
            }
            if (rmode == 1 && sf && type == 2) {               /* top half of a 128-bit register */
                if (opcode == 6) setXr(c, rd, c->v[rn][1]);
                else c->v[rd][1] = Xr(c, rn);
                return 1;
            }
            return 0;
        }
        if (n != 32 && n != 64) return 0;
        if (opcode <= 1 || opcode == 4 || opcode == 5) {
            int rm2;
            if (opcode >= 4) { if (rmode) return 0; rm2 = RM_RNA; }
            else rm2 = rmode == 0 ? RM_RN : rmode;                /* 01 P, 10 M, 11 Z map 1:1 */
            int uns = opcode & 1;
            setXr(c, rd, fpu_to_fixed(c, rd_scalar(c, rn, n / 8), n, 0, uns, rm2, ib));
            return 1;
        }
        if (opcode == 2 || opcode == 3) {
            if (rmode) return 0;
            wr_scalar(c, rd, fpu_from_fixed(c, Xr(c, rn), opcode == 2, ib, 0, n, fpcr_rmode(c)), n / 8);
            return 1;
        }
        return 0;
    }
    if (insn >> 31 || (insn >> 29) & 1) return 0;                 /* M and S must be zero below */
    if ((insn & 0x5F207C00u) == 0x1E204000u) {                 /* 1-source */
        int opcode = (insn >> 15) & 63;
        if (!n) return 0;
        uint64_t a = rd_scalar(c, rn, n / 8), r;
        if ((opcode & 0x3C) == 0x04) {                         /* FCVT */
            int to = ftype_bits(opcode & 3);
            if (!to || to == n) return 0;
            r = fpu_convert(c, a, n, to, fpcr_rmode(c));
            wr_scalar(c, rd, r, to / 8);
            return 1;
        }
        if (n == 16) return 0;
        switch (opcode) {
        case 0: r = a; break;
        case 1: r = fpu_abs(a, n); break;
        case 2: r = fpu_neg(a, n); break;
        case 3: r = fpu_sqrt(c, a, n); break;
        case 8: r = fpu_round_int(c, a, n, RM_RN, 0); break;
        case 9: r = fpu_round_int(c, a, n, RM_RP, 0); break;
        case 10: r = fpu_round_int(c, a, n, RM_RM, 0); break;
        case 11: r = fpu_round_int(c, a, n, RM_RZ, 0); break;
        case 12: r = fpu_round_int(c, a, n, RM_RNA, 0); break;
        case 14: r = fpu_round_int(c, a, n, fpcr_rmode(c), 1); break;
        case 15: r = fpu_round_int(c, a, n, fpcr_rmode(c), 0); break;
        default: return 0;
        }
        wr_scalar(c, rd, r, n / 8);
        return 1;
    }
    if (n != 32 && n != 64) return 0;
    if ((insn & 0x5F203C00u) == 0x1E202000u) {                 /* compare */
        int opc2 = insn & 31;
        if ((insn >> 14) & 3 || (opc2 & 7)) return 0;
        uint64_t a = rd_scalar(c, rn, n / 8), b = (opc2 & 8) ? 0 : rd_scalar(c, rm, n / 8);
        c->nzcv = fpu_cmp(c, a, b, n, (opc2 >> 4) & 1);
        return 1;
    }
    if ((insn & 0x5F201C00u) == 0x1E201000u) {                 /* immediate */
        if ((insn >> 5) & 31) return 0;
        wr_scalar(c, rd, vfp_expand_imm((insn >> 13) & 0xFF, n), n / 8);
        return 1;
    }
    if ((insn & 0x5F200C00u) == 0x1E200400u) {                 /* conditional compare */
        int cond = (insn >> 12) & 15, signal = (insn >> 4) & 1;
        if (cond_holds(c->nzcv, cond))
            c->nzcv = fpu_cmp(c, rd_scalar(c, rn, n / 8), rd_scalar(c, rm, n / 8), n, signal);
        else
            c->nzcv = (insn & 15u) << 28;
        return 1;
    }
    if ((insn & 0x5F200C00u) == 0x1E200800u) {                 /* 2-source */
        int ok;
        uint64_t r = fp_2src(c, (insn >> 12) & 15, rd_scalar(c, rn, n / 8), rd_scalar(c, rm, n / 8), n, &ok);
        if (!ok) return 0;
        wr_scalar(c, rd, r, n / 8);
        return 1;
    }
    if ((insn & 0x5F200C00u) == 0x1E200C00u) {                 /* FCSEL */
        int cond = (insn >> 12) & 15;
        wr_scalar(c, rd, rd_scalar(c, cond_holds(c->nzcv, cond) ? rn : rm, n / 8), n / 8);
        return 1;
    }
    return 0;
}

/* ================================================================ vector FP helpers */

/* FP operation selected by the 3-same encoding; returns 0 if unallocated */
static int fp3same(wd_cpu *c, int U, int a, int opc, uint64_t x, uint64_t y, uint64_t d, int n, uint64_t *out)
{
    uint64_t t;
    switch ((U << 6) | (a << 5) | opc) {
    case 0x18: *out = fpu_maxnm(c, x, y, n); return 1;
    case 0x19: *out = fpu_muladd(c, d, x, y, n); return 1;
    case 0x1A: *out = fpu_add(c, x, y, n); return 1;
    case 0x1B: *out = fpu_mulx(c, x, y, n); return 1;
    case 0x1C: *out = fpu_cmp_eq(c, x, y, n) ? mk(n) : 0; return 1;
    case 0x1E: *out = fpu_max(c, x, y, n); return 1;
    case 0x1F: *out = fpu_recps(c, x, y, n); return 1;
    case 0x38: *out = fpu_minnm(c, x, y, n); return 1;
    case 0x39: *out = fpu_muladd(c, d, fpu_neg(x, n), y, n); return 1;
    case 0x3A: *out = fpu_sub(c, x, y, n); return 1;
    case 0x3E: *out = fpu_min(c, x, y, n); return 1;
    case 0x3F: *out = fpu_rsqrts(c, x, y, n); return 1;
    case 0x5B: *out = fpu_mul(c, x, y, n); return 1;
    case 0x5C: *out = fpu_cmp_ge(c, x, y, n) ? mk(n) : 0; return 1;
    case 0x5D: *out = fpu_cmp_ge(c, fpu_abs(x, n), fpu_abs(y, n), n) ? mk(n) : 0; return 1;
    case 0x5F: *out = fpu_div(c, x, y, n); return 1;
    case 0x7A: t = fpu_sub(c, x, y, n); *out = fpu_abs(t, n); return 1;
    case 0x7C: *out = fpu_cmp_gt(c, x, y, n) ? mk(n) : 0; return 1;
    case 0x7D: *out = fpu_cmp_gt(c, fpu_abs(x, n), fpu_abs(y, n), n) ? mk(n) : 0; return 1;
    default: return 0;
    }
}

/* pairwise FP op (FADDP, FMAXP, ...): U=1 encodings 0x58,0x5A,0x5E,0x78,0x7E */
static int fp_pairop(wd_cpu *c, int a, int opc, uint64_t x, uint64_t y, int n, uint64_t *out)
{
    switch ((a << 5) | opc) {
    case 0x18: *out = fpu_maxnm(c, x, y, n); return 1;
    case 0x1A: *out = fpu_add(c, x, y, n); return 1;
    case 0x1E: *out = fpu_max(c, x, y, n); return 1;
    case 0x38: *out = fpu_minnm(c, x, y, n); return 1;
    case 0x3E: *out = fpu_min(c, x, y, n); return 1;
    default: return 0;
    }
}

/* FP two-register misc on one element. Returns 0 if unallocated. */
static int fp_misc(wd_cpu *c, int U, int a, int opc, uint64_t x, int n, uint64_t *out)
{
    switch ((U << 6) | (a << 5) | opc) {
    case 0x18: *out = fpu_round_int(c, x, n, RM_RN, 0); return 1;
    case 0x19: *out = fpu_round_int(c, x, n, RM_RM, 0); return 1;
    case 0x1A: *out = fpu_to_fixed(c, x, n, 0, 0, RM_RN, n); return 1;
    case 0x1B: *out = fpu_to_fixed(c, x, n, 0, 0, RM_RM, n); return 1;
    case 0x1C: *out = fpu_to_fixed(c, x, n, 0, 0, RM_RNA, n); return 1;
    case 0x1D: *out = fpu_from_fixed(c, x, 1, n, 0, n, fpcr_rmode(c)); return 1;
    case 0x2C: *out = fpu_cmp_gt(c, x, 0, n) ? mk(n) : 0; return 1;
    case 0x2D: *out = fpu_cmp_eq(c, x, 0, n) ? mk(n) : 0; return 1;
    case 0x2E: *out = fpu_cmp_gt(c, 0, x, n) ? mk(n) : 0; return 1;
    case 0x2F: *out = fpu_abs(x, n); return 1;
    case 0x38: *out = fpu_round_int(c, x, n, RM_RP, 0); return 1;
    case 0x39: *out = fpu_round_int(c, x, n, RM_RZ, 0); return 1;
    case 0x3A: *out = fpu_to_fixed(c, x, n, 0, 0, RM_RP, n); return 1;
    case 0x3B: *out = fpu_to_fixed(c, x, n, 0, 0, RM_RZ, n); return 1;
    case 0x3C: if (n != 32) return 0; *out = fpu_urecpe((uint32_t)x); return 1;
    case 0x3D: *out = fpu_recpe(c, x, n); return 1;
    case 0x3F: *out = fpu_recpx(c, x, n); return 1;            /* scalar only */
    case 0x58: *out = fpu_round_int(c, x, n, RM_RNA, 0); return 1;
    case 0x59: *out = fpu_round_int(c, x, n, fpcr_rmode(c), 1); return 1;
    case 0x5A: *out = fpu_to_fixed(c, x, n, 0, 1, RM_RN, n); return 1;
    case 0x5B: *out = fpu_to_fixed(c, x, n, 0, 1, RM_RM, n); return 1;
    case 0x5C: *out = fpu_to_fixed(c, x, n, 0, 1, RM_RNA, n); return 1;
    case 0x5D: *out = fpu_from_fixed(c, x, 0, n, 0, n, fpcr_rmode(c)); return 1;
    case 0x6C: *out = fpu_cmp_ge(c, x, 0, n) ? mk(n) : 0; return 1;
    case 0x6D: *out = fpu_cmp_ge(c, 0, x, n) ? mk(n) : 0; return 1;
    case 0x6F: *out = fpu_neg(x, n); return 1;
    case 0x79: *out = fpu_round_int(c, x, n, fpcr_rmode(c), 0); return 1;
    case 0x7A: *out = fpu_to_fixed(c, x, n, 0, 1, RM_RP, n); return 1;
    case 0x7B: *out = fpu_to_fixed(c, x, n, 0, 1, RM_RZ, n); return 1;
    case 0x7C: if (n != 32) return 0; *out = fpu_ursqrte((uint32_t)x); return 1;
    case 0x7D: *out = fpu_rsqrte(c, x, n); return 1;
    case 0x7F: *out = fpu_sqrt(c, x, n); return 1;
    default: return 0;
    }
}

/* ================================================================ integer helpers */

/* integer 3-same operation on one element; returns 0 if unallocated */
static int int3same(wd_cpu *c, int U, int opc, int size, uint64_t a, uint64_t b, uint64_t d, uint64_t *out, int *q)
{
    int bits = 8 << size;
    uint64_t M = mk(bits);
    int64_t sa = sx(a, bits), sb = sx(b, bits);
    uint64_t ua = a & M, ub = b & M;
    switch (opc) {
    case 0x00: *out = (uint64_t)(U ? (int64_t)((ua + ub) >> 1) : (int64_t)(((__int128)sa + sb) >> 1)) & M; return 1;
    case 0x01: *out = U ? usat((__int128)ua + ub, bits, q) : (uint64_t)ssat((__int128)sa + sb, bits, q) & M; return 1;
    case 0x02: *out = (uint64_t)(U ? (int64_t)(((__int128)ua + ub + 1) >> 1) : (int64_t)(((__int128)sa + sb + 1) >> 1)) & M; return 1;
    case 0x04: *out = (uint64_t)(U ? (int64_t)(((__int128)ua - ub) >> 1) : (int64_t)(((__int128)sa - sb) >> 1)) & M; return 1;
    case 0x05: *out = U ? usat((__int128)ua - ub, bits, q) : (uint64_t)ssat((__int128)sa - sb, bits, q) & M; return 1;
    case 0x06: *out = (U ? ua > ub : sa > sb) ? M : 0; return 1;
    case 0x07: *out = (U ? ua >= ub : sa >= sb) ? M : 0; return 1;
    case 0x08: *out = shl_reg(a, b, bits, U, 0, 0, q); return 1;
    case 0x09: *out = shl_reg(a, b, bits, U, 0, 1, q); return 1;
    case 0x0A: *out = shl_reg(a, b, bits, U, 1, 0, q); return 1;
    case 0x0B: *out = shl_reg(a, b, bits, U, 1, 1, q); return 1;
    case 0x0C: *out = U ? (ua > ub ? ua : ub) : (uint64_t)(sa > sb ? sa : sb) & M; return 1;
    case 0x0D: *out = U ? (ua < ub ? ua : ub) : (uint64_t)(sa < sb ? sa : sb) & M; return 1;
    case 0x0E: case 0x0F: {
        uint64_t ab = U ? (ua > ub ? ua - ub : ub - ua) : (uint64_t)(sa > sb ? (__int128)sa - sb : (__int128)sb - sa);
        *out = (opc == 0x0F ? d + ab : ab) & M;
        return 1;
    }
    case 0x10: *out = (U ? ua - ub : ua + ub) & M; return 1;
    case 0x11: *out = U ? (ua == ub ? M : 0) : ((ua & ub) ? M : 0); return 1;
    case 0x12: *out = (U ? d - ua * ub : d + ua * ub) & M; return 1;
    case 0x13:
        if (!U) { *out = (ua * ub) & M; return 1; }
        if (size) return 0;
        {                                                       /* PMUL (8-bit polynomial) */
            uint64_t r = 0;
            for (int i = 0; i < 8; i++) if ((ua >> i) & 1) r ^= ub << i;
            *out = r & M;
            return 1;
        }
    case 0x16:
        if (size == 0 || size == 3) return 0;
        *out = sqdmulh(a, b, bits, U, q);
        return 1;
    default: return 0;
    }
}

/* ================================================================ AdvSIMD vector */

static int simd_copy(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, op = (insn >> 29) & 1, imm5 = (insn >> 16) & 31, imm4 = (insn >> 11) & 15;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    if (!(imm5 & 15)) return 0;
    int size = __builtin_ctz((unsigned)imm5), esz = 1 << size;
    int idx = imm5 >> (size + 1);
    uint8_t res[16] = { 0 };
    if (op) {                                                   /* INS (element) */
        if (!q) return 0;
        int idx2 = imm4 >> size;
        memcpy(res, VB(c, rd), 16);
        se(res, idx, esz, ge(VB(c, rn), idx2, esz));
        wr_vec(c, rd, res, 1);
        return 1;
    }
    switch (imm4) {
    case 0: {                                                   /* DUP (element) */
        if (size == 3 && !q) return 0;
        uint64_t v = ge(VB(c, rn), idx, esz);
        for (int i = 0; i < (q ? 16 : 8) / esz; i++) se(res, i, esz, v);
        wr_vec(c, rd, res, q);
        return 1;
    }
    case 1: {                                                   /* DUP (general) */
        if (size == 3 && !q) return 0;
        uint64_t v = Xr(c, rn);
        for (int i = 0; i < (q ? 16 : 8) / esz; i++) se(res, i, esz, v);
        wr_vec(c, rd, res, q);
        return 1;
    }
    case 3:                                                     /* INS (general) */
        if (!q) return 0;
        memcpy(res, VB(c, rd), 16);
        se(res, idx, esz, Xr(c, rn));
        wr_vec(c, rd, res, 1);
        return 1;
    case 5: {                                                   /* SMOV */
        if (size == 3 || (size == 2 && !q)) return 0;
        uint64_t v = (uint64_t)sx(ge(VB(c, rn), idx, esz), 8 * esz);
        setXr(c, rd, q ? v : (uint32_t)v);
        return 1;
    }
    case 7:                                                     /* UMOV */
        if ((q && size != 3) || (!q && size == 3)) return 0;
        setXr(c, rd, ge(VB(c, rn), idx, esz));
        return 1;
    default: return 0;
    }
}

static int simd_3same(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 11) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    const uint8_t *A = VB(c, rn), *B = VB(c, rm), *D = VB(c, rd);
    uint8_t res[16] = { 0 };
    int bytes = q ? 16 : 8;

    if (opc == 0x03) {                                          /* bitwise */
        for (int i = 0; i < bytes; i++) {
            uint8_t a = A[i], b = B[i], d = D[i], r;
            switch ((U << 2) | size) {
            case 0: r = a & b; break;
            case 1: r = a & (uint8_t)~b; break;
            case 2: r = a | b; break;
            case 3: r = a | (uint8_t)~b; break;
            case 4: r = a ^ b; break;
            case 5: r = (uint8_t)((d & a) | (~d & b)); break;      /* BSL */
            case 6: r = (uint8_t)((b & a) | (~b & d)); break;      /* BIT */
            default: r = (uint8_t)((~b & a) | (b & d)); break;     /* BIF */
            }
            res[i] = r;
        }
        wr_vec(c, rd, res, q);
        return 1;
    }
    if (opc >= 0x18) {                                          /* FP */
        int a = size >> 1, n = (size & 1) ? 64 : 32;
        if (n == 64 && !q) return 0;
        int esz = n / 8, cnt = bytes / esz;
        if (U && (opc == 0x18 || opc == 0x1A || opc == 0x1E) && !(a == 1 && opc == 0x1A)) {   /* pairwise */
            uint8_t cat[32];
            memcpy(cat, A, (size_t)bytes);
            memcpy(cat + bytes, B, (size_t)bytes);
            for (int i = 0; i < cnt; i++) {
                uint64_t r;
                if (!fp_pairop(c, a, opc, ge(cat, 2 * i, esz), ge(cat, 2 * i + 1, esz), n, &r)) return 0;
                se(res, i, esz, r);
            }
            wr_vec(c, rd, res, q);
            return 1;
        }
        for (int i = 0; i < cnt; i++) {
            uint64_t r;
            if (!fp3same(c, U, a, opc, ge(A, i, esz), ge(B, i, esz), ge(D, i, esz), n, &r)) return 0;
            se(res, i, esz, r);
        }
        wr_vec(c, rd, res, q);
        return 1;
    }
    if (size == 3 && !q && opc != 0x03) return 0;
    int esz = 1 << size, cnt = bytes / esz, qc = 0;
    if (opc == 0x14 || opc == 0x15 || opc == 0x17) {            /* integer pairwise */
        if (opc == 0x17 && U) return 0;
        if (size == 3 && opc != 0x17) return 0;
        uint8_t cat[32];
        memcpy(cat, A, (size_t)bytes);
        memcpy(cat + bytes, B, (size_t)bytes);
        int bits = 8 * esz;
        for (int i = 0; i < cnt; i++) {
            uint64_t x = ge(cat, 2 * i, esz), y = ge(cat, 2 * i + 1, esz), r;
            if (opc == 0x17) r = x + y;
            else {
                int mx = opc == 0x14;
                if (U) r = mx ? (x > y ? x : y) : (x < y ? x : y);
                else {
                    int64_t a1 = sx(x, bits), b1 = sx(y, bits);
                    r = (uint64_t)(mx ? (a1 > b1 ? a1 : b1) : (a1 < b1 ? a1 : b1));
                }
            }
            se(res, i, esz, r & mk(bits));
        }
        wr_vec(c, rd, res, q);
        return 1;
    }
    if ((opc == 0x12 || opc == 0x13 || opc == 0x0F || opc == 0x0E || opc == 0x0C || opc == 0x0D ||
         opc == 0x00 || opc == 0x02 || opc == 0x04) && size == 3)
        return 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t r;
        if (!int3same(c, U, opc, size, ge(A, i, esz), ge(B, i, esz), ge(D, i, esz), &r, &qc)) return 0;
        se(res, i, esz, r);
    }
    if (qc) c->fpsr |= FPSR_QC;
    wr_vec(c, rd, res, q);
    return 1;
}

static int simd_3diff(wd_cpu *c, uint32_t insn, int scalar)
{
    int q = (insn >> 30) & 1, U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 12) & 15;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (size == 3 && !(opc == 14 && !U)) return 0;
    int esz = 1 << size, bits = 8 * esz, wesz = 2 * esz, wbits = 2 * bits;
    int cnt = scalar ? 1 : 8 / esz;
    int part = scalar ? 0 : q;
    const uint8_t *A = VB(c, rn), *B = VB(c, rm), *D = VB(c, rd);
    uint8_t res[16] = { 0 };
    int qc = 0;
    if (scalar && opc != 9 && opc != 11 && opc != 13) return 0;
    if (scalar && (size == 0 || size == 3)) return 0;

    if (opc == 4 || opc == 6) {                                 /* ADDHN/SUBHN (+R) */
        if (size == 3) return 0;
        memcpy(res, D, 16);
        for (int i = 0; i < cnt; i++) {
            uint64_t a = ge(A, i, wesz), b = ge(B, i, wesz);
            unsigned __int128 s = opc == 4 ? (unsigned __int128)a + b : (unsigned __int128)a - b;
            if (U) s += (unsigned __int128)1 << (bits - 1);
            se(res, i + (q ? cnt : 0), esz, (uint64_t)(s >> bits) & mk(bits));
        }
        if (!q) memset(res + 8, 0, 8);
        wr_vec(c, rd, res, 1);
        return 1;
    }
    for (int i = 0; i < cnt; i++) {
        uint64_t n_e = (opc == 1 || opc == 3) ? ge(A, i, wesz) : ge(A, i + (part ? cnt : 0), esz);
        uint64_t m_e = ge(B, i + (part ? cnt : 0), esz);
        __int128 a = (opc == 1 || opc == 3) ? (U ? (__int128)(n_e & mk(wbits)) : (__int128)sx(n_e, wbits))
                                            : (U ? (__int128)(n_e & mk(bits)) : (__int128)sx(n_e, bits));
        __int128 b = U ? (__int128)(m_e & mk(bits)) : (__int128)sx(m_e, bits);
        __int128 acc = U ? (__int128)(ge(D, i, wesz) & mk(wbits)) : (__int128)sx(ge(D, i, wesz), wbits);
        __int128 r;
        switch (opc) {
        case 0: case 1: r = a + b; break;
        case 2: case 3: r = a - b; break;
        case 5: r = acc + (a > b ? a - b : b - a); break;
        case 7: r = a > b ? a - b : b - a; break;
        case 8: r = acc + a * b; break;
        case 10: r = acc - a * b; break;
        case 12: r = a * b; break;
        case 9: case 11: case 13: {                             /* SQDMLAL/SQDMLSL/SQDMULL */
            if (U) return 0;
            __int128 p = ssat(a * b * 2, wbits, &qc);
            if (opc == 9) r = ssat(acc + p, wbits, &qc);
            else if (opc == 11) r = ssat(acc - p, wbits, &qc);
            else r = p;
            break;
        }
        case 14: {                                              /* PMULL */
            if (U || (size != 0)) return 0;
            uint64_t x = (uint64_t)a & 0xFF, y = (uint64_t)b & 0xFF, p = 0;
            for (int k = 0; k < 8; k++) if ((x >> k) & 1) p ^= y << k;
            r = (__int128)p;
            break;
        }
        default: return 0;
        }
        se(res, i, wesz, (uint64_t)r & mk(wbits));
    }
    if (qc) c->fpsr |= FPSR_QC;
    wr_vec(c, rd, res, 1);
    if (scalar) wr_scalar(c, rd, ge(res, 0, wesz), wesz);
    return 1;
}

static uint64_t sat_narrow(int64_t v_signed, uint64_t v_unsigned, int from_signed, int to_signed, int bits, int *q)
{
    if (from_signed) return to_signed ? (uint64_t)ssat(v_signed, bits, q) & mk(bits) : usat(v_signed, bits, q);
    return to_signed ? (uint64_t)ssat((__int128)v_unsigned, bits, q) & mk(bits) : usat((__int128)v_unsigned, bits, q);
}

static int simd_misc(wd_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 0 : (insn >> 30) & 1, U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 12) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    const uint8_t *A = VB(c, rn);
    uint8_t res[16] = { 0 };
    int bytes = q ? 16 : 8, qc = 0;

    /* FP group: opcodes 0x0C-0x0F with size<1>=1, and 0x16-0x1F */
    int fpgroup = (opc >= 0x18) || ((opc >= 0x0C && opc <= 0x0F) && (size & 2)) || (opc == 0x16 && U);
    if (opc == 0x16 || opc == 0x17) {
        if (opc == 0x17 && !U && !scalar) {                    /* FCVTL */
            int from = (size & 1) ? 32 : 16, to = 2 * from;
            int cnt = 8 / (from / 8);
            for (int i = 0; i < cnt; i++)
                se(res, i, to / 8, fpu_convert(c, ge(A, i + (q ? cnt : 0), from / 8), from, to, fpcr_rmode(c)));
            wr_vec(c, rd, res, 1);
            return 1;
        }
        if (opc == 0x16) {                                      /* FCVTN / FCVTXN */
            int to = (size & 1) ? 32 : 16, from = 2 * to;
            if (U && to != 32) return 0;
            int cnt = scalar ? 1 : 8 / (to / 8);
            if (scalar && !U) return 0;
            uint8_t out[16];
            memcpy(out, VB(c, rd), 16);
            for (int i = 0; i < cnt; i++) {
                uint64_t r = fpu_convert(c, ge(A, i, from / 8), from, to, U ? RM_ODD : fpcr_rmode(c));
                se(out, i + (q ? cnt : 0), to / 8, r);
            }
            if (scalar) { wr_scalar(c, rd, ge(out, 0, 4), 4); return 1; }
            if (!q) memset(out + 8, 0, 8);
            wr_vec(c, rd, out, 1);
            return 1;
        }
        return 0;
    }
    if (fpgroup) {
        int n = (size & 1) ? 64 : 32, a = (size >> 1) & 1;
        if (!scalar && n == 64 && !q) return 0;
        if (!scalar && opc == 0x1F && !U) return 0;              /* FRECPX is scalar only */
        int esz = n / 8, cnt = scalar ? 1 : bytes / esz;
        for (int i = 0; i < cnt; i++) {
            uint64_t r;
            if (!fp_misc(c, U, a, opc, ge(A, i, esz), n, &r)) return 0;
            se(res, i, esz, r);
        }
        if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
        else wr_vec(c, rd, res, q);
        return 1;
    }

    int esz = 1 << size, bits = 8 * esz, cnt = scalar ? 1 : bytes / esz;
    uint64_t M = mk(bits);
    switch (opc) {
    case 0x00: case 0x01: {                                     /* REV64 / REV32 / REV16 */
        if (scalar) return 0;
        if (opc == 1 && U) return 0;
        int cont = opc == 1 ? 16 : (U ? 32 : 64);
        if (cont / 8 <= esz) return 0;
        int cb = cont / 8;
        for (int base = 0; base < bytes; base += cb)
            for (int i = 0; i < cb / esz; i++)
                se(res + base, i, esz, ge(A + base, cb / esz - 1 - i, esz));
        break;
    }
    case 0x02: case 0x06: {                                     /* SADDLP/UADDLP, SADALP/UADALP */
        if (scalar || size == 3) return 0;
        int wesz = 2 * esz, wcnt = bytes / wesz;
        for (int i = 0; i < wcnt; i++) {
            uint64_t x = ge(A, 2 * i, esz), y = ge(A, 2 * i + 1, esz);
            int64_t s = U ? (int64_t)(x + y) : sx(x, bits) + sx(y, bits);
            if (opc == 0x06) s += (int64_t)ge(VB(c, rd), i, wesz);
            se(res, i, wesz, (uint64_t)s & mk(2 * bits));
        }
        break;
    }
    case 0x03: {                                                /* SUQADD / USQADD */
        if (size == 3 && !q && !scalar) return 0;
        for (int i = 0; i < cnt; i++) {
            uint64_t a = ge(A, i, esz), d = ge(VB(c, rd), i, esz), r;
            if (!U) r = (uint64_t)ssat((__int128)sx(d, bits) + (__int128)(a & M), bits, &qc) & M;
            else r = usat((__int128)(d & M) + sx(a, bits), bits, &qc);
            se(res, i, esz, r);
        }
        break;
    }
    case 0x04:                                                  /* CLS / CLZ */
        if (scalar || size == 3) return 0;
        for (int i = 0; i < cnt; i++) {
            uint64_t a = ge(A, i, esz) & M, r = 0;
            if (U) { for (int k = bits - 1; k >= 0 && !((a >> k) & 1); k--) r++; }
            else {
                uint64_t t = ((a >> 1) ^ a) & mk(bits - 1);
                for (int k = bits - 2; k >= 0 && !((t >> k) & 1); k--) r++;
            }
            se(res, i, esz, r);
        }
        break;
    case 0x05:                                                  /* CNT / NOT / RBIT */
        if (scalar) return 0;
        if (!U) {
            if (size) return 0;
            for (int i = 0; i < bytes; i++) res[i] = (uint8_t)__builtin_popcount(A[i]);
        } else if (size == 0) {
            for (int i = 0; i < bytes; i++) res[i] = (uint8_t)~A[i];
        } else if (size == 1) {
            for (int i = 0; i < bytes; i++) {
                uint8_t v = A[i], r = 0;
                for (int k = 0; k < 8; k++) r |= (uint8_t)(((v >> k) & 1) << (7 - k));
                res[i] = r;
            }
        } else return 0;
        break;
    case 0x07:                                                  /* SQABS / SQNEG */
        if (size == 3 && !q && !scalar) return 0;
        for (int i = 0; i < cnt; i++) {
            __int128 v = sx(ge(A, i, esz), bits);
            __int128 r = U ? -v : (v < 0 ? -v : v);
            se(res, i, esz, (uint64_t)ssat(r, bits, &qc) & M);
        }
        break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: {               /* compares with zero, ABS, NEG */
        if ((size == 3 && !q && !scalar) || (scalar && size != 3)) return 0;
        if (opc == 0x0A && U) return 0;
        for (int i = 0; i < cnt; i++) {
            int64_t v = sx(ge(A, i, esz), bits);
            uint64_t r;
            switch ((opc << 1) | U) {
            case 0x10: r = v > 0 ? M : 0; break;                /* CMGT #0 */
            case 0x11: r = v >= 0 ? M : 0; break;               /* CMGE #0 */
            case 0x12: r = v == 0 ? M : 0; break;               /* CMEQ #0 */
            case 0x13: r = v <= 0 ? M : 0; break;               /* CMLE #0 */
            case 0x14: r = v < 0 ? M : 0; break;                /* CMLT #0 */
            case 0x16: r = (uint64_t)(v < 0 ? -(__int128)v : v) & M; break;   /* ABS */
            default: r = (uint64_t)(-(__int128)v) & M; break;   /* NEG */
            }
            se(res, i, esz, r);
        }
        break;
    }
    case 0x12: case 0x14: {                                     /* XTN, SQXTN, SQXTUN, UQXTN */
        if (size == 3) return 0;
        if (opc == 0x12 && U == 0 && scalar) return 0;
        int ncnt = scalar ? 1 : 8 / esz;
        uint8_t out[16];
        memcpy(out, VB(c, rd), 16);
        for (int i = 0; i < ncnt; i++) {
            uint64_t w = ge(A, i, 2 * esz);
            uint64_t r;
            int64_t sw = sx(w, 2 * bits);
            if (opc == 0x12 && !U) r = w & M;                           /* XTN */
            else if (opc == 0x12) r = sat_narrow(sw, w, 1, 0, bits, &qc);    /* SQXTUN */
            else if (!U) r = sat_narrow(sw, w, 1, 1, bits, &qc);           /* SQXTN */
            else r = sat_narrow(sw, w & mk(2 * bits), 0, 0, bits, &qc);    /* UQXTN */
            se(out, i + (q ? ncnt : 0), esz, r);
        }
        if (qc) c->fpsr |= FPSR_QC;
        if (scalar) { wr_scalar(c, rd, ge(out, 0, esz), esz); return 1; }
        if (!q) memset(out + 8, 0, 8);
        wr_vec(c, rd, out, 1);
        return 1;
    }
    case 0x13: {                                                /* SHLL */
        if (!U || scalar || size == 3) return 0;
        int ncnt = 8 / esz;
        for (int i = 0; i < ncnt; i++)
            se(res, i, 2 * esz, (ge(A, i + (q ? ncnt : 0), esz) & M) << bits);
        wr_vec(c, rd, res, 1);
        return 1;
    }
    default:
        return 0;
    }
    if (qc) c->fpsr |= FPSR_QC;
    if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
    else wr_vec(c, rd, res, q);
    return 1;
}

static int simd_across(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 12) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    const uint8_t *A = VB(c, rn);
    int bytes = q ? 16 : 8;
    if (opc == 0x0C || opc == 0x0F) {                           /* FMAXNMV/FMAXV/FMINNMV/FMINV */
        if (!U || !q || (size & 1)) return 0;
        int mn = size >> 1;
        uint64_t (*f)(wd_cpu *, uint64_t, uint64_t, int);
        if (opc == 0x0C) f = mn ? fpu_minnm : fpu_maxnm;
        else f = mn ? fpu_min : fpu_max;
        /* pairwise tree reduction as in the ARM pseudocode */
        uint64_t l = f(c, ge(A, 0, 4), ge(A, 1, 4), 32), h = f(c, ge(A, 2, 4), ge(A, 3, 4), 32);
        wr_scalar(c, rd, f(c, l, h, 32), 4);
        return 1;
    }
    if (size == 3 || (size == 2 && !q)) return 0;
    int esz = 1 << size, bits = 8 * esz, cnt = bytes / esz;
    uint64_t M = mk(bits);
    switch (opc) {
    case 0x03: {                                                /* SADDLV / UADDLV */
        int64_t s = 0;
        for (int i = 0; i < cnt; i++) s += U ? (int64_t)(ge(A, i, esz) & M) : sx(ge(A, i, esz), bits);
        wr_scalar(c, rd, (uint64_t)s & mk(2 * bits), 2 * esz);
        return 1;
    }
    case 0x0A: case 0x1A: {                                     /* MAXV / MINV */
        int mx = opc == 0x0A;
        uint64_t best = ge(A, 0, esz);
        for (int i = 1; i < cnt; i++) {
            uint64_t v = ge(A, i, esz);
            int better = U ? (mx ? v > best : v < best) : (mx ? sx(v, bits) > sx(best, bits) : sx(v, bits) < sx(best, bits));
            if (better) best = v;
        }
        wr_scalar(c, rd, best & M, esz);
        return 1;
    }
    case 0x1B: {                                                /* ADDV */
        if (U) return 0;
        uint64_t s = 0;
        for (int i = 0; i < cnt; i++) s += ge(A, i, esz);
        wr_scalar(c, rd, s & M, esz);
        return 1;
    }
    default: return 0;
    }
}

static uint64_t expand_imm(int op, int cmode, uint64_t imm8, int *ok)
{
    uint64_t imm64 = 0, imm;
    *ok = 1;
    switch (cmode >> 1) {
    case 0: imm = imm8; goto rep32;
    case 1: imm = imm8 << 8; goto rep32;
    case 2: imm = imm8 << 16; goto rep32;
    case 3: imm = imm8 << 24; goto rep32;
    case 4: imm = imm8; goto rep16;
    case 5: imm = imm8 << 8; goto rep16;
    case 6:
        imm = (cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF);
        goto rep32;
    default:
        if (!(cmode & 1)) {
            if (!op) { for (int i = 0; i < 8; i++) imm64 |= imm8 << (8 * i); }
            else { for (int i = 0; i < 8; i++) if ((imm8 >> i) & 1) imm64 |= 0xFFULL << (8 * i); }
            return imm64;
        }
        if (!op) { imm = vfp_expand_imm((uint32_t)imm8, 32); goto rep32; }
        return vfp_expand_imm((uint32_t)imm8, 64);
    }
rep32:
    return imm | (imm << 32);
rep16:
    return imm | (imm << 16) | (imm << 32) | (imm << 48);
}

static int simd_modimm(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, op = (insn >> 29) & 1, cmode = (insn >> 12) & 15, rd = insn & 31;
    if ((insn >> 11) & 1) return 0;
    uint64_t imm8 = (((insn >> 16) & 7) << 5) | ((insn >> 5) & 31);
    if (cmode == 15 && op && !q) return 0;
    int ok;
    uint64_t imm = expand_imm(op, cmode, imm8, &ok);
    uint64_t lo = c->v[rd][0], hi = c->v[rd][1];
    int is_orr_bic = (cmode < 12) && (cmode & 1);
    if (is_orr_bic) {
        if (op) { lo &= ~imm; hi &= ~imm; }
        else { lo |= imm; hi |= imm; }
    } else {
        int mvn = op && cmode < 14;
        lo = hi = mvn ? ~imm : imm;
    }
    c->v[rd][0] = lo;
    c->v[rd][1] = q ? hi : 0;
    return 1;
}

static int simd_shift_imm(wd_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 0 : (insn >> 30) & 1, U = (insn >> 29) & 1, immh = (insn >> 19) & 15, immb = (insn >> 16) & 7;
    int opc = (insn >> 11) & 31, rd = insn & 31, rn = (insn >> 5) & 31;
    if (!immh) return 0;
    int hb = 31 - __builtin_clz((unsigned)immh);
    int esz = 1 << hb, bits = 8 * esz;
    int immv = (immh << 3) | immb;
    const uint8_t *A = VB(c, rn);
    uint8_t res[16] = { 0 };
    int bytes = q ? 16 : 8, qc = 0;
    uint64_t M = mk(bits);
    int narrow = opc >= 0x10 && opc <= 0x13, longer = opc == 0x14;

    if (narrow || longer) {
        if (hb == 3) return 0;
        if (scalar && longer) return 0;
        if (longer) {                                           /* SSHLL / USHLL */
            int sh = immv - bits, cnt = 8 / esz;
            for (int i = 0; i < cnt; i++) {
                uint64_t e = ge(A, i + (q ? cnt : 0), esz);
                uint64_t w = U ? (e & M) : (uint64_t)sx(e, bits);
                se(res, i, 2 * esz, (w << sh) & mk(2 * bits));
            }
            wr_vec(c, rd, res, 1);
            return 1;
        }
        int sh = 2 * bits - immv, cnt = scalar ? 1 : 8 / esz;
        int round = opc & 1;
        uint8_t out[16];
        memcpy(out, VB(c, rd), 16);
        for (int i = 0; i < cnt; i++) {
            uint64_t w = ge(A, i, 2 * esz);
            int src_signed = !(opc >= 0x12 && U);
            __int128 v = src_signed ? (__int128)sx(w, 2 * bits) : (__int128)(w & mk(2 * bits));
            if (opc <= 0x11 && !U) v = (__int128)(w & mk(2 * bits));   /* SHRN/RSHRN: plain bits */
            if (round) v += (__int128)1 << (sh - 1);
            v >>= sh;
            uint64_t r;
            if (opc <= 0x11 && !U) { if (scalar) return 0; r = (uint64_t)v & M; }
            else if (opc <= 0x11) r = usat(v, bits, &qc);                  /* SQSHRUN / SQRSHRUN */
            else if (!U) r = (uint64_t)ssat(v, bits, &qc) & M;             /* SQSHRN / SQRSHRN */
            else r = usat(v, bits, &qc);                                   /* UQSHRN / UQRSHRN */
            se(out, i + (q ? cnt : 0), esz, r);
        }
        if (qc) c->fpsr |= FPSR_QC;
        if (scalar) { wr_scalar(c, rd, ge(out, 0, esz), esz); return 1; }
        if (!q) memset(out + 8, 0, 8);
        wr_vec(c, rd, out, 1);
        return 1;
    }
    if (hb == 3 && !q && !scalar) return 0;
    int rsh = 2 * bits - immv, lsh = immv - bits;
    int cnt = scalar ? 1 : bytes / esz;

    if (opc == 0x1C || opc == 0x1F) {                           /* fixed-point conversions */
        if (hb < 2) return 0;
        int n = bits, fbits = rsh;
        for (int i = 0; i < cnt; i++) {
            uint64_t e = ge(A, i, esz), r;
            if (opc == 0x1C) r = fpu_from_fixed(c, e, !U, n, fbits, n, fpcr_rmode(c));
            else r = fpu_to_fixed(c, e, n, fbits, U, RM_RZ, n);
            se(res, i, esz, r);
        }
        if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
        else wr_vec(c, rd, res, q);
        return 1;
    }
    if (scalar && hb != 3 && !(opc == 0x0E || opc == 0x0C)) return 0;
    for (int i = 0; i < cnt; i++) {
        uint64_t e = ge(A, i, esz) & M, d = ge(VB(c, rd), i, esz) & M, r;
        __int128 sv = U ? (__int128)e : (__int128)sx(e, bits);
        switch (opc) {
        case 0x00: case 0x02: case 0x04: case 0x06: {          /* [S|U][R]SHR, [S|U][R]SRA */
            __int128 v = sv;
            if (opc & 4) v += (__int128)1 << (rsh - 1);
            v >>= rsh;
            r = (uint64_t)v & M;
            if (opc & 2) r = (r + d) & M;
            break;
        }
        case 0x08:                                              /* SRI */
            if (!U) return 0;
            {
                uint64_t keep = rsh >= bits ? M : (M & ~(M >> rsh));
                r = (d & keep) | (rsh >= bits ? 0 : (e >> rsh));
            }
            break;
        case 0x0A:                                              /* SHL / SLI */
            r = (e << lsh) & M;
            if (U) r |= d & (lsh ? mk(lsh) : 0);
            break;
        case 0x0C:                                              /* SQSHLU */
            if (!U) return 0;
            r = usat(((__int128)sx(e, bits)) * ((__int128)1 << lsh), bits, &qc);
            break;
        case 0x0E:                                              /* SQSHL / UQSHL (imm) */
            r = U ? usat((__int128)e * ((__int128)1 << lsh), bits, &qc)
                  : (uint64_t)ssat((__int128)sx(e, bits) * ((__int128)1 << lsh), bits, &qc) & M;
            break;
        default: return 0;
        }
        se(res, i, esz, r);
    }
    if (qc) c->fpsr |= FPSR_QC;
    if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
    else wr_vec(c, rd, res, q);
    return 1;
}

static int simd_indexed(wd_cpu *c, uint32_t insn, int scalar)
{
    int q = scalar ? 0 : (insn >> 30) & 1, U = (insn >> 29) & 1, size = (insn >> 22) & 3;
    int L = (insn >> 21) & 1, Mb = (insn >> 20) & 1, rm = (insn >> 16) & 15, opc = (insn >> 12) & 15, H = (insn >> 11) & 1;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    const uint8_t *A = VB(c, rn), *D = VB(c, rd);
    uint8_t res[16] = { 0 };
    int bytes = q ? 16 : 8, qc = 0;

    int fp = (opc == 1 || opc == 5 || opc == 9) && (size & 2);
    if (fp) {
        int n = (size & 1) ? 64 : 32, idx;
        int rmf = (Mb << 4) | rm;
        if (n == 32) idx = (H << 1) | L;
        else { if (L) return 0; idx = H; if (!q && !scalar) return 0; }
        int esz = n / 8, cnt = scalar ? 1 : bytes / esz;
        uint64_t e2 = ge(VB(c, rmf), idx, esz);
        for (int i = 0; i < cnt; i++) {
            uint64_t a = ge(A, i, esz), d = ge(D, i, esz), r;
            if (opc == 1) r = fpu_muladd(c, d, a, e2, n);
            else if (opc == 5) r = fpu_muladd(c, d, fpu_neg(a, n), e2, n);
            else r = U ? fpu_mulx(c, a, e2, n) : fpu_mul(c, a, e2, n);
            se(res, i, esz, r);
        }
        if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
        else wr_vec(c, rd, res, q);
        return 1;
    }
    if (size == 0 || size == 3) return 0;
    int idx, rmi;
    if (size == 1) { idx = (H << 2) | (L << 1) | Mb; rmi = rm; }
    else { idx = (H << 1) | L; rmi = (Mb << 4) | rm; }
    int esz = 1 << size, bits = 8 * esz;
    uint64_t M = mk(bits);
    uint64_t e2 = ge(VB(c, rmi), idx, esz);

    int is_long = opc == 2 || opc == 3 || opc == 6 || opc == 7 || opc == 10 || opc == 11;
    if (is_long) {
        if ((opc == 3 || opc == 7 || opc == 11) && U) return 0;
        int cnt = scalar ? 1 : 8 / esz;
        if (scalar && !(opc == 3 || opc == 7 || opc == 11)) return 0;
        for (int i = 0; i < cnt; i++) {
            uint64_t a = ge(A, i + (q ? cnt : 0), esz);
            __int128 x = U ? (__int128)(a & M) : sx(a, bits), y = U ? (__int128)(e2 & M) : sx(e2, bits);
            __int128 acc = U ? (__int128)(ge(D, i, 2 * esz) & mk(2 * bits)) : sx(ge(D, i, 2 * esz), 2 * bits), r;
            switch (opc) {
            case 2: r = acc + x * y; break;
            case 6: r = acc - x * y; break;
            case 10: r = x * y; break;
            default: {
                __int128 p = ssat(x * y * 2, 2 * bits, &qc);
                if (opc == 3) r = ssat(acc + p, 2 * bits, &qc);
                else if (opc == 7) r = ssat(acc - p, 2 * bits, &qc);
                else r = p;
            }
            }
            se(res, i, 2 * esz, (uint64_t)r & mk(2 * bits));
        }
        if (qc) c->fpsr |= FPSR_QC;
        if (scalar) wr_scalar(c, rd, ge(res, 0, 2 * esz), 2 * esz);
        else wr_vec(c, rd, res, 1);
        return 1;
    }
    int cnt = scalar ? 1 : bytes / esz;
    for (int i = 0; i < cnt; i++) {
        uint64_t a = ge(A, i, esz), d = ge(D, i, esz), r;
        switch ((opc << 1) | U) {
        case 0x01: if (scalar) return 0; r = (d + a * e2) & M; break;            /* MLA */
        case 0x09: if (scalar) return 0; r = (d - a * e2) & M; break;            /* MLS */
        case 0x10: if (scalar) return 0; r = (a * e2) & M; break;                /* MUL */
        case 0x18: r = sqdmulh(a, e2, bits, 0, &qc); break;                      /* SQDMULH */
        case 0x1A: r = sqdmulh(a, e2, bits, 1, &qc); break;                      /* SQRDMULH */
        default: return 0;
        }
        se(res, i, esz, r);
    }
    if (qc) c->fpsr |= FPSR_QC;
    if (scalar) wr_scalar(c, rd, ge(res, 0, esz), esz);
    else wr_vec(c, rd, res, q);
    return 1;
}

static int simd_permute(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, size = (insn >> 22) & 3, opc = (insn >> 12) & 7;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (size == 3 && !q) return 0;
    int esz = 1 << size, cnt = (q ? 16 : 8) / esz, half = cnt / 2;
    const uint8_t *A = VB(c, rn), *B = VB(c, rm);
    uint8_t res[16] = { 0 };
    int part = opc >> 2;
    switch (opc & 3) {
    case 1:                                                     /* UZP */
        for (int i = 0; i < half; i++) {
            se(res, i, esz, ge(A, 2 * i + part, esz));
            se(res, i + half, esz, ge(B, 2 * i + part, esz));
        }
        break;
    case 2:                                                     /* TRN */
        for (int i = 0; i < half; i++) {
            se(res, 2 * i, esz, ge(A, 2 * i + part, esz));
            se(res, 2 * i + 1, esz, ge(B, 2 * i + part, esz));
        }
        break;
    case 3:                                                     /* ZIP */
        for (int i = 0; i < half; i++) {
            se(res, 2 * i, esz, ge(A, i + part * half, esz));
            se(res, 2 * i + 1, esz, ge(B, i + part * half, esz));
        }
        break;
    default: return 0;
    }
    wr_vec(c, rd, res, q);
    return 1;
}

static int simd_ext(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, imm4 = (insn >> 11) & 15;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (!q && (imm4 & 8)) return 0;
    int bytes = q ? 16 : 8;
    uint8_t cat[32], res[16] = { 0 };
    memcpy(cat, VB(c, rn), (size_t)bytes);
    memcpy(cat + bytes, VB(c, rm), (size_t)bytes);
    memcpy(res, cat + imm4, (size_t)bytes);
    wr_vec(c, rd, res, q);
    return 1;
}

static int simd_tbl(wd_cpu *c, uint32_t insn)
{
    int q = (insn >> 30) & 1, len = ((insn >> 13) & 3) + 1, tbx = (insn >> 12) & 1;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    uint8_t table[64], res[16], idx[16];
    for (int i = 0; i < len; i++) memcpy(table + 16 * i, VB(c, (rn + i) % 32), 16);
    memcpy(idx, VB(c, rm), 16);
    memcpy(res, VB(c, rd), 16);
    int bytes = q ? 16 : 8;
    for (int i = 0; i < bytes; i++) {
        if (idx[i] < 16 * len) res[i] = table[idx[i]];
        else if (!tbx) res[i] = 0;
    }
    wr_vec(c, rd, res, q);
    return 1;
}

/* ================================================================ AdvSIMD scalar */

static int scalar_3same(wd_cpu *c, uint32_t insn)
{
    int U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 11) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    if (opc >= 0x18) {
        int a = size >> 1, n = (size & 1) ? 64 : 32;
        uint64_t r;
        /* scalar FP 3-same: FMULX, FCMEQ, FRECPS, FRSQRTS, FCMGE, FACGE, FABD, FCMGT, FACGT */
        int k = (U << 6) | (a << 5) | opc;
        if (k != 0x1B && k != 0x1C && k != 0x1F && k != 0x3F && k != 0x5C && k != 0x5D && k != 0x7A && k != 0x7C && k != 0x7D)
            return 0;
        if (!fp3same(c, U, a, opc, rd_scalar(c, rn, n / 8), rd_scalar(c, rm, n / 8), 0, n, &r)) return 0;
        wr_scalar(c, rd, r, n / 8);
        return 1;
    }
    int qc = 0;
    switch (opc) {
    case 0x01: case 0x05: case 0x09: case 0x0B: break;          /* saturating add/sub/shift: any size */
    case 0x06: case 0x07: case 0x08: case 0x0A: case 0x10: case 0x11:
        if (size != 3) return 0;
        break;
    case 0x16: if (size != 1 && size != 2) return 0; break;
    default: return 0;
    }
    int esz = 1 << size;
    uint64_t r;
    if (!int3same(c, U, opc, size, rd_scalar(c, rn, esz), rd_scalar(c, rm, esz), 0, &r, &qc)) return 0;
    if (qc) c->fpsr |= FPSR_QC;
    wr_scalar(c, rd, r, esz);
    return 1;
}

static int scalar_pairwise(wd_cpu *c, uint32_t insn)
{
    int U = (insn >> 29) & 1, size = (insn >> 22) & 3, opc = (insn >> 12) & 31;
    int rd = insn & 31, rn = (insn >> 5) & 31;
    const uint8_t *A = VB(c, rn);
    if (!U && opc == 0x1B) {                                    /* ADDP (scalar) */
        if (size != 3) return 0;
        wr_scalar(c, rd, ge(A, 0, 8) + ge(A, 1, 8), 8);
        return 1;
    }
    if (!U) return 0;                                           /* FP16 forms */
    int n = (size & 1) ? 64 : 32, a = size >> 1, esz = n / 8;
    int fopc = opc == 0x0C ? 0x18 : opc == 0x0D ? 0x1A : opc == 0x0F ? 0x1E : -1;
    if (fopc < 0 || (fopc == 0x1A && a)) return 0;
    uint64_t r;
    if (!fp_pairop(c, a, fopc, ge(A, 0, esz), ge(A, 1, esz), n, &r)) return 0;
    wr_scalar(c, rd, r, esz);
    return 1;
}

static int scalar_copy(wd_cpu *c, uint32_t insn)
{
    int imm5 = (insn >> 16) & 31, rd = insn & 31, rn = (insn >> 5) & 31;
    if (!(imm5 & 15) || ((insn >> 11) & 15)) return 0;
    int size = __builtin_ctz((unsigned)imm5), esz = 1 << size, idx = imm5 >> (size + 1);
    wr_scalar(c, rd, ge(VB(c, rn), idx, esz), esz);
    return 1;
}

/* ================================================================ dispatcher */

void wd_simd_dp(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    wd_fp_check(m);
    int ok = 0;
    if (!((insn >> 30) & 1) && ((insn >> 28) & 1)) {            /* scalar FP / conversions */
        ok = fp_scalar(m, insn);
    } else if ((insn & 0x9F000000u) == 0x0E000000u) {           /* AdvSIMD vector, 01110 */
        if ((insn & 0xBF208C00u) == 0x0E000000u) ok = simd_tbl(c, insn);
        else if ((insn & 0xBF208C00u) == 0x0E000800u) ok = simd_permute(c, insn);
        else if ((insn & 0xBFE08400u) == 0x2E000000u) ok = simd_ext(c, insn);
        else if ((insn & 0x9FE08400u) == 0x0E000400u) ok = simd_copy(c, insn);
        else if ((insn & 0x9F200400u) == 0x0E200400u) ok = simd_3same(c, insn);
        else if ((insn & 0x9F3E0C00u) == 0x0E200800u) ok = simd_misc(c, insn, 0);
        else if ((insn & 0x9F3E0C00u) == 0x0E300800u) ok = simd_across(c, insn);
        else if ((insn & 0x9F200C00u) == 0x0E200000u) ok = simd_3diff(c, insn, 0);
    } else if ((insn & 0x9F000000u) == 0x0F000000u) {           /* AdvSIMD vector, 01111 */
        if ((insn & 0x9FF80400u) == 0x0F000400u) ok = simd_modimm(c, insn);
        else if ((insn & 0x9F800400u) == 0x0F000400u) ok = simd_shift_imm(c, insn, 0);
        else if (!((insn >> 10) & 1)) ok = simd_indexed(c, insn, 0);
    } else if ((insn & 0xDF000000u) == 0x5E000000u) {           /* AdvSIMD scalar, 11110 */
        if ((insn & 0xFFE08400u) == 0x5E000400u) ok = scalar_copy(c, insn);
        else if ((insn & 0xDF200400u) == 0x5E200400u) ok = scalar_3same(c, insn);
        else if ((insn & 0xDF3E0C00u) == 0x5E200800u) ok = simd_misc(c, insn, 1);
        else if ((insn & 0xDF3E0C00u) == 0x5E300800u) ok = scalar_pairwise(c, insn);
        else if ((insn & 0xDF200C00u) == 0x5E200000u) ok = simd_3diff(c, insn, 1);
    } else if ((insn & 0xDF000000u) == 0x5F000000u) {           /* AdvSIMD scalar, 11111 */
        if ((insn & 0xDF800400u) == 0x5F000400u && ((insn >> 19) & 15)) ok = simd_shift_imm(c, insn, 1);
        else if (!((insn >> 10) & 1)) ok = simd_indexed(c, insn, 1);
    }
    if (!ok) wd_undef(m);
}

/* ================================================================ loads and stores */

static void vload(wd_machine *m, uint64_t addr, uint8_t *dst, int bytes)
{
    for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i < 8 ? bytes - i : 8;
        uint64_t v = wd_read(m, addr + (uint64_t)i, n, 0);
        memcpy(dst + i, &v, (size_t)n);
    }
}

static void vstore(wd_machine *m, uint64_t addr, const uint8_t *src, int bytes)
{
    for (int i = 0; i < bytes; i += 8) {
        int n = bytes - i < 8 ? bytes - i : 8;
        uint64_t v = 0;
        memcpy(&v, src + i, (size_t)n);
        wd_write(m, addr + (uint64_t)i, n, v, 0);
    }
}

static inline uint64_t base_reg(wd_cpu *c, int rn) { return rn == 31 ? *wd_sp_ptr(c) : c->x[rn]; }
static inline void set_base(wd_cpu *c, int rn, uint64_t v) { if (rn == 31) *wd_sp_ptr(c) = v; else c->x[rn] = v; }

static void ldst_structs(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int q = (insn >> 30) & 1, L = (insn >> 22) & 1, post = (insn >> 23) & 1, single = (insn >> 24) & 1;
    int rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31, size = (insn >> 10) & 3;
    int opcode = (insn >> 12) & 15;
    uint64_t addr = base_reg(c, rn), offs = 0;
    if (!post && rm) wd_undef(m);
    uint8_t regs[4][16];

    if (!single) {
        int rpt, selem;
        switch (opcode) {
        case 0: rpt = 1; selem = 4; break;
        case 2: rpt = 4; selem = 1; break;
        case 4: rpt = 1; selem = 3; break;
        case 6: rpt = 3; selem = 1; break;
        case 7: rpt = 1; selem = 1; break;
        case 8: rpt = 1; selem = 2; break;
        case 10: rpt = 2; selem = 1; break;
        default: wd_undef(m); return;
        }
        if ((insn >> 21) & 1) wd_undef(m);
        if (size == 3 && !q && selem > 1) wd_undef(m);
        int esz = 1 << size, elements = (q ? 16 : 8) / esz, nregs = rpt * selem;
        for (int r = 0; r < nregs; r++) memcpy(regs[r], VB(c, (rt + r) % 32), 16);
        for (int r = 0; r < rpt; r++)
            for (int e = 0; e < elements; e++)
                for (int s = 0; s < selem; s++) {
                    int reg = r + s;
                    if (L) {
                        uint64_t v = wd_read(m, addr + offs, esz, 0);
                        se(regs[reg], e, esz, v);
                    } else {
                        wd_write(m, addr + offs, esz, ge(regs[reg], e, esz), 0);
                    }
                    offs += (uint64_t)esz;
                }
        if (L) for (int r = 0; r < nregs; r++) wr_vec(c, (rt + r) % 32, regs[r], q);
    } else {
        int R = (insn >> 21) & 1, S = (insn >> 12) & 1;
        int selem = ((((int)(insn >> 13) & 1) << 1) | R) + 1;   /* opcode<0> is bit 13 */
        int scale = opcode >> 2, idx, esz, replicate = 0;
        switch (scale) {
        case 0: idx = (q << 3) | (S << 2) | size; esz = 1; break;
        case 1: if (size & 1) wd_undef(m); idx = (q << 2) | (S << 1) | (size >> 1); esz = 2; break;
        case 2:
            if (size & 2) wd_undef(m);
            if (size & 1) { if (S) wd_undef(m); idx = q; esz = 8; }
            else { idx = (q << 1) | S; esz = 4; }
            break;
        default:
            if (!L || S) wd_undef(m);
            replicate = 1; esz = 1 << size; idx = 0;
            break;
        }
        for (int s = 0; s < selem; s++) memcpy(regs[s], VB(c, (rt + s) % 32), 16);
        for (int s = 0; s < selem; s++) {
            if (replicate) {
                uint64_t v = wd_read(m, addr + offs, esz, 0);
                uint8_t rep[16] = { 0 };
                for (int e = 0; e < (q ? 16 : 8) / esz; e++) se(rep, e, esz, v);
                memcpy(regs[s], rep, 16);
            } else if (L) {
                se(regs[s], idx, esz, wd_read(m, addr + offs, esz, 0));
            } else {
                wd_write(m, addr + offs, esz, ge(regs[s], idx, esz), 0);
            }
            offs += (uint64_t)esz;
        }
        if (L) for (int s = 0; s < selem; s++) wr_vec(c, (rt + s) % 32, regs[s], replicate ? q : 1);
    }
    if (post) set_base(c, rn, addr + (rm == 31 ? offs : c->x[rm]));
}

void wd_simd_ldst(wd_machine *m, uint32_t insn)
{
    wd_cpu *c = &m->cpu;
    int rt = insn & 31, rn = (insn >> 5) & 31;
    wd_fp_check(m);

    if ((insn & 0xBF000000u) == 0x0C000000u || (insn & 0xBF000000u) == 0x0D000000u) {
        ldst_structs(m, insn);
        return;
    }
    if ((insn & 0x3B000000u) == 0x18000000u) {                  /* LDR (literal) */
        int opc = insn >> 30;
        if (opc == 3) wd_undef(m);
        int bytes = 4 << opc;
        uint8_t t[16] = { 0 };
        vload(m, c->pc + (wd_sext((insn >> 5) & 0x7FFFF, 19) << 2), t, bytes);
        memcpy(VB(c, rt), t, 16);
        return;
    }
    if ((insn & 0x3A000000u) == 0x28000000u) {                  /* LDP/STP (SIMD&FP) */
        int opc = insn >> 30, mode = (insn >> 23) & 3, L = (insn >> 22) & 1, rt2 = (insn >> 10) & 31;
        if (opc == 3) wd_undef(m);
        int bytes = 4 << opc;
        int64_t off = (int64_t)wd_sext((insn >> 15) & 0x7F, 7) * bytes;
        uint64_t base = base_reg(c, rn), addr = mode == 1 ? base : base + (uint64_t)off;
        if (L) {
            uint8_t a[16] = { 0 }, b[16] = { 0 };
            vload(m, addr, a, bytes);
            vload(m, addr + (uint64_t)bytes, b, bytes);
            memcpy(VB(c, rt), a, 16);
            memcpy(VB(c, rt2), b, 16);
        } else {
            uint8_t a[16], b[16];
            memcpy(a, VB(c, rt), 16);
            memcpy(b, VB(c, rt2), 16);
            vstore(m, addr, a, bytes);
            vstore(m, addr + (uint64_t)bytes, b, bytes);
        }
        if (mode == 1 || mode == 3) set_base(c, rn, base + (uint64_t)off);
        return;
    }
    if ((insn & 0x3A000000u) == 0x38000000u) {                  /* LDR/STR (register forms) */
        int size = insn >> 30, opc = (insn >> 22) & 3;
        int bytes, scale;
        if (opc & 2) {
            if (size) wd_undef(m);
            bytes = 16; scale = 4;
        } else {
            bytes = 1 << size; scale = size;
        }
        int L = opc & 1;
        uint64_t base = base_reg(c, rn), addr;
        int wb = 0;
        int64_t off = 0;
        if ((insn >> 24) & 1) {
            addr = base + (((insn >> 10) & 0xFFFULL) << scale);
        } else if ((insn >> 21) & 1) {
            if (((insn >> 10) & 3) != 2) wd_undef(m);
            int rm = (insn >> 16) & 31, option = (insn >> 13) & 7, S = (insn >> 12) & 1;
            if (!(option & 2)) wd_undef(m);
            uint64_t x = rm == 31 ? 0 : c->x[rm];
            switch (option) {
            case 2: x = (uint32_t)x; break;
            case 6: x = (uint64_t)(int64_t)(int32_t)x; break;
            default: break;
            }
            addr = base + (x << (S ? scale : 0));
        } else {
            int k = (insn >> 10) & 3;
            off = wd_sext((insn >> 12) & 0x1FF, 9);
            if (k == 2) wd_undef(m);
            if (k == 1) { addr = base; wb = 1; }
            else { addr = base + (uint64_t)off; wb = k == 3; }
        }
        if (L) {
            uint8_t t[16] = { 0 };
            vload(m, addr, t, bytes);
            memcpy(VB(c, rt), t, 16);
        } else {
            uint8_t t[16];
            memcpy(t, VB(c, rt), 16);
            vstore(m, addr, t, bytes);
        }
        if (wb) set_base(c, rn, base + (uint64_t)off);
        return;
    }
    wd_undef(m);
}
