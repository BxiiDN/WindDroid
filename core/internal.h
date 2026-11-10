/*
 * WindDroid - engine internals shared by core/, devices/ and host/.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef WD_INTERNAL_H
#define WD_INTERNAL_H

#include <setjmp.h>
#include <stdio.h>
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
#define GIC_LPI_BASE 8192
#define GIC_NLPI 8192        /* GICD_TYPER.IDbits = 13 */

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
    /* LPIs (INTID 8192 .. 8192 + GIC_NLPI - 1), delivered through the ITS */
    uint32_t rctlr;              /* GICR_CTLR: EnableLPIs */
    uint64_t propbaser, pendbaser;
    uint8_t lpi_pend[GIC_NLPI];
    int lpi_npend;
} wd_gic;

/* per-CPU part of the GIC (SGIs/PPIs, CPU interface, redistributor), banked on CPU switches */
typedef struct {
    uint8_t enabled[32], latch[32], line[32], active[32], edge[32], group[32], prio[32];
    uint8_t pmr, bpr0, bpr1, igrpen0, igrpen1;
    uint32_t ctlr;
    uint8_t ap_prio[32];
    uint16_t ap_id[32];
    int ap_n;
    uint32_t rwaker, rctlr;
    uint64_t propbaser, pendbaser;
    uint8_t lpi_pend[GIC_NLPI];
    int lpi_npend;
} wd_gic_cpu;

#define WD_MAX_CPUS 8

/* ---------------------------------------------------------------- ITS */

typedef struct { uint32_t devid, eventid, pintid; uint16_t icid; } wd_its_map;

typedef struct {
    uint32_t ctlr;
    uint64_t cbaser, cwriter, creadr;
    uint64_t baser[8];
    struct { uint32_t devid; uint8_t size; } dev[256];
    int ndev;
    wd_its_map *map;
    int nmap, cap;
    uint8_t coll_valid[256];
    uint8_t coll_target[256];    /* processor number of each collection */
} wd_its;

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
#define FTLB_SIZE 256

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
    uint8_t ro;              /* host pointer is read-only (flash in array mode) */
    uint8_t attr;            /* MAIR attribute index */
} wd_tlbe;

/* ---------------------------------------------------------------- flash */

typedef struct {
    uint8_t *mem;
    uint64_t size;
    int mode, readonly, dirty;
    uint64_t used;           /* bytes worth saving: initial image or highest written offset */
    uint32_t status;
    uint64_t wb_addr;
    unsigned wb_words, wb_len;
    uint8_t wb[4096];
} wd_flash;

/* ---------------------------------------------------------------- fw_cfg */

#define WD_FWCFG_MAX 64
typedef struct wd_fwcfg_item wd_fwcfg_item;
struct wd_fwcfg_item {
    uint16_t sel;
    char name[56];
    uint8_t *data;
    uint32_t len;
    int writable;
    void (*on_write)(struct wd_machine *m, wd_fwcfg_item *it);
};

typedef struct {
    wd_fwcfg_item items[WD_FWCFG_MAX];
    int n;
    uint16_t next_file;
    wd_fwcfg_item *cur;
    uint32_t off;
    uint32_t dma_hi;
} wd_fwcfg;

/* ---------------------------------------------------------------- PCI */

enum { WD_BAR_NONE = 0, WD_BAR_MEM32, WD_BAR_MEM64, WD_BAR_IO };
#define WD_MAX_PCI 16

typedef struct wd_pcidev wd_pcidev;
struct wd_pcidev {
    int devfn;
    uint8_t cfg[4096], wmask[4096], w1cmask[4096];
    uint64_t bar_size[6];
    uint8_t bar_type[6];
    int cap_next;
    int intx;
    uint64_t (*bar_read)(struct wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size);
    void (*bar_write)(struct wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size, uint64_t val);
    void (*on_cfg_write)(struct wd_machine *m, wd_pcidev *d, uint32_t off, int size, uint32_t val);
    void (*on_free)(wd_pcidev *d);
    void *opaque;
    /* MSI-X (wd_pci_msix_init) */
    int msix_cap, msix_n, msix_bar;
    uint32_t msix_table_off, msix_pba_off;
    uint8_t *msix_table;         /* msix_n entries of 16 bytes */
    uint64_t msix_pba[2];        /* up to 128 vectors */
};

/* ---------------------------------------------------------------- machine */

