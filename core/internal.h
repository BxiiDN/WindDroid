/*
 * WindDroid - engine internals shared by core/, devices/ and host/.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef WD_INTERNAL_H
#define WD_INTERNAL_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "winddroid.h"

#define WD_UNUSED(x) (void)(x)

/* ---------------------------------------------------------------- CPU */

#define PSTATE_N (1u << 31)
#define PSTATE_Z (1u << 30)
#define PSTATE_C (1u << 29)
#define PSTATE_V (1u << 28)
#define DAIF_D   (1u << 9)
#define DAIF_A   (1u << 8)
#define DAIF_I   (1u << 7)
#define DAIF_F   (1u << 6)

typedef struct {
    uint64_t x[32];          /* x[31] is never read; register 31 is SP or ZR */
    uint64_t sp_el0, sp_el1;
    uint64_t pc;
    uint32_t nzcv;           /* bits 31..28 */
    uint32_t daif;           /* bits 9..6 */
    int el;                  /* 0 or 1 */
    int spsel;               /* 0: SP_EL0, 1: SP_ELx */
    int pan;
    int ss_lock;

    /* EL1 system registers */
    uint64_t sctlr, tcr, ttbr0, ttbr1, mair, amair, vbar;
    uint64_t esr, far, elr, spsr, par;
    uint64_t tpidr_el0, tpidrro_el0, tpidr_el1, contextidr, cpacr;
    uint64_t afsr0, afsr1, mdscr, cntkctl, csselr, actlr;
    uint64_t oslsr, osdlr, disr;

    /* generic timer */
    uint64_t cntv_ctl, cntv_cval, cntp_ctl, cntp_cval;

    /* FP / AdvSIMD */
    uint64_t v[32][2];       /* little-endian: v[n][0] holds bits 63..0 */
    uint32_t fpcr, fpsr;

    /* exclusive monitor */
    uint64_t excl_addr;
    int excl_valid;

    uint64_t icount;
} wd_cpu;

/* ---------------------------------------------------------------- GIC */

#define GIC_NIRQ 96          /* 32 private + 64 SPIs */

typedef struct {
    uint32_t dctlr;
    uint32_t rwaker;
    uint8_t enabled[GIC_NIRQ];
    uint8_t latch[GIC_NIRQ];     /* edge / software pending */
    uint8_t line[GIC_NIRQ];      /* level of the input line */
    uint8_t active[GIC_NIRQ];
    uint8_t edge[GIC_NIRQ];      /* 1 = edge triggered */
    uint8_t group[GIC_NIRQ];
    uint8_t prio[GIC_NIRQ];
    uint64_t irouter[GIC_NIRQ];
    /* CPU interface */
    uint8_t pmr;
    uint8_t bpr0, bpr1;
    uint8_t igrpen0, igrpen1;
    uint32_t ctlr;
    uint8_t ap_prio[32];         /* stack of active priorities */
    uint16_t ap_id[32];
    int ap_n;
} wd_gic;

/* ---------------------------------------------------------------- UART */

#define UART_FIFO 256

typedef struct {
    uint8_t rx[UART_FIFO];
    unsigned rx_head, rx_count;
    uint32_t cr, lcr_h, ibrd, fbrd, ifls, imsc, ris, dmacr;
} wd_uart;

/* ---------------------------------------------------------------- TLB */

#define TLB_BITS 10
#define TLB_SIZE (1u << TLB_BITS)

enum {
    P_R0 = 1, P_W0 = 2, P_X0 = 4,
    P_R1 = 8, P_W1 = 16, P_X1 = 32,
    P_USER = 64              /* EL0 accessible (for PAN) */
};

typedef struct {
    uint64_t vpn;            /* va >> 12, ~0 when invalid */
    uint64_t pa_page;        /* physical page address */
    uint8_t *host;           /* host pointer when the page is RAM, else NULL */
    uint8_t perm;
    uint8_t level;           /* translation level of the leaf, for fault reports */
    uint8_t attr;            /* MAIR attribute index */
} wd_tlbe;

/* ---------------------------------------------------------------- machine */

struct wd_machine {
    wd_cpu cpu;
    wd_gic gic;
    wd_uart uart;

    uint8_t *ram;
    uint64_t ram_size;
    int realtime;
    uint64_t rt_base_ns;

    wd_tlbe tlb[TLB_SIZE];
    uint64_t fetch_vpn;
    uint8_t *fetch_host;
    int fetch_el;

    wd_uart_tx_fn uart_tx;
    void *uart_opaque;

    jmp_buf jb;
    uint64_t budget;
    int stop;                /* wd_run_result to return, or -1 */
    int irq_dirty;
    int irq_pend;
    int exit_code;
    int trace;
    uint64_t idle_ns;
    int wfi;

    char err[256];
    uint64_t warned_sysreg[16];
    int warned_n;
};

