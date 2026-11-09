/*
 * WindDroid - USB 3 eXtensible Host Controller (xHCI 1.1) on PCIe.
 *
 * 8 high-speed root ports, 8 device slots, one interrupter with INTx.
 * Transfers are executed synchronously when the doorbell rings; IN
 * endpoints without data (HID devices) stay pending until the device
 * calls wd_xhci_wakeup(). EDK2's XhciDxe and Windows' usbxhci drive it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "usb.h"

#define NPORTS   8
#define NSLOTS   8
#define NINTRS   1

#define CAP_LEN  0x40
#define OP_BASE  CAP_LEN
#define PORT_BASE (OP_BASE + 0x400)
#define RT_BASE  0x1000
#define DB_BASE  0x2000
#define XECP     0x3000           /* extended capabilities */
#define BAR_SIZE 0x4000
#define TD_MAX   (4u << 20)

/* TRB types */
enum {
    TR_NORMAL = 1, TR_SETUP, TR_DATA, TR_STATUS, TR_ISOCH, TR_LINK, TR_EVDATA, TR_NOOP,
    CR_ENABLE_SLOT, CR_DISABLE_SLOT, CR_ADDRESS_DEVICE, CR_CONFIGURE_EP, CR_EVALUATE_CTX,
    CR_RESET_EP, CR_STOP_EP, CR_SET_TR_DEQ, CR_RESET_DEVICE, CR_FORCE_EVENT, CR_NEGOTIATE_BW,
    CR_SET_LT, CR_GET_PORT_BW, CR_FORCE_HEADER, CR_NOOP,
    ER_TRANSFER = 32, ER_COMMAND_COMPLETE, ER_PORT_STATUS_CHANGE, ER_BANDWIDTH_REQUEST,
    ER_DOORBELL, ER_HOST_CONTROLLER, ER_DEVICE_NOTIFICATION, ER_MFINDEX_WRAP,
};

/* completion codes */
enum {
    CC_SUCCESS = 1, CC_DATA_BUFFER = 2, CC_BABBLE = 3, CC_TRANSACTION = 4, CC_TRB_ERROR = 5, CC_STALL = 6,
    CC_RESOURCE = 7, CC_NO_SLOTS = 9, CC_SLOT_NOT_ENABLED = 11, CC_EP_NOT_ENABLED = 12, CC_SHORT_PACKET = 13,
    CC_PARAMETER = 17, CC_CONTEXT_STATE = 19, CC_EVENT_RING_FULL = 21, CC_STOPPED = 26,
};

enum { EP_DISABLED = 0, EP_RUNNING, EP_HALTED, EP_STOPPED, EP_ERROR };

typedef struct { uint64_t addr, param; uint32_t status, control; } trb;

typedef struct {
    int state;
    int type;                    /* EP type field 1..7 */
    uint64_t deq;                /* transfer ring dequeue pointer */
    int ccs;
    uint32_t mps;
} xhci_ep;

typedef struct {
    int enabled, addressed;
    int port;                    /* 1-based root port */
    uint64_t ctx;                /* output device context */
    xhci_ep ep[32];              /* by DCI */
} xhci_slot;

typedef struct {
    uint32_t portsc;
    wd_usbdev *dev;
} xhci_port;

typedef struct {
    wd_pcidev pci;
    wd_machine *m;
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t crcr, dcbaap;
    uint64_t cmd_deq;
    int cmd_ccs, cmd_running;
    /* interrupter 0 */
    uint32_t iman, imod, erstsz;
    uint64_t erstba, erdp;
    uint64_t er_seg_base;
    uint32_t er_seg_size, er_seg, er_idx;
    int er_pcs;
    int er_full;
    xhci_port port[NPORTS];
    xhci_slot slot[NSLOTS + 1];
    uint8_t *buf;                /* one TD */
    uint64_t mfindex_base;
} xhci;

static uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int read_trb(xhci *x, uint64_t addr, trb *t)
{
    uint8_t b[16];
    if (wd_dma_read(x->m, addr, b, 16)) return -1;
    t->addr = addr;
    t->param = rd32le(b) | (uint64_t)rd32le(b + 4) << 32;
    t->status = rd32le(b + 8);
    t->control = rd32le(b + 12);
    return 0;
}

#define TRB_TYPE(t)  (((t).control >> 10) & 0x3F)
#define TRB_CYCLE    (1u << 0)
#define TRB_TC       (1u << 1)
#define TRB_ISP      (1u << 2)
#define TRB_CH       (1u << 4)
#define TRB_IOC      (1u << 5)
#define TRB_IDT      (1u << 6)

/* ---------------------------------------------------------------- interrupts and events */

static void update_irq(xhci *x)
{
    int level = (x->iman & 1) && (x->iman & 2) && (x->usbcmd & 4);
    wd_pci_set_intx(x->m, &x->pci, level);
}

