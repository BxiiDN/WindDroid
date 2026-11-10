/*
 * WindDroid - ACPI tables for the virt machine, published through fw_cfg
 * ("etc/acpi/tables", "etc/acpi/rsdp", "etc/table-loader") in the format of
 * QEMU's BIOS linker/loader so that EDK2's AcpiPlatformDxe installs them.
 *
 * Tables: DSDT, FADT (hardware reduced, PSCI over HVC), MADT (GICv3 + ITS), IORT, GTDT,
 * MCFG, SPCR, DBG2, XSDT and RSDP (ACPI 6.x).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"
#include "platform.h"

typedef struct { uint8_t *d; size_t len, cap; } buf;

static void grow(buf *b, size_t n)
{
    if (b->len + n <= b->cap) return;
    size_t c = b->cap ? b->cap * 2 : 1024;
    while (c < b->len + n) c *= 2;
    b->d = realloc(b->d, c);
    b->cap = c;
}

static void put(buf *b, const void *p, size_t n) { grow(b, n); memcpy(b->d + b->len, p, n); b->len += n; }
static void put8(buf *b, uint8_t v) { put(b, &v, 1); }
static void putn(buf *b, uint64_t v, int n) { for (int i = 0; i < n; i++) put8(b, (uint8_t)(v >> (8 * i))); }
static void zero(buf *b, size_t n) { grow(b, n); memset(b->d + b->len, 0, n); b->len += n; }
static void set32(buf *b, size_t off, uint32_t v) { for (int i = 0; i < 4; i++) b->d[off + i] = (uint8_t)(v >> (8 * i)); }

/* ---------------------------------------------------------------- AML encoder */

static void pkglen(buf *out, size_t body)
{
    /* the encoded length includes the PkgLength bytes themselves */
    if (body + 1 < 0x40) { put8(out, (uint8_t)(body + 1)); return; }
    size_t total;
    int extra;
    if (body + 2 < 0x1000) { extra = 1; total = body + 2; }
    else if (body + 3 < 0x100000) { extra = 2; total = body + 3; }
    else { extra = 3; total = body + 4; }
    put8(out, (uint8_t)((extra << 6) | (total & 0xF)));
    total >>= 4;
    for (int i = 0; i < extra; i++) { put8(out, (uint8_t)total); total >>= 8; }
}

/* wrap 'body' as opcode(s) + PkgLength + body */
static void aml_pkg(buf *out, const uint8_t *op, int oplen, buf *body)
{
    put(out, op, (size_t)oplen);
    pkglen(out, body->len);
    put(out, body->d, body->len);
    free(body->d);
    memset(body, 0, sizeof *body);
}

static void aml_nameseg(buf *b, const char *s)
{
    char seg[4] = { '_', '_', '_', '_' };
    for (int i = 0; i < 4 && s[i]; i++) seg[i] = s[i];
    put(b, seg, 4);
}

static void aml_int(buf *b, uint64_t v)
{
    if (v == 0) put8(b, 0x00);
    else if (v == 1) put8(b, 0x01);
    else if (v <= 0xFF) { put8(b, 0x0A); putn(b, v, 1); }
    else if (v <= 0xFFFF) { put8(b, 0x0B); putn(b, v, 2); }
    else if (v <= 0xFFFFFFFF) { put8(b, 0x0C); putn(b, v, 4); }
    else { put8(b, 0x0E); putn(b, v, 8); }
}

static void aml_string(buf *b, const char *s) { put8(b, 0x0D); put(b, s, strlen(s) + 1); }

