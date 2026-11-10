/*
 * MSI-X through the GICv3 ITS into LPIs, the way an OS (Windows, Linux) drives
 * PCIe devices: LPI property/pending tables, ITS command queue (MAPC, MAPD,
 * MAPTI, INT, SYNC), MSI-X tables of the NVMe and xHCI controllers, NVMe
 * admin and I/O queues with per-queue vectors, vector masking with the
 * pending bit array, and an xHCI command completion interrupt.
 *
 * The runner attaches a raw disk whose first sector starts with "WDMSI".
 */
#include "rt.h"

#define ITS_BASE   0x08080000UL
#define TRANSLATER (ITS_BASE + 0x10040UL)
#define ECAM       0x3F000000UL
#define NVME_BAR   0x10000000UL
#define XHCI_BAR   0x10010000UL
#define NVME_DEVID 0x08          /* bus 0, device 1 */
#define XHCI_DEVID 0x10          /* bus 0, device 2 */

static uint8_t prop[8192] __attribute__((aligned(4096)));
static uint8_t pend[16384] __attribute__((aligned(65536)));
static uint8_t cmdq[4096] __attribute__((aligned(4096)));
static uint8_t devtab[4096] __attribute__((aligned(4096)));
static uint8_t colltab[4096] __attribute__((aligned(4096)));
static uint8_t itt[2][256] __attribute__((aligned(256)));
static uint8_t asq[4096] __attribute__((aligned(4096))), acq[4096] __attribute__((aligned(4096)));
static uint8_t iosq[4096] __attribute__((aligned(4096))), iocq[4096] __attribute__((aligned(4096)));
static uint8_t idbuf[4096] __attribute__((aligned(4096)));
static uint8_t data[8192] __attribute__((aligned(4096)));
static uint64_t dcbaa[16] __attribute__((aligned(64)));
static uint8_t xcmd[1024] __attribute__((aligned(64)));
static uint8_t xev[1024] __attribute__((aligned(64)));
static uint64_t erst[2] __attribute__((aligned(64)));

static uint8_t inctx[2048] __attribute__((aligned(64)));
static uint8_t devctx[2048] __attribute__((aligned(1024)));
static uint32_t ep0ring[16 * 4] __attribute__((aligned(64)));
static uint8_t desc[64] __attribute__((aligned(64)));

static volatile int lpi_count[4];
static volatile int other_irq;
static int cmd_n;

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static int hook(struct frame *f, int kind)
{
    (void)f;
    if (kind != 5) return 0;
    uint64_t iar = READ_SYS(s3_0_c12_c12_0);
    uint32_t id = (uint32_t)iar & 0xFFFFFF;
    if (id == 1023) return 1;
    if (id >= 8192 && id < 8196) lpi_count[id - 8192]++;
    else other_irq++;
    WRITE_SYS(s3_0_c12_c12_1, iar);
    return 1;
}

static int wait_lpi(int n, int before)
{
    uint64_t start = icount();
    while (lpi_count[n] == before) {
        if (icount() - start > 50000000ULL) return 0;
        __asm__ volatile("wfi");
    }
    return 1;
}

/* ---------------------------------------------------------------- GIC + ITS */

static void its_cmd(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3)
{
    uint64_t *c = (uint64_t *)(cmdq + 32 * cmd_n);
    c[0] = d0; c[1] = d1; c[2] = d2; c[3] = d3;
    cmd_n++;
    dsb();
    mmio_w64(ITS_BASE + 0x88, (uint64_t)cmd_n * 32);
    while (mmio_r64(ITS_BASE + 0x90) != (uint64_t)cmd_n * 32) {}
}