static void event(xhci *x, uint64_t param, uint32_t status, uint32_t control)
{
    if (!x->erstsz || x->er_full || (x->usbsts & 1)) return;
    /* ring full when the next slot is the one the guest has not consumed yet */
    uint64_t cur = x->er_seg_base + (uint64_t)x->er_idx * 16;
    uint32_t nseg = x->er_seg, nidx = x->er_idx + 1;
    if (nidx >= x->er_seg_size) { nidx = 0; nseg = (nseg + 1) % x->erstsz; }
    uint8_t e[16];
    uint64_t next;
    {
        uint8_t ent[16];
        if (nidx == 0) {
            if (wd_dma_read(x->m, x->erstba + (uint64_t)nseg * 16, ent, 16)) return;
            next = (rd32le(ent) | (uint64_t)rd32le(ent + 4) << 32) & ~63ULL;
        } else {
            next = x->er_seg_base + (uint64_t)nidx * 16;
        }
    }
    if (next == (x->erdp & ~15ULL)) {
        /* last free slot: report Event Ring Full instead */
        wr32le(e, 0); wr32le(e + 4, 0);
        wr32le(e + 8, (uint32_t)CC_EVENT_RING_FULL << 24);
        wr32le(e + 12, (uint32_t)ER_HOST_CONTROLLER << 10 | (uint32_t)x->er_pcs);
        x->er_full = 1;
    } else {
        wr32le(e, (uint32_t)param);
        wr32le(e + 4, (uint32_t)(param >> 32));
        wr32le(e + 8, status);
        wr32le(e + 12, (control & ~1u) | (uint32_t)x->er_pcs);
    }
    wd_dma_write(x->m, cur, e, 16);
    x->er_idx++;
    if (x->er_idx >= x->er_seg_size) {
        x->er_idx = 0;
        x->er_seg++;
        if (x->er_seg >= x->erstsz) { x->er_seg = 0; x->er_pcs ^= 1; }
        uint8_t ent[16];
        if (!wd_dma_read(x->m, x->erstba + (uint64_t)x->er_seg * 16, ent, 16)) {
            x->er_seg_base = (rd32le(ent) | (uint64_t)rd32le(ent + 4) << 32) & ~63ULL;
            x->er_seg_size = rd32le(ent + 8) & 0xFFFF;
        }
    }
    x->erdp |= 8;                                    /* event handler busy */
    x->iman |= 1;                                    /* interrupt pending */
    x->usbsts |= 8;                                  /* EINT */
    update_irq(x);
}

static void er_setup(xhci *x)
{
    x->er_seg = 0;
    x->er_idx = 0;
    x->er_pcs = 1;
    x->er_full = 0;
    uint8_t ent[16];
    if (x->erstsz && !wd_dma_read(x->m, x->erstba, ent, 16)) {
        x->er_seg_base = (rd32le(ent) | (uint64_t)rd32le(ent + 4) << 32) & ~63ULL;
        x->er_seg_size = rd32le(ent + 8) & 0xFFFF;
    }
}

static void port_event(xhci *x, int port)
{
    event(x, (uint64_t)port << 24, (uint32_t)CC_SUCCESS << 24, (uint32_t)ER_PORT_STATUS_CHANGE << 10);
}

/* ---------------------------------------------------------------- ports */

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_LWS  (1u << 16)
#define PORTSC_CSC  (1u << 17)
#define PORTSC_PEC  (1u << 18)
#define PORTSC_WRC  (1u << 19)
#define PORTSC_OCC  (1u << 20)
#define PORTSC_PRC  (1u << 21)
#define PORTSC_PLC  (1u << 22)
#define PORTSC_CEC  (1u << 23)
#define PORTSC_CHANGE (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | PORTSC_CEC)

static void port_connect(xhci *x, int i)
{
    xhci_port *p = &x->port[i];
    p->portsc = PORTSC_PP | (7u << 5);               /* powered, link state RxDetect... */
    if (p->dev) {
        p->portsc = PORTSC_PP | PORTSC_CCS | PORTSC_CSC | (7u << 5) | ((uint32_t)p->dev->speed << 10);
        p->portsc = (p->portsc & ~(0xFu << 5)) | (7u << 5);   /* USB2: Polling until reset */
    } else {
        p->portsc = PORTSC_PP | (5u << 5);           /* RxDetect */
    }
}

static void port_reset(xhci *x, int i)
{
    xhci_port *p = &x->port[i];
    if (!p->dev) return;
    if (p->dev->reset) p->dev->reset(p->dev);
    p->portsc &= ~(PORTSC_PR | (0xFu << 5));
    p->portsc |= PORTSC_PED | PORTSC_PRC;            /* enabled, U0 */
    port_event(x, i + 1);
}

/* ---------------------------------------------------------------- contexts */

static uint64_t dev_ctx_addr(xhci *x, int slot)
{
    uint64_t a = 0;
    wd_dma_read(x->m, x->dcbaap + 8ULL * (unsigned)slot, &a, 8);
    return a;
}

static void write_ep_state(xhci *x, int slotid, int dci)
{
    xhci_slot *s = &x->slot[slotid];
    xhci_ep *e = &s->ep[dci];
    uint8_t c[32];
    if (!s->ctx || wd_dma_read(x->m, s->ctx + 32ULL * (unsigned)dci, c, 32)) return;
    c[0] = (uint8_t)((c[0] & ~7) | e->state);
    uint64_t dq = (e->deq & ~15ULL) | (uint64_t)e->ccs;
    wr32le(c + 8, (uint32_t)dq);
    wr32le(c + 12, (uint32_t)(dq >> 32));
    wd_dma_write(x->m, s->ctx + 32ULL * (unsigned)dci, c, 32);
}

static void write_slot_state(xhci *x, int slotid, int state, int addr)
{
    xhci_slot *s = &x->slot[slotid];
    uint8_t c[32];
    if (!s->ctx || wd_dma_read(x->m, s->ctx, c, 32)) return;
    uint32_t dw3 = rd32le(c + 12);
    dw3 = (dw3 & ~(0x1Fu << 27) & ~0xFFu) | (uint32_t)state << 27 | (uint32_t)addr;
    wr32le(c + 12, dw3);
    wd_dma_write(x->m, s->ctx, c, 32);
}

static void load_ep(xhci *x, xhci_ep *e, const uint8_t *c)
{
    (void)x;
    uint32_t dw1 = rd32le(c + 4);
    uint64_t dq = rd32le(c + 8) | (uint64_t)rd32le(c + 12) << 32;
    e->type = (int)((dw1 >> 3) & 7);
    e->mps = dw1 >> 16;
    e->deq = dq & ~15ULL;
    e->ccs = (int)(dq & 1);
    e->state = EP_RUNNING;
}

/* ---------------------------------------------------------------- transfers */

