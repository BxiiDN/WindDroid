/*
 * WindDroid - NVM Express 1.4 controller on PCIe (one namespace, 512-byte
 * blocks). Works with EDK2's NvmExpressDxe and Windows' inbox stornvme.
 * Commands are executed synchronously when the submission doorbell is rung.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"

#define NVME_MAXQ   17          /* admin + 16 I/O queue pairs */
#define PAGE        4096u
#define MDTS        7           /* 2^7 pages = 512 KiB per command */

/* status codes (SCT 0) */
#define SC_OK          0x00
#define SC_INVALID_OP  0x01
#define SC_INVALID_FLD 0x02
#define SC_INTERNAL    0x06
#define SC_INVALID_NS  0x0B
#define SC_LBA_RANGE   0x80
#define SC_WRITE_PROT  0x20      /* namespace is write protected */
/* command specific (SCT 1) */
#define SC_INVALID_QID   0x101
#define SC_INVALID_QSIZE 0x102
#define SC_INVALID_CQ    0x100

typedef struct { uint64_t base; uint32_t size, head, tail; uint16_t cqid; int valid; } nvme_sq;
typedef struct { uint64_t base; uint32_t size, head, tail; int phase, valid, ien; uint16_t iv; } nvme_cq;

typedef struct {
    wd_pcidev pci;
    wd_disk disk;
    uint32_t cc, csts, aqa, intms;
    uint64_t asq, acq;
    nvme_sq sq[NVME_MAXQ];
    nvme_cq cq[NVME_MAXQ];
    uint32_t features[256];
    uint8_t *bounce;
    char serial[21];
    int lba_shift;
} nvme_ctrl;

#define LBA_SHIFT (c->lba_shift)

static void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void st64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t ld32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t ld64(const uint8_t *p) { return ld32(p) | (uint64_t)ld32(p + 4) << 32; }

static void pad_str(uint8_t *dst, const char *s, int n)
{
    memset(dst, ' ', (size_t)n);
    memcpy(dst, s, strlen(s) < (size_t)n ? strlen(s) : (size_t)n);
}

/* ---------------------------------------------------------------- interrupts */

static void update_irq(wd_machine *m, nvme_ctrl *c)
{
    int level = 0;
    for (int i = 0; i < NVME_MAXQ; i++) {
        nvme_cq *q = &c->cq[i];
        if (q->valid && q->ien && q->head != q->tail && !(c->intms & 1)) level = 1;
    }
    wd_pci_set_intx(m, &c->pci, level);
}

/* ---------------------------------------------------------------- PRP data transfer */

/* Copy 'len' bytes between 'buf' and the PRP-described guest memory. to_guest selects the direction. */
static int prp_xfer(wd_machine *m, uint64_t prp1, uint64_t prp2, uint8_t *buf, uint64_t len, int to_guest)
{
    uint64_t done = 0;
    uint64_t first = PAGE - (prp1 & (PAGE - 1));
    if (first > len) first = len;
    if (to_guest ? wd_dma_write(m, prp1, buf, first) : wd_dma_read(m, prp1, buf, first)) return -1;
    done = first;
    if (done == len) return 0;
    uint64_t rest = len - done;
    if (rest <= PAGE) {
        return to_guest ? wd_dma_write(m, prp2, buf + done, rest) : wd_dma_read(m, prp2, buf + done, rest);
    }
    uint64_t list = prp2;
    while (done < len) {
        uint64_t idx_in_page = (list & (PAGE - 1)) / 8;
        uint64_t entry;
        if (wd_dma_read(m, list, &entry, 8)) return -1;
        if (idx_in_page == PAGE / 8 - 1 && len - done > PAGE) {   /* last slot chains to the next list page */
            list = entry;
            continue;
        }
        uint64_t n = len - done < PAGE ? len - done : PAGE;
        if (to_guest ? wd_dma_write(m, entry, buf + done, n) : wd_dma_read(m, entry, buf + done, n)) return -1;
        done += n;
        list += 8;
    }
    return 0;
}

