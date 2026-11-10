/*
 * WindDroid - machine: physical bus, loader, PSCI, timers and run loop.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define NET_POLL_INSNS 200000ULL
#define NET_IDLE_NS    2000000ULL

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../core/internal.h"
#include "platform.h"

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------- lifecycle */

wd_machine *wd_create(const wd_config *cfg)
{
    wd_machine *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    uint64_t size = cfg && cfg->ram_size ? cfg->ram_size : 256ULL << 20;
    size = (size + 0xFFF) & ~0xFFFULL;
    m->ram = calloc(1, (size_t)size);
    if (!m->ram) { free(m); return NULL; }
    m->ram_size = size;
    m->realtime = cfg ? cfg->realtime : 0;
    m->rt_base_ns = mono_ns();
    m->stop = -1;
    wd_gic_reset(&m->gic);
    wd_uart_reset(&m->uart);
    wd_fwcfg_init(&m->fwcfg);
    wd_pci_host_init(m);
    m->ncpu = cfg && cfg->ncpu > 1 ? (cfg->ncpu > WD_MAX_CPUS ? WD_MAX_CPUS : cfg->ncpu) : 1;
    for (int k = 0; k < m->ncpu; k++) wd_gic_cpu_reset(&m->gicp[k]);
    m->cpu_on[0] = 1;
    wd_cpu_reset(m);
    m->use_jit = wd_jit_available() && !getenv("WINDDROID_NO_JIT");
    return m;
}

int wd_set_option(wd_machine *m, const char *name, int value)
{
    if (!strcmp(name, "no_fast")) { m->no_fast = value; return 0; }
    if (!strcmp(name, "jit")) { m->use_jit = value && wd_jit_available(); return m->use_jit == !!value ? 0 : -1; }
    return -1;
}

void wd_destroy(wd_machine *m)
{
    if (!m) return;
    wd_platform_free(m);
    wd_jit_free(m);
    free(m->ram);
    free(m);
}

void wd_set_uart_tx(wd_machine *m, wd_uart_tx_fn fn, void *opaque)
{
    m->uart_tx = fn;
    m->uart_opaque = opaque;
}

void wd_set_trace(wd_machine *m, int on) { m->trace = on; }
int wd_exit_code(wd_machine *m) { return m->exit_code; }
uint64_t wd_icount(wd_machine *m) { return m->cpu.icount; }
uint64_t wd_pc(wd_machine *m) { return m->cpu.pc; }
const char *wd_error(wd_machine *m) { return m->err; }
uint64_t wd_idle_ns(wd_machine *m) { return m->idle_ns; }
void wd_set_entry(wd_machine *m, uint64_t pc) { m->cpu.pc = pc; }

void wd_dump_state(wd_machine *m, char *buf, size_t len)
{
    wd_cpu *c = &m->cpu;
    int n = snprintf(buf, len, "pc=%016llx EL%d sp=%016llx nzcv=%x daif=%x icount=%llu\n"
                     "esr=%016llx far=%016llx elr=%016llx sctlr=%016llx\n",
                     (unsigned long long)c->pc, c->el, (unsigned long long)*wd_sp_ptr(c),
                     c->nzcv >> 28, c->daif >> 6, (unsigned long long)c->icount,
                     (unsigned long long)c->esr, (unsigned long long)c->far,
                     (unsigned long long)c->elr, (unsigned long long)c->sctlr);
    for (int i = 0; i < 31 && n > 0 && (size_t)n < len; i++) {
        n += snprintf(buf + n, len - (size_t)n, "x%-2d=%016llx%s", i, (unsigned long long)c->x[i],
                      (i % 4 == 3) ? "\n" : "  ");
    }
    if (n > 0 && (size_t)n < len)
        n += snprintf(buf + n, len - (size_t)n,
                      "timer: cnt=%llx cntv_ctl=%llx cval=%llx cntp_ctl=%llx cval=%llx\n"
                      "gic: dctlr=%x pmr=%x igrpen0=%d igrpen1=%d rpr=%x ap_n=%d\n",
                      (unsigned long long)wd_counter(m), (unsigned long long)c->cntv_ctl,
                      (unsigned long long)c->cntv_cval, (unsigned long long)c->cntp_ctl,
                      (unsigned long long)c->cntp_cval, m->gic.dctlr, m->gic.pmr, m->gic.igrpen0,
                      m->gic.igrpen1, (unsigned)wd_gic_rpr(m), m->gic.ap_n);
    for (int i = 0; i < GIC_NIRQ && n > 0 && (size_t)n < len; i++) {
        wd_gic *g = &m->gic;
        if (!g->enabled[i] && !g->latch[i] && !g->line[i]) continue;
        n += snprintf(buf + n, len - (size_t)n, "  irq%d en=%d latch=%d line=%d act=%d grp=%d prio=%x edge=%d\n", i,
                      g->enabled[i], g->latch[i], g->line[i], g->active[i], g->group[i], g->prio[i], g->edge[i]);
    }
}

