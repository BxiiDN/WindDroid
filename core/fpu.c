/*
 * WindDroid - floating point with AArch64 semantics.
 *
 * Arithmetic (+ - * / sqrt fma) uses the host FPU, which is IEEE 754
 * correctly rounded; everything around it follows the ARM ARM pseudocode:
 * NaN propagation order, default NaN, flush-to-zero, comparisons, and all
 * rounding of conversions, which is done in software.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <fenv.h>
#include <math.h>

#include "fpu.h"

#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif

typedef struct { int E, F, bias; } fmt_t;

static fmt_t fmt(int n)
{
    if (n == 16) return (fmt_t){ 5, 10, 15 };
    if (n == 32) return (fmt_t){ 8, 23, 127 };
    return (fmt_t){ 11, 52, 1023 };
}

enum { C_ZERO, C_FINITE, C_INF, C_QNAN, C_SNAN };

typedef struct {
    int cls, sign;
    uint64_t mant;     /* value = mant * 2^exp for finite numbers */
    int exp;
    uint64_t bits;     /* operand after input flushing */
} fpv;

static inline uint64_t sign_bit(int n) { return 1ULL << (n - 1); }

static uint64_t default_nan(int n)
{
    if (n == 16) return 0x7E00;
    if (n == 32) return 0x7FC00000;
    return 0x7FF8000000000000ULL;
}

static uint64_t fp_inf(int sign, int n)
{
    fmt_t f = fmt(n);
    return ((uint64_t)sign << (n - 1)) | (((1ULL << f.E) - 1) << f.F);
}

static uint64_t fp_zero(int sign, int n) { return (uint64_t)sign << (n - 1); }

static uint64_t fp_max_normal(int sign, int n)
{
    fmt_t f = fmt(n);
    return ((uint64_t)sign << (n - 1)) | (((1ULL << f.E) - 2) << f.F) | ((1ULL << f.F) - 1);
}

static inline int fz_on(wd_cpu *c, int n) { return n != 16 && (c->fpcr & FPCR_FZ); }

static fpv unpack(wd_cpu *c, uint64_t a, int n, int honor_fz)
{
    fmt_t f = fmt(n);
    fpv r;
    a &= n == 64 ? ~0ULL : ((1ULL << n) - 1);
    r.bits = a;
    r.sign = (int)((a >> (n - 1)) & 1);
    uint64_t e = (a >> f.F) & ((1ULL << f.E) - 1);
    uint64_t fr = a & ((1ULL << f.F) - 1);
    if (e == 0) {
        if (fr == 0) { r.cls = C_ZERO; r.mant = 0; r.exp = 0; }
        else if (honor_fz && fz_on(c, n)) {
            r.cls = C_ZERO;
            r.mant = 0;
            r.exp = 0;
            r.bits = fp_zero(r.sign, n);
            c->fpsr |= FPSR_IDC;
        } else {
            r.cls = C_FINITE;
            r.mant = fr;
            r.exp = 1 - f.bias - f.F;
        }
    } else if (e == (1ULL << f.E) - 1) {
        if (fr == 0) r.cls = C_INF;
        else r.cls = (fr >> (f.F - 1)) & 1 ? C_QNAN : C_SNAN;
        r.mant = 0;
        r.exp = 0;
    } else {
        r.cls = C_FINITE;
        r.mant = fr | (1ULL << f.F);
        r.exp = (int)e - f.bias - f.F;
    }
    return r;
}

static inline int is_nan(const fpv *v) { return v->cls == C_QNAN || v->cls == C_SNAN; }

static uint64_t process_nan(wd_cpu *c, const fpv *v, int n)
{
    uint64_t r = v->bits;
    if (v->cls == C_SNAN) {
        c->fpsr |= FPSR_IOC;
        r |= 1ULL << (fmt(n).F - 1);
    }
    if (c->fpcr & FPCR_DN) r = default_nan(n);
    return r;
}

