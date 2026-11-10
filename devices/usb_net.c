/*
 * WindDroid - USB network adapter: CDC NCM (Network Control Model) with
 * 16-bit transfer blocks, as handled by Windows' inbox UsbNcm driver and
 * Linux's cdc_ncm. Frames go to and from the user-mode NAT (nat.c).
 *
 * Interfaces: 0 = communication (interrupt IN 0x81 for notifications),
 * 1 = data (setting 0 without endpoints, setting 1 with bulk IN 0x82 and
 * bulk OUT 0x02). An IAD groups them into one function.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "nat.h"
#include "usb.h"

#define VID 0x1209
#define PID 0x4E43                         /* "NC" */
#define RXQ 256                            /* frames waiting for the guest */
#define FRAME_MAX 1514
#define NTB_IN_MAX  32768
#define NTB_OUT_MAX 32768

typedef struct {
    wd_usbdev dev;
    wd_nat *nat;
    char mac_str[13];
    uint8_t mac[6];
    wd_usb_descs descs;
    uint8_t config[128];
    int active;                            /* data interface in setting 1 */
    uint32_t ntb_in_size;
    uint16_t seq_in;
    int filter;
    /* to the guest */
    uint8_t (*rxq)[FRAME_MAX];
    uint16_t rxlen[RXQ];
    int rxhead, rxcount;
    /* notifications pending on the interrupt endpoint */
    int notify;                            /* bit 0: speed change, bit 1: connected */
    uint64_t dropped;
} ncm_dev;

static const uint8_t ncm_device[18] = {
    18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,  /* USB 2.0, miscellaneous / IAD */
    VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8, 0x00, 0x01, 1, 2, 4, 1,
};

static const uint8_t ncm_qualifier[10] = { 10, 6, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0 };

