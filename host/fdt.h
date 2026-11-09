/* WindDroid - minimal flattened device tree writer. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WD_FDT_H
#define WD_FDT_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *data; size_t len, cap; } fdt_buf;
typedef struct { fdt_buf st, str; } fdt;

void fdt_init(fdt *f);
void fdt_free(fdt *f);
void fdt_begin(fdt *f, const char *name);
void fdt_end(fdt *f);
void fdt_prop(fdt *f, const char *name, const void *val, size_t len);
void fdt_prop_empty(fdt *f, const char *name);
void fdt_prop_str(fdt *f, const char *name, const char *s);
void fdt_prop_strs(fdt *f, const char *name, const char *const *list);
void fdt_prop_cells(fdt *f, const char *name, const uint32_t *cells, int n);
void fdt_prop_u32(fdt *f, const char *name, uint32_t v);
void fdt_prop_reg64(fdt *f, const char *name, const uint64_t *pairs, int npairs);
uint8_t *fdt_finish(fdt *f, size_t *size);

#endif