static wd_usbdev *slot_dev(xhci *x, int slotid)
{
    xhci_slot *s = &x->slot[slotid];
    if (!s->enabled || s->port < 1 || s->port > NPORTS) return NULL;
    return x->port[s->port - 1].dev;
}

/* Collect the TRBs of the next TD (until a TRB without CH, or the status stage for control).
 * Returns the count, 0 if the ring holds no complete TD, -1 on a ring error. */
static int gather_td(xhci *x, xhci_ep *e, trb *out, int max, uint64_t *next_deq, int *next_ccs, int control)
{
    uint64_t p = e->deq;
    int ccs = e->ccs, n = 0;
    for (int guard = 0; guard < 4096; guard++) {
        trb t;
        if (read_trb(x, p, &t)) return -1;
        if ((int)(t.control & TRB_CYCLE) != ccs) return 0;
        if (TRB_TYPE(t) == TR_LINK) {
            if (t.control & TRB_TC) ccs ^= 1;
            p = t.param & ~15ULL;
            continue;
        }
        if (n == max) return -1;
        out[n++] = t;
        p += 16;
        int end = control ? TRB_TYPE(t) == TR_STATUS : !(t.control & TRB_CH);
        if (end) {
            *next_deq = p;
            *next_ccs = ccs;
            return n;
        }
    }
    return -1;
}

/* Report a finished TD the way real controllers do (one event per IOC, short packet on ISP). */
static void report(xhci *x, int slotid, int dci, trb *t, int n, uint32_t actual, int cc)
{
    uint32_t left = actual, edtla = 0;
    int reported = 0, shortpkt = 0;
    for (int i = 0; i < n; i++) {
        uint32_t chunk = 0;
        int type = TRB_TYPE(t[i]);
        if (type == TR_SETUP) chunk = 8 < (t[i].status & 0x1FFFF) ? 8 : (t[i].status & 0x1FFFF);
        else if (type == TR_DATA || type == TR_NORMAL || type == TR_ISOCH) {
            chunk = t[i].status & 0x1FFFF;
            if (chunk > left) { chunk = left; if (cc == CC_SUCCESS) shortpkt = 1; }
            left -= chunk;
            edtla += chunk;
        } else if (type == TR_STATUS) {
            reported = 0;
            shortpkt = 0;
        }
        if (!reported && ((t[i].control & TRB_IOC) || (shortpkt && (t[i].control & TRB_ISP)) ||
                          (cc != CC_SUCCESS && left == 0))) {
            uint64_t ptr = t[i].addr;
            uint32_t len = (t[i].status & 0x1FFFF) - chunk;
            uint32_t flags = 0;
            int code = cc == CC_SUCCESS ? (shortpkt ? CC_SHORT_PACKET : CC_SUCCESS) : cc;
            if (type == TR_EVDATA) {
                ptr = t[i].param;
                flags = 1u << 2;
                len = edtla & 0xFFFFFF;
                edtla = 0;
            }
            if (type == TR_SETUP || type == TR_STATUS) len = 0;
            event(x, ptr, (uint32_t)code << 24 | (len & 0xFFFFFF),
                  (uint32_t)slotid << 24 | (uint32_t)dci << 16 | (uint32_t)ER_TRANSFER << 10 | flags);
            reported = 1;
            if (cc != CC_SUCCESS) return;
        }
        if (type == TR_SETUP) { reported = 0; shortpkt = 0; }
    }
}

/* data buffers of a TD to/from x->buf */
static int td_buffer(xhci *x, trb *t, int n, int to_guest, uint32_t total)
{
    uint32_t off = 0;
    for (int i = 0; i < n && off < total; i++) {
        int type = TRB_TYPE(t[i]);
        if (type != TR_DATA && type != TR_NORMAL && type != TR_ISOCH) continue;
        uint32_t len = t[i].status & 0x1FFFF;
        if (len > total - off) len = total - off;
        if (t[i].control & TRB_IDT) {
            if (to_guest) return -1;
            memcpy(x->buf + off, &t[i].param, len > 8 ? 8 : len);
        } else if (len) {
            if (to_guest ? wd_dma_write(x->m, t[i].param, x->buf + off, len)
                         : wd_dma_read(x->m, t[i].param, x->buf + off, len)) return -1;
        }
        off += len;
    }
    return 0;
}

static uint32_t td_length(trb *t, int n)
{
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        int type = TRB_TYPE(t[i]);
        if (type == TR_DATA || type == TR_NORMAL || type == TR_ISOCH) total += t[i].status & 0x1FFFF;
    }
    return total;
}