/* ---------------------------------------------------------------- physical bus */

int wd_phys_is_ram(wd_machine *m, uint64_t pa)
{
    return pa >= WD_RAM_BASE && pa - WD_RAM_BASE < m->ram_size;
}

uint8_t *wd_phys_host(wd_machine *m, uint64_t pa)
{
    if (pa >= WD_RAM_BASE && pa - WD_RAM_BASE < m->ram_size) return m->ram + (pa - WD_RAM_BASE);
    if (pa < 2 * WD_FLASH_SIZE) {
        wd_flash *f = &m->flash[pa >= WD_FLASH_SIZE];
        return wd_flash_host(f, pa % WD_FLASH_SIZE);
    }
    return NULL;
}

static int in(uint64_t pa, uint64_t base, uint64_t size) { return pa >= base && pa - base < size; }

uint64_t wd_phys_read(wd_machine *m, uint64_t pa, int size)
{
    if (wd_phys_is_ram(m, pa)) {
        uint64_t v = 0;
        memcpy(&v, m->ram + (pa - WD_RAM_BASE), (size_t)size);
        return v;
    }
    if (pa < 2 * WD_FLASH_SIZE) {
        wd_flash *f = &m->flash[pa >= WD_FLASH_SIZE];
        return f->mem ? wd_flash_read(m, f, pa % WD_FLASH_SIZE, size) : 0;
    }
    if (in(pa, WD_GICD_BASE, 0x10000)) return wd_gicd_read(m, pa - WD_GICD_BASE, size);
    if (in(pa, WD_GICR_BASE, 0x20000ULL * (uint64_t)m->ncpu)) return wd_gicr_read(m, pa - WD_GICR_BASE, size);
    if (in(pa, WD_GITS_BASE, 0x20000)) return wd_its_read(m, pa - WD_GITS_BASE, size);
    if (in(pa, WD_UART_BASE, 0x1000)) return wd_uart_read(m, (pa - WD_UART_BASE) & ~3ULL);
    if (in(pa, WD_RTC_BASE, 0x1000)) return wd_rtc_read(m, (pa - WD_RTC_BASE) & ~3ULL);
    if (in(pa, WD_FWCFG_BASE, 0x18)) return wd_fwcfg_read(m, &m->fwcfg, pa - WD_FWCFG_BASE, size);
    if (in(pa, WD_CTL_BASE, 0x1000)) return wd_ctl_read(m, pa - WD_CTL_BASE);
    if (in(pa, WD_PCIE_ECAM, WD_PCIE_ECAM_SZ)) return wd_ecam_read(m, pa - WD_PCIE_ECAM, size);
    if (in(pa, WD_PCIE_MMIO, WD_PCIE_MMIO_SZ) || in(pa, WD_PCIE_MMIO_HI, WD_PCIE_MMIO_HI_SZ))
        return wd_pci_mmio_read(m, pa, size, 0);
    if (in(pa, WD_PCIE_PIO, 0x10000)) return wd_pci_mmio_read(m, pa - WD_PCIE_PIO, size, 1);
    return 0;                                   /* unassigned: reads as zero */
}