/* ---------------------------------------------------------------- completions */

static void complete(wd_machine *m, nvme_ctrl *c, int sqid, uint16_t cid, uint32_t dw0, uint16_t status)
{
    nvme_cq *q = &c->cq[c->sq[sqid].cqid];
    if (!q->valid) return;
    uint8_t e[16];
    st32(e, dw0);
    st32(e + 4, 0);
    st16(e + 8, (uint16_t)c->sq[sqid].head);
    st16(e + 10, (uint16_t)sqid);
    st16(e + 12, cid);
    st16(e + 14, (uint16_t)((status << 1) | (q->phase & 1)));
    wd_dma_write(m, q->base + (uint64_t)q->tail * 16, e, 16);
    if (++q->tail == q->size) { q->tail = 0; q->phase ^= 1; }
}

/* ---------------------------------------------------------------- admin commands */

static uint16_t identify(wd_machine *m, nvme_ctrl *c, const uint8_t *cmd)
{
    uint8_t d[4096];
    memset(d, 0, sizeof d);
    uint32_t cns = ld32(cmd + 40) & 0xFF, nsid = ld32(cmd + 4);
    uint64_t blocks = c->disk.size >> LBA_SHIFT;
    switch (cns) {
    case 0:                                                       /* namespace */
        if (nsid != 1) return nsid == 0xFFFFFFFF ? SC_INVALID_NS : SC_INVALID_NS;
        st64(d + 0, blocks);
        st64(d + 8, blocks);
        st64(d + 16, blocks);
        d[24] = 0;                                                /* NSFEAT */
        d[25] = 0;                                                /* one LBA format */
        d[26] = 0;
        d[99] = c->disk.write ? 0 : 1;                            /* NSATTR: write protected */
        d[130] = (uint8_t)LBA_SHIFT;                              /* LBAF0: block size */
        memcpy(d + 104, "WINDDROIDNVMENS1", 16);                  /* NGUID */
        break;
    case 1: {                                                     /* controller */
        st16(d + 0, 0x1B36);
        st16(d + 2, 0x1AF4);
        pad_str(d + 4, c->serial, 20);
        pad_str(d + 24, "WindDroid NVMe Disk", 40);
        pad_str(d + 64, "1.0", 8);
        d[72] = 6;                                                /* RAB */
        d[77] = MDTS;
        st16(d + 78, 1);                                          /* CNTLID */
        st32(d + 80, 0x00010400);                                 /* version 1.4 */
        d[111] = 1;                                               /* I/O controller */
        d[258] = 3;                                               /* ACL */
        d[259] = 3;                                               /* AERL */
        d[260] = 0x03;                                            /* FRMW: slot 1 read-only, 1 slot */
        d[261] = 0x02;                                            /* LPA: command effects log */
        d[263] = 0;                                               /* NPSS: one power state */
        st16(d + 266, 0x0157);                                    /* WCTEMP 343 K */
        st16(d + 268, 0x0175);                                    /* CCTEMP 373 K */
        d[512] = 0x66;                                            /* SQES */
        d[513] = 0x44;                                            /* CQES */
        st16(d + 514, 0);
        st32(d + 516, 1);                                         /* NN */
        st16(d + 520, 0x000C);                                    /* ONCS: DSM, write zeroes */
        d[525] = 0x01;                                            /* VWC present */
        memcpy(d + 768, "nqn.2026-11.com.bxiidn:winddroid-nvme", 37);
        st16(d + 2048, 2500);                                     /* PSD0 max power: 25 W */
        break;
    }
    case 2:                                                       /* active namespace list */
        if (nsid < 1) st32(d, 1);
        break;
    case 3:                                                       /* namespace identification descriptors */
        if (nsid != 1) return SC_INVALID_NS;
        d[0] = 2; d[1] = 16;                                      /* NIDT 2: NGUID */
        memcpy(d + 4, "WINDDROIDNVMENS1", 16);
        break;
    default:
        return SC_INVALID_FLD;
    }
    return prp_xfer(m, ld64(cmd + 24), ld64(cmd + 32), d, sizeof d, 1) ? SC_INTERNAL : SC_OK;
}