struct wd_machine {
    wd_cpu cpu;
    wd_gic gic;
    wd_its its;
    /* SMP: m->cpu and the banked GIC state belong to CPU 'cur'; the others wait here */
    int ncpu, cur;
    wd_cpu cpus[WD_MAX_CPUS];
    wd_gic_cpu gicp[WD_MAX_CPUS];
    uint8_t cpu_on[WD_MAX_CPUS], cpu_wfi[WD_MAX_CPUS];
    uint64_t slice_end;
    int yield;
    wd_uart uart;

    uint8_t *ram;
    uint64_t ram_size;
    int realtime;
    uint64_t rt_base_ns;

    wd_tlbe tlb[TLB_SIZE];
    /* fast TLB for loads/stores that hit RAM: [mode][index], mode 0 = EL0, 1 = EL1, 2 = EL1 with PAN */
    struct { uint64_t vpn; uint8_t *host; } ftlb_r[3][FTLB_SIZE], ftlb_w[3][FTLB_SIZE];
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
    int no_fast;             /* use only the general decoder (tests) */
    int use_jit;             /* block translation on AArch64 hosts */
    void *jit;
    int64_t jit_left;        /* instructions translated code may still run */
    uint32_t *jit_exit_site; /* chainable exit taken by the last block */
    uint8_t *code_page;      /* per RAM page: holds translated code (writes are tracked) */
    void *usbnet;            /* USB network adapter (wd_usbdev), polled from the run loop */
    uint64_t net_next;       /* icount of the next network poll */
    uint64_t idle_ns;
    int wfi;

    char err[256];
    uint64_t warned_sysreg[16];
    int warned_n;

    /* platform */
    wd_flash flash[2];
    wd_fwcfg fwcfg;
    wd_pcidev *pci[WD_MAX_PCI];
    int npci;
    int firmware;            /* booting UEFI firmware from flash */
    struct { uint64_t addr; uint32_t width, height, stride, generation; int valid; } fb;
    uint8_t *dtb;
    size_t dtb_size;
};

/* cpu.c */
void wd_cpu_reset(wd_machine *m);
void wd_cpu_step(wd_machine *m);
void wd_cpu_run(wd_machine *m, uint64_t n);
void wd_state_trace(wd_machine *m);
/* jit.c */
uint64_t wd_jit_run(wd_machine *m, uint64_t max);
void wd_jit_free(wd_machine *m);
int wd_jit_available(void);
void wd_jit_page_written(wd_machine *m, uint64_t page);
/* a write of [pa, pa + len) in RAM done outside the CPU's store path (DMA) */
static inline void wd_code_write_check(wd_machine *m, uint64_t pa, size_t len)
{
    if (!m->code_page || pa < WD_RAM_BASE || pa - WD_RAM_BASE >= m->ram_size || !len) return;
    uint64_t first = (pa - WD_RAM_BASE) >> 12, last = (pa - WD_RAM_BASE + len - 1) >> 12;
    for (uint64_t p = first; p <= last && p < (m->ram_size >> 12); p++)
        if (m->code_page[p]) wd_jit_page_written(m, p);
}
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
uint64_t wd_read_slow(wd_machine *m, uint64_t va, int size, int unpriv);
void wd_write_slow(wd_machine *m, uint64_t va, int size, uint64_t val, int unpriv);
static inline int wd_mem_mode(const wd_machine *m);
static inline uint64_t wd_read(wd_machine *m, uint64_t va, int size, int unpriv);
static inline void wd_write(wd_machine *m, uint64_t va, int size, uint64_t val, int unpriv);
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
uint8_t *wd_flash_host(wd_flash *f, uint64_t off);
uint64_t wd_flash_read(wd_machine *m, wd_flash *f, uint64_t off, int size);
void wd_flash_write(wd_machine *m, wd_flash *f, uint64_t off, int size, uint64_t val);
int wd_flash_init(wd_flash *f, uint64_t size, const uint8_t *data, size_t len, int readonly);
void wd_flash_free(wd_flash *f);

void wd_ramfb_init(wd_machine *m);
void wd_fwcfg_init(wd_fwcfg *f);
void wd_fwcfg_free(wd_fwcfg *f);
wd_fwcfg_item *wd_fwcfg_add_file(wd_fwcfg *f, const char *name, const void *data, uint32_t len, int writable);
wd_fwcfg_item *wd_fwcfg_file(wd_fwcfg *f, const char *name);
uint64_t wd_fwcfg_read(wd_machine *m, wd_fwcfg *f, uint64_t off, int size);
void wd_fwcfg_write(wd_machine *m, wd_fwcfg *f, uint64_t off, int size, uint64_t val);

void wd_pci_init_dev(wd_pcidev *d, int devfn, uint16_t vendor, uint16_t device, uint32_t class_rev,
                     uint16_t subvendor, uint16_t subdev);