static int process_nans2(wd_cpu *c, const fpv *a, const fpv *b, int n, uint64_t *out)
{
    if (a->cls == C_SNAN) { *out = process_nan(c, a, n); return 1; }
    if (b->cls == C_SNAN) { *out = process_nan(c, b, n); return 1; }
    if (a->cls == C_QNAN) { *out = process_nan(c, a, n); return 1; }
    if (b->cls == C_QNAN) { *out = process_nan(c, b, n); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ rounding */

/* Round sign * mant * 2^exp to format n. Implements FPRound from the ARM ARM. */
static uint64_t fp_round(wd_cpu *c, int sign, uint64_t mant, int exp, int n, int rmode, int fz)
{
    fmt_t f = fmt(n);
    int emin = 1 - f.bias;
    if (mant == 0) return fp_zero(sign, n);
    int p = 63 - __builtin_clzll(mant);
    int e = exp + p;
    if (e < emin && fz) {
        c->fpsr |= FPSR_UFC;
        return fp_zero(sign, n);
    }
    int tiny = e < emin;
    int lsb = tiny ? emin - f.F : e - f.F;
    int drop = lsb - exp;
    uint64_t sig, rem = 0, half = 0;
    int inexact = 0, cmp = 0;                    /* cmp: rem vs half: -1 below, 0 equal, 1 above */
    if (drop <= 0) {
        sig = mant << -drop;
    } else if (drop > 64) {
        sig = 0;
        inexact = 1;
        cmp = -1;
    } else if (drop == 64) {
        sig = 0;
        inexact = 1;
        uint64_t top = mant >> 63, low = mant & ~(1ULL << 63);
        cmp = top ? (low ? 1 : 0) : -1;
    } else {
        sig = mant >> drop;
        rem = mant & ((1ULL << drop) - 1);
        half = 1ULL << (drop - 1);
        inexact = rem != 0;
        cmp = rem > half ? 1 : (rem == half ? 0 : -1);
    }
    int inc = 0;
    if (inexact) {
        switch (rmode) {
        case RM_RN: inc = cmp > 0 || (cmp == 0 && (sig & 1)); break;
        case RM_RNA: inc = cmp >= 0; break;
        case RM_RP: inc = !sign; break;
        case RM_RM: inc = sign; break;
        case RM_ODD: if (!(sig & 1)) sig |= 1; break;
        default: break;
        }
    }
    sig += (uint64_t)inc;
    if (sig >> (f.F + 1)) { sig >>= 1; lsb++; }
    uint64_t expfield, frac;
    if (sig >> f.F) { expfield = (uint64_t)(lsb + f.F + f.bias); frac = sig & ((1ULL << f.F) - 1); }
    else { expfield = 0; frac = sig; }
    if (expfield >= (1ULL << f.E) - 1) {
        c->fpsr |= FPSR_OFC | FPSR_IXC;
        int to_inf;
        switch (rmode) {
        case RM_RP: to_inf = !sign; break;
        case RM_RM: to_inf = sign; break;
        case RM_RZ: case RM_ODD: to_inf = 0; break;
        default: to_inf = 1; break;
        }
        return to_inf ? fp_inf(sign, n) : fp_max_normal(sign, n);
    }
    if (inexact) {
        c->fpsr |= FPSR_IXC;
        if (tiny) c->fpsr |= FPSR_UFC;
    }
    return ((uint64_t)sign << (n - 1)) | (expfield << f.F) | frac;
}

/* ------------------------------------------------------------------ host arithmetic */

static int host_round(int rmode)
{
    switch (rmode) {
    case RM_RP: return FE_UPWARD;
    case RM_RM: return FE_DOWNWARD;
    case RM_RZ: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
    }
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT, OP_FMA };

/* One host operation in rounding mode rm; returns the result bits and host flags. */
static uint64_t host_compute(int op, uint64_t a, uint64_t b, uint64_t d, int n, int rm, int *ex)
{
    int saved = fegetround();
    if (rm != saved) fesetround(rm);
    feclearexcept(FE_ALL_EXCEPT);
    uint64_t res;
    if (n == 32) {
        volatile float x, y, z, r;
        uint32_t t;
        t = (uint32_t)a; memcpy((void *)&x, &t, 4);
        t = (uint32_t)b; memcpy((void *)&y, &t, 4);
        t = (uint32_t)d; memcpy((void *)&z, &t, 4);
        switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV: r = x / y; break;
        case OP_SQRT: r = sqrtf(x); break;
        default: r = fmaf(x, y, z); break;
        }
        float rr = r;
        memcpy(&t, &rr, 4);
        res = t;
    } else {
        volatile double x, y, z, r;
        memcpy((void *)&x, &a, 8);
        memcpy((void *)&y, &b, 8);
        memcpy((void *)&z, &d, 8);
        switch (op) {
        case OP_ADD: r = x + y; break;
        case OP_SUB: r = x - y; break;
        case OP_MUL: r = x * y; break;
        case OP_DIV: r = x / y; break;
        case OP_SQRT: r = sqrt(x); break;
        default: r = fma(x, y, z); break;
        }
        double rr = r;
        memcpy(&res, &rr, 8);
    }
    *ex = fetestexcept(FE_ALL_EXCEPT);
    if (rm != saved) fesetround(saved);
    return res;
}

static uint64_t host_op(wd_cpu *c, int op, uint64_t a, uint64_t b, uint64_t d, int n)
{
    fmt_t f = fmt(n);
    int ex;
    uint64_t res = host_compute(op, a, b, d, n, host_round(fpcr_rmode(c)), &ex);

    uint32_t flags = 0;
    if (ex & FE_INVALID) flags |= FPSR_IOC;
    if (ex & FE_DIVBYZERO) flags |= FPSR_DZC;
    if (ex & FE_OVERFLOW) flags |= FPSR_OFC;
    if (ex & FE_INEXACT) flags |= FPSR_IXC;

    fpv r = unpack(c, res, n, 0);
    if (is_nan(&r)) {
        c->fpsr |= FPSR_IOC;                        /* only invalid operations get here */
        return default_nan(n);
    }
    /* ARM detects tininess before rounding; hosts may detect it after rounding,
     * so decide it here. A result is tiny when the exact value is non-zero and
     * below the smallest normal number. */
    uint64_t expf = (res >> f.F) & ((1ULL << f.E) - 1), frac = res & ((1ULL << f.F) - 1);
    int inexact = (ex & FE_INEXACT) != 0, tiny = 0;
    if (r.cls != C_INF) {
        if (expf == 0) tiny = frac != 0 || inexact;
        else if (expf == 1 && frac == 0 && inexact) {
            int ex2;
            uint64_t rz = host_compute(op, a, b, d, n, FE_TOWARDZERO, &ex2);
            tiny = ((rz >> f.F) & ((1ULL << f.E) - 1)) == 0;
        }
    }
    if (tiny && fz_on(c, n)) {
        c->fpsr |= (flags & ~FPSR_IXC) | FPSR_UFC;
        return fp_zero(r.sign, n);
    }
    if (tiny && inexact) flags |= FPSR_UFC;
    c->fpsr |= flags;
    return res;
}

static uint64_t arith2(wd_cpu *c, int op, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    uint64_t r;
    if (process_nans2(c, &x, &y, n, &r)) return r;
    return host_op(c, op, x.bits, y.bits, 0, n);
}

uint64_t fpu_add(wd_cpu *c, uint64_t a, uint64_t b, int n) { return arith2(c, OP_ADD, a, b, n); }
uint64_t fpu_sub(wd_cpu *c, uint64_t a, uint64_t b, int n) { return arith2(c, OP_SUB, a, b, n); }
uint64_t fpu_mul(wd_cpu *c, uint64_t a, uint64_t b, int n) { return arith2(c, OP_MUL, a, b, n); }
uint64_t fpu_div(wd_cpu *c, uint64_t a, uint64_t b, int n) { return arith2(c, OP_DIV, a, b, n); }

uint64_t fpu_mulx(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    uint64_t r;
    if (process_nans2(c, &x, &y, n, &r)) return r;
    if ((x.cls == C_INF && y.cls == C_ZERO) || (x.cls == C_ZERO && y.cls == C_INF)) {
        fmt_t f = fmt(n);
        return ((uint64_t)(x.sign ^ y.sign) << (n - 1)) | ((uint64_t)f.bias + 1) << f.F;   /* +/-2.0 */
    }
    return host_op(c, OP_MUL, x.bits, y.bits, 0, n);
}

uint64_t fpu_muladd(wd_cpu *c, uint64_t addend, uint64_t a, uint64_t b, int n)
{
    fpv A = unpack(c, addend, n, 1), x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    int inf_zero = (x.cls == C_INF && y.cls == C_ZERO) || (x.cls == C_ZERO && y.cls == C_INF);
    if (A.cls == C_QNAN && inf_zero) {
        c->fpsr |= FPSR_IOC;
        return default_nan(n);
    }
    const fpv *ord[3] = { &A, &x, &y };
    for (int k = 0; k < 3; k++) if (ord[k]->cls == C_SNAN) return process_nan(c, ord[k], n);
    for (int k = 0; k < 3; k++) if (ord[k]->cls == C_QNAN) return process_nan(c, ord[k], n);
    return host_op(c, OP_FMA, x.bits, y.bits, A.bits, n);
}

uint64_t fpu_sqrt(wd_cpu *c, uint64_t a, int n)
{
    fpv x = unpack(c, a, n, 1);
    if (is_nan(&x)) return process_nan(c, &x, n);
    if (x.cls == C_ZERO) return x.bits;
    if (x.sign) { c->fpsr |= FPSR_IOC; return default_nan(n); }
    if (x.cls == C_INF) return x.bits;
    return host_op(c, OP_SQRT, x.bits, 0, 0, n);
}

/* compare finite/inf/zero values: -1, 0, 1 */
static int cmp_val(const fpv *x, const fpv *y)
{
    if (x->cls == C_ZERO && y->cls == C_ZERO) return 0;
    int sx = x->cls == C_ZERO ? 0 : (x->sign ? -1 : 1);
    int sy = y->cls == C_ZERO ? 0 : (y->sign ? -1 : 1);
    if (sx != sy) return sx < sy ? -1 : 1;
    /* same sign, both non-zero */
    int mag;
    if (x->cls == C_INF || y->cls == C_INF) mag = (x->cls == C_INF) - (y->cls == C_INF);
    else {
        int px = 63 - __builtin_clzll(x->mant) + x->exp, py = 63 - __builtin_clzll(y->mant) + y->exp;
        if (px != py) mag = px < py ? -1 : 1;
        else {
            /* align mantissas to compare */
            int sh = x->exp - y->exp;
            uint64_t mx = x->mant, my = y->mant;
            if (sh > 0) mx <<= sh; else my <<= -sh;
            mag = mx == my ? 0 : (mx < my ? -1 : 1);
        }
    }
    return sx < 0 ? -mag : mag;
}

static uint64_t maxmin(wd_cpu *c, uint64_t a, uint64_t b, int n, int is_max, int num)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    if (num) {
        if (x.cls == C_QNAN && y.cls != C_QNAN && y.cls != C_SNAN) return y.bits;
        if (y.cls == C_QNAN && x.cls != C_QNAN && x.cls != C_SNAN) return x.bits;
    }
    uint64_t r;
    if (process_nans2(c, &x, &y, n, &r)) return r;
    if (x.cls == C_ZERO && y.cls == C_ZERO) {
        int s = is_max ? (x.sign & y.sign) : (x.sign | y.sign);
        return fp_zero(s, n);
    }
    int k = cmp_val(&x, &y);
    if (is_max) return k > 0 ? x.bits : y.bits;
    return k < 0 ? x.bits : y.bits;
}

