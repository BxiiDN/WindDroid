/*
 * WindDroid - QEMU firmware configuration interface (fw_cfg), MMIO variant
 * with DMA, as used by EDK2's QemuFwCfgLib. Register layout at WD_FWCFG_BASE:
 *   0x00 data (8 bytes), 0x08 selector (16-bit, big endian), 0x10 DMA address (64-bit, big endian)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "../core/internal.h"

#define SEL_SIGNATURE 0x0000
#define SEL_ID        0x0001
#define SEL_FILE_DIR  0x0019
#define SEL_FILE_BASE 0x0020

#define DMA_ERROR  0x01
#define DMA_READ   0x02
#define DMA_SKIP   0x04
#define DMA_SELECT 0x08
#define DMA_WRITE  0x10

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static wd_fwcfg_item *find(wd_fwcfg *f, uint16_t sel)
{
    for (int i = 0; i < f->n; i++)
        if (f->items[i].sel == sel) return &f->items[i];
    return NULL;
}

static void rebuild_dir(wd_fwcfg *f)
{
    int nfiles = 0;
    for (int i = 0; i < f->n; i++) if (f->items[i].name[0]) nfiles++;
    size_t len = 4 + (size_t)nfiles * 64;
    wd_fwcfg_item *d = find(f, SEL_FILE_DIR);
    free(d->data);
    d->data = calloc(1, len);
    d->len = (uint32_t)len;
    wbe32(d->data, (uint32_t)nfiles);
    uint8_t *e = d->data + 4;
    for (int i = 0; i < f->n; i++) {
        wd_fwcfg_item *it = &f->items[i];
        if (!it->name[0]) continue;
        wbe32(e, it->len);
        wbe16(e + 4, it->sel);
        memcpy(e + 8, it->name, strlen(it->name) + 1);
        e += 64;
    }
}

static wd_fwcfg_item *add(wd_fwcfg *f, uint16_t sel, const char *name, const void *data, uint32_t len, int writable)
{
    if (f->n >= WD_FWCFG_MAX) return NULL;
    wd_fwcfg_item *it = &f->items[f->n++];
    memset(it, 0, sizeof *it);
    it->sel = sel;
    if (name) snprintf(it->name, sizeof it->name, "%s", name);
    it->data = calloc(1, len ? len : 1);
    if (data) memcpy(it->data, data, len);
    it->len = len;
    it->writable = writable;
    return it;
}

void wd_fwcfg_init(wd_fwcfg *f)
{
    memset(f, 0, sizeof *f);
    add(f, SEL_SIGNATURE, NULL, "QEMU", 4, 0);
    uint8_t id[4] = { 3, 0, 0, 0 };                 /* traditional interface + DMA */
    add(f, SEL_ID, NULL, id, 4, 0);
    add(f, SEL_FILE_DIR, NULL, NULL, 4, 0);
    uint8_t ncpu[2] = { 1, 0 };
    add(f, 0x0005, NULL, ncpu, 2, 0);               /* FW_CFG_NB_CPUS */
    add(f, 0x000F, NULL, ncpu, 2, 0);               /* FW_CFG_MAX_CPUS */
    f->next_file = SEL_FILE_BASE;
    f->cur = NULL;
    rebuild_dir(f);
}

void wd_fwcfg_free(wd_fwcfg *f)
{
    for (int i = 0; i < f->n; i++) free(f->items[i].data);
    f->n = 0;
}

wd_fwcfg_item *wd_fwcfg_add_file(wd_fwcfg *f, const char *name, const void *data, uint32_t len, int writable)
{
    wd_fwcfg_item *it = add(f, f->next_file++, name, data, len, writable);
    rebuild_dir(f);
    return it;
}

wd_fwcfg_item *wd_fwcfg_file(wd_fwcfg *f, const char *name)
{
    for (int i = 0; i < f->n; i++)
        if (f->items[i].name[0] && !strcmp(f->items[i].name, name)) return &f->items[i];
    return NULL;
}

static void select_item(wd_fwcfg *f, uint16_t sel)
{
    f->cur = find(f, sel);
    f->off = 0;
}

static void dma(wd_machine *m, wd_fwcfg *f, uint64_t addr)
{
    uint8_t *desc = wd_phys_host(m, addr);
    if (!desc || !wd_phys_host(m, addr + 15)) return;
    uint32_t control = be32(desc), length = be32(desc + 4);
    uint64_t target = be64(desc + 8);
    if (control & DMA_SELECT) select_item(f, (uint16_t)(control >> 16));
    int rd = (control & DMA_READ) != 0, wr = !rd && (control & DMA_WRITE), skip = !rd && !wr && (control & DMA_SKIP);
    if (!rd && !wr && !skip) length = 0;
    uint32_t status = 0;
    while (length && !(status & DMA_ERROR)) {
        wd_fwcfg_item *it = f->cur;
        uint32_t n;
        if (!it || f->off >= it->len) {
            n = length;
            if (rd) {
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *p = wd_phys_host(m, target + i);
                    if (!p) { status |= DMA_ERROR; break; }
                    *p = 0;
                }
            }
            if (wr) status |= DMA_ERROR;
        } else {
            n = length <= it->len - f->off ? length : it->len - f->off;
            if (rd) {
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *p = wd_phys_host(m, target + i);
                    if (!p) { status |= DMA_ERROR; break; }
                    *p = it->data[f->off + i];
                }
            }
            if (wr) {
                if (!it->writable || n != length) status |= DMA_ERROR;
                else {
                    for (uint32_t i = 0; i < n; i++) {
                        uint8_t *p = wd_phys_host(m, target + i);
                        if (!p) { status |= DMA_ERROR; break; }
                        it->data[f->off + i] = *p;
                    }
                    if (!(status & DMA_ERROR) && it->on_write) it->on_write(m, it);
                }
            }
            f->off += n;
        }
        target += n;
        length -= n;
    }
    wbe32(desc, status);
    wd_tlb_flush(m);
}

uint64_t wd_fwcfg_read(wd_machine *m, wd_fwcfg *f, uint64_t off, int size)
{
    (void)m;
    if (off < 8) {
        uint64_t v = 0;
        wd_fwcfg_item *it = f->cur;
        for (int i = 0; i < size; i++) {
            uint8_t b = 0;
            if (it && f->off < it->len) b = it->data[f->off++];
            v |= (uint64_t)b << (8 * i);
        }
        return v;
    }
    if (off == 0x10 && size == 8) return __builtin_bswap64(0x51454D5520434647ULL);   /* "QEMU CFG" */
    return 0;
}

void wd_fwcfg_write(wd_machine *m, wd_fwcfg *f, uint64_t off, int size, uint64_t val)
{
    if (off == 0x08 && size == 2) { select_item(f, __builtin_bswap16((uint16_t)val)); return; }
    if (off == 0x10 && size == 8) { dma(m, f, __builtin_bswap64(val)); return; }
    if (off == 0x10 && size == 4) { f->dma_hi = __builtin_bswap32((uint32_t)val); return; }
    if (off == 0x14 && size == 4) {
        uint64_t a = ((uint64_t)f->dma_hi << 32) | __builtin_bswap32((uint32_t)val);
        f->dma_hi = 0;
        dma(m, f, a);
    }
}