static void gic_its_init(void)
{
    mmio_w32(GICR_BASE + 0x14, mmio_r32(GICR_BASE + 0x14) & ~2u);
    while (mmio_r32(GICR_BASE + 0x14) & 4) {}
    mmio_w32(GICD_BASE + 0x0, 3);
    WRITE_SYS(s3_0_c12_c12_5, 7);
    ISB();
    WRITE_SYS(s3_0_c4_c6_0, 0xF0);
    WRITE_SYS(s3_0_c12_c12_7, 1);
    ISB();

    uint32_t typer = mmio_r32(GICD_BASE + 0x4);
    CHECK(typer & (1u << 17));                            /* LPIs supported */
    CHECK(((typer >> 19) & 0x1F) >= 13);                  /* at least 14 ID bits */
    CHECK(mmio_r64(GICR_BASE + 0x8) & 1);                 /* GICR_TYPER.PLPIS */

    for (int i = 0; i < 4; i++) prop[i] = 0xA0 | 2 | 1;  /* priority 0xA0, enabled */
    dsb();
    mmio_w64(GICR_BASE + 0x70, (uint64_t)(uintptr_t)prop | 13);   /* 14 ID bits */
    mmio_w64(GICR_BASE + 0x78, (uint64_t)(uintptr_t)pend);
    CHECK_EQ(mmio_r64(GICR_BASE + 0x70) & ~0xFFFULL, (uint64_t)(uintptr_t)prop);
    mmio_w32(GICR_BASE + 0x0, 1);                         /* EnableLPIs */
    CHECK(mmio_r32(GICR_BASE + 0x0) & 1);

    uint64_t ityper = mmio_r64(ITS_BASE + 0x8);
    CHECK(ityper & 1);                                    /* physical LPIs */
    CHECK_EQ((ityper >> 4) & 0xF, 7);                     /* 8-byte ITT entries */
    CHECK(((ityper >> 13) & 0x1F) >= 7);                  /* DeviceID bits */
    CHECK(mmio_r32(ITS_BASE + 0x0) & (1u << 31));         /* quiescent while disabled */
    for (int n = 0; n < 2; n++) {
        uint64_t b = mmio_r64(ITS_BASE + 0x100 + 8 * n);
        int type = (int)((b >> 56) & 7);
        CHECK_EQ(type, n == 0 ? 1 : 4);                   /* devices, collections */
        CHECK_EQ((b >> 48) & 0x1F, 7);
        mmio_w64(ITS_BASE + 0x100 + 8 * n, (1ULL << 63) | (uint64_t)(uintptr_t)(n ? colltab : devtab));
        CHECK_EQ(mmio_r64(ITS_BASE + 0x100 + 8 * n) & 0x0000FFFFFFFFF000ULL, (uint64_t)(uintptr_t)(n ? colltab : devtab));
    }
    mmio_w64(ITS_BASE + 0x80, (1ULL << 63) | (uint64_t)(uintptr_t)cmdq);   /* one page */
    mmio_w64(ITS_BASE + 0x88, 0);
    mmio_w32(ITS_BASE + 0x0, 1);
    CHECK(mmio_r32(ITS_BASE + 0x0) & 1);

    its_cmd(0x09, 0, (1ULL << 63) | 0, 0);                                /* MAPC icid 0 -> PE 0 */
    its_cmd(0x08 | (uint64_t)NVME_DEVID << 32, 4, (1ULL << 63) | (uint64_t)(uintptr_t)itt[0], 0);  /* MAPD, 32 events */
    its_cmd(0x0A | (uint64_t)NVME_DEVID << 32, 0 | (8192ULL << 32), 0, 0);   /* MAPTI ev 0 -> 8192 */
    its_cmd(0x0A | (uint64_t)NVME_DEVID << 32, 1 | (8193ULL << 32), 0, 0);   /* MAPTI ev 1 -> 8193 */
    its_cmd(0x08 | (uint64_t)XHCI_DEVID << 32, 0, (1ULL << 63) | (uint64_t)(uintptr_t)itt[1], 0);  /* MAPD, 2 events */
    its_cmd(0x0A | (uint64_t)XHCI_DEVID << 32, 0 | (8194ULL << 32), 0, 0);   /* MAPTI ev 0 -> 8194 */
    its_cmd(0x0A | (uint64_t)XHCI_DEVID << 32, 5 | (8195ULL << 32), 0, 0);   /* event 5 is out of range: ignored */
    its_cmd(0x05, 0, 0, 0);                                                 /* SYNC */

    /* INT: software-generated LPI through the translation */
    int b = lpi_count[0];
    its_cmd(0x03 | (uint64_t)NVME_DEVID << 32, 0, 0, 0);
    CHECK(wait_lpi(0, b));
    /* a disabled LPI stays pending until its property byte enables it (then INV) */
    prop[1] &= ~1;
    dsb();
    b = lpi_count[1];
    its_cmd(0x03 | (uint64_t)NVME_DEVID << 32, 1, 0, 0);    /* INT event 1 -> 8193 */
    for (volatile int i = 0; i < 2000; i++) {}
    CHECK_EQ(lpi_count[1], b);
    prop[1] |= 1;
    dsb();
    its_cmd(0x0C | (uint64_t)NVME_DEVID << 32, 1, 0, 0);    /* INV */
    CHECK(wait_lpi(1, b));
    /* an event beyond the device's MAPD size was never mapped */
    its_cmd(0x03 | (uint64_t)XHCI_DEVID << 32, 5, 0, 0);
    for (volatile int i = 0; i < 2000; i++) {}
    CHECK_EQ(lpi_count[3], 0);
    /* a raw write to GITS_TRANSLATER is attributed to DeviceID 0, which is not mapped */
    mmio_w32(TRANSLATER, 0);
    for (volatile int i = 0; i < 1000; i++) {}
    CHECK_EQ(lpi_count[0], b + 1);
    CHECK_EQ(other_irq, 0);
}