uint64_t fpu_max(wd_cpu *c, uint64_t a, uint64_t b, int n) { return maxmin(c, a, b, n, 1, 0); }
uint64_t fpu_min(wd_cpu *c, uint64_t a, uint64_t b, int n) { return maxmin(c, a, b, n, 0, 0); }
uint64_t fpu_maxnm(wd_cpu *c, uint64_t a, uint64_t b, int n) { return maxmin(c, a, b, n, 1, 1); }
uint64_t fpu_minnm(wd_cpu *c, uint64_t a, uint64_t b, int n) { return maxmin(c, a, b, n, 0, 1); }

uint32_t fpu_cmp(wd_cpu *c, uint64_t a, uint64_t b, int n, int signal_all)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN || signal_all) c->fpsr |= FPSR_IOC;
        return 0x3u << 28;
    }
    int k = cmp_val(&x, &y);
    if (k == 0) return 0x6u << 28;
    return k < 0 ? 0x8u << 28 : 0x2u << 28;
}

int fpu_cmp_eq(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    if (is_nan(&x) || is_nan(&y)) {
        if (x.cls == C_SNAN || y.cls == C_SNAN) c->fpsr |= FPSR_IOC;
        return 0;
    }
    return cmp_val(&x, &y) == 0;
}

int fpu_cmp_ge(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    if (is_nan(&x) || is_nan(&y)) { c->fpsr |= FPSR_IOC; return 0; }
    return cmp_val(&x, &y) >= 0;
}

