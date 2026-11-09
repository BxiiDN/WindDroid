/*
 * WindDroid - IEEE 754 operations with AArch64 semantics (NaN handling,
 * FPCR.DN/FZ, rounding modes, FPSR cumulative flags).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef WD_FPU_H
#define WD_FPU_H

#include "internal.h"

/* FPSR cumulative exception bits */
#define FPSR_IOC 0x01
#define FPSR_DZC 0x02
#define FPSR_OFC 0x04
#define FPSR_UFC 0x08
#define FPSR_IXC 0x10
#define FPSR_IDC 0x80
#define FPSR_QC  (1u << 27)

#define FPCR_DN  (1u << 25)
#define FPCR_FZ  (1u << 24)

/* rounding modes (values 0..3 are FPCR.RMode) */
enum { RM_RN = 0, RM_RP = 1, RM_RM = 2, RM_RZ = 3, RM_RNA = 4, RM_ODD = 5 };

static inline int fpcr_rmode(const wd_cpu *c) { return (c->fpcr >> 22) & 3; }

uint64_t fpu_add(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_sub(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_mul(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_mulx(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_div(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_muladd(wd_cpu *c, uint64_t addend, uint64_t a, uint64_t b, int n);
uint64_t fpu_sqrt(wd_cpu *c, uint64_t a, int n);
uint64_t fpu_max(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_min(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_maxnm(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_minnm(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint32_t fpu_cmp(wd_cpu *c, uint64_t a, uint64_t b, int n, int signal_all);  /* NZCV << 28 */
int fpu_cmp_eq(wd_cpu *c, uint64_t a, uint64_t b, int n);
int fpu_cmp_ge(wd_cpu *c, uint64_t a, uint64_t b, int n);
int fpu_cmp_gt(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_round_int(wd_cpu *c, uint64_t a, int n, int rmode, int exact);
uint64_t fpu_to_fixed(wd_cpu *c, uint64_t a, int n, int fbits, int is_unsigned, int rmode, int outbits);
uint64_t fpu_from_fixed(wd_cpu *c, uint64_t v, int is_signed, int inbits, int fbits, int n, int rmode);
uint64_t fpu_convert(wd_cpu *c, uint64_t a, int from, int to, int rmode);
uint64_t fpu_recpe(wd_cpu *c, uint64_t a, int n);
uint64_t fpu_rsqrte(wd_cpu *c, uint64_t a, int n);
uint64_t fpu_recps(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_rsqrts(wd_cpu *c, uint64_t a, uint64_t b, int n);
uint64_t fpu_recpx(wd_cpu *c, uint64_t a, int n);
uint32_t fpu_urecpe(uint32_t a);
uint32_t fpu_ursqrte(uint32_t a);

static inline uint64_t fpu_neg(uint64_t a, int n) { return a ^ (1ULL << (n - 1)); }
static inline uint64_t fpu_abs(uint64_t a, int n) { return a & ~(1ULL << (n - 1)); }

#endif
