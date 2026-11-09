/*
 * WindDroid - PCI Express host bridge (ECAM, like QEMU's gpex) and the
 * generic part of PCI functions: configuration space, BAR decoding and
 * legacy INTx routing to GIC SPIs 3..6.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

void wd_pci_init_dev(wd_pcidev *d, int devfn, uint16_t vendor, uint16_t device, uint32_t class_rev,
                     uint16_t subvendor, uint16_t subdev)
{
    memset(d, 0, sizeof *d);
    d->devfn = devfn;
    wr16(d->cfg + 0x00, vendor);
    wr16(d->cfg + 0x02, device);
    wr32(d->cfg + 0x08, class_rev);
    wr16(d->cfg + 0x2C, subvendor);
    wr16(d->cfg + 0x2E, subdev);
    wr16(d->wmask + 0x04, 0x0547);              /* I/O, memory, bus master, parity, SERR, INTx disable */
    d->wmask[0x0C] = 0xFF;                       /* cache line size */
    d->wmask[0x0D] = 0xFF;                       /* latency timer */
    d->wmask[0x3C] = 0xFF;                       /* interrupt line */
    wr16(d->cfg + 0x06, 0x0010);                 /* capabilities list present */
}

void wd_pci_set_bar(wd_pcidev *d, int bar, uint64_t size, int type)
{
    d->bar_size[bar] = size;
    d->bar_type[bar] = (uint8_t)type;
    uint32_t low = type == WD_BAR_IO ? 1 : (type == WD_BAR_MEM64 ? 4 : 0);
    wr32(d->cfg + 0x10 + 4 * bar, low);
    uint64_t mask = ~(size - 1);
    wr32(d->wmask + 0x10 + 4 * bar, (uint32_t)mask & (type == WD_BAR_IO ? ~3u : ~15u));
    if (type == WD_BAR_MEM64) wr32(d->wmask + 0x10 + 4 * (bar + 1), (uint32_t)(mask >> 32));
}

/* add a capability, returns its config offset */
int wd_pci_add_cap(wd_pcidev *d, uint8_t id, int len)
{
    int off = d->cap_next ? d->cap_next : 0x40;
    d->cfg[off] = id;
    d->cfg[off + 1] = d->cfg[0x34];
    d->cfg[0x34] = (uint8_t)off;
    d->cap_next = (off + len + 3) & ~3;
    return off;
}

uint64_t wd_pci_bar_addr(const wd_pcidev *d, int bar)
{
    uint64_t lo = rd32(d->cfg + 0x10 + 4 * bar);
    if (d->bar_type[bar] == WD_BAR_IO) return lo & ~3ULL;
    uint64_t a = lo & ~15ULL;
    if (d->bar_type[bar] == WD_BAR_MEM64) a |= (uint64_t)rd32(d->cfg + 0x14 + 4 * bar) << 32;
    return a;
}

static void update_intx(wd_machine *m, wd_pcidev *d)
{
    int pin = d->cfg[0x3D];
    if (!pin) return;
    int disabled = (rd16(d->cfg + 0x04) >> 10) & 1;
    uint16_t st = rd16(d->cfg + 0x06);
    st = (uint16_t)((st & ~0x0008) | (d->intx ? 0x0008 : 0));
    wr16(d->cfg + 0x06, st);
    int slot = d->devfn >> 3;
    int spi = 3 + ((slot + pin - 1) & 3);
    int level = 0;
    /* several functions can share a line: OR them */
    for (int i = 0; i < m->npci; i++) {
        wd_pcidev *o = m->pci[i];
        if (!o->cfg[0x3D]) continue;
        if (3 + (((o->devfn >> 3) + o->cfg[0x3D] - 1) & 3) != spi) continue;
        if (o->intx && !((rd16(o->cfg + 0x04) >> 10) & 1)) level = 1;
    }
    (void)disabled;
    wd_gic_set_line(m, 32 + spi, level);
}

void wd_pci_set_intx(wd_machine *m, wd_pcidev *d, int level)
{
    d->intx = level ? 1 : 0;
    update_intx(m, d);
}

uint32_t wd_pci_cfg_read(wd_pcidev *d, uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)d->cfg[(off + i) & 0xFFF] << (8 * i);
    return v;
}

void wd_pci_cfg_write(wd_machine *m, wd_pcidev *d, uint32_t off, int size, uint32_t val)
{
    for (int i = 0; i < size; i++) {
        uint32_t o = (off + i) & 0xFFF;
        uint8_t b = (uint8_t)(val >> (8 * i)), w = d->wmask[o];
        d->cfg[o] = (uint8_t)((d->cfg[o] & ~w) | (b & w));
        if (o == 0x06 || o == 0x07) d->cfg[o] &= (uint8_t)~(b & d->w1cmask[o]);
    }
    if (d->on_cfg_write) d->on_cfg_write(m, d, off, size, val);
    if (off <= 0x05 && off + (uint32_t)size > 0x04) update_intx(m, d);
}