int fpu_cmp_gt(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, a, n, 1), y = unpack(c, b, n, 1);
    if (is_nan(&x) || is_nan(&y)) { c->fpsr |= FPSR_IOC; return 0; }
    return cmp_val(&x, &y) > 0;
}

/* ------------------------------------------------------------------ integer rounding */

/* Round |value| = mant * 2^exp (exp < 0) to an integer magnitude. */
static unsigned __int128 round_mag(uint64_t mant, int exp, int sign, int rmode, int *inexact)
{
    unsigned __int128 ip;
    uint64_t rem_hi;                /* comparison against half done via 128-bit */
    int shift = -exp;
    if (shift >= 128) {
        ip = 0;
        *inexact = mant != 0;
        /* value < 0.5 */
        switch (rmode) {
        case RM_RP: return (!sign && *inexact) ? 1 : 0;
        case RM_RM: return (sign && *inexact) ? 1 : 0;
        default: return 0;
        }
    }
    unsigned __int128 m = mant;
    unsigned __int128 one = (unsigned __int128)1 << shift;      /* shift < 128 */
    if (shift >= 64) {
        ip = 0;
    } else {
        ip = m >> shift;
    }
    unsigned __int128 rem = shift >= 64 ? m : (m & (one - 1));
    unsigned __int128 half = one >> 1;
    (void)rem_hi;
    *inexact = rem != 0;
    int inc = 0;
    if (*inexact) {
        switch (rmode) {
        case RM_RN: inc = rem > half || (rem == half && (ip & 1)); break;
        case RM_RNA: inc = rem >= half; break;
        case RM_RP: inc = !sign; break;
        case RM_RM: inc = sign; break;
        default: break;
        }
    }
    return ip + (unsigned)inc;
}