static uint32_t eisaid(const char *id)
{
    uint32_t b0 = (uint32_t)(((id[0] - 0x40) & 0x1F) << 2 | ((id[1] - 0x40) & 0x1F) >> 3);
    uint32_t b1 = (uint32_t)(((id[1] - 0x40) & 0x7) << 5 | ((id[2] - 0x40) & 0x1F));
    unsigned h[4];
    for (int i = 0; i < 4; i++) {
        char c = id[3 + i];
        h[i] = (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    uint32_t b2 = h[0] << 4 | h[1], b3 = h[2] << 4 | h[3];
    return b0 | b1 << 8 | b2 << 16 | b3 << 24;
}

static void aml_name_int(buf *b, const char *name, uint64_t v) { put8(b, 0x08); aml_nameseg(b, name); aml_int(b, v); }
static void aml_name_str(buf *b, const char *name, const char *s) { put8(b, 0x08); aml_nameseg(b, name); aml_string(b, s); }
static void aml_name_eisa(buf *b, const char *name, const char *id)
{
    put8(b, 0x08);
    aml_nameseg(b, name);
    put8(b, 0x0C);
    putn(b, eisaid(id), 4);
}

/* Name(name, ResourceTemplate(){ res }) */
static void aml_name_crs(buf *b, const char *name, buf *res)
{
    put8(res, 0x79);                                  /* end tag */
    put8(res, 0x00);
    buf body = { 0 };
    aml_int(&body, res->len);
    put(&body, res->d, res->len);
    free(res->d);
    memset(res, 0, sizeof *res);
    put8(b, 0x08);
    aml_nameseg(b, name);
    static const uint8_t bufop = 0x11;
    aml_pkg(b, &bufop, 1, &body);
}

static void res_mem32_fixed(buf *r, uint32_t base, uint32_t len)
{
    put8(r, 0x86); putn(r, 9, 2); put8(r, 1); putn(r, base, 4); putn(r, len, 4);
}

static void res_interrupt(buf *r, uint32_t gsi)
{
    put8(r, 0x89); putn(r, 6, 2);
    put8(r, 0x01);                                   /* consumer, level, active high, exclusive */
    put8(r, 1);
    putn(r, gsi, 4);
}

/* Word/DWord/QWord address space descriptors */
static void res_word_bus(buf *r, uint16_t min, uint16_t max)
{
    put8(r, 0x88); putn(r, 13, 2);
    put8(r, 2);                                      /* bus number range */
    put8(r, 0x0C);                                   /* MinFixed | MaxFixed, producer */
    put8(r, 0);
    putn(r, 0, 2); putn(r, min, 2); putn(r, max, 2); putn(r, 0, 2); putn(r, (uint32_t)(max - min + 1), 2);
}

static void res_dword(buf *r, int type, uint32_t min, uint32_t max, uint32_t xlat)
{
    put8(r, 0x87); putn(r, 23, 2);
    put8(r, (uint8_t)type);                          /* 0 memory, 1 I/O */
    put8(r, 0x0C);
    put8(r, type == 0 ? 0x01 : 0x03);                /* memory: read/write, non-cacheable; I/O: entire range */
    putn(r, 0, 4); putn(r, min, 4); putn(r, max, 4); putn(r, xlat, 4); putn(r, max - min + 1, 4);
}

static void res_qword_mem(buf *r, uint64_t min, uint64_t max)
{
    put8(r, 0x8A); putn(r, 43, 2);
    put8(r, 0);
    put8(r, 0x0C);
    put8(r, 0x01);
    putn(r, 0, 8); putn(r, min, 8); putn(r, max, 8); putn(r, 0, 8); putn(r, max - min + 1, 8);
}

static void aml_device(buf *parent, const char *name, buf *body)
{
    buf full = { 0 };
    aml_nameseg(&full, name);
    put(&full, body->d, body->len);
    free(body->d);
    memset(body, 0, sizeof *body);
    static const uint8_t op[2] = { 0x5B, 0x82 };
    aml_pkg(parent, op, 2, &full);
}

static void build_dsdt_body(buf *sb, int ncpu)
{
    /* CPU */
    buf d = { 0 };
    for (int k = 0; k < ncpu; k++) {
        char name[5] = { 'C', '0', '0', (char)('0' + k), 0 };
        aml_name_str(&d, "_HID", "ACPI0007");
        aml_name_int(&d, "_UID", (uint64_t)k);
        aml_device(sb, name, &d);
    }

    /* PL011 UART */
    aml_name_str(&d, "_HID", "ARMH0011");
    aml_name_int(&d, "_UID", 0);
    buf r = { 0 };
    res_mem32_fixed(&r, (uint32_t)WD_UART_BASE, 0x1000);
    res_interrupt(&r, WD_IRQ_UART);
    aml_name_crs(&d, "_CRS", &r);
    aml_device(sb, "COM0", &d);

    /* fw_cfg */
    aml_name_str(&d, "_HID", "QEMU0002");
    aml_name_int(&d, "_STA", 0x0B);
    aml_name_int(&d, "_CCA", 1);
    res_mem32_fixed(&r, (uint32_t)WD_FWCFG_BASE, 0x18);
    aml_name_crs(&d, "_CRS", &r);
    aml_device(sb, "FWCF", &d);

    /* PCI Express root bridge */
    aml_name_eisa(&d, "_HID", "PNP0A08");
    aml_name_eisa(&d, "_CID", "PNP0A03");
    aml_name_int(&d, "_SEG", 0);
    aml_name_int(&d, "_BBN", 0);
    aml_name_int(&d, "_UID", 0);
    aml_name_str(&d, "_STR", "PCIe 0 Device");
    aml_name_int(&d, "_CCA", 1);
    uint16_t nbus = (uint16_t)(WD_PCIE_ECAM_SZ >> 20);
    res_word_bus(&r, 0, (uint16_t)(nbus - 1));
    res_dword(&r, 0, (uint32_t)WD_PCIE_MMIO, (uint32_t)(WD_PCIE_MMIO + WD_PCIE_MMIO_SZ - 1), 0);
    res_dword(&r, 1, 0, 0xFFFF, (uint32_t)WD_PCIE_PIO);
    res_qword_mem(&r, WD_PCIE_MMIO_HI, WD_PCIE_MMIO_HI + WD_PCIE_MMIO_HI_SZ - 1);
    aml_name_crs(&d, "_CRS", &r);
    {
        /* _PRT: 32 slots x 4 pins, GSI 35 + (slot + pin) % 4 */
        buf pkg = { 0 };
        put8(&pkg, 128);
        for (int slot = 0; slot < 32; slot++)
            for (int pin = 0; pin < 4; pin++) {
                buf e = { 0 };
                put8(&e, 4);
                aml_int(&e, ((uint64_t)slot << 16) | 0xFFFF);
                aml_int(&e, (uint64_t)pin);
                aml_int(&e, 0);
                aml_int(&e, 32 + 3 + ((slot + pin) & 3));
                static const uint8_t pop = 0x12;
                aml_pkg(&pkg, &pop, 1, &e);
            }
        put8(&d, 0x08);
        aml_nameseg(&d, "_PRT");
        static const uint8_t pop = 0x12;
        aml_pkg(&d, &pop, 1, &pkg);
    }
    {
        /* Method(_OSC, 4) { Return (Arg3) }: grant every requested control */
        buf m = { 0 };
        aml_nameseg(&m, "_OSC");
        put8(&m, 0x04);
        put8(&m, 0xA4);
        put8(&m, 0x6B);
        static const uint8_t mop = 0x14;
        aml_pkg(&d, &mop, 1, &m);
    }
    {
        /* ECAM reservation */
        buf res0 = { 0 };
        aml_name_eisa(&res0, "_HID", "PNP0C02");
        res_qword_mem(&r, WD_PCIE_ECAM, WD_PCIE_ECAM + WD_PCIE_ECAM_SZ - 1);
        aml_name_crs(&res0, "_CRS", &r);
        aml_device(&d, "RES0", &res0);
    }
    aml_device(sb, "PCI0", &d);
}

/* ---------------------------------------------------------------- tables */

static const char OEMID[6] = { 'W', 'I', 'N', 'D', 'R', 'D' };
static const char OEMTBL[8] = { 'W', 'D', 'V', 'I', 'R', 'T', ' ', ' ' };

static size_t table_begin(buf *b, const char *sig, uint8_t rev)
{
    while (b->len & 7) put8(b, 0);                    /* keep tables 8-byte aligned */
    size_t start = b->len;
    put(b, sig, 4);
    putn(b, 0, 4);                                    /* length, patched */
    put8(b, rev);
    put8(b, 0);                                       /* checksum, set by the loader */
    put(b, OEMID, 6);
    put(b, OEMTBL, 8);
    putn(b, 1, 4);
    put(b, "WDRD", 4);
    putn(b, 1, 4);
    return start;
}

static void table_end(buf *b, size_t start) { set32(b, start + 4, (uint32_t)(b->len - start)); }

static void gas(buf *b, uint8_t space, uint8_t width, uint8_t offset, uint8_t access, uint64_t addr)
{
    put8(b, space); put8(b, width); put8(b, offset); put8(b, access); putn(b, addr, 8);
}

/* loader script */
typedef struct { buf b; } loader;

static void cmd_name(buf *b, const char *s) { char n[56] = { 0 }; snprintf(n, sizeof n, "%s", s); put(b, n, 56); }

static void ld_alloc(buf *l, const char *file, uint32_t align, uint8_t zone)
{
    size_t s = l->len;
    putn(l, 1, 4); cmd_name(l, file); putn(l, align, 4); put8(l, zone);
    zero(l, 128 - (l->len - s));
}

static void ld_pointer(buf *l, const char *dst, const char *src, uint32_t off, uint8_t size)
{
    size_t s = l->len;
    putn(l, 2, 4); cmd_name(l, dst); cmd_name(l, src); putn(l, off, 4); put8(l, size);
    zero(l, 128 - (l->len - s));
}

static void ld_checksum(buf *l, const char *file, uint32_t off, uint32_t start, uint32_t len)
{
    size_t s = l->len;
    putn(l, 3, 4); cmd_name(l, file); putn(l, off, 4); putn(l, start, 4); putn(l, len, 4);
    zero(l, 128 - (l->len - s));
}

#define TABLES "etc/acpi/tables"
#define RSDP   "etc/acpi/rsdp"

void wd_acpi_build(wd_machine *m)
{
    buf t = { 0 }, l = { 0 }, rsdp = { 0 };
    size_t off[16];
    int n = 0;

    /* DSDT */
    size_t dsdt = table_begin(&t, "DSDT", 2);
    {
        buf sb = { 0 };
        build_dsdt_body(&sb, m->ncpu);
        buf scope = { 0 };
        put(&scope, "\\_SB_", 5);
        put(&scope, sb.d, sb.len);
        free(sb.d);
        static const uint8_t sop = 0x10;
        aml_pkg(&t, &sop, 1, &scope);
    }
    table_end(&t, dsdt);

    /* FADT */
    size_t fadt = table_begin(&t, "FACP", 6);
    zero(&t, 276 - 36);
    set32(&t, fadt + 112, 1u << 20);                  /* HW_REDUCED_ACPI */
    t.d[fadt + 129] = 3;                              /* ARM_BOOT_ARCH: PSCI compliant, use HVC */
    t.d[fadt + 130] = 0;
    t.d[fadt + 131] = 3;                              /* FADT minor version */
    memcpy(t.d + fadt + 140, (uint64_t[]){ dsdt }, 8);
    memcpy(t.d + fadt + 268, "WINDDROD", 8);          /* hypervisor vendor identity */
    table_end(&t, fadt);
    off[n++] = fadt;

    /* MADT */
    size_t madt = table_begin(&t, "APIC", 4);
    putn(&t, 0, 4); putn(&t, 0, 4);
    put8(&t, 0xC); put8(&t, 24); putn(&t, 0, 2);      /* GIC distributor */
    putn(&t, 0, 4); putn(&t, WD_GICD_BASE, 8); putn(&t, 0, 4); put8(&t, 3); zero(&t, 3);
    for (int k = 0; k < m->ncpu; k++) {
        put8(&t, 0xB); put8(&t, 80); putn(&t, 0, 2);  /* GICC: CPU interface k, processor UID k, enabled */
        putn(&t, (uint64_t)k, 4); putn(&t, (uint64_t)k, 4); putn(&t, 1, 4); putn(&t, 0, 4);
        putn(&t, 0, 4);                               /* performance interrupt */
        putn(&t, 0, 8); putn(&t, 0, 8); putn(&t, 0, 8); putn(&t, 0, 8);
        putn(&t, 0, 4);                               /* VGIC maintenance interrupt */
        putn(&t, 0, 8);                               /* GICR base (use the GICR structure) */
        putn(&t, (uint64_t)k, 8);                     /* MPIDR */
        put8(&t, 0); zero(&t, 3);
    }
    put8(&t, 0xE); put8(&t, 16); putn(&t, 0, 2);      /* GIC redistributor region: one frame per CPU */
    putn(&t, WD_GICR_BASE, 8); putn(&t, 0x20000ULL * (uint64_t)m->ncpu, 4);
    put8(&t, 0xF); put8(&t, 20); putn(&t, 0, 2);      /* GIC ITS 0 */
    putn(&t, 0, 4); putn(&t, WD_GITS_BASE, 8); putn(&t, 0, 4);
    table_end(&t, madt);
    off[n++] = madt;

    /* IORT (revision 0 layout): one ITS group, the PCIe root complex maps every
     * requester ID 1:1 to an ITS DeviceID */
    size_t iort = table_begin(&t, "IORT", 0);
    putn(&t, 2, 4); putn(&t, 48, 4); putn(&t, 0, 4);  /* node count, offset of the first node */
    size_t its_node = t.len - iort;
    put8(&t, 0); putn(&t, 24, 2); put8(&t, 0); putn(&t, 0, 4);   /* ITS group node */
    putn(&t, 0, 4); putn(&t, 0, 4);                   /* no ID mappings */
    putn(&t, 1, 4); putn(&t, 0, 4);                   /* one ITS: identifier 0 */
    put8(&t, 2); putn(&t, 52, 2); put8(&t, 0); putn(&t, 0, 4);   /* root complex node */
    putn(&t, 1, 4); putn(&t, 32, 4);                  /* one ID mapping at offset 32 */
    putn(&t, 1, 4); put8(&t, 0); putn(&t, 0, 2); put8(&t, 3);    /* coherent, CPM | DACS */
    putn(&t, 0, 4);                                   /* ATS not supported */
    putn(&t, 0, 4);                                   /* PCI segment 0 */
    putn(&t, 0, 4); putn(&t, 0xFFFF, 4);              /* input base, number of IDs - 1 */
    putn(&t, 0, 4); putn(&t, its_node, 4); putn(&t, 0, 4);   /* output base, ITS group, flags */
    table_end(&t, iort);
    off[n++] = iort;

    /* GTDT */
    size_t gtdt = table_begin(&t, "GTDT", 3);
    putn(&t, ~0ULL, 8); putn(&t, 0, 4);
    putn(&t, 29, 4); putn(&t, 0, 4);                  /* secure EL1 */
    putn(&t, 30, 4); putn(&t, 1u << 2, 4);            /* non-secure EL1, always-on */
    putn(&t, 27, 4); putn(&t, 0, 4);                  /* virtual */
    putn(&t, 26, 4); putn(&t, 0, 4);                  /* EL2 */
    putn(&t, ~0ULL, 8); putn(&t, 0, 4); putn(&t, 0, 4);
    putn(&t, 0, 4); putn(&t, 0, 4);                   /* virtual EL2 timer: none */
    table_end(&t, gtdt);
    off[n++] = gtdt;

    /* MCFG */
    size_t mcfg = table_begin(&t, "MCFG", 1);
    zero(&t, 8);
    putn(&t, WD_PCIE_ECAM, 8); putn(&t, 0, 2); put8(&t, 0); put8(&t, (uint8_t)((WD_PCIE_ECAM_SZ >> 20) - 1)); putn(&t, 0, 4);
    table_end(&t, mcfg);
    off[n++] = mcfg;

    /* SPCR */
    size_t spcr = table_begin(&t, "SPCR", 2);
    put8(&t, 3); zero(&t, 3);                         /* ARM PL011 */
    gas(&t, 0, 32, 0, 3, WD_UART_BASE);
    put8(&t, 8);                                      /* interrupt type: ARMH GIC */
    put8(&t, 0);
    putn(&t, WD_IRQ_UART, 4);
    put8(&t, 7);                                      /* 115200 baud */
    put8(&t, 0); put8(&t, 1); put8(&t, 0);            /* parity, stop bits, flow control */
    put8(&t, 0);                                      /* terminal type VT100 */
    put8(&t, 0);                                      /* language */
    putn(&t, 0xFFFF, 2); putn(&t, 0xFFFF, 2);         /* not a PCI device */
    put8(&t, 0); put8(&t, 0); put8(&t, 0); putn(&t, 0, 4); put8(&t, 0); zero(&t, 4);
    table_end(&t, spcr);
    off[n++] = spcr;

    /* DBG2 */
    size_t dbg2 = table_begin(&t, "DBG2", 0);
    putn(&t, 44, 4); putn(&t, 1, 4);
    const char ns[] = "\\_SB.COM0";
    uint16_t devlen = (uint16_t)(22 + 12 + 4 + sizeof ns);
    put8(&t, 0); putn(&t, devlen, 2); put8(&t, 1);
    putn(&t, sizeof ns, 2); putn(&t, 38, 2); putn(&t, 0, 2); putn(&t, 0, 2);
    putn(&t, 0x8000, 2); putn(&t, 0x0003, 2); putn(&t, 0, 2);
    putn(&t, 22, 2); putn(&t, 34, 2);
    gas(&t, 0, 32, 0, 3, WD_UART_BASE);
    putn(&t, 0x1000, 4);
    put(&t, ns, sizeof ns);
    table_end(&t, dbg2);
    off[n++] = dbg2;

    /* XSDT */
    size_t xsdt = table_begin(&t, "XSDT", 1);
    size_t xent = t.len;
    for (int i = 0; i < n; i++) putn(&t, off[i], 8);
    table_end(&t, xsdt);

    /* RSDP */
    put(&rsdp, "RSD PTR ", 8);
    put8(&rsdp, 0);
    put(&rsdp, OEMID, 6);
    put8(&rsdp, 2);
    putn(&rsdp, 0, 4);
    putn(&rsdp, 36, 4);
    putn(&rsdp, xsdt, 8);
    put8(&rsdp, 0);
    zero(&rsdp, 3);

    /* loader script: allocate, patch pointers, then checksums */
    ld_alloc(&l, RSDP, 16, 2);
    ld_alloc(&l, TABLES, 64, 1);
    ld_pointer(&l, TABLES, TABLES, (uint32_t)(fadt + 140), 8);
    for (int i = 0; i < n; i++) ld_pointer(&l, TABLES, TABLES, (uint32_t)(xent + 8 * (size_t)i), 8);
    ld_pointer(&l, RSDP, TABLES, 24, 8);
    size_t starts[] = { dsdt, fadt, madt, iort, gtdt, mcfg, spcr, dbg2, xsdt };
    for (size_t i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        uint32_t len = (uint32_t)(t.d[starts[i] + 4] | t.d[starts[i] + 5] << 8 | t.d[starts[i] + 6] << 16 |
                                  (uint32_t)t.d[starts[i] + 7] << 24);
        ld_checksum(&l, TABLES, (uint32_t)(starts[i] + 9), (uint32_t)starts[i], len);
    }
    ld_checksum(&l, RSDP, 8, 0, 20);
    ld_checksum(&l, RSDP, 32, 0, 36);

    wd_fwcfg_add_file(&m->fwcfg, RSDP, rsdp.d, (uint32_t)rsdp.len, 0);
    wd_fwcfg_add_file(&m->fwcfg, TABLES, t.d, (uint32_t)t.len, 0);
    wd_fwcfg_add_file(&m->fwcfg, "etc/table-loader", l.d, (uint32_t)l.len, 0);
    free(t.d);
    free(l.d);
    free(rsdp.d);
}