static void process_ep(xhci *x, int slotid, int dci)
{
    xhci_slot *s = &x->slot[slotid];
    xhci_ep *e = &s->ep[dci];
    wd_usbdev *dev = slot_dev(x, slotid);
    if (!dev) return;
    trb td[64];
    for (int guard = 0; guard < 256 && e->state == EP_RUNNING; guard++) {
        uint64_t ndeq;
        int nccs;
        int control = dci == 1;
        int n = gather_td(x, e, td, 64, &ndeq, &nccs, control);
        if (n <= 0) {
            if (n < 0) { e->state = EP_ERROR; write_ep_state(x, slotid, dci); }
            return;
        }
        uint32_t total = td_length(td, n);
        if (total > TD_MAX) {
            e->deq = ndeq; e->ccs = nccs;
            report(x, slotid, dci, td, n, 0, CC_TRB_ERROR);
            continue;
        }
        int cc = CC_SUCCESS;
        uint32_t actual = 0;
        if (control) {
            if (TRB_TYPE(td[0]) != TR_SETUP || !(td[0].control & TRB_IDT)) {
                e->deq = ndeq; e->ccs = nccs;
                report(x, slotid, dci, td, n, 0, CC_TRB_ERROR);
                continue;
            }
            uint8_t setup[8];
            memcpy(setup, &td[0].param, 8);
            int in = setup[0] & 0x80;
            uint32_t wlen = (uint32_t)(setup[6] | setup[7] << 8);
            if (wlen > total) wlen = total;
            memset(x->buf, 0, wlen);
            if (!in && wlen && td_buffer(x, td, n, 0, wlen)) cc = CC_DATA_BUFFER;
            int r = cc == CC_SUCCESS ? wd_usb_control(dev, setup, x->buf) : 0;
            if (r == USB_RET_STALL) cc = CC_STALL;
            else if (in) {
                actual = (uint32_t)r;
                if (actual > wlen) actual = wlen;
                if (td_buffer(x, td, n, 1, actual)) cc = CC_DATA_BUFFER;
            } else {
                actual = wlen;
            }
        } else {
            int in = dci & 1;
            int epn = dci / 2;
            if (!in && td_buffer(x, td, n, 0, total)) cc = CC_DATA_BUFFER;
            int r = cc == CC_SUCCESS ? dev->data(dev, epn, in, x->buf, (int)total) : 0;
            if (r == USB_RET_NAK) return;                 /* stays pending */
            if (r == USB_RET_STALL) cc = CC_STALL;
            else {
                actual = (uint32_t)r;
                if (in && td_buffer(x, td, n, 1, actual)) cc = CC_DATA_BUFFER;
            }
        }
        e->deq = ndeq;
        e->ccs = nccs;
        if (cc == CC_STALL) {
            e->state = EP_HALTED;
            write_ep_state(x, slotid, dci);
            report(x, slotid, dci, td, n, actual, cc);
            return;
        }
        report(x, slotid, dci, td, n, actual, cc);
        if (!control && !(dci & 1)) {
            /* an OUT transfer (a mass storage command) can make IN data available */
            for (int i = 3; i < 32; i += 2)
                if (s->ep[i].state == EP_RUNNING && i != dci) process_ep(x, slotid, i);
        }
    }
}

void wd_xhci_wakeup(wd_usbdev *d, int ep)
{
    xhci *x = d->hc;
    if (!x || !(x->usbcmd & 1)) return;
    for (int sl = 1; sl <= NSLOTS; sl++)
        if (x->slot[sl].enabled && x->slot[sl].port == d->port && x->slot[sl].ep[ep * 2 + 1].state == EP_RUNNING)
            process_ep(x, sl, ep * 2 + 1);
}

/* ---------------------------------------------------------------- commands */

static void cmd_complete(xhci *x, uint64_t ptr, int cc, int slotid)
{
    event(x, ptr, (uint32_t)cc << 24, (uint32_t)slotid << 24 | (uint32_t)ER_COMMAND_COMPLETE << 10);
}

static int find_port_by_ctx(xhci *x, const uint8_t *slotctx)
{
    (void)x;
    return (int)((rd32le(slotctx + 4) >> 16) & 0xFF);
}

static int cmd_address(xhci *x, int slotid, uint64_t inctx, int bsr)
{
    xhci_slot *s = &x->slot[slotid];
    uint8_t ic[32 * 3];
    if (wd_dma_read(x->m, inctx, ic, sizeof ic)) return CC_TRB_ERROR;
    uint32_t add = rd32le(ic + 4);
    if ((add & 3) != 3) return CC_PARAMETER;
    int port = find_port_by_ctx(x, ic + 32);
    if (port < 1 || port > NPORTS || !x->port[port - 1].dev) return CC_TRB_ERROR;
    s->ctx = dev_ctx_addr(x, slotid);
    if (!s->ctx) return CC_TRB_ERROR;
    s->port = port;
    x->port[port - 1].dev->port = port;
    /* output slot + EP0 contexts */
    uint8_t out[64];
    memcpy(out, ic + 32, 64);
    uint32_t dw3 = (uint32_t)(bsr ? 1 : 2) << 27 | (uint32_t)(bsr ? 0 : slotid);
    wr32le(out + 12, dw3);
    out[32] = (uint8_t)((out[32] & ~7) | EP_RUNNING);
    if (wd_dma_write(x->m, s->ctx, out, 64)) return CC_TRB_ERROR;
    memset(s->ep, 0, sizeof s->ep);
    load_ep(x, &s->ep[1], ic + 64);
    s->addressed = !bsr;
    return CC_SUCCESS;
}

static int cmd_configure(xhci *x, int slotid, uint64_t inctx, int dc)
{
    xhci_slot *s = &x->slot[slotid];
    if (!s->ctx) return CC_CONTEXT_STATE;
    if (dc) {
        for (int i = 2; i < 32; i++) {
            s->ep[i].state = EP_DISABLED;
            uint8_t z[32] = { 0 };
            wd_dma_write(x->m, s->ctx + 32ULL * (unsigned)i, z, 32);
        }
        write_slot_state(x, slotid, 2, slotid);
        return CC_SUCCESS;
    }
    uint8_t ic[33 * 32];
    if (wd_dma_read(x->m, inctx, ic, sizeof ic)) return CC_TRB_ERROR;
    uint32_t drop = rd32le(ic), add = rd32le(ic + 4);
    if ((drop & 3) || (add & 2)) return CC_TRB_ERROR;
    for (int i = 2; i < 32; i++) {
        if (drop & (1u << i)) {
            s->ep[i].state = EP_DISABLED;
            uint8_t z[32] = { 0 };
            wd_dma_write(x->m, s->ctx + 32ULL * (unsigned)i, z, 32);
        }
    }
    for (int i = 2; i < 32; i++) {
        if (!(add & (1u << i))) continue;
        uint8_t *c = ic + 32 * (i + 1);
        load_ep(x, &s->ep[i], c);
        uint8_t oc[32];
        memcpy(oc, c, 32);
        oc[0] = (uint8_t)((oc[0] & ~7) | EP_RUNNING);
        wd_dma_write(x->m, s->ctx + 32ULL * (unsigned)i, oc, 32);
    }
    /* slot context: context entries from the input, state configured */
    uint8_t sc[32];
    if (wd_dma_read(x->m, s->ctx, sc, 32)) return CC_TRB_ERROR;
    if (add & 1) {
        uint32_t dw0 = rd32le(ic + 32);
        wr32le(sc, (rd32le(sc) & 0x07FFFFFFu) | (dw0 & 0xF8000000u));
    }
    uint32_t dw3 = (rd32le(sc + 12) & ~(0x1Fu << 27)) | 3u << 27;
    wr32le(sc + 12, dw3);
    wd_dma_write(x->m, s->ctx, sc, 32);
    return CC_SUCCESS;
}