static uint16_t admin(wd_machine *m, nvme_ctrl *c, const uint8_t *cmd, uint32_t *dw0, int *no_completion)
{
    uint8_t opc = cmd[0];
    uint32_t cdw10 = ld32(cmd + 40), cdw11 = ld32(cmd + 44);
    switch (opc) {
    case 0x06: return identify(m, c, cmd);
    case 0x05: {                                                  /* create I/O completion queue */
        uint16_t qid = (uint16_t)cdw10, qsize = (uint16_t)((cdw10 >> 16) + 1);
        if (!qid || qid >= NVME_MAXQ || c->cq[qid].valid) return SC_INVALID_QID;
        if (qsize < 2 || qsize > 2048) return SC_INVALID_QSIZE;
        if (!(cdw11 & 1)) return SC_INVALID_FLD;                  /* physically contiguous only */
        nvme_cq *q = &c->cq[qid];
        memset(q, 0, sizeof *q);
        q->base = ld64(cmd + 24) & ~(uint64_t)(PAGE - 1);
        q->size = qsize;
        q->phase = 1;
        q->ien = (cdw11 >> 1) & 1;
        q->iv = (uint16_t)(cdw11 >> 16);
        q->valid = 1;
        return SC_OK;
    }
    case 0x01: {                                                  /* create I/O submission queue */
        uint16_t qid = (uint16_t)cdw10, qsize = (uint16_t)((cdw10 >> 16) + 1), cqid = (uint16_t)(cdw11 >> 16);
        if (!qid || qid >= NVME_MAXQ || c->sq[qid].valid) return SC_INVALID_QID;
        if (qsize < 2 || qsize > 2048) return SC_INVALID_QSIZE;
        if (!cqid || cqid >= NVME_MAXQ || !c->cq[cqid].valid) return SC_INVALID_CQ;
        if (!(cdw11 & 1)) return SC_INVALID_FLD;
        nvme_sq *q = &c->sq[qid];
        memset(q, 0, sizeof *q);
        q->base = ld64(cmd + 24) & ~(uint64_t)(PAGE - 1);
        q->size = qsize;
        q->cqid = cqid;
        q->valid = 1;
        return SC_OK;
    }
    case 0x00: {                                                  /* delete I/O submission queue */
        uint16_t qid = (uint16_t)cdw10;
        if (!qid || qid >= NVME_MAXQ || !c->sq[qid].valid) return SC_INVALID_QID;
        c->sq[qid].valid = 0;
        return SC_OK;
    }
    case 0x04: {                                                  /* delete I/O completion queue */
        uint16_t qid = (uint16_t)cdw10;
        if (!qid || qid >= NVME_MAXQ || !c->cq[qid].valid) return SC_INVALID_QID;
        for (int i = 1; i < NVME_MAXQ; i++)
            if (c->sq[i].valid && c->sq[i].cqid == qid) return 0x10C;   /* invalid queue deletion */
        c->cq[qid].valid = 0;
        update_irq(m, c);
        return SC_OK;
    }
    case 0x09: case 0x0A: {                                       /* set / get features */
        uint8_t fid = (uint8_t)cdw10;
        if (fid == 0x07) {                                        /* number of queues */
            *dw0 = ((NVME_MAXQ - 2u) << 16) | (NVME_MAXQ - 2u);
            return SC_OK;
        }
        switch (fid) {
        case 0x01: case 0x02: case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0A: case 0x0B:
            if (opc == 0x09) c->features[fid] = cdw11;
            else *dw0 = c->features[fid];
            if (fid == 0x04 && opc == 0x0A && !c->features[fid]) *dw0 = 0x0157;
            return SC_OK;
        default:
            return SC_INVALID_FLD;
        }
    }
    case 0x02: {                                                  /* get log page: all zeroes */
        uint32_t numd = ((cdw10 >> 16) & 0xFFFF) | ((ld32(cmd + 44) & 0xFFFF) << 16);
        uint64_t len = ((uint64_t)numd + 1) * 4;
        if (len > 1 << 20) return SC_INVALID_FLD;
        uint8_t *z = calloc(1, (size_t)len);
        if (!z) return SC_INTERNAL;
        if ((cdw10 & 0xFF) == 0x02 && len >= 4) st16(z + 1, 0x0127);   /* SMART: composite temperature 295 K */
        int r = prp_xfer(m, ld64(cmd + 24), ld64(cmd + 32), z, len, 1);
        free(z);
        return r ? SC_INTERNAL : SC_OK;
    }
    case 0x0C:                                                    /* asynchronous event request: kept pending */
        *no_completion = 1;
        return SC_OK;
    case 0x08:                                                    /* abort: command not aborted */
        *dw0 = 1;
        return SC_OK;
    default:
        return SC_INVALID_OP;
    }
}