uint64_t fpu_round_int(wd_cpu *c, uint64_t a, int n, int rmode, int exact)
{
    fpv x = unpack(c, a, n, 1);
    if (is_nan(&x)) return process_nan(c, &x, n);
    if (x.cls != C_FINITE) return x.bits;
    if (x.exp >= 0) return x.bits;                          /* already an integer */
    int inexact;
    unsigned __int128 r = round_mag(x.mant, x.exp, x.sign, rmode, &inexact);
    if (exact && inexact) c->fpsr |= FPSR_IXC;
    if (r == 0) return fp_zero(x.sign, n);
    uint32_t saved = c->fpsr;
    uint64_t out = fp_round(c, x.sign, (uint64_t)r, 0, n, RM_RN, 0);   /* exact: r has <= F+1 bits */
    c->fpsr = saved;
    return out;
}

uint64_t fpu_to_fixed(wd_cpu *c, uint64_t a, int n, int fbits, int is_unsigned, int rmode, int outbits)
{
    fpv x = unpack(c, a, n, 1);
    uint64_t umax = outbits == 64 ? ~0ULL : ((1ULL << outbits) - 1);
    int64_t smax = (int64_t)(umax >> 1), smin = -smax - 1;
    if (is_nan(&x)) { c->fpsr |= FPSR_IOC; return 0; }
    if (x.cls == C_ZERO) return 0;
    if (x.cls == C_INF) {
        c->fpsr |= FPSR_IOC;
        if (is_unsigned) return x.sign ? 0 : umax;
        return (uint64_t)(x.sign ? smin : smax) & umax;
    }
    int exp = x.exp + fbits;
    unsigned __int128 mag = 0;
    int inexact = 0, big = 0;
    if (exp >= 0) {
        if (exp >= 64) big = 1;
        else {
            mag = (unsigned __int128)x.mant << exp;     /* mant < 2^53, so this fits */
            if (mag >> 64) big = 1;
        }
    } else {
        mag = round_mag(x.mant, exp, x.sign, rmode, &inexact);
    }
    /* saturate */
    if (is_unsigned) {
        if (x.sign && (mag != 0 || big)) { c->fpsr |= FPSR_IOC; return 0; }
        if (big || mag > umax) { c->fpsr |= FPSR_IOC; return umax; }
        if (inexact) c->fpsr |= FPSR_IXC;
        return (uint64_t)mag;
    }
    unsigned __int128 lim = x.sign ? (unsigned __int128)smax + 1 : (unsigned __int128)smax;
    if (big || mag > lim) { c->fpsr |= FPSR_IOC; return (uint64_t)(x.sign ? smin : smax) & umax; }
    if (inexact) c->fpsr |= FPSR_IXC;
    uint64_t v = (uint64_t)mag;
    if (x.sign) v = 0 - v;
    return v & umax;
}