void wd_pci_set_bar(wd_pcidev *d, int bar, uint64_t size, int type);
int wd_pci_add_cap(wd_pcidev *d, uint8_t id, int len);
uint64_t wd_pci_bar_addr(const wd_pcidev *d, int bar);
void wd_pci_set_intx(wd_machine *m, wd_pcidev *d, int level);
uint32_t wd_pci_cfg_read(wd_pcidev *d, uint32_t off, int size);
void wd_pci_cfg_write(wd_machine *m, wd_pcidev *d, uint32_t off, int size, uint32_t val);
uint64_t wd_ecam_read(wd_machine *m, uint64_t off, int size);
void wd_ecam_write(wd_machine *m, uint64_t off, int size, uint64_t val);
uint64_t wd_pci_mmio_read(wd_machine *m, uint64_t pa, int size, int io);
void wd_pci_mmio_write(wd_machine *m, uint64_t pa, int size, uint64_t val, int io);
int wd_dma_read(wd_machine *m, uint64_t pa, void *buf, size_t len);
int wd_dma_write(wd_machine *m, uint64_t pa, const void *buf, size_t len);
void wd_pci_add(wd_machine *m, wd_pcidev *d);
void wd_pci_host_init(wd_machine *m);
int wd_pci_free_slot(wd_machine *m);
int wd_pci_msix_init(wd_pcidev *d, int nvec, int bar, uint32_t table_off, uint32_t pba_off);
int wd_pci_msix_enabled(const wd_pcidev *d);
void wd_pci_msix_notify(wd_machine *m, wd_pcidev *d, int vector);
void wd_pci_add_std_caps(wd_pcidev *d);
int wd_phys_is_ram(wd_machine *m, uint64_t pa);

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
void wd_gic_set_lpi(wd_machine *m, uint32_t intid);
void wd_gic_set_lpi_cpu(wd_machine *m, int cpu, uint32_t intid);
void wd_gic_bank_save(wd_machine *m, int cpu);
void wd_gic_bank_load(wd_machine *m, int cpu);
void wd_gic_cpu_reset(wd_gic_cpu *g);
void wd_start_cpu(wd_machine *m, int k, uint64_t entry, uint64_t context);
/* its.c */
uint64_t wd_its_read(wd_machine *m, uint64_t off, int size);
void wd_its_write(wd_machine *m, uint64_t off, int size, uint64_t val);
void wd_its_free(wd_machine *m);
/* MSI write from a PCI function (requester ID = bus:dev.fn) */
void wd_msi_write(wd_machine *m, uint32_t requester, uint64_t addr, uint32_t data);

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

/* ---------------------------------------------------------------- inline memory fast path */

static inline int wd_mem_mode(const wd_machine *m)
{
    const wd_cpu *c = &m->cpu;
    if (c->el == 0) return 0;
    return (c->pan && (c->sctlr & 1)) ? 2 : 1;
}

static inline uint64_t wd_read(wd_machine *m, uint64_t va, int size, int unpriv)
{
    uint64_t vpn = va >> 12;
    if (!unpriv && (va & 0xFFF) + (uint64_t)size <= 0x1000 && !((m->cpu.sctlr & 2) && (va & (uint64_t)(size - 1)))) {
        int mode = wd_mem_mode(m);
        unsigned i = (unsigned)vpn & (FTLB_SIZE - 1);
        if (m->ftlb_r[mode][i].vpn == vpn) {
            const uint8_t *p = m->ftlb_r[mode][i].host + (va & 0xFFF);
            switch (size) {
            case 1: return *p;
            case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
            case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
            default: { uint64_t v; memcpy(&v, p, 8); return v; }
            }
        }
    }
    return wd_read_slow(m, va, size, unpriv);
}

static inline void wd_write(wd_machine *m, uint64_t va, int size, uint64_t val, int unpriv)
{
    uint64_t vpn = va >> 12;
    if (!unpriv && (va & 0xFFF) + (uint64_t)size <= 0x1000 && !((m->cpu.sctlr & 2) && (va & (uint64_t)(size - 1)))) {
        int mode = wd_mem_mode(m);
        unsigned i = (unsigned)vpn & (FTLB_SIZE - 1);
        if (m->ftlb_w[mode][i].vpn == vpn) {
            uint8_t *p = m->ftlb_w[mode][i].host + (va & 0xFFF);
            switch (size) {
            case 1: *p = (uint8_t)val; return;
            case 2: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); return; }
            case 4: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); return; }
            default: memcpy(p, &val, 8); return;
            }
        }
    }
    wd_write_slow(m, va, size, val, unpriv);
}

void wd_net_poll(wd_machine *m);

#endif

