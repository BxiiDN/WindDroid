/*
 * WindDroid - GICv3 (single CPU, single security state) with physical
 * LPIs. Distributor at WD_GICD_BASE, one redistributor at WD_GICR_BASE,
 * CPU interface through the ICC_* system registers; LPIs are made pending
 * by the ITS (its.c) and configured through the LPI property table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../core/internal.h"

#define PRIO_MASK 0xF8                      /* 5 priority bits implemented */
#define SPURIOUS  1023

void wd_gic_reset(wd_gic *g)
{
    memset(g, 0, sizeof *g);
    for (int i = 0; i < 16; i++) g->edge[i] = 1;
    g->rwaker = 6;                           /* ProcessorSleep | ChildrenAsleep */
}

void wd_gic_cpu_reset(wd_gic_cpu *g)
{
    memset(g, 0, sizeof *g);
    for (int i = 0; i < 16; i++) g->edge[i] = 1;
    g->rwaker = 6;
}

/* copy the private part of the live GIC state to CPU 'cpu's bank */
void wd_gic_bank_save(wd_machine *m, int cpu)
{
    wd_gic *from = &m->gic;
    wd_gic_cpu *to = &m->gicp[cpu];
    memcpy(to->enabled, from->enabled, 32); memcpy(to->latch, from->latch, 32);
    memcpy(to->line, from->line, 32); memcpy(to->active, from->active, 32);
    memcpy(to->edge, from->edge, 32); memcpy(to->group, from->group, 32); memcpy(to->prio, from->prio, 32);
    to->pmr = from->pmr; to->bpr0 = from->bpr0; to->bpr1 = from->bpr1;
    to->igrpen0 = from->igrpen0; to->igrpen1 = from->igrpen1; to->ctlr = from->ctlr;
    memcpy(to->ap_prio, from->ap_prio, sizeof to->ap_prio); memcpy(to->ap_id, from->ap_id, sizeof to->ap_id);
    to->ap_n = from->ap_n; to->rwaker = from->rwaker; to->rctlr = from->rctlr;
    to->propbaser = from->propbaser; to->pendbaser = from->pendbaser;
    memcpy(to->lpi_pend, from->lpi_pend, sizeof to->lpi_pend); to->lpi_npend = from->lpi_npend;
}

void wd_gic_bank_load(wd_machine *m, int cpu)
{
    wd_gic *to = &m->gic;
    wd_gic_cpu *from = &m->gicp[cpu];
    memcpy(to->enabled, from->enabled, 32); memcpy(to->latch, from->latch, 32);
    memcpy(to->line, from->line, 32); memcpy(to->active, from->active, 32);
    memcpy(to->edge, from->edge, 32); memcpy(to->group, from->group, 32); memcpy(to->prio, from->prio, 32);
    to->pmr = from->pmr; to->bpr0 = from->bpr0; to->bpr1 = from->bpr1;
    to->igrpen0 = from->igrpen0; to->igrpen1 = from->igrpen1; to->ctlr = from->ctlr;
    memcpy(to->ap_prio, from->ap_prio, sizeof to->ap_prio); memcpy(to->ap_id, from->ap_id, sizeof to->ap_id);
    to->ap_n = from->ap_n; to->rwaker = from->rwaker; to->rctlr = from->rctlr;
    to->propbaser = from->propbaser; to->pendbaser = from->pendbaser;
    memcpy(to->lpi_pend, from->lpi_pend, sizeof to->lpi_pend); to->lpi_npend = from->lpi_npend;
    m->irq_dirty = 1;
}

/* SPI routed to this CPU? (GICD_IROUTER: IRM = any CPU, else Aff0) */
static int routed_here(const wd_machine *m, int i)
{
    if (i < 32 || m->ncpu <= 1) return 1;
    uint64_t r = m->gic.irouter[i];
    if (r & (1ULL << 31)) return m->cur == 0;      /* 1-of-N: deliver on CPU 0 */
    return (int)(r & 0xFF) == m->cur;
}

static int pending(const wd_gic *g, int i)
{
    return g->latch[i] || (!g->edge[i] && g->line[i]);
}