/* cpu.c */
void wd_cpu_reset(wd_machine *m);
void wd_cpu_step(wd_machine *m);
void wd_take_irq(wd_machine *m);
void wd_undef(wd_machine *m);
_Noreturn void wd_exception(wd_machine *m, uint32_t ec, uint32_t iss, uint64_t far,
                            int has_far, uint64_t ret_pc);
_Noreturn void wd_fatal(wd_machine *m, const char *fmt, ...);
uint64_t *wd_sp_ptr(wd_cpu *c);
uint32_t wd_get_pstate(wd_cpu *c);

/* mmu.c */
enum { ACC_READ = 0, ACC_WRITE = 1, ACC_EXEC = 2 };
void wd_tlb_flush(wd_machine *m);
/* Translate va; on fault raise the guest exception (does not return).
 * 'unpriv' forces EL0 permission checks (LDTR/STTR). */
wd_tlbe *wd_translate(wd_machine *m, uint64_t va, int acc, int unpriv);
/* AT instructions: translate without raising, write PAR_EL1. */
void wd_at(wd_machine *m, uint64_t va, int acc, int unpriv);
uint64_t wd_read(wd_machine *m, uint64_t va, int size, int unpriv);
void wd_write(wd_machine *m, uint64_t va, int size, uint64_t val, int unpriv);
uint32_t wd_fetch(wd_machine *m, uint64_t va);
void wd_check_align(wd_machine *m, uint64_t va, int size, int write);

/* bus (machine.c) */
uint64_t wd_phys_read(wd_machine *m, uint64_t pa, int size);
void wd_phys_write(wd_machine *m, uint64_t pa, int size, uint64_t val);
uint8_t *wd_phys_host(wd_machine *m, uint64_t pa);
uint64_t wd_counter(wd_machine *m);
void wd_update_timers(wd_machine *m);
int wd_psci(wd_machine *m);

/* simd.c / fpu.c */
void wd_simd_dp(wd_machine *m, uint32_t insn);
void wd_simd_ldst(wd_machine *m, uint32_t insn);
void wd_fp_check(wd_machine *m);

/* sysreg.c */
bool wd_sysreg_read(wd_machine *m, uint32_t key, uint64_t *val);
bool wd_sysreg_write(wd_machine *m, uint32_t key, uint64_t val);
void wd_sys_insn(wd_machine *m, uint32_t insn);

/* devices */
void wd_gic_reset(wd_gic *g);
uint64_t wd_gicd_read(wd_machine *m, uint64_t off, int size);
void wd_gicd_write(wd_machine *m, uint64_t off, int size, uint64_t val);
uint64_t wd_gicr_read(wd_machine *m, uint64_t off, int size);
void wd_gicr_write(wd_machine *m, uint64_t off, int size, uint64_t val);
void wd_gic_set_line(wd_machine *m, int irq, int level);
int wd_gic_irq_pending(wd_machine *m);       /* deliverable IRQ for the CPU */
int wd_gic_any_pending(wd_machine *m);       /* any enabled pending (wakes WFI) */
uint64_t wd_gic_iar(wd_machine *m);
void wd_gic_eoi(wd_machine *m, uint32_t id);
void wd_gic_dir(wd_machine *m, uint32_t id);
uint64_t wd_gic_hppir(wd_machine *m);
uint64_t wd_gic_rpr(wd_machine *m);
void wd_gic_sgi(wd_machine *m, uint64_t val);

void wd_uart_reset(wd_uart *u);
uint64_t wd_uart_read(wd_machine *m, uint64_t off);
void wd_uart_write(wd_machine *m, uint64_t off, uint64_t val);
void wd_uart_update(wd_machine *m);

uint64_t wd_rtc_read(wd_machine *m, uint64_t off);
void wd_rtc_write(wd_machine *m, uint64_t off, uint64_t val);

uint64_t wd_ctl_read(wd_machine *m, uint64_t off);
void wd_ctl_write(wd_machine *m, uint64_t off, uint64_t val);

/* helpers */
static inline uint64_t wd_sext(uint64_t v, int bits)
{
    if (bits >= 64) return v;
    uint64_t m = 1ULL << (bits - 1);
    v &= (1ULL << bits) - 1;
    return (v ^ m) - m;
}

static inline uint64_t wd_mask(int bits)
{
    return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
}

/* Exception classes */
#define EC_UNKNOWN      0x00
#define EC_WFX          0x01
#define EC_FP           0x07
#define EC_ILLEGAL      0x0E
#define EC_SVC64        0x15
#define EC_HVC64        0x16
#define EC_SMC64        0x17
#define EC_SYSREG       0x18
#define EC_IABT_LOW     0x20
#define EC_IABT_CUR     0x21
#define EC_PC_ALIGN     0x22
#define EC_DABT_LOW     0x24
#define EC_DABT_CUR     0x25
#define EC_SP_ALIGN     0x26
#define EC_BRK          0x3C

/* System register key: bits 19..5 of MRS/MSR (o0:op1:CRn:CRm:op2). */
#define SR(op0, op1, crn, crm, op2) \
    ((uint32_t)((((op0) - 2) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2)))

#endif