uint64_t fpu_from_fixed(wd_cpu *c, uint64_t v, int is_signed, int inbits, int fbits, int n, int rmode)
{
    int sign = 0;
    if (inbits < 64) v &= (1ULL << inbits) - 1;
    if (is_signed && ((v >> (inbits - 1)) & 1)) {
        sign = 1;
        v = (inbits == 64 ? 0 - v : ((1ULL << inbits) - v));
        if (inbits < 64) v &= (1ULL << inbits) - 1;
        if (v == 0) v = 1ULL << (inbits - 1);
    }
    if (v == 0) return fp_zero(0, n);
    return fp_round(c, sign, v, -fbits, n, rmode, fz_on(c, n));
}

uint64_t fpu_convert(wd_cpu *c, uint64_t a, int from, int to, int rmode)
{
    fpv x = unpack(c, a, from, 1);
    if (is_nan(&x)) {
        if (x.cls == C_SNAN) c->fpsr |= FPSR_IOC;
        if (c->fpcr & FPCR_DN) return default_nan(to);
        fmt_t fi = fmt(from), fo = fmt(to);
        uint64_t payload = x.bits & ((1ULL << (fi.F - 1)) - 1);
        uint64_t p51 = payload << (51 - (fi.F - 1));
        uint64_t po = p51 >> (51 - (fo.F - 1));
        return ((uint64_t)x.sign << (to - 1)) | (((1ULL << fo.E) - 1) << fo.F) | (1ULL << (fo.F - 1)) | po;
    }
    if (x.cls == C_INF) return fp_inf(x.sign, to);
    if (x.cls == C_ZERO) return fp_zero(x.sign, to);
    return fp_round(c, x.sign, x.mant, x.exp, to, rmode, fz_on(c, to));
}

/* ------------------------------------------------------------------ estimates */

static unsigned recip_estimate(unsigned a)        /* a in 256..511 */
{
    a = a * 2 + 1;
    unsigned b = (1u << 19) / a;
    return (b + 1) / 2;
}

static unsigned rsqrt_estimate(unsigned a)        /* a in 128..511 */
{
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    unsigned b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ULL << 28)) b++;
    return (b + 1) / 2;
}

uint32_t fpu_urecpe(uint32_t a)
{
    if (!(a >> 31)) return 0xFFFFFFFFu;
    return recip_estimate(a >> 23) << 23;
}

uint32_t fpu_ursqrte(uint32_t a)
{
    if (!(a >> 30)) return 0xFFFFFFFFu;
    return rsqrt_estimate(a >> 23) << 23;
}

uint64_t fpu_recpe(wd_cpu *c, uint64_t a, int n)
{
    fmt_t f = fmt(n);
    fpv x = unpack(c, a, n, 1);
    if (is_nan(&x)) return process_nan(c, &x, n);
    if (x.cls == C_INF) return fp_zero(x.sign, n);
    if (x.cls == C_ZERO) { c->fpsr |= FPSR_DZC; return fp_inf(x.sign, n); }
    /* overflow: |x| < 2^-(bias+1) */
    int lead = 63 - __builtin_clzll(x.mant) + x.exp;
    if (lead < -(f.bias + 1)) {
        c->fpsr |= FPSR_OFC | FPSR_IXC;
        int rm = fpcr_rmode(c), to_inf;
        switch (rm) {
        case RM_RP: to_inf = !x.sign; break;
        case RM_RM: to_inf = x.sign; break;
        case RM_RZ: to_inf = 0; break;
        default: to_inf = 1; break;
        }
        return to_inf ? fp_inf(x.sign, n) : fp_max_normal(x.sign, n);
    }
    if (fz_on(c, n) && lead >= f.bias - 1) {       /* result would be denormal */
        c->fpsr |= FPSR_UFC;
        return fp_zero(x.sign, n);
    }
    int exp = (int)((x.bits >> f.F) & ((1ULL << f.E) - 1));
    uint64_t fraction = (x.bits & ((1ULL << f.F) - 1)) << (52 - f.F);   /* 52-bit fraction */
    if (exp == 0) {
        if (!((fraction >> 51) & 1)) { exp = -1; fraction = (fraction << 2) & ((1ULL << 52) - 1); }
        else fraction = (fraction << 1) & ((1ULL << 52) - 1);
    }
    unsigned scaled = 256 | (unsigned)(fraction >> 44);
    int result_exp = 2 * f.bias - 1 - exp;           /* 253 - exp, 2045 - exp, 29 - exp */
    unsigned est = recip_estimate(scaled);
    uint64_t fr = (uint64_t)(est & 0xFF) << 44;
    if (result_exp == 0) fr = (1ULL << 51) | (fr >> 1);
    else if (result_exp == -1) { fr = (1ULL << 50) | (fr >> 2); result_exp = 0; }
    return ((uint64_t)x.sign << (n - 1)) | ((uint64_t)result_exp << f.F) | (fr >> (52 - f.F));
}