static int cmd_evaluate(xhci *x, int slotid, uint64_t inctx)
{
    xhci_slot *s = &x->slot[slotid];
    if (!s->ctx) return CC_CONTEXT_STATE;
    uint8_t ic[32 * 3];
    if (wd_dma_read(x->m, inctx, ic, sizeof ic)) return CC_TRB_ERROR;
    uint32_t add = rd32le(ic + 4);
    if (add & 1) {
        uint8_t sc[32];
        if (wd_dma_read(x->m, s->ctx, sc, 32)) return CC_TRB_ERROR;
        wr32le(sc + 4, (rd32le(sc + 4) & 0xFFFF0000u) | (rd32le(ic + 36) & 0xFFFF));   /* max exit latency */
        wr32le(sc + 8, (rd32le(sc + 8) & 0x003FFFFFu) | (rd32le(ic + 40) & 0xFFC00000u)); /* interrupter */
        wd_dma_write(x->m, s->ctx, sc, 32);
    }
    if (add & 2) {
        uint8_t ec[32];
        if (wd_dma_read(x->m, s->ctx + 32, ec, 32)) return CC_TRB_ERROR;
        wr32le(ec + 4, (rd32le(ec + 4) & 0xFFFF) | (rd32le(ic + 68) & 0xFFFF0000u));      /* max packet size */
        wd_dma_write(x->m, s->ctx + 32, ec, 32);
        s->ep[1].mps = rd32le(ic + 68) >> 16;
    }
    return CC_SUCCESS;
}

static void stop_pending(xhci *x, int slotid, int dci)
{
    /* a TD waiting for data (interrupt IN) is reported as stopped */
    xhci_ep *e = &x->slot[slotid].ep[dci];
    trb t;
    if (read_trb(x, e->deq, &t) == 0 && (int)(t.control & TRB_CYCLE) == e->ccs && TRB_TYPE(t) != TR_LINK &&
        dci != 1 && (dci & 1)) {
        event(x, t.addr, (uint32_t)CC_STOPPED << 24 | (t.status & 0x1FFFF),
              (uint32_t)slotid << 24 | (uint32_t)dci << 16 | (uint32_t)ER_TRANSFER << 10);
    }
}

static void process_commands(xhci *x)
{
    for (int guard = 0; guard < 256; guard++) {
        trb t;
        if (read_trb(x, x->cmd_deq, &t)) return;
        if ((int)(t.control & TRB_CYCLE) != x->cmd_ccs) return;
        int type = TRB_TYPE(t);
        if (type == TR_LINK) {
            if (t.control & TRB_TC) x->cmd_ccs ^= 1;
            x->cmd_deq = t.param & ~15ULL;
            continue;
        }
        int slotid = (int)(t.control >> 24);
        int cc = CC_SUCCESS, retslot = slotid;
        int valid_slot = slotid >= 1 && slotid <= NSLOTS && x->slot[slotid].enabled;
        switch (type) {
        case CR_ENABLE_SLOT:
            retslot = 0;
            cc = CC_NO_SLOTS;
            for (int i = 1; i <= NSLOTS && (int)(x->config & 0xFF) >= i; i++)
                if (!x->slot[i].enabled) {
                    memset(&x->slot[i], 0, sizeof x->slot[i]);
                    x->slot[i].enabled = 1;
                    retslot = i;
                    cc = CC_SUCCESS;
                    break;
                }
            break;
        case CR_DISABLE_SLOT:
            if (!valid_slot) { cc = CC_SLOT_NOT_ENABLED; break; }
            write_slot_state(x, slotid, 0, 0);
            memset(&x->slot[slotid], 0, sizeof x->slot[slotid]);
            break;
        case CR_ADDRESS_DEVICE:
            cc = valid_slot ? cmd_address(x, slotid, t.param & ~15ULL, (t.control >> 9) & 1) : CC_SLOT_NOT_ENABLED;
            break;
        case CR_CONFIGURE_EP:
            cc = valid_slot ? cmd_configure(x, slotid, t.param & ~15ULL, (t.control >> 9) & 1) : CC_SLOT_NOT_ENABLED;
            break;
        case CR_EVALUATE_CTX:
            cc = valid_slot ? cmd_evaluate(x, slotid, t.param & ~15ULL) : CC_SLOT_NOT_ENABLED;
            break;
        case CR_RESET_EP: {
            int dci = (int)((t.control >> 16) & 0x1F);
            if (!valid_slot) { cc = CC_SLOT_NOT_ENABLED; break; }
            xhci_ep *e = &x->slot[slotid].ep[dci];
            if (dci < 1 || e->state != EP_HALTED) { cc = CC_CONTEXT_STATE; break; }
            e->state = EP_STOPPED;
            write_ep_state(x, slotid, dci);
            /* clear the device-side halt as well (the guest also sends CLEAR_FEATURE) */
            break;
        }
        case CR_STOP_EP: {
            int dci = (int)((t.control >> 16) & 0x1F);
            if (!valid_slot) { cc = CC_SLOT_NOT_ENABLED; break; }
            xhci_ep *e = &x->slot[slotid].ep[dci];
            if (dci < 1 || e->state == EP_DISABLED) { cc = CC_CONTEXT_STATE; break; }
            if (e->state == EP_RUNNING) stop_pending(x, slotid, dci);
            e->state = EP_STOPPED;
            write_ep_state(x, slotid, dci);
            break;
        }
        case CR_SET_TR_DEQ: {
            int dci = (int)((t.control >> 16) & 0x1F);
            if (!valid_slot) { cc = CC_SLOT_NOT_ENABLED; break; }
            xhci_ep *e = &x->slot[slotid].ep[dci];
            if (dci < 1 || (e->state != EP_STOPPED && e->state != EP_ERROR)) { cc = CC_CONTEXT_STATE; break; }
            e->deq = t.param & ~15ULL;
            e->ccs = (int)(t.param & 1);
            e->state = EP_STOPPED;
            write_ep_state(x, slotid, dci);
            break;
        }
        case CR_RESET_DEVICE:
            if (!valid_slot) { cc = CC_SLOT_NOT_ENABLED; break; }
            for (int i = 2; i < 32; i++) x->slot[slotid].ep[i].state = EP_DISABLED;
            write_slot_state(x, slotid, 1, 0);
            if (slot_dev(x, slotid) && slot_dev(x, slotid)->reset) slot_dev(x, slotid)->reset(slot_dev(x, slotid));
            break;
        case CR_NOOP:
            break;
        case CR_NEGOTIATE_BW:
        case CR_SET_LT:
            break;
        case CR_GET_PORT_BW: {
            /* one byte per port (index 0 reserved): plenty of bandwidth everywhere */
            uint8_t bw[NPORTS + 1];
            memset(bw, 0, sizeof bw);
            for (int i = 1; i <= NPORTS; i++) bw[i] = 80;
            if (wd_dma_write(x->m, t.param & ~15ULL, bw, sizeof bw)) cc = CC_TRB_ERROR;
            retslot = 0;
            break;
        }
        default:
            cc = CC_TRB_ERROR;
            break;
        }
        cmd_complete(x, t.addr, cc, retslot);
        x->cmd_deq += 16;
    }
}