static int group_on(const wd_gic *g, int i)
{
    if (g->group[i]) return (g->dctlr & 2) && g->igrpen1;
    return (g->dctlr & 1) && g->igrpen0;
}

static int running_prio(const wd_gic *g)
{
    return g->ap_n ? g->ap_prio[g->ap_n - 1] : 0xFF;
}

/* LPI configuration byte from the property table in guest memory: priority[7:2], enable[0] */
static int lpi_config(wd_machine *m, int n)
{
    wd_gic *g = &m->gic;
    int idbits = (int)(g->propbaser & 0x1F) + 1;
    if (GIC_LPI_BASE + n >= (1 << idbits) || idbits < 14) return 0;
    uint8_t *p = wd_phys_host(m, (g->propbaser & 0x000FFFFFFFFFF000ULL) + (uint64_t)n);
    return p ? *p : 0;
}

/* Highest priority pending interrupt that passes the masks; -1 if none.
 * grp: 0, 1 or -1 for either group. */
static int hppi(wd_machine *m, int grp, int apply_masks)
{
    const wd_gic *g = &m->gic;
    int best = -1, bp = 0x100;
    for (int i = 0; i < GIC_NIRQ; i++) {
        if (!g->enabled[i] || !pending(g, i) || g->active[i]) continue;
        if (grp >= 0 && g->group[i] != grp) continue;
        if (!routed_here(m, i)) continue;
        if (!group_on(g, i)) continue;
        int p = g->prio[i] & PRIO_MASK;
        if (p < bp) { bp = p; best = i; }
    }
    /* LPIs: always group 1, edge, no active state */
    if (g->lpi_npend && (g->rctlr & 1) && grp != 0 && (g->dctlr & 2) && g->igrpen1) {
        for (int n = 0; n < GIC_NLPI; n++) {
            if (!g->lpi_pend[n]) continue;
            int cfg = lpi_config(m, n);
            if (!(cfg & 1)) continue;
            int p = cfg & 0xFC & PRIO_MASK;
            if (p < bp) { bp = p; best = GIC_LPI_BASE + n; }
        }
    }
    if (best >= 0 && apply_masks) {
        if (bp >= g->pmr || bp >= running_prio(g)) return -1;
    }
    return best;
}

int wd_gic_irq_pending(wd_machine *m)
{
    return hppi(m, -1, 1) >= 0;
}

int wd_gic_any_pending(wd_machine *m)
{
    return hppi(m, -1, 1) >= 0;
}

void wd_gic_set_lpi(wd_machine *m, uint32_t intid)
{
    wd_gic_set_lpi_cpu(m, m->cur, intid);
}

void wd_gic_set_lpi_cpu(wd_machine *m, int cpu, uint32_t intid)
{
    if (intid < GIC_LPI_BASE || intid >= GIC_LPI_BASE + GIC_NLPI) return;
    uint8_t *p;
    int *n;
    if (cpu == m->cur || cpu < 0 || cpu >= m->ncpu) { p = &m->gic.lpi_pend[intid - GIC_LPI_BASE]; n = &m->gic.lpi_npend; }
    else { p = &m->gicp[cpu].lpi_pend[intid - GIC_LPI_BASE]; n = &m->gicp[cpu].lpi_npend; }
    if (!*p) { *p = 1; (*n)++; }
    m->irq_dirty = 1;
}

void wd_gic_set_line(wd_machine *m, int irq, int level)
{
    wd_gic *g = &m->gic;
    if (irq < 0 || irq >= GIC_NIRQ) return;
    level = !!level;
    if (g->edge[irq] && level && !g->line[irq]) g->latch[irq] = 1;
    if (g->line[irq] != level) m->irq_dirty = 1;
    g->line[irq] = (uint8_t)level;
}