/* ---------------------------------------------------------------- I/O commands */

static uint16_t io(wd_machine *m, nvme_ctrl *c, const uint8_t *cmd)
{
    uint8_t opc = cmd[0];
    uint32_t nsid = ld32(cmd + 4);
    if (opc == 0x00) return c->disk.flush && c->disk.flush(c->disk.opaque) ? SC_INTERNAL : SC_OK;
    if (nsid != 1) return SC_INVALID_NS;
    uint64_t slba = ld64(cmd + 40);
    uint64_t nlb = (ld32(cmd + 48) & 0xFFFF) + 1ULL;
    uint64_t blocks = c->disk.size >> LBA_SHIFT;
    switch (opc) {
    case 0x01: case 0x02: {                                       /* write / read */
        if (slba + nlb > blocks || slba + nlb < slba) return SC_LBA_RANGE;
        uint64_t len = nlb << LBA_SHIFT;
        if (len > (PAGE << MDTS)) return SC_INVALID_FLD;
        if (opc == 0x02) {
            if (c->disk.read(c->disk.opaque, slba << LBA_SHIFT, c->bounce, len)) return SC_INTERNAL;
            if (prp_xfer(m, ld64(cmd + 24), ld64(cmd + 32), c->bounce, len, 1)) return SC_INTERNAL;
        } else {
            if (!c->disk.write) return SC_WRITE_PROT;
            if (prp_xfer(m, ld64(cmd + 24), ld64(cmd + 32), c->bounce, len, 0)) return SC_INTERNAL;
            if (c->disk.write(c->disk.opaque, slba << LBA_SHIFT, c->bounce, len)) return SC_INTERNAL;
        }
        return SC_OK;
    }
    case 0x08: {                                                  /* write zeroes */
        if (slba + nlb > blocks) return SC_LBA_RANGE;
        if (!c->disk.write) return SC_WRITE_PROT;
        memset(c->bounce, 0, PAGE << MDTS);
        uint64_t off = slba << LBA_SHIFT, left = nlb << LBA_SHIFT;
        while (left) {
            uint64_t n = left < (PAGE << MDTS) ? left : (PAGE << MDTS);
            if (c->disk.write(c->disk.opaque, off, c->bounce, n)) return SC_INTERNAL;
            off += n; left -= n;
        }
        return SC_OK;
    }
    case 0x09:                                                    /* dataset management (deallocate): no-op */
        return SC_OK;
    default:
        return SC_INVALID_OP;
    }
}