uint64_t fpu_rsqrte(wd_cpu *c, uint64_t a, int n)
{
    fmt_t f = fmt(n);
    fpv x = unpack(c, a, n, 1);
    if (is_nan(&x)) return process_nan(c, &x, n);
    if (x.cls == C_ZERO) { c->fpsr |= FPSR_DZC; return fp_inf(x.sign, n); }
    if (x.sign) { c->fpsr |= FPSR_IOC; return default_nan(n); }
    if (x.cls == C_INF) return fp_zero(0, n);
    int exp = (int)((x.bits >> f.F) & ((1ULL << f.E) - 1));
    uint64_t fraction = (x.bits & ((1ULL << f.F) - 1)) << (52 - f.F);
    if (exp == 0) {
        while (!((fraction >> 51) & 1)) { fraction = (fraction << 1) & ((1ULL << 52) - 1); exp--; }
        fraction = (fraction << 1) & ((1ULL << 52) - 1);
    }
    unsigned scaled = (exp & 1) ? (128 | (unsigned)(fraction >> 45)) : (256 | (unsigned)(fraction >> 44));
    int result_exp = (3 * f.bias - 1 - exp) / 2;     /* (380-exp)/2, (3068-exp)/2, (44-exp)/2 */
    unsigned est = rsqrt_estimate(scaled);
    return ((uint64_t)result_exp << f.F) | ((uint64_t)(est & 0xFF) << (f.F - 8));
}

uint64_t fpu_recps(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, fpu_neg(a, n), n, 1), y = unpack(c, b, n, 1);
    uint64_t r;
    if (process_nans2(c, &x, &y, n, &r)) return r;
    fmt_t f = fmt(n);
    if ((x.cls == C_INF && y.cls == C_ZERO) || (x.cls == C_ZERO && y.cls == C_INF))
        return (uint64_t)(f.bias + 1) << f.F;                         /* 2.0 */
    if (x.cls == C_INF || y.cls == C_INF) return fp_inf(x.sign ^ y.sign, n);
    return host_op(c, OP_FMA, x.bits, y.bits, (uint64_t)(f.bias + 1) << f.F, n);
}

uint64_t fpu_rsqrts(wd_cpu *c, uint64_t a, uint64_t b, int n)
{
    fpv x = unpack(c, fpu_neg(a, n), n, 1), y = unpack(c, b, n, 1);
    uint64_t r;
    if (process_nans2(c, &x, &y, n, &r)) return r;
    fmt_t f = fmt(n);
    uint64_t one_half = ((uint64_t)f.bias << f.F) | (1ULL << (f.F - 1));   /* 1.5 */
    if ((x.cls == C_INF && y.cls == C_ZERO) || (x.cls == C_ZERO && y.cls == C_INF)) return one_half;
    if (x.cls == C_INF || y.cls == C_INF) return fp_inf(x.sign ^ y.sign, n);
    /* (3 + x*y) / 2 = 1.5 + x * (y/2); halve whichever operand stays exact */
    uint64_t ye = (y.bits >> f.F) & ((1ULL << f.E) - 1), xe = (x.bits >> f.F) & ((1ULL << f.E) - 1);
    uint64_t xb = x.bits, yb = y.bits;
    if (ye > 1) yb -= 1ULL << f.F;
    else if (xe > 1) xb -= 1ULL << f.F;
    return host_op(c, OP_FMA, xb, yb, one_half, n);
}

uint64_t fpu_recpx(wd_cpu *c, uint64_t a, int n)
{
    fmt_t f = fmt(n);
    fpv x = unpack(c, a, n, 1);
    if (is_nan(&x)) return process_nan(c, &x, n);
    uint64_t exp = (x.bits >> f.F) & ((1ULL << f.E) - 1);
    uint64_t emask = (1ULL << f.E) - 1;
    uint64_t rexp = exp == 0 ? emask - 1 : (~exp & emask);
    return ((uint64_t)x.sign << (n - 1)) | (rexp << f.F);
}