/* ---------------------------------------------------------------- registers */

static void hc_reset(xhci *x)
{
    x->usbcmd = 0;
    x->usbsts = 1;                               /* halted */
    x->dnctrl = 0;
    x->config = 0;
    x->crcr = 0;
    x->dcbaap = 0;
    x->cmd_deq = 0;
    x->cmd_ccs = 1;
    x->iman = 0;
    x->imod = 4000;
    x->erstsz = 0;
    x->erstba = 0;
    x->erdp = 0;
    memset(x->slot, 0, sizeof x->slot);
    for (int i = 0; i < NPORTS; i++) {
        if (x->port[i].dev && x->port[i].dev->reset) x->port[i].dev->reset(x->port[i].dev);
        port_connect(x, i);
    }
    update_irq(x);
}

static uint32_t cap_read(uint64_t off)
{
    switch (off) {
    case 0x00: return 0x01100000u | CAP_LEN;                         /* HCIVERSION 1.10, CAPLENGTH */
    case 0x04: return (uint32_t)NPORTS << 24 | (uint32_t)NINTRS << 8 | NSLOTS;    /* HCSPARAMS1 */
    case 0x08: return 4u << 4 | 0xF;                                 /* ERST max 2^4, IST 8 frames */
    case 0x0C: return 0;                                             /* HCSPARAMS3 */
    case 0x10: return (uint32_t)(XECP / 4) << 16 | 1u << 0;          /* HCCPARAMS1: AC64, 32-byte contexts */
    case 0x14: return DB_BASE;
    case 0x18: return RT_BASE;
    case 0x1C: return 0;                                             /* HCCPARAMS2 */
    default: return 0;
    }
}

static uint32_t xecp_read(uint64_t off)
{
    /* supported protocol capability: USB 2.0, ports 1..NPORTS */
    switch (off) {
    case 0x00: return 0x02000000u | 0x02u;                           /* rev 2.0, no next, id 2 */
    case 0x04: return 0x20425355u;                                   /* "USB " */
    case 0x08: return (uint32_t)NPORTS << 8 | 1u;                    /* port offset 1, count */
    case 0x0C: return 0;                                             /* default speed IDs */
    default: return 0;
    }
}

static uint64_t reg_read(wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size)
{
    (void)m; (void)bar;
    xhci *x = d->opaque;
    if (size == 8) return reg_read(m, d, bar, off, 4) | reg_read(m, d, bar, off + 4, 4) << 32;
    uint64_t o = off & ~3ULL;
    uint32_t v = 0;
    if (o < CAP_LEN) v = cap_read(o);
    else if (o >= OP_BASE && o < PORT_BASE) {
        switch (o - OP_BASE) {
        case 0x00: v = x->usbcmd; break;
        case 0x04: v = x->usbsts; break;
        case 0x08: v = 1; break;                                     /* PAGESIZE 4 KiB */
        case 0x14: v = x->dnctrl; break;
        case 0x18: v = (uint32_t)(x->crcr & 0x30) | (x->cmd_running ? 8u : 0u); break;   /* CRR */
        case 0x1C: v = 0; break;
        case 0x30: v = (uint32_t)x->dcbaap; break;
        case 0x34: v = (uint32_t)(x->dcbaap >> 32); break;
        case 0x38: v = x->config; break;
        default: v = 0; break;
        }
    } else if (o >= PORT_BASE && o < PORT_BASE + 0x10 * NPORTS) {
        int p = (int)((o - PORT_BASE) / 0x10);
        if ((o & 0xF) == 0) v = x->port[p].portsc;
    } else if (o >= RT_BASE && o < RT_BASE + 0x20 + 0x20 * NINTRS) {
        if (o == RT_BASE) {
            uint64_t uf = wd_counter(x->m) * 2 / 15625;              /* 62.5 MHz counter -> 125 us microframes */
            v = (uint32_t)(uf & 0x3FFF);
        } else {
            switch (o - RT_BASE - 0x20) {
            case 0x00: v = x->iman; break;
            case 0x04: v = x->imod; break;
            case 0x08: v = x->erstsz; break;
            case 0x10: v = (uint32_t)x->erstba; break;
            case 0x14: v = (uint32_t)(x->erstba >> 32); break;
            case 0x18: v = (uint32_t)x->erdp; break;
            case 0x1C: v = (uint32_t)(x->erdp >> 32); break;
            default: v = 0; break;
            }
        }
    } else if (o >= XECP && o < XECP + 0x10) {
        v = xecp_read(o - XECP);
    }
    v >>= 8 * (off & 3);
    return v & wd_mask(8 * size);
}