/* ---------------------------------------------------------------- PCI */

static uintptr_t cfg(int dev, int off) { return ECAM + ((uintptr_t)dev << 15) + (uintptr_t)off; }

static int find_dev(uint16_t vendor, uint16_t device)
{
    for (int d = 0; d < 32; d++)
        if (mmio_r32(cfg(d, 0)) == ((uint32_t)device << 16 | vendor)) return d;
    return -1;
}

static int find_cap(int dev, int id)
{
    int p = *(volatile uint8_t *)cfg(dev, 0x34);
    for (int guard = 0; p && guard < 48; guard++) {
        if (*(volatile uint8_t *)cfg(dev, p) == id) return p;
        p = *(volatile uint8_t *)cfg(dev, p + 1);
    }
    return 0;
}

/* program BAR0 (64-bit), enable memory + bus master, set MSI-X entries; returns the table address */
static uintptr_t setup_msix(int dev, uintptr_t bar, int nvec, const uint32_t *datas)
{
    mmio_w32(cfg(dev, 0x10), 0xFFFFFFFFu);
    uint32_t sz = mmio_r32(cfg(dev, 0x10));
    CHECK((sz & 6) == 4);                                 /* 64-bit memory BAR */
    mmio_w32(cfg(dev, 0x10), (uint32_t)bar);
    mmio_w32(cfg(dev, 0x14), 0);
    mmio_w32(cfg(dev, 0x04), mmio_r32(cfg(dev, 0x04)) | 6);
    int c = find_cap(dev, 0x11);
    CHECK(c != 0);
    if (!c) return 0;
    uint16_t ctl = *(volatile uint16_t *)cfg(dev, c + 2);
    CHECK((ctl & 0x7FF) + 1 >= nvec);
    uint32_t tbl = mmio_r32(cfg(dev, c + 4));
    CHECK_EQ(tbl & 7, 0);                                 /* BIR 0 */
    uintptr_t t = bar + (tbl & ~7u);
    for (int v = 0; v < nvec; v++) {
        CHECK(mmio_r32(t + 16 * v + 12) & 1);             /* masked after reset */
        mmio_w32(t + 16 * v + 0, (uint32_t)TRANSLATER);
        mmio_w32(t + 16 * v + 4, 0);
        mmio_w32(t + 16 * v + 8, datas[v]);
        mmio_w32(t + 16 * v + 12, 0);
    }
    *(volatile uint16_t *)cfg(dev, c + 2) = (uint16_t)(ctl | 0x8000);   /* MSI-X enable */
    return t;
}

/* ---------------------------------------------------------------- NVMe */

static int sq_tail[2], cq_head[2], cq_phase[2] = { 1, 1 };

static void nvme_submit(int q, const uint32_t *cmd16)
{
    uint8_t *sq = q ? iosq : asq;
    memcpy(sq + 64 * sq_tail[q], cmd16, 64);
    sq_tail[q] = (sq_tail[q] + 1) % 16;
    dsb();
    mmio_w32(NVME_BAR + 0x1000 + 8 * (uintptr_t)q, (uint32_t)sq_tail[q]);
}