static void process_sq(wd_machine *m, nvme_ctrl *c, int qid)
{
    nvme_sq *q = &c->sq[qid];
    int guard = 0;
    while (q->valid && q->head != q->tail && guard++ < 4096) {
        uint8_t cmd[64];
        if (wd_dma_read(m, q->base + (uint64_t)q->head * 64, cmd, 64)) { c->csts |= 2; break; }  /* fatal */
        if (++q->head == q->size) q->head = 0;
        uint16_t cid = (uint16_t)(cmd[2] | cmd[3] << 8);
        uint32_t dw0 = 0;
        int none = 0;
        uint16_t st = qid == 0 ? admin(m, c, cmd, &dw0, &none) : io(m, c, cmd);
        if (!none) complete(m, c, qid, cid, dw0, st);
    }
    update_irq(m, c);
}

/* ---------------------------------------------------------------- registers */

static void reset_ctrl(wd_machine *m, nvme_ctrl *c)
{
    memset(c->sq, 0, sizeof c->sq);
    memset(c->cq, 0, sizeof c->cq);
    c->csts = 0;
    c->intms = 0;
    update_irq(m, c);
}

static uint64_t cap_value(void)
{
    return 2047ULL                       /* MQES */
         | (1ULL << 16)                  /* contiguous queues required */
         | (0x0FULL << 24)               /* timeout 7.5 s */
         | (1ULL << 37);                 /* NVM command set */
}

static uint64_t reg_read(wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size)
{
    (void)m; (void)bar;
    nvme_ctrl *c = d->opaque;
    uint64_t v;
    switch (off & ~3ULL) {
    case 0x00: v = cap_value(); break;
    case 0x04: v = cap_value() >> 32; break;
    case 0x08: v = 0x00010400; break;
    case 0x0C: case 0x10: v = c->intms; break;
    case 0x14: v = c->cc; break;
    case 0x1C: v = c->csts; break;
    case 0x24: v = c->aqa; break;
    case 0x28: v = c->asq; break;
    case 0x2C: v = c->asq >> 32; break;
    case 0x30: v = c->acq; break;
    case 0x34: v = c->acq >> 32; break;
    default: v = 0; break;
    }
    if (size == 8) {
        if (off == 0x00) return cap_value();
        if (off == 0x28) return c->asq;
        if (off == 0x30) return c->acq;
        return v | (reg_read(m, d, bar, off + 4, 4) << 32);
    }
    return v & wd_mask(8 * size);
}

static void reg_write(wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size, uint64_t val)
{
    nvme_ctrl *c = d->opaque;
    if (size == 8 && off < 0x1000) {
        reg_write(m, d, bar, off, 4, (uint32_t)val);
        reg_write(m, d, bar, off + 4, 4, val >> 32);
        return;
    }
    uint32_t v = (uint32_t)val;
    if (off >= 0x1000) {                                         /* doorbells */
        uint32_t idx = (uint32_t)(off - 0x1000) / 4, qid = idx / 2;
        if (qid >= NVME_MAXQ || !(c->csts & 1)) return;
        if (!(idx & 1)) {
            nvme_sq *q = &c->sq[qid];
            if (!q->valid || v >= q->size) return;
            q->tail = v;
            process_sq(m, c, (int)qid);
        } else {
            nvme_cq *q = &c->cq[qid];
            if (!q->valid || v >= q->size) return;
            q->head = v;
            update_irq(m, c);
        }
        return;
    }
    switch (off) {
    case 0x0C: c->intms |= v; update_irq(m, c); break;
    case 0x10: c->intms &= ~v; update_irq(m, c); break;
    case 0x14: {
        uint32_t old = c->cc;
        c->cc = v;
        if ((v & 1) && !(old & 1)) {                             /* enable */
            nvme_sq *sq = &c->sq[0];
            nvme_cq *cq = &c->cq[0];
            memset(sq, 0, sizeof *sq);
            memset(cq, 0, sizeof *cq);
            sq->base = c->asq & ~0xFFFULL;
            sq->size = (c->aqa & 0xFFF) + 1;
            sq->valid = 1;
            cq->base = c->acq & ~0xFFFULL;
            cq->size = ((c->aqa >> 16) & 0xFFF) + 1;
            cq->phase = 1;
            cq->ien = 1;
            cq->valid = 1;
            c->csts = 1;
        } else if (!(v & 1) && (old & 1)) {
            reset_ctrl(m, c);
        }
        if ((v >> 14) & 3) c->csts = (c->csts & ~0xCu) | (2u << 2);   /* shutdown complete */
        break;
    }
    case 0x24: c->aqa = v; break;
    case 0x28: c->asq = (c->asq & 0xFFFFFFFF00000000ULL) | v; break;
    case 0x2C: c->asq = (c->asq & 0xFFFFFFFFULL) | (uint64_t)v << 32; break;
    case 0x30: c->acq = (c->acq & 0xFFFFFFFF00000000ULL) | v; break;
    case 0x34: c->acq = (c->acq & 0xFFFFFFFFULL) | (uint64_t)v << 32; break;
    default: break;
    }
}