void wd_phys_write(wd_machine *m, uint64_t pa, int size, uint64_t val)
{
    if (wd_phys_is_ram(m, pa)) { memcpy(m->ram + (pa - WD_RAM_BASE), &val, (size_t)size); return; }
    if (pa < 2 * WD_FLASH_SIZE) {
        wd_flash *f = &m->flash[pa >= WD_FLASH_SIZE];
        if (f->mem) wd_flash_write(m, f, pa % WD_FLASH_SIZE, size, val);
        return;
    }
    if (in(pa, WD_GICD_BASE, 0x10000)) { wd_gicd_write(m, pa - WD_GICD_BASE, size, val); return; }
    if (in(pa, WD_GICR_BASE, 0x20000ULL * (uint64_t)m->ncpu)) { wd_gicr_write(m, pa - WD_GICR_BASE, size, val); return; }
    if (in(pa, WD_GITS_BASE, 0x20000)) { wd_its_write(m, pa - WD_GITS_BASE, size, val); return; }
    if (in(pa, WD_UART_BASE, 0x1000)) { wd_uart_write(m, (pa - WD_UART_BASE) & ~3ULL, val); return; }
    if (in(pa, WD_RTC_BASE, 0x1000)) { wd_rtc_write(m, (pa - WD_RTC_BASE) & ~3ULL, val); return; }
    if (in(pa, WD_FWCFG_BASE, 0x18)) { wd_fwcfg_write(m, &m->fwcfg, pa - WD_FWCFG_BASE, size, val); return; }
    if (in(pa, WD_CTL_BASE, 0x1000)) { wd_ctl_write(m, pa - WD_CTL_BASE, val); return; }
    if (in(pa, WD_PCIE_ECAM, WD_PCIE_ECAM_SZ)) { wd_ecam_write(m, pa - WD_PCIE_ECAM, size, val); return; }
    if (in(pa, WD_PCIE_MMIO, WD_PCIE_MMIO_SZ) || in(pa, WD_PCIE_MMIO_HI, WD_PCIE_MMIO_HI_SZ)) {
        wd_pci_mmio_write(m, pa, size, val, 0);
        return;
    }
    if (in(pa, WD_PCIE_PIO, 0x10000)) { wd_pci_mmio_write(m, pa - WD_PCIE_PIO, size, val, 1); return; }
}

/* ---------------------------------------------------------------- loading */

int wd_load_raw(wd_machine *m, const uint8_t *data, size_t len, uint64_t paddr)
{
    if (paddr < WD_RAM_BASE || paddr - WD_RAM_BASE + len > m->ram_size) {
        snprintf(m->err, sizeof m->err, "image (%zu bytes at %#llx) does not fit in RAM", len,
                 (unsigned long long)paddr);
        return -1;
    }
    memcpy(m->ram + (paddr - WD_RAM_BASE), data, len);
    wd_tlb_flush(m);
    return 0;
}

