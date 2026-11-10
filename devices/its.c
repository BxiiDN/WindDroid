/*
 * WindDroid - GICv3 Interrupt Translation Service (ITS).
 *
 * Translates MSI writes (DeviceID = PCI requester ID, EventID = data) into
 * physical LPIs through the mappings the guest makes with MAPD/MAPC/MAPTI.
 * The translation tables are kept inside the emulator; the memory the guest
 * hands over in GITS_BASER<n> is accepted but not used, which the
 * architecture allows (the table formats are IMPLEMENTATION DEFINED).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"

#define CMDQ_ENTRY 32

static wd_its_map *find_map(wd_its *s, uint32_t devid, uint32_t eventid)
{
    for (int i = 0; i < s->nmap; i++)
        if (s->map[i].devid == devid && s->map[i].eventid == eventid) return &s->map[i];
    return NULL;
}

static int dev_index(wd_its *s, uint32_t devid)
{
    for (int i = 0; i < s->ndev; i++) if (s->dev[i].devid == devid) return i;
    return -1;
}

static void unmap_device(wd_its *s, uint32_t devid)
{
    int j = 0;
    for (int i = 0; i < s->nmap; i++) if (s->map[i].devid != devid) s->map[j++] = s->map[i];
    s->nmap = j;
}

static void add_map(wd_its *s, uint32_t devid, uint32_t eventid, uint32_t pintid, uint16_t icid)
{
    wd_its_map *e = find_map(s, devid, eventid);
    if (!e) {
        if (s->nmap == s->cap) {
            int cap = s->cap ? s->cap * 2 : 64;
            wd_its_map *n = realloc(s->map, (size_t)cap * sizeof *n);
            if (!n) return;
            s->map = n;
            s->cap = cap;
        }
        e = &s->map[s->nmap++];
    }
    e->devid = devid;
    e->eventid = eventid;
    e->pintid = pintid;
    e->icid = icid;
}

static int event_ok(wd_its *s, uint32_t devid, uint32_t eventid)
{
    int d = dev_index(s, devid);
    return d >= 0 && eventid < (1u << (s->dev[d].size + 1));
}

static void deliver(wd_machine *m, uint32_t devid, uint32_t eventid)
{
    wd_its *s = &m->its;
    if (!(s->ctlr & 1) || !event_ok(s, devid, eventid)) return;
    wd_its_map *e = find_map(s, devid, eventid);
    if (!e || e->icid >= 256 || !s->coll_valid[e->icid]) return;
    wd_gic_set_lpi_cpu(m, s->coll_target[e->icid], e->pintid);
}

static void run_command(wd_machine *m, const uint64_t *c)
{
    wd_its *s = &m->its;
    uint8_t cmd = (uint8_t)c[0];
    uint32_t devid = (uint32_t)(c[0] >> 32);
    uint32_t eventid = (uint32_t)c[1];
    uint16_t icid = (uint16_t)c[2];
    switch (cmd) {
    case 0x08: {                                   /* MAPD */
        int valid = (int)(c[2] >> 63);
        int d = dev_index(s, devid);
        unmap_device(s, devid);                    /* remapping a device drops its old translations */
        if (!valid) {
            if (d >= 0) s->dev[d] = s->dev[--s->ndev];
            break;
        }
        if (d < 0) {
            if (s->ndev == 256) break;
            d = s->ndev++;
        }
        s->dev[d].devid = devid;
        s->dev[d].size = (uint8_t)(c[1] & 0x1F);
        break;
    }
    case 0x09:                                     /* MAPC: RDbase is the processor number (PTA = 0) */
        if (icid < 256) {
            s->coll_valid[icid] = (uint8_t)(c[2] >> 63);
            s->coll_target[icid] = (uint8_t)((c[2] >> 16) & 0xFF);
        }
        break;
    case 0x0A: {                                   /* MAPTI */
        uint32_t pintid = (uint32_t)(c[1] >> 32);
        if (event_ok(s, devid, eventid) && pintid >= GIC_LPI_BASE && pintid < GIC_LPI_BASE + GIC_NLPI)
            add_map(s, devid, eventid, pintid, icid);
        break;
    }
    case 0x0B:                                     /* MAPI: pINTID = EventID */
        if (event_ok(s, devid, eventid) && eventid >= GIC_LPI_BASE && eventid < GIC_LPI_BASE + GIC_NLPI)
            add_map(s, devid, eventid, eventid, icid);
        break;
    case 0x01: {                                   /* MOVI */
        wd_its_map *e = find_map(s, devid, eventid);
        if (e) e->icid = icid;
        break;
    }
    case 0x0F: {                                   /* DISCARD */
        wd_its_map *e = find_map(s, devid, eventid);
        if (e) {
            if (m->gic.lpi_pend[e->pintid - GIC_LPI_BASE]) {
                m->gic.lpi_pend[e->pintid - GIC_LPI_BASE] = 0;
                m->gic.lpi_npend--;
            }
            *e = s->map[--s->nmap];
        }
        break;
    }
    case 0x03:                                     /* INT */
        if (find_map(s, devid, eventid)) {
            wd_its_map *e = find_map(s, devid, eventid);
            if (e->icid < 256 && s->coll_valid[e->icid]) wd_gic_set_lpi_cpu(m, s->coll_target[e->icid], e->pintid);
        }
        break;
    case 0x04: {                                   /* CLEAR */
        wd_its_map *e = find_map(s, devid, eventid);
        if (e && m->gic.lpi_pend[e->pintid - GIC_LPI_BASE]) {
            m->gic.lpi_pend[e->pintid - GIC_LPI_BASE] = 0;
            m->gic.lpi_npend--;
        }
        break;
    }
    case 0x05: case 0x0C: case 0x0D: case 0x0E:    /* SYNC, INV, INVALL, MOVALL: nothing cached */
        m->irq_dirty = 1;
        break;
    default:
        break;
    }
}