static void reg_write(wd_machine *m, wd_pcidev *d, int bar, uint64_t off, int size, uint64_t val)
{
    (void)m; (void)bar;
    xhci *x = d->opaque;
    if (size == 8) {
        reg_write(m, d, bar, off, 4, (uint32_t)val);
        reg_write(m, d, bar, off + 4, 4, val >> 32);
        return;
    }
    if (size != 4) {
        /* narrow writes: merge into the register */
        uint64_t o = off & ~3ULL;
        uint32_t cur = (uint32_t)reg_read(m, d, bar, o, 4);
        int sh = 8 * (int)(off & 3);
        uint32_t mask = (uint32_t)wd_mask(8 * size) << sh;
        /* RW1C bits read as 1 would be cleared by the merge: only for the registers below that matter */
        if (o >= PORT_BASE && o < PORT_BASE + 0x10 * NPORTS && (o & 0xF) == 0) cur &= ~PORTSC_CHANGE;
        if (o == OP_BASE + 4) cur = 0;
        if (o == RT_BASE + 0x20) cur &= ~1u;
        val = (cur & ~mask) | (((uint32_t)val << sh) & mask);
        off = o;
    }
    uint32_t v = (uint32_t)val;
    uint64_t o = off;
    if (o >= OP_BASE && o < PORT_BASE) {
        switch (o - OP_BASE) {
        case 0x00:
            if (v & 2) { hc_reset(x); return; }                     /* HCRST */
            if ((v & 1) && !(x->usbcmd & 1)) x->usbsts &= ~1u;
            if (!(v & 1) && (x->usbcmd & 1)) { x->usbsts |= 1; x->cmd_running = 0; }
            x->usbcmd = v & 0x00003F0Fu & ~2u;
            update_irq(x);
            break;
        case 0x04:
            x->usbsts &= ~(v & 0x041Cu);                            /* HSE, EINT, PCD, SRE are RW1C */
            break;
        case 0x14: x->dnctrl = v & 0xFFFF; break;
        case 0x18:
            if (x->cmd_running) {
                if (v & (1u << 1) || v & (1u << 2)) {                /* CS / CA: stop the command ring */
                    x->cmd_running = 0;
                    cmd_complete(x, x->cmd_deq, 24, 0);              /* command ring stopped */
                }
            } else {
                x->crcr = (x->crcr & 0xFFFFFFFF00000000ULL) | (v & ~0x3Fu & 0xFFFFFFFFu) | (v & 1);
                x->cmd_deq = (x->cmd_deq & 0xFFFFFFFF00000000ULL) | (v & ~0x3Fu);
                x->cmd_ccs = (int)(v & 1);
            }
            break;
        case 0x1C:
            if (!x->cmd_running) {
                x->crcr = (x->crcr & 0xFFFFFFFFULL) | (uint64_t)v << 32;
                x->cmd_deq = (x->cmd_deq & 0xFFFFFFFFULL) | (uint64_t)v << 32;
            }
            break;
        case 0x30: x->dcbaap = (x->dcbaap & 0xFFFFFFFF00000000ULL) | (v & ~0x3Fu); break;
        case 0x34: x->dcbaap = (x->dcbaap & 0xFFFFFFFFULL) | (uint64_t)v << 32; break;
        case 0x38: x->config = v & 0x3FF; break;
        default: break;
        }
        return;
    }
    if (o >= PORT_BASE && o < PORT_BASE + 0x10 * NPORTS) {
        int p = (int)((o - PORT_BASE) / 0x10);
        if ((o & 0xF) != 0) return;
        xhci_port *pt = &x->port[p];
        pt->portsc &= ~(v & PORTSC_CHANGE);                          /* RW1C change bits */
        if (v & PORTSC_PED) {                                        /* writing 1 disables the port */
            pt->portsc &= ~PORTSC_PED;
        }
        if ((v & PORTSC_PR) && pt->dev) port_reset(x, p);
        if ((v & PORTSC_LWS) && pt->dev) {
            uint32_t pls = (v >> 5) & 0xF;
            if (pls == 0 || pls == 3) {                              /* U0 / U3 */
                uint32_t old = (pt->portsc >> 5) & 0xF;
                pt->portsc = (pt->portsc & ~(0xFu << 5)) | pls << 5;
                if (old == 3 && pls == 0) { pt->portsc |= PORTSC_PLC; port_event(x, p + 1); }
            }
        }
        return;
    }
    if (o >= RT_BASE + 0x20 && o < RT_BASE + 0x20 + 0x20 * NINTRS) {
        switch (o - RT_BASE - 0x20) {
        case 0x00:
            x->iman = (x->iman & ~(v & 1u)) & ~2u;
            x->iman |= v & 2u;
            update_irq(x);
            break;
        case 0x04: x->imod = v; break;
        case 0x08: x->erstsz = v & 0xFFFF; break;
        case 0x10: x->erstba = (x->erstba & 0xFFFFFFFF00000000ULL) | (v & ~0x3Fu); er_setup(x); break;
        case 0x14: x->erstba = (x->erstba & 0xFFFFFFFFULL) | (uint64_t)v << 32; er_setup(x); break;
        case 0x18: {
            uint64_t nv = (x->erdp & 0xFFFFFFFF00000000ULL) | (v & ~0xFu);
            uint64_t ehb = (x->erdp & 8) && !(v & 8) ? 8 : 0;       /* EHB is RW1C */
            x->erdp = nv | ehb;
            x->er_full = 0;
            break;
        }
        case 0x1C: x->erdp = (x->erdp & 0xFFFFFFFFULL) | (uint64_t)v << 32; break;
        default: break;
        }
        return;
    }
    if (o >= DB_BASE && o < DB_BASE + 4 * (NSLOTS + 1)) {
        int db = (int)((o - DB_BASE) / 4);
        if (!(x->usbcmd & 1)) return;
        if (db == 0) {
            if ((v & 0xFF) == 0) {
                x->cmd_running = 1;
                process_commands(x);
            }
        } else {
            int dci = (int)(v & 0xFF);
            if (dci < 1 || dci > 31 || !x->slot[db].enabled) return;
            xhci_ep *e = &x->slot[db].ep[dci];
            if (e->state == EP_STOPPED) { e->state = EP_RUNNING; write_ep_state(x, db, dci); }
            if (e->state == EP_RUNNING) process_ep(x, db, dci);
        }
    }
}