/* Add the PCI Express and power management capabilities devices on the root complex carry. */
void wd_pci_add_std_caps(wd_pcidev *d)
{
    int pm = wd_pci_add_cap(d, 0x01, 8);
    d->cfg[pm + 2] = 0x03; d->cfg[pm + 3] = 0x00;              /* PMC: version 3 */
    d->wmask[pm + 4] = 0x03;                                   /* PMCSR power state */
    int px = wd_pci_add_cap(d, 0x10, 60);
    d->cfg[px + 2] = 0x92;                                     /* version 2, root complex integrated endpoint */
    d->cfg[px + 3] = 0x00;
    st32(d->cfg + px + 4, 0x00008000 | 0x1);                   /* DevCap: role-based error reporting, 256-byte payload */
    st16(d->wmask + px + 8, 0xFFFF);                           /* DevCtl */
}

static void nvme_free(wd_pcidev *d)
{
    nvme_ctrl *c = d->opaque;
    if (c->disk.close) c->disk.close(c->disk.opaque);
    free(c->bounce);
}

int wd_attach_nvme(wd_machine *m, const wd_disk *disk)
{
    int slot = wd_pci_free_slot(m);
    if (m->npci >= WD_MAX_PCI || slot < 0) { snprintf(m->err, sizeof m->err, "no free PCI slot"); return -1; }
    if (!disk || disk->size < 512 || !disk->read) { snprintf(m->err, sizeof m->err, "invalid disk"); return -1; }
    if (disk->block_size && disk->block_size != 512 && disk->block_size != 4096) {
        snprintf(m->err, sizeof m->err, "NVMe needs 512 or 4096-byte blocks (CD images go on the USB CD drive)");
        return -1;
    }
    nvme_ctrl *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->bounce = malloc(PAGE << MDTS);
    if (!c->bounce) { free(c); return -1; }
    c->disk = *disk;
    c->lba_shift = disk->block_size == 4096 ? 12 : 9;
    snprintf(c->serial, sizeof c->serial, "WD%016llX", (unsigned long long)(disk->size ^ 0x5744524F4944ULL));
    wd_pci_init_dev(&c->pci, slot << 3, 0x1B36, 0x0010, 0x01080202, 0x1AF4, 0x1100);
    wd_pci_set_bar(&c->pci, 0, 0x4000, WD_BAR_MEM64);
    c->pci.cfg[0x3D] = 1;                                     /* INTA */
    wd_pci_add_std_caps(&c->pci);
    c->pci.opaque = c;
    c->pci.bar_read = reg_read;
    c->pci.bar_write = reg_write;
    c->pci.on_free = nvme_free;
    wd_pci_add(m, &c->pci);
    return 0;
}
