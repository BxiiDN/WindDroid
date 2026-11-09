/*
 * WindDroid - CFI parallel NOR flash (Intel/Sharp command set, as QEMU's
 * pflash_cfi01) configured like the virt board: 32-bit bank made of two
 * interleaved x16 devices, 256 KiB erase blocks. Bank 0 holds the UEFI
 * firmware, bank 1 the UEFI variable store.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"

#define BLOCK  (256u << 10)
#define REPL(v) (((uint64_t)(v) & 0xFFFF) | (((uint64_t)(v) & 0xFFFF) << 16))

enum { M_ARRAY, M_STATUS, M_ID, M_CFI, M_WB_COUNT, M_WB_DATA, M_PROGRAM, M_ERASE, M_LOCK };

static void set_mode(wd_machine *m, wd_flash *f, int mode)
{
    if ((f->mode == M_ARRAY) != (mode == M_ARRAY)) wd_tlb_flush(m);   /* reads switch between ROM and device */
    f->mode = mode;
}

uint8_t *wd_flash_host(wd_flash *f, uint64_t off)
{
    if (!f->mem || f->mode != M_ARRAY || off >= f->size) return NULL;
    return f->mem + off;
}

static uint8_t cfi_byte(wd_flash *f, unsigned idx)
{
    /* Minimal CFI query table for one x16 device of half the bank size. */
    unsigned size_log2 = 0;
    while ((1ULL << size_log2) < f->size / 2) size_log2++;
    unsigned nblocks = (unsigned)(f->size / BLOCK) - 1;
    switch (idx) {
    case 0x10: return 'Q';
    case 0x11: return 'R';
    case 0x12: return 'Y';
    case 0x13: return 0x01;            /* Intel command set */
    case 0x15: return 0x31;            /* primary extended table at 0x31 */
    case 0x1B: return 0x45;
    case 0x1C: return 0x55;
    case 0x1F: return 0x07;
    case 0x20: return 0x07;
    case 0x21: return 0x0A;
    case 0x27: return (uint8_t)size_log2;
    case 0x28: return 0x01;            /* x16 */
    case 0x2A: return 0x0B;            /* 2 KiB write buffer per device */
    case 0x2C: return 0x01;            /* one erase region */
    case 0x2D: return (uint8_t)nblocks;
    case 0x2E: return (uint8_t)(nblocks >> 8);
    case 0x2F: return (uint8_t)((BLOCK / 2 / 256) & 0xFF);
    case 0x30: return (uint8_t)((BLOCK / 2 / 256) >> 8);
    case 0x31: return 'P';
    case 0x32: return 'R';
    case 0x33: return 'I';
    case 0x34: return '1';
    case 0x35: return '0';
    default: return 0;
    }
}

uint64_t wd_flash_read(wd_machine *m, wd_flash *f, uint64_t off, int size)
{
    (void)m;
    uint64_t v = 0;
    switch (f->mode) {
    case M_ARRAY:
        if (off + (uint64_t)size <= f->size) memcpy(&v, f->mem + off, (size_t)size);
        return v;
    case M_ID: {
        unsigned word = (unsigned)(off >> 2);                    /* index of the 32-bit bank word */
        uint64_t id = word == 0 ? 0x89 : (word == 1 ? 0x18 : 0);
        v = REPL(id);
        break;
    }
    case M_CFI:
        v = REPL(cfi_byte(f, (unsigned)(off >> 2)));
        break;
    default:
        v = REPL(f->status);
        break;
    }
    if (size == 8) v |= v << 32;
    return v & wd_mask(8 * size);
}

static void touched(wd_flash *f, uint64_t end)
{
    f->dirty = 1;
    end = (end + 0xFFFF) & ~0xFFFFULL;                         /* whole 64 KiB units */
    if (end > f->size) end = f->size;
    if (end > f->used) f->used = end;
}

static void wb_commit(wd_flash *f)
{
    if (f->wb_addr + f->wb_len <= f->size) {
        memcpy(f->mem + f->wb_addr, f->wb, f->wb_len);
        touched(f, f->wb_addr + f->wb_len);
    }
}

void wd_flash_write(wd_machine *m, wd_flash *f, uint64_t off, int size, uint64_t val)
{
    uint8_t cmd = (uint8_t)val;
    switch (f->mode) {
    case M_PROGRAM:                                           /* word program data */
        if (off + (uint64_t)size <= f->size && !f->readonly) {
            memcpy(f->mem + off, &val, (size_t)size);
            touched(f, off + (uint64_t)size);
        }
        f->status = 0x80;
        set_mode(m, f, M_STATUS);
        return;
    case M_WB_COUNT:
        f->wb_words = (unsigned)(val & 0xFFFF) + 1;
        f->wb_addr = off & ~(uint64_t)(sizeof f->wb - 1) & ~3ULL;
        f->wb_addr = off;
        f->wb_len = 0;
        set_mode(m, f, M_WB_DATA);
        return;
    case M_WB_DATA:
        if (f->wb_words) {
            if (f->wb_len == 0) f->wb_addr = off;
            if (f->wb_len + (unsigned)size <= sizeof f->wb) {
                memcpy(f->wb + f->wb_len, &val, (size_t)size);
                f->wb_len += (unsigned)size;
            }
            f->wb_words--;
            return;
        }
        if (cmd == 0xD0 && !f->readonly) wb_commit(f);
        f->status = cmd == 0xD0 ? 0x80 : 0xB0;                 /* command sequence error otherwise */
        set_mode(m, f, M_STATUS);
        return;
    case M_ERASE:
        if (cmd == 0xD0 && !f->readonly) {
            uint64_t base = off & ~(uint64_t)(BLOCK - 1);
            if (base + BLOCK <= f->size) memset(f->mem + base, 0xFF, BLOCK);
            touched(f, base + BLOCK);
            f->status = 0x80;
        } else {
            f->status = 0xB0;
        }
        set_mode(m, f, M_STATUS);
        return;
    case M_LOCK:                                              /* 0x01 lock / 0xD0 unlock: accepted */
        f->status = 0x80;
        set_mode(m, f, M_STATUS);
        return;
    default:
        break;
    }
    switch (cmd) {
    case 0x10: case 0x40: set_mode(m, f, M_PROGRAM); break;
    case 0x20: set_mode(m, f, M_ERASE); break;
    case 0x50: f->status = 0x80; set_mode(m, f, f->mode == M_ARRAY ? M_ARRAY : M_STATUS); break;
    case 0x60: set_mode(m, f, M_LOCK); break;
    case 0x70: set_mode(m, f, M_STATUS); break;
    case 0x90: set_mode(m, f, M_ID); break;
    case 0x98: set_mode(m, f, M_CFI); break;
    case 0xE8: f->status = 0x80; set_mode(m, f, M_WB_COUNT); break;
    case 0xFF: case 0xF0: case 0x00: set_mode(m, f, M_ARRAY); break;
    default: set_mode(m, f, M_ARRAY); break;
    }
}

int wd_flash_init(wd_flash *f, uint64_t size, const uint8_t *data, size_t len, int readonly)
{
    memset(f, 0, sizeof *f);
    f->mem = malloc((size_t)size);
    if (!f->mem) return -1;
    memset(f->mem, 0xFF, (size_t)size);
    if (data) memcpy(f->mem, data, len > size ? (size_t)size : len);
    f->size = size;
    f->used = data ? (len > size ? size : len) : 0;
    f->readonly = readonly;
    f->status = 0x80;
    f->mode = M_ARRAY;
    return 0;
}

void wd_flash_free(wd_flash *f)
{
    free(f->mem);
    f->mem = NULL;
}