/* consume one completion; returns the status field or -1 */
static int nvme_reap(int q)
{
    uint8_t *cq = q ? iocq : acq;
    volatile uint16_t *st = (volatile uint16_t *)(cq + 16 * cq_head[q] + 14);
    uint64_t start = icount();
    while ((*st & 1) != cq_phase[q]) if (icount() - start > 50000000ULL) return -1;
    int status = *st >> 1;
    cq_head[q]++;
    if (cq_head[q] == 16) { cq_head[q] = 0; cq_phase[q] ^= 1; }
    mmio_w32(NVME_BAR + 0x1000 + 8 * (uintptr_t)q + 4, (uint32_t)cq_head[q]);
    return status;
}

static void test_nvme(void)
{
    int dev = find_dev(0x1B36, 0x0010);
    CHECK_EQ(dev, NVME_DEVID >> 3);
    if (dev < 0) return;
    uint32_t datas[2] = { 0, 1 };
    uintptr_t t = setup_msix(dev, NVME_BAR, 2, datas);
    CHECK(!(mmio_r32(cfg(dev, 0x04)) & (1u << 19)));      /* no INTx status while MSI-X is on */

    mmio_w32(NVME_BAR + 0x14, 0);
    while (mmio_r32(NVME_BAR + 0x1C) & 1) {}
    mmio_w32(NVME_BAR + 0x24, (15u << 16) | 15u);
    mmio_w64(NVME_BAR + 0x28, (uint64_t)(uintptr_t)asq);
    mmio_w64(NVME_BAR + 0x30, (uint64_t)(uintptr_t)acq);
    mmio_w32(NVME_BAR + 0x14, (4u << 20) | (6u << 16) | 1u);
    while (!(mmio_r32(NVME_BAR + 0x1C) & 1)) {}

    /* identify controller: admin completion on vector 0 -> LPI 8192 */
    uint32_t c[16] = { 0 };
    int b = lpi_count[0];
    c[0] = 0x06 | (1u << 16);
    c[6] = (uint32_t)(uintptr_t)idbuf;
    c[10] = 1;
    nvme_submit(0, c);
    CHECK(wait_lpi(0, b));
    CHECK_EQ(nvme_reap(0), 0);
    CHECK(!memcmp(idbuf + 24, "WindDroid NVMe Disk", 19));

    /* I/O queue pair on vector 1 */
    memset(c, 0, sizeof c);
    c[0] = 0x05 | (2u << 16);
    c[6] = (uint32_t)(uintptr_t)iocq;
    c[10] = (15u << 16) | 1;
    c[11] = (1u << 16) | 3;                               /* IV 1, IEN, PC */
    b = lpi_count[0];
    nvme_submit(0, c);
    CHECK(wait_lpi(0, b));
    CHECK_EQ(nvme_reap(0), 0);
    memset(c, 0, sizeof c);
    c[0] = 0x01 | (3u << 16);
    c[6] = (uint32_t)(uintptr_t)iosq;
    c[10] = (15u << 16) | 1;
    c[11] = (1u << 16) | 1;                               /* CQ 1, PC */
    nvme_submit(0, c);
    CHECK(wait_lpi(0, b + 1));
    CHECK_EQ(nvme_reap(0), 0);

    /* read sector 0 */
    memset(c, 0, sizeof c);
    c[0] = 0x02 | (4u << 16);
    c[1] = 1;
    c[6] = (uint32_t)(uintptr_t)data;
    c[12] = 7;                                            /* 8 blocks */
    b = lpi_count[1];
    nvme_submit(1, c);
    CHECK(wait_lpi(1, b));
    CHECK_EQ(nvme_reap(1), 0);
    CHECK(!memcmp(data, "WDMSI", 5));

    /* masked vector: the message waits in the PBA until unmasked */
    uint32_t pba_off = mmio_r32(cfg(dev, find_cap(dev, 0x11) + 8)) & ~7u;
    mmio_w32(t + 16 + 12, 1);
    memset(data, 0x5A, 512);
    memset(c, 0, sizeof c);
    c[0] = 0x01 | (5u << 16);                             /* write LBA 16 */
    c[1] = 1;
    c[6] = (uint32_t)(uintptr_t)data;
    c[10] = 16;
    b = lpi_count[1];
    nvme_submit(1, c);
    CHECK_EQ(nvme_reap(1), 0);                            /* completion is there ... */
    for (volatile int i = 0; i < 2000; i++) {}
    CHECK_EQ(lpi_count[1], b);                            /* ... but no interrupt */
    CHECK(mmio_r32(NVME_BAR + pba_off) & 2);              /* pending bit for vector 1 */
    mmio_w32(t + 16 + 12, 0);                             /* unmask */
    CHECK(wait_lpi(1, b));
    CHECK(!(mmio_r32(NVME_BAR + pba_off) & 2));

    /* read it back */
    memset(data, 0, 512);
    memset(c, 0, sizeof c);
    c[0] = 0x02 | (6u << 16);
    c[1] = 1;
    c[6] = (uint32_t)(uintptr_t)data;
    c[10] = 16;
    b = lpi_count[1];
    nvme_submit(1, c);
    CHECK(wait_lpi(1, b));
    CHECK_EQ(nvme_reap(1), 0);
    int same = 1;
    for (int i = 0; i < 512; i++) if (data[i] != 0x5A) same = 0;
    CHECK(same);

    /* function mask holds every vector */
    int cap = find_cap(dev, 0x11);
    volatile uint16_t *ctl = (volatile uint16_t *)cfg(dev, cap + 2);
    *ctl = (uint16_t)(*ctl | 0x4000);
    memset(c, 0, sizeof c);
    c[0] = 0x00 | (7u << 16);                             /* flush */
    c[1] = 1;
    b = lpi_count[1];
    nvme_submit(1, c);
    CHECK_EQ(nvme_reap(1), 0);
    CHECK_EQ(lpi_count[1], b);
    *ctl = (uint16_t)(*ctl & ~0x4000);
    CHECK(wait_lpi(1, b));
}

