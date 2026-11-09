/*
 * WindDroid - minimal flattened device tree (DTB) writer.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include "fdt.h"

#define FDT_MAGIC      0xD00DFEED
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_END        9

static void grow(fdt_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 1024;
    while (cap < b->len + extra) cap *= 2;
    b->data = realloc(b->data, cap);
    b->cap = cap;
}

static void put(fdt_buf *b, const void *p, size_t n)
{
    grow(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put32(fdt_buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(b, x, 4);
}

static void align4(fdt_buf *b)
{
    static const uint8_t z[4] = { 0 };
    put(b, z, (4 - (b->len & 3)) & 3);
}

void fdt_init(fdt *f)
{
    memset(f, 0, sizeof *f);
}

void fdt_free(fdt *f)
{
    free(f->st.data);
    free(f->str.data);
}

static uint32_t str_off(fdt *f, const char *name)
{
    size_t n = strlen(name) + 1;
    for (size_t i = 0; i + n <= f->str.len; i++)
        if (!memcmp(f->str.data + i, name, n) && (i == 0 || f->str.data[i - 1] == 0)) return (uint32_t)i;
    uint32_t off = (uint32_t)f->str.len;
    put(&f->str, name, n);
    return off;
}

void fdt_begin(fdt *f, const char *name)
{
    put32(&f->st, FDT_BEGIN_NODE);
    put(&f->st, name, strlen(name) + 1);
    align4(&f->st);
}

void fdt_end(fdt *f) { put32(&f->st, FDT_END_NODE); }

void fdt_prop(fdt *f, const char *name, const void *val, size_t len)
{
    put32(&f->st, FDT_PROP);
    put32(&f->st, (uint32_t)len);
    put32(&f->st, str_off(f, name));
    if (len) put(&f->st, val, len);
    align4(&f->st);
}

void fdt_prop_empty(fdt *f, const char *name) { fdt_prop(f, name, NULL, 0); }

void fdt_prop_str(fdt *f, const char *name, const char *s) { fdt_prop(f, name, s, strlen(s) + 1); }

/* list of NUL separated strings, terminated by an empty string argument */
void fdt_prop_strs(fdt *f, const char *name, const char *const *list)
{
    fdt_buf t = { 0 };
    for (; *list; list++) put(&t, *list, strlen(*list) + 1);
    fdt_prop(f, name, t.data, t.len);
    free(t.data);
}

void fdt_prop_cells(fdt *f, const char *name, const uint32_t *cells, int n)
{
    fdt_buf t = { 0 };
    for (int i = 0; i < n; i++) put32(&t, cells[i]);
    fdt_prop(f, name, t.data, t.len);
    free(t.data);
}

void fdt_prop_u32(fdt *f, const char *name, uint32_t v) { fdt_prop_cells(f, name, &v, 1); }

void fdt_prop_reg64(fdt *f, const char *name, const uint64_t *pairs, int npairs)
{
    uint32_t c[32];
    int n = 0;
    for (int i = 0; i < npairs * 2 && n < 32; i++) {
        c[n++] = (uint32_t)(pairs[i] >> 32);
        c[n++] = (uint32_t)pairs[i];
    }
    fdt_prop_cells(f, name, c, n);
}

/* Assemble the blob; returns malloc'ed buffer. */
uint8_t *fdt_finish(fdt *f, size_t *size)
{
    put32(&f->st, FDT_END);
    uint32_t hdr = 40, rsv = 16;
    uint32_t off_st = hdr + rsv;
    uint32_t off_str = off_st + (uint32_t)f->st.len;
    uint32_t total = off_str + (uint32_t)f->str.len;
    fdt_buf b = { 0 };
    uint32_t h[10] = { FDT_MAGIC, total, off_st, off_str, hdr, 17, 16, 0, (uint32_t)f->str.len, (uint32_t)f->st.len };
    for (int i = 0; i < 10; i++) put32(&b, h[i]);
    for (int i = 0; i < 4; i++) put32(&b, 0);                 /* empty memory reservation map */
    put(&b, f->st.data, f->st.len);
    put(&b, f->str.data, f->str.len);
    *size = b.len;
    return b.data;
}