static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int wd_load_elf(wd_machine *m, const uint8_t *d, size_t len)
{
    if (len < 64 || memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 2 || d[5] != 1) {
        snprintf(m->err, sizeof m->err, "not a 64-bit little-endian ELF file");
        return -1;
    }
    if (rd16(d + 18) != 183) {
        snprintf(m->err, sizeof m->err, "ELF machine is not AArch64");
        return -1;
    }
    uint64_t entry = rd64(d + 24), phoff = rd64(d + 32);
    uint16_t phentsize = rd16(d + 54), phnum = rd16(d + 56);
    if (phoff + (uint64_t)phnum * phentsize > len || phentsize < 56) {
        snprintf(m->err, sizeof m->err, "truncated ELF program headers");
        return -1;
    }
    int loaded = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;                    /* PT_LOAD */
        uint64_t off = rd64(ph + 8), paddr = rd64(ph + 24);
        uint64_t filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (off + filesz > len || filesz > memsz) {
            snprintf(m->err, sizeof m->err, "bad ELF segment %u", i);
            return -1;
        }
        if (paddr < WD_RAM_BASE || paddr - WD_RAM_BASE + memsz > m->ram_size) {
            snprintf(m->err, sizeof m->err, "ELF segment at %#llx (%llu bytes) is outside RAM",
                     (unsigned long long)paddr, (unsigned long long)memsz);
            return -1;
        }
        memcpy(m->ram + (paddr - WD_RAM_BASE), d + off, filesz);
        memset(m->ram + (paddr - WD_RAM_BASE) + filesz, 0, memsz - filesz);
        loaded++;
    }
    if (!loaded) {
        snprintf(m->err, sizeof m->err, "ELF has no loadable segments");
        return -1;
    }
    wd_tlb_flush(m);
    m->cpu.pc = entry;
    return 0;
}

int wd_load_image(wd_machine *m, const uint8_t *data, size_t len)
{
    if (len >= 4 && memcmp(data, "\x7f" "ELF", 4) == 0) return wd_load_elf(m, data, len);
    if (wd_load_raw(m, data, len, WD_RAM_BASE) != 0) return -1;
    m->cpu.pc = WD_RAM_BASE;
    return 0;
}

/* ---------------------------------------------------------------- timers */

uint64_t wd_counter(wd_machine *m)
{
    if (m->realtime) return (mono_ns() - m->rt_base_ns) / 16;   /* 62.5 MHz */
    return m->cpu.icount;
}

void wd_update_timers(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    uint64_t now = wd_counter(m);
    int v = (c->cntv_ctl & 1) && !(c->cntv_ctl & 2) && now >= c->cntv_cval;
    int p = (c->cntp_ctl & 1) && !(c->cntp_ctl & 2) && now >= c->cntp_cval;
    wd_gic_set_line(m, WD_PPI_VTIMER, v);
    wd_gic_set_line(m, WD_PPI_PTIMER, p);
}

/* Earliest future deadline of an armed timer, or UINT64_MAX. */
static uint64_t next_deadline1(const wd_machine *m, const wd_cpu *c);

/* earliest timer deadline of all CPUs that are on */
static uint64_t next_deadline(wd_machine *m)
{
    uint64_t d = next_deadline1(m, &m->cpu);
    for (int k = 0; k < m->ncpu; k++) {
        if (k == m->cur || !m->cpu_on[k]) continue;
        uint64_t e = next_deadline1(m, &m->cpus[k]);
        if (e < d) d = e;
    }
    return d;
}

static uint64_t next_deadline1(const wd_machine *m, const wd_cpu *c)
{
    uint64_t now = wd_counter((wd_machine *)m), d = UINT64_MAX;
    if ((c->cntv_ctl & 3) == 1 && c->cntv_cval > now && c->cntv_cval < d) d = c->cntv_cval;
    if ((c->cntp_ctl & 3) == 1 && c->cntp_cval > now && c->cntp_cval < d) d = c->cntp_cval;
    return d;
}

/* ---------------------------------------------------------------- SMP */

#define SLICE 20000                                     /* instructions per CPU turn */

/* make CPU k the running one */
static void switch_to(wd_machine *m, int k)
{
    if (k == m->cur) return;
    uint64_t now = m->cpu.icount;                       /* one counter for the whole machine */
    m->cpu.excl_valid = 0;                              /* another CPU may write the location */
    m->cpus[m->cur] = m->cpu;
    m->cpu_wfi[m->cur] = (uint8_t)m->wfi;
    wd_gic_bank_save(m, m->cur);
    m->cur = k;
    m->cpu = m->cpus[k];
    m->cpu.icount = now;
    m->wfi = m->cpu_wfi[k];
    wd_gic_bank_load(m, k);
    wd_tlb_flush(m);
    m->irq_dirty = 1;
}