static int build_config(uint8_t *c)
{
    static const uint8_t body[] = {
        /* IAD: interfaces 0-1, CDC NCM */
        8, 0x0B, 0, 2, 0x02, 0x0D, 0x00, 0,
        /* interface 0: communication, NCM */
        9, 4, 0, 0, 1, 0x02, 0x0D, 0x00, 0,
        5, 0x24, 0x00, 0x10, 0x01,                     /* CDC header 1.10 */
        5, 0x24, 0x06, 0, 1,                           /* union: master 0, slave 1 */
        13, 0x24, 0x0F, 3, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0,   /* Ethernet: MAC in string 3, 1514 */
        6, 0x24, 0x1A, 0x00, 0x01, 0x01,               /* NCM 1.0, SetEthernetPacketFilter */
        7, 5, 0x81, 0x03, 16, 0, 8,                    /* interrupt IN, 16 bytes, 16 ms */
        /* interface 1 setting 0: no endpoints */
        9, 4, 1, 0, 0, 0x0A, 0x00, 0x01, 0,
        /* interface 1 setting 1: bulk IN and OUT */
        9, 4, 1, 1, 2, 0x0A, 0x00, 0x01, 0,
        7, 5, 0x82, 0x02, 0x00, 0x02, 0,
        7, 5, 0x02, 0x02, 0x00, 0x02, 0,
    };
    int len = 9 + (int)sizeof body;
    uint8_t hdr[9] = { 9, 2, (uint8_t)len, (uint8_t)(len >> 8), 2, 1, 0, 0x80, 50 };
    memcpy(c, hdr, 9);
    memcpy(c + 9, body, sizeof body);
    return len;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

/* ---------------------------------------------------------------- NAT side */

static int nat_send(void *opaque, const uint8_t *frame, int len)
{
    ncm_dev *n = opaque;
    if (!n->active || len > FRAME_MAX) return 0;      /* link down: dropped like on a wire */
    if (n->rxcount == RXQ) { n->dropped++; return -1; }
    int i = (n->rxhead + n->rxcount) % RXQ;
    memcpy(n->rxq[i], frame, (size_t)len);
    n->rxlen[i] = (uint16_t)len;
    n->rxcount++;                                     /* (the endpoint is woken by the poll or by */
    return 0;                                         /*  the controller after an OUT transfer) */
}

static int nat_room(void *opaque)
{
    ncm_dev *n = opaque;
    return n->active ? RXQ - n->rxcount : RXQ;
}

/* ---------------------------------------------------------------- transfers */

static int align4(int v) { return (v + 3) & ~3; }

/* as many queued frames as fit into one NTB16 of at most max bytes */
static int build_ntb(ncm_dev *n, uint8_t *buf, int max)
{
    int pos = 12, cnt = 0;
    int idx[64], ln[64];
    while (cnt < n->rxcount && cnt < 63) {
        int i = (n->rxhead + cnt) % RXQ;
        int p = align4(pos), l = n->rxlen[i];
        int ndp = align4(p + l);
        if (ndp + 8 + 4 * (cnt + 2) > max) break;
        memcpy(buf + p, n->rxq[i], (size_t)l);
        idx[cnt] = p; ln[cnt] = l;
        pos = p + l;
        cnt++;
    }
    if (!cnt) {
        if (n->rxcount) {                             /* cannot fit even one: the buffer is too small */
            n->rxhead = (n->rxhead + 1) % RXQ;
            n->rxcount--;
            n->dropped++;
        }
        return 0;
    }
    n->rxhead = (n->rxhead + cnt) % RXQ;
    n->rxcount -= cnt;
    int ndp = align4(pos);
    int ndplen = 8 + 4 * (cnt + 1);
    memset(buf + pos, 0, (size_t)(ndp - pos));
    put32(buf + ndp, 0x304D434E);                     /* "NCM0" */
    put16(buf + ndp + 4, (uint32_t)ndplen);
    put16(buf + ndp + 6, 0);
    for (int k = 0; k < cnt; k++) { put16(buf + ndp + 8 + 4 * k, (uint32_t)idx[k]); put16(buf + ndp + 10 + 4 * k, (uint32_t)ln[k]); }
    put16(buf + ndp + 8 + 4 * cnt, 0); put16(buf + ndp + 10 + 4 * cnt, 0);
    int total = ndp + ndplen;
    put32(buf, 0x484D434E);                           /* "NCMH" */
    put16(buf + 4, 12);
    put16(buf + 6, n->seq_in++);
    put16(buf + 8, (uint32_t)total);
    put16(buf + 10, (uint32_t)ndp);
    return total;
}

static void parse_ntb(ncm_dev *n, const uint8_t *b, int len)
{
    if (len < 12 || get32(b) != 0x484D434E || get16(b + 4) != 12) return;
    int blen = (int)get16(b + 8);
    if (blen == 0 || blen > len) blen = len;
    int ndp = (int)get16(b + 10);
    for (int guard = 0; guard < 8 && ndp; guard++) {
        if (ndp < 12 || ndp + 16 > blen || (ndp & 3)) return;
        uint32_t sig = get32(b + ndp);
        if (sig != 0x304D434E && sig != 0x314D434E) return;   /* NCM0 (NCM1 carries a CRC we don't check) */
        int nlen = (int)get16(b + ndp + 4);
        if (nlen < 16 || ndp + nlen > blen) return;
        for (int e = ndp + 8; e + 4 <= ndp + nlen; e += 4) {
            int idx = (int)get16(b + e), l = (int)get16(b + e + 2);
            if (!idx || !l) break;
            if (sig == 0x314D434E) l -= 4;
            if (idx + l > blen || l < 14) continue;
            wd_nat_input(n->nat, b + idx, l);
        }
        ndp = (int)get16(b + ndp + 6);
    }
}

static int ncm_data(wd_usbdev *d, int ep, int in, uint8_t *buf, int len)
{
    ncm_dev *n = (ncm_dev *)d;
    if (ep == 1 && in) {                              /* notifications */
        if (n->notify & 1) {
            static const uint8_t speed[8] = { 0xA1, 0x2A, 0, 0, 0, 0, 8, 0 };
            if (len < 16) return USB_RET_STALL;
            memcpy(buf, speed, 8);
            put32(buf + 8, 1000000000u);              /* 1 Gbit/s down and up */
            put32(buf + 12, 1000000000u);
            n->notify &= ~1;
            return 16;
        }
        if (n->notify & 2) {
            static const uint8_t conn[8] = { 0xA1, 0x00, 1, 0, 0, 0, 0, 0 };
            if (len < 8) return USB_RET_STALL;
            memcpy(buf, conn, 8);
            n->notify &= ~2;
            return 8;
        }
        return USB_RET_NAK;
    }
    if (!n->active) return USB_RET_NAK;
    if (ep == 2 && in) {
        wd_nat_poll(n->nat);
        if (!n->rxcount) return USB_RET_NAK;
        int max = len < (int)n->ntb_in_size ? len : (int)n->ntb_in_size;
        int r = build_ntb(n, buf, max);
        if (r == 0) return USB_RET_NAK;
        wd_nat_kick(n->nat);                          /* room again: senders may continue */
        return r;
    }
    if (ep == 2 && !in) {
        parse_ntb(n, buf, len);
        return len;
    }
    return USB_RET_STALL;
}

static int ncm_class(wd_usbdev *d, const uint8_t *setup, uint8_t *data)
{
    ncm_dev *n = (ncm_dev *)d;
    int req = setup[1], value = setup[2] | setup[3] << 8, length = setup[6] | setup[7] << 8;
    if ((setup[0] & 0x7F) != 0x21) return USB_RET_STALL;    /* class, interface */
    switch (setup[0] << 8 | req) {
    case 0xA180:                                      /* GET_NTB_PARAMETERS */
        memset(data, 0, 28);
        put16(data, 28);
        put16(data + 2, 1);                           /* NTB16 only */
        put32(data + 4, NTB_IN_MAX);
        put16(data + 8, 4); put16(data + 10, 0); put16(data + 12, 4);
        put32(data + 16, NTB_OUT_MAX);
        put16(data + 20, 4); put16(data + 22, 0); put16(data + 24, 4);
        put16(data + 26, 0);
        return 28;
    case 0xA183: put16(data, 0); return 2;            /* GET_NTB_FORMAT: NTB16 */
    case 0x2184: return value == 0 ? 0 : USB_RET_STALL;  /* SET_NTB_FORMAT */
    case 0xA185: put32(data, n->ntb_in_size); return 4;  /* GET_NTB_INPUT_SIZE */
    case 0x2186: {                                    /* SET_NTB_INPUT_SIZE */
        if (length < 4) return USB_RET_STALL;
        uint32_t v = get32(data);
        if (v < 2048 || v > NTB_IN_MAX) return USB_RET_STALL;
        n->ntb_in_size = v;
        return 0;
    }
    case 0xA187: put16(data, FRAME_MAX); return 2;    /* GET_MAX_DATAGRAM_SIZE */
    case 0x2188: return 0;                            /* SET_MAX_DATAGRAM_SIZE */
    case 0xA189: put16(data, 0); return 2;            /* GET_CRC_MODE */
    case 0x218A: return value == 0 ? 0 : USB_RET_STALL;
    case 0xA182: memcpy(data, n->mac, 6); return 6;   /* GET_NET_ADDRESS */
    case 0x2181: if (length >= 6) memcpy(n->mac, data, 6); return 0;   /* SET_NET_ADDRESS */
    case 0x2143: n->filter = value; return 0;         /* SET_ETHERNET_PACKET_FILTER */
    case 0x2140: return 0;                            /* SET_ETHERNET_MULTICAST_FILTERS */
    case 0x2100: case 0xA101: return USB_RET_STALL;   /* encapsulated commands: not used by NCM */
    default: return USB_RET_STALL;
    }
}

static void reset_data(ncm_dev *n)
{
    n->rxhead = n->rxcount = 0;
    n->seq_in = 0;
    n->ntb_in_size = NTB_IN_MAX;
}

static int ncm_set_interface(wd_usbdev *d, int iface, int alt)
{
    ncm_dev *n = (ncm_dev *)d;
    if (iface < 0) {                                  /* SET_CONFIGURATION */
        n->active = 0;
        n->notify = 0;
        reset_data(n);
        return 0;
    }
    if (iface == 0) return alt == 0 ? 0 : -1;
    if (iface != 1 || alt > 1) return -1;
    n->active = alt;
    reset_data(n);
    n->notify = alt ? 3 : 0;                          /* sent at the next poll */
    return 0;
}

static void ncm_reset(wd_usbdev *d)
{
    ncm_dev *n = (ncm_dev *)d;
    n->active = 0;
    n->notify = 0;
    reset_data(n);
}

static void ncm_destroy(wd_usbdev *d)
{
    ncm_dev *n = (ncm_dev *)d;
    wd_nat_free(n->nat);
    free(n->rxq);
    free(n);
}

void wd_usb_net_poll(wd_usbdev *d)
{
    ncm_dev *n = (ncm_dev *)d;
    if (n->notify) wd_xhci_wakeup(d, 1);
    if (!n->active) return;
    wd_nat_poll(n->nat);
    if (n->rxcount) wd_xhci_wakeup(d, 2);
}

wd_usbdev *wd_usb_net_new(const uint8_t mac[6])
{
    ncm_dev *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->rxq = malloc(sizeof *n->rxq * RXQ);
    wd_nat_ops ops = { nat_send, nat_room, n };
    n->nat = wd_nat_new(&ops);
    if (!n->rxq || !n->nat) { free(n->rxq); wd_nat_free(n->nat); free(n); return NULL; }
    memcpy(n->mac, mac, 6);
    snprintf(n->mac_str, sizeof n->mac_str, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    n->descs.device = ncm_device;
    n->descs.qualifier = ncm_qualifier;
    n->descs.config = n->config;
    n->descs.config_len = build_config(n->config);
    n->descs.strings[1] = "WindDroid";
    n->descs.strings[2] = "WindDroid USB Ethernet (NCM)";
    n->descs.strings[3] = n->mac_str;
    n->descs.strings[4] = "WD0001";
    n->dev.speed = USB_SPEED_HIGH;
    n->dev.descs = &n->descs;
    n->dev.class_control = ncm_class;
    n->dev.data = ncm_data;
    n->dev.set_interface = ncm_set_interface;
    n->dev.reset = ncm_reset;
    n->dev.destroy = ncm_destroy;
    reset_data(n);
    return &n->dev;
}