static uint64_t ack(wd_machine *m, int grp)
{
    wd_gic *g = &m->gic;
    int i = hppi(m, grp, 1);
    if (i < 0) return SPURIOUS;
    int prio;
    if (i >= GIC_LPI_BASE) {
        g->lpi_pend[i - GIC_LPI_BASE] = 0;
        g->lpi_npend--;
        prio = lpi_config(m, i - GIC_LPI_BASE) & 0xFC & PRIO_MASK;
    } else {
        g->latch[i] = 0;
        g->active[i] = 1;
        prio = g->prio[i] & PRIO_MASK;
    }
    if (g->ap_n < 32) {
        g->ap_prio[g->ap_n] = (uint8_t)prio;
        g->ap_id[g->ap_n] = (uint16_t)i;
        g->ap_n++;
    }
    m->irq_dirty = 1;
    return (uint64_t)i;
}

uint64_t wd_gic_iar(wd_machine *m)
{
    /* Group 0 and group 1 are both delivered as IRQ in this model. */
    return ack(m, -1);
}

uint64_t wd_gic_hppir(wd_machine *m)
{
    int i = hppi(m, -1, 0);
    return i < 0 ? SPURIOUS : (uint64_t)i;
}

uint64_t wd_gic_rpr(wd_machine *m)
{
    return (uint64_t)running_prio(&m->gic);
}

void wd_gic_dir(wd_machine *m, uint32_t id)
{
    if (id < GIC_NIRQ) m->gic.active[id] = 0;
    m->irq_dirty = 1;
}

void wd_gic_eoi(wd_machine *m, uint32_t id)
{
    wd_gic *g = &m->gic;
    if (id >= GIC_NIRQ && (id < GIC_LPI_BASE || id >= GIC_LPI_BASE + GIC_NLPI)) return;
    for (int k = g->ap_n - 1; k >= 0; k--) {
        if (g->ap_id[k] == id) {
            memmove(&g->ap_prio[k], &g->ap_prio[k + 1], (size_t)(g->ap_n - k - 1));
            memmove(&g->ap_id[k], &g->ap_id[k + 1], (size_t)(g->ap_n - k - 1) * sizeof g->ap_id[0]);
            g->ap_n--;
            break;
        }
    }
    if (id < GIC_NIRQ && !(g->ctlr & 2)) g->active[id] = 0;   /* EOImode == 0: drop and deactivate */
    m->irq_dirty = 1;
}

void wd_gic_sgi(wd_machine *m, uint64_t v)
{
    int intid = (int)((v >> 24) & 15);
    int irm = (int)((v >> 40) & 1);
    int aff1 = (int)((v >> 16) & 0xFF), aff2 = (int)((v >> 32) & 0xFF), aff3 = (int)((v >> 48) & 0xFF);
    int targets = (int)(v & 0xFFFF);
    int ncpu = m->ncpu > 0 ? m->ncpu : 1;
    for (int k = 0; k < ncpu; k++) {
        int hit = irm ? k != m->cur : (aff1 == 0 && aff2 == 0 && aff3 == 0 && ((targets >> k) & 1));
        if (!hit) continue;
        if (k == m->cur) m->gic.latch[intid] = 1;
        else m->gicp[k].latch[intid] = 1;
        m->irq_dirty = 1;
    }
}

/* ---------------------------------------------------------------- MMIO helpers */

static uint32_t bits_read(const uint8_t *arr, int base)
{
    uint32_t v = 0;
    for (int i = 0; i < 32; i++)
        if (base + i < GIC_NIRQ && arr[base + i]) v |= 1u << i;
    return v;
}

static uint32_t pend_read(const wd_gic *g, int base)
{
    uint32_t v = 0;
    for (int i = 0; i < 32; i++)
        if (base + i < GIC_NIRQ && pending(g, base + i)) v |= 1u << i;
    return v;
}

static void bits_set(uint8_t *arr, int base, uint32_t val, uint8_t to)
{
    for (int i = 0; i < 32; i++)
        if ((val >> i) & 1 && base + i < GIC_NIRQ) arr[base + i] = to;
}

static uint32_t cfg_read(const wd_gic *g, int base)
{
    uint32_t v = 0;
    for (int i = 0; i < 16; i++)
        if (base + i < GIC_NIRQ && g->edge[base + i]) v |= 2u << (2 * i);
    return v;
}

static void cfg_write(wd_gic *g, int base, uint32_t val)
{
    for (int i = 0; i < 16; i++) {
        int irq = base + i;
        if (irq < 16 || irq >= GIC_NIRQ) continue;  /* SGIs are always edge */
        g->edge[irq] = (val >> (2 * i + 1)) & 1;
    }
}