void wd_start_cpu(wd_machine *m, int k, uint64_t entry, uint64_t context)
{
    wd_cpu saved = m->cpu;
    wd_cpu_reset(m);                                    /* fresh state in m->cpu, then park it */
    m->cpu.pc = entry;
    m->cpu.x[0] = context;
    m->cpus[k] = m->cpu;
    m->cpu = saved;
    wd_tlb_flush(m);
    wd_gic_cpu_reset(&m->gicp[k]);
    m->cpu_on[k] = 1;
    m->cpu_wfi[k] = 0;
}

/* Pick the next CPU that can run (round robin); stays on an idle CPU if none can. */
static void schedule(wd_machine *m)
{
    m->yield = 0;
    m->slice_end = m->cpu.icount + SLICE;
    int start = m->cur;
    for (int step = 1; step <= m->ncpu; step++) {
        int k = (start + step) % m->ncpu;
        if (!m->cpu_on[k]) continue;
        switch_to(m, k);
        wd_update_timers(m);
        if (!m->wfi || wd_gic_any_pending(m)) return;
    }
    /* nobody can run: keep the CPU that is on (and idle) for the deadline computation */
    if (!m->cpu_on[m->cur])
        for (int k = 0; k < m->ncpu; k++) if (m->cpu_on[k]) { switch_to(m, k); break; }
}

/* ---------------------------------------------------------------- PSCI */

#define PSCI_SUCCESS        0
#define PSCI_NOT_SUPPORTED  (-1)
#define PSCI_INVALID_PARAMS (-2)
#define PSCI_ALREADY_ON     (-4)

int wd_psci(wd_machine *m)
{
    wd_cpu *c = &m->cpu;
    uint32_t fn = (uint32_t)c->x[0];
    int64_t ret;
    switch (fn) {
    case 0x80000000: ret = 0x10001; break;              /* SMCCC_VERSION 1.1 */
    case 0x84000000: ret = 0x10000; break;              /* PSCI_VERSION 1.0 */
    case 0x84000001: case 0xC4000001:                   /* CPU_SUSPEND */
        m->wfi = 1;
        ret = PSCI_SUCCESS;
        break;
    case 0x84000002: {                                  /* CPU_OFF */
        m->cpu_on[m->cur] = 0;
        int any = 0;
        for (int k = 0; k < m->ncpu; k++) any |= m->cpu_on[k];
        if (!any) m->stop = WD_RUN_SHUTDOWN;
        m->yield = 1;
        m->irq_dirty = 1;
        ret = PSCI_SUCCESS;                             /* (not returned: the CPU is off) */
        break;
    }
    case 0x84000003: case 0xC4000003: {                 /* CPU_ON */
        uint64_t mpidr = c->x[1] & 0xFF00FFFFFFULL;
        int k = (int)(mpidr & 0xFF);
        if ((mpidr & ~0xFFULL) || k >= m->ncpu) { ret = PSCI_INVALID_PARAMS; break; }
        if (m->cpu_on[k]) { ret = PSCI_ALREADY_ON; break; }
        wd_start_cpu(m, k, fn == 0x84000003 ? (uint32_t)c->x[2] : c->x[2], fn == 0x84000003 ? (uint32_t)c->x[3] : c->x[3]);
        ret = PSCI_SUCCESS;
        break;
    }
    case 0x84000004: case 0xC4000004: {                 /* AFFINITY_INFO */
        uint64_t mpidr = c->x[1] & 0xFF00FFFFFFULL;
        int k = (int)(mpidr & 0xFF);
        if ((mpidr & ~0xFFULL) || k >= m->ncpu) { ret = PSCI_INVALID_PARAMS; break; }
        ret = m->cpu_on[k] ? 0 : 1;                     /* ON / OFF */
        break;
    }
    case 0x84000006: ret = 2; break;                    /* MIGRATE_INFO_TYPE: not needed */
    case 0x84000008:                                    /* SYSTEM_OFF */
        m->stop = WD_RUN_SHUTDOWN;
        ret = PSCI_SUCCESS;
        break;
    case 0x84000009:                                    /* SYSTEM_RESET */
        m->stop = WD_RUN_RESET;
        ret = PSCI_SUCCESS;
        break;
    case 0x8400000A: {                                  /* PSCI_FEATURES */
        uint32_t q = (uint32_t)c->x[1];
        switch (q) {
        case 0x84000000: case 0x84000001: case 0xC4000001: case 0x84000002:
        case 0x84000003: case 0xC4000003: case 0x84000004: case 0xC4000004:
        case 0x84000006: case 0x84000008: case 0x84000009: case 0x8400000A:
            ret = 0; break;
        default: ret = PSCI_NOT_SUPPORTED; break;
        }
        break;
    }
    default: ret = PSCI_NOT_SUPPORTED; break;
    }
    c->x[0] = (uint64_t)ret;
    return 0;
}