/* ---------------------------------------------------------------- xHCI */

static void test_xhci(void)
{
    int dev = find_dev(0x1B36, 0x000D);
    CHECK_EQ(dev, XHCI_DEVID >> 3);
    if (dev < 0) return;
    uint32_t datas[1] = { 0 };
    setup_msix(dev, XHCI_BAR, 1, datas);
    uintptr_t op = XHCI_BAR + (mmio_r32(XHCI_BAR) & 0xFF);
    uintptr_t rt = XHCI_BAR + (mmio_r32(XHCI_BAR + 0x18) & ~0x1Fu);
    uintptr_t db = XHCI_BAR + (mmio_r32(XHCI_BAR + 0x14) & ~3u);
    mmio_w32(op + 0x00, 2);                               /* HCRST */
    while (mmio_r32(op + 0x00) & 2) {}
    while (mmio_r32(op + 0x04) & (1u << 11)) {}           /* CNR */
    mmio_w32(op + 0x38, 8);
    mmio_w64(op + 0x30, (uint64_t)(uintptr_t)dcbaa);
    mmio_w64(op + 0x18, (uint64_t)(uintptr_t)xcmd | 1);
    erst[0] = (uint64_t)(uintptr_t)xev;
    erst[1] = 64;
    mmio_w32(rt + 0x28, 1);                               /* ERSTSZ */
    mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)xev);        /* ERDP */
    mmio_w64(rt + 0x30, (uint64_t)(uintptr_t)erst);       /* ERSTBA */
    mmio_w32(rt + 0x20, 2);                               /* IMAN.IE */
    mmio_w32(op + 0x00, 1 | 4);                           /* run, interrupts on */
    while (mmio_r32(op + 0x04) & 1) {}

    /* two no-op commands: the second waits for EHB to be cleared */
    uint32_t *t = (uint32_t *)xcmd;
    t[0] = t[1] = t[2] = 0;
    t[3] = (23u << 10) | 1;
    int b = lpi_count[2];
    dsb();
    mmio_w32(db, 0);
    CHECK(wait_lpi(2, b));
    uint32_t *e = (uint32_t *)xev;
    CHECK_EQ((e[3] >> 10) & 0x3F, 33);                    /* command completion */
    CHECK_EQ(e[2] >> 24, 1);                              /* success */
    CHECK_EQ(e[0], (uint32_t)(uintptr_t)xcmd);
    CHECK_EQ(e[3] & 1, 1);
    CHECK(!(mmio_r32(rt + 0x20) & 1));                    /* IP cleared by the MSI-X write */
    t[4] = t[5] = t[6] = 0;
    t[7] = (23u << 10) | 1;
    dsb();
    mmio_w32(db, 0);
    for (volatile int i = 0; i < 2000; i++) {}
    CHECK_EQ(lpi_count[2], b + 1);                        /* EHB still set: no new message */
    CHECK_EQ((e[7] >> 10) & 0x3F, 33);
    mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)(xev + 16) | 8);   /* consumed one, clear EHB */
    CHECK(wait_lpi(2, b + 1));                            /* the unconsumed event interrupts again */
    mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)(xev + 32) | 8);
    for (volatile int i = 0; i < 2000; i++) {}
    CHECK_EQ(lpi_count[2], b + 2);

    /* wrap the command ring with a Link TRB that toggles the cycle state */
    t[0] = t[1] = t[2] = 0;
    t[3] = (23u << 10) | 0;                               /* no-op, cycle 0 (second lap) */
    t[4] = t[5] = t[6] = 0;
    t[7] = (23u << 10) | 0;
    t[8] = (uint32_t)(uintptr_t)xcmd; t[9] = 0; t[10] = 0;
    t[11] = (6u << 10) | 2 | 1;                           /* link, toggle cycle, cycle 1 (first lap) */
    dsb();
    mmio_w32(db, 0);
    CHECK(wait_lpi(2, b + 2));
    CHECK_EQ((e[11] >> 10) & 0x3F, 33);
    CHECK_EQ(e[8], (uint32_t)(uintptr_t)xcmd);            /* both no-ops of the second lap completed */
    CHECK_EQ((e[15] >> 10) & 0x3F, 33);
    CHECK_EQ(e[12], (uint32_t)(uintptr_t)(xcmd + 16));
    CHECK_EQ(e[19] & 1, 0);                               /* and nothing else */
    mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)(xev + 64) | 8);

    /* ---- a minimal USB driver: slot, address, control transfers on a 16-TRB ring */
    int er_i = 4, er_cyc = 1;