/* Common register block shared by GICD (irq_base from offset) and GICR SGI frame. */
static int common_read(wd_gic *g, uint64_t off, int irq_lo, int irq_hi, uint32_t *out)
{
    int base;
    if (off >= 0x0080 && off < 0x0100) { base = (int)(off - 0x0080) * 8; goto bits_grp; }
    if (off >= 0x0100 && off < 0x0180) { base = (int)(off - 0x0100) * 8; if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; } *out = bits_read(g->enabled, base); return 1; }
    if (off >= 0x0180 && off < 0x0200) { base = (int)(off - 0x0180) * 8; if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; } *out = bits_read(g->enabled, base); return 1; }
    if (off >= 0x0200 && off < 0x0300) { base = (int)((off - 0x0200) & 0x7F) * 8; if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; } *out = pend_read(g, base); return 1; }
    if (off >= 0x0300 && off < 0x0400) { base = (int)((off - 0x0300) & 0x7F) * 8; if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; } *out = bits_read(g->active, base); return 1; }
    if (off >= 0x0C00 && off < 0x0D00) { base = (int)(off - 0x0C00) * 4; if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; } *out = cfg_read(g, base); return 1; }
    if (off >= 0x0D00 && off < 0x0D80) { *out = 0; return 1; }
    return 0;
bits_grp:
    if (base < irq_lo || base >= irq_hi) { *out = 0; return 1; }
    *out = bits_read(g->group, base);
    return 1;
}

static int common_write(wd_machine *m, uint64_t off, int irq_lo, int irq_hi, uint32_t v)
{
    wd_gic *g = &m->gic;
    int base;
    m->irq_dirty = 1;
    if (off >= 0x0080 && off < 0x0100) {
        base = (int)(off - 0x0080) * 8;
        if (base >= irq_lo && base < irq_hi)
            for (int i = 0; i < 32; i++) if (base + i < GIC_NIRQ) g->group[base + i] = (v >> i) & 1;
        return 1;
    }
    if (off >= 0x0100 && off < 0x0180) { base = (int)(off - 0x0100) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->enabled, base, v, 1); return 1; }
    if (off >= 0x0180 && off < 0x0200) { base = (int)(off - 0x0180) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->enabled, base, v, 0); return 1; }
    if (off >= 0x0200 && off < 0x0280) { base = (int)(off - 0x0200) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->latch, base, v, 1); return 1; }
    if (off >= 0x0280 && off < 0x0300) { base = (int)(off - 0x0280) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->latch, base, v, 0); return 1; }
    if (off >= 0x0300 && off < 0x0380) { base = (int)(off - 0x0300) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->active, base, v, 1); return 1; }
    if (off >= 0x0380 && off < 0x0400) { base = (int)(off - 0x0380) * 8; if (base >= irq_lo && base < irq_hi) bits_set(g->active, base, v, 0); return 1; }
    if (off >= 0x0C00 && off < 0x0D00) { base = (int)(off - 0x0C00) * 4; if (base >= irq_lo && base < irq_hi) cfg_write(g, base, v); return 1; }
    if (off >= 0x0D00 && off < 0x0D80) return 1;
    return 0;
}

static uint64_t prio_read(wd_gic *g, uint64_t off, int size, int irq_lo, int irq_hi)
{
    uint64_t v = 0;
    for (int k = 0; k < size; k++) {
        int irq = (int)(off - 0x400) + k;
        if (irq >= irq_lo && irq < irq_hi && irq < GIC_NIRQ) v |= (uint64_t)g->prio[irq] << (8 * k);
    }
    return v;
}

static void prio_write(wd_machine *m, uint64_t off, int size, uint64_t v, int irq_lo, int irq_hi)
{
    for (int k = 0; k < size; k++) {
        int irq = (int)(off - 0x400) + k;
        if (irq >= irq_lo && irq < irq_hi && irq < GIC_NIRQ)
            m->gic.prio[irq] = (uint8_t)((v >> (8 * k)) & PRIO_MASK);
    }
    m->irq_dirty = 1;
}