static void xhci_free(wd_pcidev *d)
{
    xhci *x = d->opaque;
    free(x->buf);
    for (int i = 0; i < NPORTS; i++)
        if (x->port[i].dev && x->port[i].dev->destroy) x->port[i].dev->destroy(x->port[i].dev);
}

static xhci *get_xhci(wd_machine *m, int create)
{
    for (int i = 0; i < m->npci; i++)
        if (m->pci[i]->bar_read == reg_read) return m->pci[i]->opaque;
    if (!create) return NULL;
    int slot = wd_pci_free_slot(m);
    if (slot < 0 || m->npci >= WD_MAX_PCI) return NULL;
    xhci *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->buf = malloc(TD_MAX);
    if (!x->buf) { free(x); return NULL; }
    x->m = m;
    wd_pci_init_dev(&x->pci, slot << 3, 0x1B36, 0x000D, 0x0C033001, 0x1AF4, 0x1100);
    wd_pci_set_bar(&x->pci, 0, BAR_SIZE, WD_BAR_MEM64);
    x->pci.cfg[0x3D] = 1;
    x->pci.cfg[0x60] = 0x30;                     /* SBRN: USB 3.0 */
    x->pci.cfg[0x61] = 0x20;                     /* FLADJ */
    wd_pci_add_std_caps(&x->pci);
    x->pci.opaque = x;
    x->pci.bar_read = reg_read;
    x->pci.bar_write = reg_write;
    x->pci.on_free = xhci_free;
    hc_reset(x);
    wd_pci_add(m, &x->pci);
    return x;
}

int wd_xhci_attach(wd_machine *m, wd_usbdev *d)
{
    xhci *x = get_xhci(m, 1);
    if (!x || !d) return -1;
    for (int i = 0; i < NPORTS; i++) {
        if (x->port[i].dev) continue;
        x->port[i].dev = d;
        d->m = m;
        d->hc = x;
        d->port = i + 1;
        port_connect(x, i);
        if (x->usbcmd & 1) port_event(x, i + 1);
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------- public API */

int wd_usb_add_xhci(wd_machine *m)
{
    return get_xhci(m, 1) ? 0 : -1;
}

static wd_usbdev *find_dev(wd_machine *m, int kind)
{
    xhci *x = get_xhci(m, 0);
    if (!x) return NULL;
    for (int i = 0; i < NPORTS; i++) {
        wd_usbdev *d = x->port[i].dev;
        if (!d) continue;
        if (kind == 0 && d->descs->device[10] == 0x01) return d;
        if (kind == 1 && d->descs->device[10] == 0x02) return d;
    }
    return NULL;
}

int wd_attach_usb_keyboard(wd_machine *m)
{
    wd_usbdev *d = wd_usb_keyboard_new();
    if (!d || wd_xhci_attach(m, d)) { if (d) d->destroy(d); snprintf(m->err, sizeof m->err, "no free USB port"); return -1; }
    return 0;
}

int wd_attach_usb_tablet(wd_machine *m)
{
    wd_usbdev *d = wd_usb_tablet_new();
    if (!d || wd_xhci_attach(m, d)) { if (d) d->destroy(d); snprintf(m->err, sizeof m->err, "no free USB port"); return -1; }
    return 0;
}

int wd_attach_usb_storage(wd_machine *m, const wd_disk *disk, int cdrom)
{
    xhci *x = get_xhci(m, 1);
    int free_port = 0;
    for (int i = 0; x && i < NPORTS; i++) if (!x->port[i].dev) free_port = 1;
    if (!free_port) { snprintf(m->err, sizeof m->err, "no free USB port"); return -1; }
    wd_usbdev *d = wd_usb_storage_new(disk, cdrom);
    if (!d) { snprintf(m->err, sizeof m->err, "invalid disk for USB storage"); return -1; }
    return wd_xhci_attach(m, d);
}

void wd_key_event(wd_machine *m, int hid_usage, int down)
{
    wd_usbdev *d = find_dev(m, 0);
    if (d) wd_usb_keyboard_event(d, hid_usage, down);
}

void wd_pointer_event(wd_machine *m, int x, int y, int buttons, int wheel)
{
    wd_usbdev *d = find_dev(m, 1);
    if (d) wd_usb_tablet_event(d, x, y, buttons, wheel);
}