#define NEXT_EVENT(out) do { \
        uint32_t *ev_ = (uint32_t *)(xev + 16 * er_i); uint64_t st_ = icount(); \
        while ((int)(ev_[3] & 1) != er_cyc) if (icount() - st_ > 50000000ULL) break; \
        memcpy((out), ev_, 16); \
        if (++er_i == 64) { er_i = 0; er_cyc ^= 1; } \
        mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)(xev + 16 * er_i) | 8); \
    } while (0)
    uint32_t ev[4];
    int cmd_i = 2, cmd_cyc = 0;                           /* command ring position after the wrap */
#define COMMAND(d0, d1, d2, d3) do { \
        uint32_t *c_ = (uint32_t *)(xcmd + 16 * cmd_i); \
        c_[0] = (d0); c_[1] = (d1); c_[2] = (d2); c_[3] = ((d3) & ~1u) | (uint32_t)cmd_cyc; \
        cmd_i++; dsb(); mmio_w32(db, 0); \
    } while (0)
    /* the link TRB at index 2 still has cycle 1 from the first lap: make it valid for this lap */
    cmd_i = 0;                                            /* restart the ring at its base: */
    {
        uint32_t *l = (uint32_t *)(xcmd + 32);
        l[3] = (6u << 10) | 2 | (uint32_t)cmd_cyc;        /* (the controller is waiting at index 2) */
        cmd_i = 0;
    }
    /* the controller follows the link back to index 0 and toggles to cycle 1 */
    cmd_cyc = 1;
    uintptr_t port1 = op + 0x400;
    CHECK(mmio_r32(port1) & 1);                           /* keyboard connected on port 1 */
    mmio_w32(port1, (mmio_r32(port1) & 0x0E00C3E0u) | (1u << 4));   /* port reset */
    NEXT_EVENT(ev);
    CHECK_EQ((ev[3] >> 10) & 0x3F, 34);                   /* port status change */
    CHECK_EQ(ev[0] >> 24, 1);
    CHECK(mmio_r32(port1) & 2);                           /* enabled */
    mmio_w32(port1, (mmio_r32(port1) & 0x0E00C3E0u) | (1u << 21) | (1u << 17));   /* clear PRC, CSC */

    COMMAND(0, 0, 0, 9u << 10);                           /* enable slot */
    NEXT_EVENT(ev);
    int slot = (int)(ev[3] >> 24);
    CHECK_EQ(ev[2] >> 24, 1);
    CHECK(slot >= 1);
    dcbaa[slot] = (uint64_t)(uintptr_t)devctx;
    memset(inctx, 0, sizeof inctx);
    uint32_t *icc = (uint32_t *)inctx, *sc = (uint32_t *)(inctx + 32), *ep0 = (uint32_t *)(inctx + 64);
    icc[1] = 3;                                           /* add slot + EP0 */
    sc[0] = (1u << 27) | (3u << 20);                      /* one context entry, high speed */
    sc[1] = 1u << 16;                                     /* root hub port 1 */
    ep0[1] = (4u << 3) | (3u << 1) | (64u << 16);         /* control, CErr 3, MPS 64 */
    ep0[2] = (uint32_t)(uintptr_t)ep0ring | 1;
    ep0[4] = 8;
    memset(ep0ring, 0, sizeof ep0ring);
    COMMAND((uint32_t)(uintptr_t)inctx, 0, 0, (11u << 10) | ((uint32_t)slot << 24));   /* address device */
    NEXT_EVENT(ev);
    CHECK_EQ((ev[3] >> 10) & 0x3F, 33);
    CHECK_EQ(ev[2] >> 24, 1);
    CHECK_EQ(((uint32_t *)devctx)[3] >> 27, 2);           /* slot state: addressed */
    CHECK_EQ(((uint32_t *)devctx)[3] & 0xFF, slot);       /* USB address */
    CHECK_EQ(((uint32_t *)devctx)[8] & 7, 1);             /* EP0 running */

    /* 70 GET_DESCRIPTOR(device) transfers: the 16-entry transfer ring wraps through
     * its link TRB many times and the 64-entry event ring wraps once */
    int tr_i = 0, tr_cyc = 1, good = 0;
    for (int k = 0; k < 70; k++) {
        uint32_t td[3][4] = {
            { 0x01000680u, 18u << 16, 8, (2u << 10) | (1u << 6) | (3u << 16) },   /* setup, IN data */
            { (uint32_t)(uintptr_t)desc, 0, 18, (3u << 10) | (1u << 16) },        /* data IN */
            { 0, 0, 0, (4u << 10) | (1u << 5) },                                  /* status OUT, IOC */
        };
        memset(desc, 0, sizeof desc);
        for (int j = 0; j < 3; j++) {
            if (tr_i == 15) {                             /* link back, toggle cycle */
                uint32_t *l = ep0ring + 4 * 15;
                l[0] = (uint32_t)(uintptr_t)ep0ring; l[1] = 0; l[2] = 0;
                l[3] = (6u << 10) | 2 | (uint32_t)tr_cyc;
                tr_i = 0;
                tr_cyc ^= 1;
            }
            uint32_t *t2 = ep0ring + 4 * tr_i++;
            t2[0] = td[j][0]; t2[1] = td[j][1]; t2[2] = td[j][2];
            dsb();
            t2[3] = td[j][3] | (uint32_t)tr_cyc;
        }
        dsb();
        mmio_w32(db + 4 * (uintptr_t)slot, 1);
        NEXT_EVENT(ev);
        if (((ev[3] >> 10) & 0x3F) == 32 && (ev[2] >> 24) == 1 && desc[0] == 18 && desc[1] == 1 &&
            desc[2] == 0x00 && desc[3] == 0x02 && desc[10] == 0x01)
            good++;
    }
    CHECK_EQ(good, 70);
}

int main(void)
{
    exc_hook = hook;
    __asm__ volatile("msr daifclr, #2");
    gic_its_init();
    test_nvme();
    test_xhci();
    CHECK_EQ(other_irq, 0);
    printf_("msi: %d failures (LPIs %d %d %d)\n", test_failures, lpi_count[0], lpi_count[1], lpi_count[2]);
    return test_failures ? 1 : 0;
}