/* ---------------------------------------------------------------- host bridge */

static wd_pcidev *find_devfn(wd_machine *m, int bus, int devfn)
{
    if (bus != 0) return NULL;
    for (int i = 0; i < m->npci; i++)
        if (m->pci[i]->devfn == devfn) return m->pci[i];
    return NULL;
}

uint64_t wd_ecam_read(wd_machine *m, uint64_t off, int size)
{
    wd_pcidev *d = find_devfn(m, (int)(off >> 20), (int)((off >> 12) & 0xFF));
    if (!d) return size == 8 ? ~0ULL : wd_mask(8 * size);
    uint32_t reg = (uint32_t)(off & 0xFFF);
    if (size == 8) return wd_pci_cfg_read(d, reg, 4) | ((uint64_t)wd_pci_cfg_read(d, reg + 4, 4) << 32);
    return wd_pci_cfg_read(d, reg, size);
}

void wd_ecam_write(wd_machine *m, uint64_t off, int size, uint64_t val)
{
    wd_pcidev *d = find_devfn(m, (int)(off >> 20), (int)((off >> 12) & 0xFF));
    if (!d) return;
    uint32_t reg = (uint32_t)(off & 0xFFF);
    if (size == 8) {
        wd_pci_cfg_write(m, d, reg, 4, (uint32_t)val);
        wd_pci_cfg_write(m, d, reg + 4, 4, (uint32_t)(val >> 32));
        return;
    }
    wd_pci_cfg_write(m, d, reg, size, (uint32_t)val);
}

/* Find the BAR that decodes a physical address (memory or I/O space). */
static wd_pcidev *bar_lookup(wd_machine *m, uint64_t pa, int io, int *bar, uint64_t *off)
{
    for (int i = 0; i < m->npci; i++) {
        wd_pcidev *d = m->pci[i];
        uint16_t cmd = rd16(d->cfg + 0x04);
        for (int b = 0; b < 6; b++) {
            int t = d->bar_type[b];
            if (!t || (io != (t == WD_BAR_IO))) continue;
            if (io ? !(cmd & 1) : !(cmd & 2)) continue;
            uint64_t a = wd_pci_bar_addr(d, b);
            if (!a) continue;
            if (pa >= a && pa - a < d->bar_size[b]) { *bar = b; *off = pa - a; return d; }
            if (t == WD_BAR_MEM64) b++;
        }
    }
    return NULL;
}

uint64_t wd_pci_mmio_read(wd_machine *m, uint64_t pa, int size, int io)
{
    int bar;
    uint64_t off;
    wd_pcidev *d = bar_lookup(m, pa, io, &bar, &off);
    if (!d || !d->bar_read) return 0;
    return d->bar_read(m, d, bar, off, size);
}

void wd_pci_mmio_write(wd_machine *m, uint64_t pa, int size, uint64_t val, int io)
{
    int bar;
    uint64_t off;
    wd_pcidev *d = bar_lookup(m, pa, io, &bar, &off);
    if (d && d->bar_write) d->bar_write(m, d, bar, off, size, val);
}

/* DMA helpers for devices (guest physical memory, RAM only) */
int wd_dma_read(wd_machine *m, uint64_t pa, void *buf, size_t len)
{
    uint8_t *dst = buf;
    while (len) {
        uint8_t *p = wd_phys_host(m, pa);
        if (!p) return -1;
        size_t chunk = 4096 - (pa & 4095);
        if (chunk > len) chunk = len;
        memcpy(dst, p, chunk);
        dst += chunk; pa += chunk; len -= chunk;
    }
    return 0;
}

int wd_dma_write(wd_machine *m, uint64_t pa, const void *buf, size_t len)
{
    const uint8_t *src = buf;
    while (len) {
        uint8_t *p = wd_phys_host(m, pa);
        if (!p || !wd_phys_is_ram(m, pa)) return -1;
        size_t chunk = 4096 - (pa & 4095);
        if (chunk > len) chunk = len;
        memcpy(p, src, chunk);
        src += chunk; pa += chunk; len -= chunk;
    }
    return 0;
}

void wd_pci_add(wd_machine *m, wd_pcidev *d)
{
    if (m->npci < WD_MAX_PCI) m->pci[m->npci++] = d;
}

void wd_pci_host_init(wd_machine *m)
{
    wd_pcidev *h = calloc(1, sizeof *h);
    if (!h) return;
    wd_pci_init_dev(h, 0, 0x1B36, 0x0008, 0x06000000, 0x1AF4, 0x1100);
    wr16(h->cfg + 0x06, 0);
    wd_pci_add(m, h);
}