static void process_queue(wd_machine *m)
{
    wd_its *s = &m->its;
    if (!(s->ctlr & 1) || !(s->cbaser >> 63)) return;
    uint64_t base = s->cbaser & 0x000FFFFFFFFFF000ULL;
    uint64_t size = ((s->cbaser & 0xFF) + 1) * 4096;
    for (int guard = 0; guard < 4096 && s->creadr != s->cwriter; guard++) {
        uint64_t c[4];
        if (s->creadr >= size || wd_dma_read(m, base + s->creadr, c, sizeof c)) {
            s->creadr |= 1;                        /* stalled */
            return;
        }
        run_command(m, c);
        s->creadr = (s->creadr + CMDQ_ENTRY) % size;
    }
}

void wd_msi_write(wd_machine *m, uint32_t requester, uint64_t addr, uint32_t data)
{
    if (addr == WD_GITS_TRANSLATER) deliver(m, requester, data);
}

/* ---------------------------------------------------------------- registers */

/* physical LPIs, 8-byte ITT entries, 16 EventID bits, 16 DeviceID bits, PTA = 0, HCC = 0 */
#define TYPER  (1ULL << 0 | 7ULL << 4 | 15ULL << 8 | 15ULL << 13)

static uint64_t baser_type(int n)
{
    if (n == 0) return 1ULL << 56;                 /* devices */
    if (n == 1) return 4ULL << 56;                 /* interrupt collections */
    return 0;
}

uint64_t wd_its_read(wd_machine *m, uint64_t off, int size)
{
    wd_its *s = &m->its;
    uint64_t v = 0;
    uint64_t o = off & ~7ULL;
    if (off >= 0x10000) return 0;                  /* GITS_TRANSLATER is write-only */
    switch (o) {
    case 0x0000: v = (s->ctlr & 1) | (s->ctlr & 1 ? 0 : 1ULL << 31) | (uint64_t)0x0000043B << 32; break;   /* CTLR, IIDR */
    case 0x0008: v = TYPER; break;
    case 0x0080: v = s->cbaser; break;
    case 0x0088: v = s->cwriter; break;
    case 0x0090: v = s->creadr; break;
    default:
        if (o >= 0x0100 && o < 0x0140) {
            int n = (int)((o - 0x0100) / 8);
            v = s->baser[n] | baser_type(n) | (n < 2 ? 7ULL << 48 : 0);   /* entry size 8 bytes */
        } else if (o == 0xFFE8) {
            v = 0x3B;                              /* PIDR2: ArchRev 3 */
        }
        break;
    }
    if (size == 8) return v;
    v >>= 8 * (off & 7);
    return v & wd_mask(8 * size);
}

static uint64_t merge(uint64_t old, uint64_t off, int size, uint64_t val)
{
    if (size == 8) return val;
    int sh = 8 * (int)(off & 7);
    uint64_t mask = wd_mask(8 * size) << sh;
    return (old & ~mask) | ((val << sh) & mask);
}

void wd_its_write(wd_machine *m, uint64_t off, int size, uint64_t val)
{
    wd_its *s = &m->its;
    if (off == 0x10040) {                          /* GITS_TRANSLATER without a requester: device 0 */
        deliver(m, 0, (uint32_t)val);
        return;
    }
    uint64_t o = off & ~7ULL;
    switch (o) {
    case 0x0000:
        if ((off & 7) == 0) {
            s->ctlr = (uint32_t)val & 1;
            process_queue(m);
        }
        return;
    case 0x0080:
        if (!(s->ctlr & 1)) {
            s->cbaser = merge(s->cbaser, off, size, val) & (0x8000000000000000ULL | 0x000FFFFFFFFFF000ULL | 0xFF |
                                                         7ULL << 59 | 7ULL << 53 | 3ULL << 10);
            s->creadr = 0;                         /* writing CBASER resets CREADR */
        }
        return;
    case 0x0088:
        s->cwriter = merge(s->cwriter, off, size, val) & 0xFFFE0;
        process_queue(m);
        return;
    default:
        if (o >= 0x0100 && o < 0x0140 && !(s->ctlr & 1)) {
            int n = (int)((o - 0x0100) / 8);
            if (n >= 2) return;
            uint64_t keep = 1ULL << 63 | 7ULL << 59 | 7ULL << 53 | 0x0000FFFFFFFFF000ULL | 3ULL << 10 | 0xFF;   /* flat tables only */
            s->baser[n] = merge(s->baser[n], off, size, val) & keep;   /* page size field 0: 4 KiB */
        }
        return;
    }
}

void wd_its_free(wd_machine *m)
{
    free(m->its.map);
    memset(&m->its, 0, sizeof m->its);
}