/* ---------------------------------------------------------------- distributor */

uint64_t wd_gicd_read(wd_machine *m, uint64_t off, int size)
{
    wd_gic *g = &m->gic;
    uint32_t v;
    if (off >= 0x0400 && off < 0x0800) return prio_read(g, off, size, 32, GIC_NIRQ);
    if (off >= 0x6000 && off < 0x8000) {
        int irq = (int)((off - 0x6000) / 8);
        uint64_t r = irq < GIC_NIRQ ? g->irouter[irq] : 0;
        if (size == 4 && (off & 4)) r >>= 32;
        return size == 8 ? r : (uint32_t)r;
    }
    if (size == 8) return wd_gicd_read(m, off, 4) | (wd_gicd_read(m, off + 4, 4) << 32);
    off &= ~3ULL;
    switch (off) {
    case 0x0000: return (g->dctlr & 3) | 0x10 | 0x40;
    case 0x0004: return (13u << 19) | (1u << 17) | (uint32_t)((m->ncpu > 0 ? m->ncpu : 1) - 1) << 5 |
                        (GIC_NIRQ / 32 - 1);   /* IDbits 14, LPIS, CPUNumber */
    case 0x0008: return 0x0000043B;
    case 0x000C: case 0x0010: return 0;
    case 0xFFE8: return 0x3B;
    default: break;
    }
    if (common_read(g, off, 32, GIC_NIRQ, &v)) return v;
    return 0;
}

void wd_gicd_write(wd_machine *m, uint64_t off, int size, uint64_t val)
{
    wd_gic *g = &m->gic;
    if (off >= 0x0400 && off < 0x0800) { prio_write(m, off, size, val, 32, GIC_NIRQ); return; }
    if (off >= 0x6000 && off < 0x8000) {
        int irq = (int)((off - 0x6000) / 8);
        if (irq < GIC_NIRQ) {
            if (size == 8) g->irouter[irq] = val;
            else if (off & 4) g->irouter[irq] = (g->irouter[irq] & 0xFFFFFFFFULL) | (val << 32);
            else g->irouter[irq] = (g->irouter[irq] & ~0xFFFFFFFFULL) | (uint32_t)val;
        }
        return;
    }
    if (size == 8) { wd_gicd_write(m, off, 4, (uint32_t)val); wd_gicd_write(m, off + 4, 4, val >> 32); return; }
    off &= ~3ULL;
    if (off == 0x0000) { g->dctlr = (uint32_t)val & 3; m->irq_dirty = 1; return; }
    common_write(m, off, 32, GIC_NIRQ, (uint32_t)val);
}

/* ---------------------------------------------------------------- redistributor */

static uint64_t gicr_read1(wd_machine *m, uint64_t off, int size, int frame);
static void gicr_write1(wd_machine *m, uint64_t off, int size, uint64_t val, int frame);

/* GICR frames of other CPUs: operate on their bank through the live state */
uint64_t wd_gicr_read(wd_machine *m, uint64_t off, int size)
{
    int frame = (int)(off / 0x20000), ncpu = m->ncpu > 0 ? m->ncpu : 1;
    if (frame >= ncpu) return 0;
    off %= 0x20000;
    if (frame == m->cur) return gicr_read1(m, off, size, frame);
    int cur = m->cur;
    wd_gic_bank_save(m, cur);
    wd_gic_bank_load(m, frame);
    uint64_t v = gicr_read1(m, off, size, frame);
    wd_gic_bank_save(m, frame);
    wd_gic_bank_load(m, cur);
    return v;
}

void wd_gicr_write(wd_machine *m, uint64_t off, int size, uint64_t val)
{
    int frame = (int)(off / 0x20000), ncpu = m->ncpu > 0 ? m->ncpu : 1;
    if (frame >= ncpu) return;
    off %= 0x20000;
    if (frame == m->cur) { gicr_write1(m, off, size, val, frame); return; }
    int cur = m->cur;
    wd_gic_bank_save(m, cur);
    wd_gic_bank_load(m, frame);
    gicr_write1(m, off, size, val, frame);
    wd_gic_bank_save(m, frame);
    wd_gic_bank_load(m, cur);
}