/* ---------------------------------------------------------------- run loop */

wd_run_result wd_run(wd_machine *m, uint64_t max_insns)
{
    wd_cpu *c = &m->cpu;
    m->stop = -1;
    m->budget = max_insns;
    m->idle_ns = 0;
    m->irq_dirty = 1;
    if (m->usbnet) { wd_net_poll(m); m->net_next = c->icount + NET_POLL_INSNS; }

    if (setjmp(m->jb)) {
        if (m->stop >= 0) goto done;
        if (m->budget) m->budget--;
    }

    while (m->budget) {
        {
            uint32_t i = m->diag_n++ % WD_DIAG_SAMPLES;
            m->diag_pc[i] = c->pc;
            m->diag_cpu[i] = (uint8_t)m->cur;
        }
        /* SMP: another CPU's turn at the end of a slice, on WFI/WFE, or when this one is off */
        if (m->ncpu > 1 && (m->yield || m->wfi || !m->cpu_on[m->cur] || c->icount >= m->slice_end)) {
            if (m->wfi && !m->yield && m->cpu_on[m->cur] && c->icount < m->slice_end) {
                wd_update_timers(m);
                if (!wd_gic_any_pending(m)) schedule(m);
            } else {
                schedule(m);
            }
        }
        if (m->usbnet && c->icount >= m->net_next) {
            m->net_next = c->icount + NET_POLL_INSNS;
            wd_net_poll(m);
        }
        if (m->irq_dirty || (c->icount & 255) == 0) {
            m->irq_dirty = 0;
            wd_update_timers(m);
            m->irq_pend = wd_gic_irq_pending(m);
        }
        if (m->wfi || !m->cpu_on[m->cur]) {
            if (m->cpu_on[m->cur] && wd_gic_any_pending(m)) {
                m->wfi = 0;
            } else {
                uint64_t d = next_deadline(m);
                if (!m->realtime) {
                    if (d == UINT64_MAX) { m->stop = WD_RUN_IDLE; goto done; }
                    c->icount = d;                      /* skip idle time */
                    m->irq_dirty = 1;
                    continue;
                }
                if (d == UINT64_MAX) m->idle_ns = UINT64_MAX;
                else {
                    uint64_t now = wd_counter(m);
                    m->idle_ns = (d - now) * 16;
                }
                if (m->usbnet && m->idle_ns > NET_IDLE_NS) m->idle_ns = NET_IDLE_NS;   /* look at the sockets again soon */
                m->stop = WD_RUN_IDLE;
                goto done;
            }
        }
        if (m->irq_pend && !(c->daif & DAIF_I)) { m->irq_pend = 0; wd_take_irq(m); }
        /* run until the next periodic interrupt check (every 256 instructions) */
        uint64_t n = 256 - (c->icount & 255);
        if (n > m->budget) n = m->budget;
        wd_cpu_run(m, n);
        if (m->stop >= 0) goto done;
    }
    return WD_RUN_BUDGET;

done:
    return (wd_run_result)m->stop;
}