static uint64_t gicr_read1(wd_machine *m, uint64_t off, int size, int frame)
{
    wd_gic *g = &m->gic;
    if (off >= 0x20000) return 0;
    if (off >= 0x10000) {
        uint64_t o = off - 0x10000;
        uint32_t v;
        if (o >= 0x0400 && o < 0x0420) return prio_read(g, o, size, 0, 32);
        if (size == 8) return gicr_read1(m, off, 4, frame) | (gicr_read1(m, off + 4, 4, frame) << 32);
        if (common_read(g, o & ~3ULL, 0, 32, &v)) return v;
        return 0;
    }
    if (off == 0x0008 || off == 0x000C) {     /* GICR_TYPER: affinity, processor number, Last, physical LPIs */
        int ncpu = m->ncpu > 0 ? m->ncpu : 1;
        uint64_t t = (uint64_t)frame << 32 | (uint64_t)frame << 8 | (frame == ncpu - 1 ? 0x10u : 0) | 1;
        if (off == 0x000C) return t >> 32;
        return size == 8 ? t : (uint32_t)t;
    }
    if (off == 0x0070) return size == 8 ? g->propbaser : (uint32_t)g->propbaser;
    if (off == 0x0074) return g->propbaser >> 32;
    if (off == 0x0078) return size == 8 ? g->pendbaser : (uint32_t)g->pendbaser;
    if (off == 0x007C) return g->pendbaser >> 32;
    if (size == 8) return gicr_read1(m, off, 4, frame) | (gicr_read1(m, off + 4, 4, frame) << 32);
    switch (off & ~3ULL) {
    case 0x0000: return g->rctlr & 1;          /* GICR_CTLR: EnableLPIs, RWP = 0 */
    case 0x0004: return 0x0000043B;           /* GICR_IIDR */
    case 0x0014: return g->rwaker;
    case 0xFFE8: return 0x3B;
    default: return 0;
    }
}

static void gicr_write1(wd_machine *m, uint64_t off, int size, uint64_t val, int frame)
{
    wd_gic *g = &m->gic;
    if (off >= 0x20000) return;
    if (off >= 0x10000) {
        uint64_t o = off - 0x10000;
        if (o >= 0x0400 && o < 0x0420) { prio_write(m, o, size, val, 0, 32); return; }
        if (size == 8) { gicr_write1(m, off, 4, (uint32_t)val, frame); gicr_write1(m, off + 4, 4, val >> 32, frame); return; }
        common_write(m, o & ~3ULL, 0, 32, (uint32_t)val);
        return;
    }
    if ((off & ~3ULL) == 0x0014) {
        uint32_t sleep = (uint32_t)val & 2;
        g->rwaker = sleep | (sleep ? 4 : 0);
    }
    if (off == 0x0000) {
        /* EnableLPIs: once set it cannot be cleared on this implementation (GICR_CTLR.EnableLPIs is RES1 after enable is allowed) */
        if (val & 1) g->rctlr |= 1;
        m->irq_dirty = 1;
    }
    if (!(g->rctlr & 1)) {                    /* the tables can only change while LPIs are disabled */
        uint64_t mask70 = 0x000FFFFFFFFFF000ULL | 0x1F | (7ULL << 7) | (3ULL << 10) | (7ULL << 56);
        uint64_t mask78 = 0x000FFFFFFFFF0000ULL | (7ULL << 7) | (3ULL << 10) | (7ULL << 56) | (1ULL << 62);
        if (off == 0x0070) g->propbaser = (size == 8 ? val : (g->propbaser & ~0xFFFFFFFFULL) | (uint32_t)val) & mask70;
        if (off == 0x0074) g->propbaser = ((g->propbaser & 0xFFFFFFFFULL) | (val << 32)) & mask70;
        if (off == 0x0078) g->pendbaser = (size == 8 ? val : (g->pendbaser & ~0xFFFFFFFFULL) | (uint32_t)val) & mask78 & ~(1ULL << 62);
        if (off == 0x007C) g->pendbaser = ((g->pendbaser & 0xFFFFFFFFULL) | (val << 32)) & mask78 & ~(1ULL << 62);
    }
}
