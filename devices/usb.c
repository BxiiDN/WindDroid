/*
 * WindDroid - USB device side: standard requests, HID keyboard (boot
 * protocol) and HID tablet (absolute pointer).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "usb.h"

#define VID 0x1209          /* pid.codes */

/* ---------------------------------------------------------------- standard requests */

static int string_desc(const wd_usb_descs *ds, int idx, uint8_t *data)
{
    if (idx == 0) {
        data[0] = 4; data[1] = 3; data[2] = 0x09; data[3] = 0x04;    /* English (US) */
        return 4;
    }
    if (idx >= 8 || !ds->strings[idx]) return USB_RET_STALL;
    const char *s = ds->strings[idx];
    int n = 0;
    while (s[n] && n < 126) { data[2 + 2 * n] = (uint8_t)s[n]; data[3 + 2 * n] = 0; n++; }
    data[0] = (uint8_t)(2 + 2 * n);
    data[1] = 3;
    return 2 + 2 * n;
}

int wd_usb_control(wd_usbdev *d, const uint8_t *setup, uint8_t *data)
{
    int type = setup[0], req = setup[1];
    int value = setup[2] | setup[3] << 8, index = setup[4] | setup[5] << 8, length = setup[6] | setup[7] << 8;
    int r = USB_RET_STALL;
    const wd_usb_descs *ds = d->descs;
    switch (type << 8 | req) {
    case 0x8006:                                              /* GET_DESCRIPTOR (device) */
        switch (value >> 8) {
        case 1: memcpy(data, ds->device, 18); r = 18; break;
        case 2:
            if ((value & 0xFF) != 0) break;
            memcpy(data, ds->config, (size_t)ds->config_len);
            r = ds->config_len;
            break;
        case 3: r = string_desc(ds, value & 0xFF, data); break;
        case 6:
            if (ds->qualifier) { memcpy(data, ds->qualifier, 10); r = 10; }
            break;
        case 7:                                               /* other speed configuration */
            if (ds->qualifier) {
                memcpy(data, ds->config, (size_t)ds->config_len);
                data[1] = 7;
                r = ds->config_len;
            }
            break;
        default: break;
        }
        break;
    case 0x8008: data[0] = (uint8_t)d->configuration; r = 1; break;     /* GET_CONFIGURATION */
    case 0x0009:                                              /* SET_CONFIGURATION */
        if (value > 1) break;
        d->configuration = value;
        memset(d->alt, 0, sizeof d->alt);
        if (d->set_interface) d->set_interface(d, -1, 0);
        memset(d->halted_in, 0, sizeof d->halted_in);
        memset(d->halted_out, 0, sizeof d->halted_out);
        r = 0;
        break;
    case 0x8000: data[0] = 0; data[1] = 0; r = 2; break;      /* GET_STATUS (device): bus powered */
    case 0x8100: data[0] = 0; data[1] = 0; r = 2; break;      /* GET_STATUS (interface) */
    case 0x8200: {                                            /* GET_STATUS (endpoint) */
        int ep = index & 15, in = index & 0x80;
        data[0] = (uint8_t)(ep && (in ? d->halted_in[ep] : d->halted_out[ep]));
        data[1] = 0;
        r = 2;
        break;
    }
    case 0x0001: case 0x0003: r = 0; break;                   /* CLEAR/SET_FEATURE (device): remote wakeup */
    case 0x0101: case 0x0103: r = 0; break;
    case 0x0201: case 0x0203: {                               /* CLEAR/SET_FEATURE (endpoint): halt */
        int ep = index & 15, in = index & 0x80;
        if (value != 0) break;
        if (ep) {
            if (in) d->halted_in[ep] = req == 3;
            else d->halted_out[ep] = req == 3;
        }
        r = 0;
        break;
    }
    case 0x810A: data[0] = (uint8_t)(index < 4 ? d->alt[index] : 0); r = 1; break;   /* GET_INTERFACE */
    case 0x010B:                                              /* SET_INTERFACE */
        if (index >= 4) break;
        if (d->set_interface ? d->set_interface(d, index, value) == 0 : value == 0) { d->alt[index] = value; r = 0; }
        break;
    default:
        if (d->class_control) r = d->class_control(d, setup, data);
        break;
    }
    if (r > length) r = length;
    return r;
}

/* ---------------------------------------------------------------- HID */

typedef struct {
    wd_usbdev dev;
    int kind;                     /* 0 keyboard, 1 tablet */
    uint8_t queue[64][8];
    int qhead, qlen, qsent_tail;  /* qsent_tail: the newest queued report has not been sent yet */
    int rlen;
    uint8_t keys[6], mods;
    uint8_t leds, protocol, idle;
    const uint8_t *report_desc;
    int report_desc_len;
} hid_dev;

static const uint8_t kbd_report[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

static const uint8_t tablet_report[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};

#define HID_CONFIG(proto, sub, rdlen, mps)                                                        \
    9, 2, 34, 0, 1, 1, 0, 0xA0, 50,                         /* configuration, remote wakeup, 100 mA */ \
    9, 4, 0, 0, 1, 3, sub, proto, 0,                        /* interface: HID */                       \
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, (rdlen) & 0xFF, (rdlen) >> 8,                                     \
    7, 5, 0x81, 3, mps, 0, 7                                /* interrupt IN, 8 ms */

static const uint8_t kbd_device[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, VID & 0xFF, VID >> 8, 0x01, 0x00,
                                        0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t tablet_device[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, VID & 0xFF, VID >> 8, 0x02, 0x00,
                                           0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t hs_qualifier[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0 };
static const uint8_t kbd_config[] = { HID_CONFIG(1, 1, sizeof kbd_report, 8) };
static const uint8_t tablet_config[] = { HID_CONFIG(0, 0, sizeof tablet_report, 6) };

static const wd_usb_descs kbd_descs = {
    kbd_device, hs_qualifier, kbd_config, sizeof kbd_config,
    { NULL, "WindDroid", "WindDroid USB Keyboard", "WDKBD0001" },
};
static const wd_usb_descs tablet_descs = {
    tablet_device, hs_qualifier, tablet_config, sizeof tablet_config,
    { NULL, "WindDroid", "WindDroid USB Tablet", "WDTAB0001" },
};

static void hid_current(hid_dev *h, uint8_t *r)
{
    memset(r, 0, 8);
    r[0] = h->mods;
    memcpy(r + 2, h->keys, 6);
}

static int hid_class_control(wd_usbdev *d, const uint8_t *setup, uint8_t *data)
{
    hid_dev *h = (hid_dev *)d;
    int type = setup[0], req = setup[1], value = setup[2] | setup[3] << 8;
    switch (type << 8 | req) {
    case 0x8106:                                         /* GET_DESCRIPTOR (interface) */
        if ((value >> 8) == 0x22) { memcpy(data, h->report_desc, (size_t)h->report_desc_len); return h->report_desc_len; }
        if ((value >> 8) == 0x21) { memcpy(data, h->dev.descs->config + 18, 9); return 9; }
        return USB_RET_STALL;
    case 0xA101:                                         /* GET_REPORT */
        if (h->kind == 0) { hid_current(h, data); return 8; }
        memset(data, 0, 6);
        return 6;
    case 0x2109:                                         /* SET_REPORT (keyboard LEDs) */
        h->leds = data[0];
        return 0;
    case 0xA102: data[0] = h->idle; return 1;            /* GET_IDLE */
    case 0x210A: h->idle = (uint8_t)(value >> 8); return 0;   /* SET_IDLE */
    case 0xA103: data[0] = h->protocol; return 1;        /* GET_PROTOCOL */
    case 0x210B: h->protocol = (uint8_t)value; return 0; /* SET_PROTOCOL */
    default: return USB_RET_STALL;
    }
}

static int hid_data(wd_usbdev *d, int ep, int in, uint8_t *buf, int len)
{
    hid_dev *h = (hid_dev *)d;
    if (ep != 1 || !in) return USB_RET_STALL;
    if (!h->qlen) return USB_RET_NAK;
    int n = h->rlen < len ? h->rlen : len;
    memcpy(buf, h->queue[h->qhead], (size_t)n);
    h->qhead = (h->qhead + 1) % 64;
    h->qlen--;
    return n;
}

static void hid_push(hid_dev *h, const uint8_t *r)
{
    if (h->qlen == 64) {                                   /* full: replace the newest report */
        memcpy(h->queue[(h->qhead + 63) % 64], r, 8);
    } else {
        memcpy(h->queue[(h->qhead + h->qlen) % 64], r, 8);
        h->qlen++;
    }
    if (h->dev.hc) wd_xhci_wakeup(&h->dev, 1);
}

void wd_usb_keyboard_event(wd_usbdev *d, int usage, int down)
{
    hid_dev *h = (hid_dev *)d;
    if (!h || h->kind != 0 || usage <= 0 || usage > 0xE7 || (usage > 0x65 && usage < 0xE0)) return;
    if (usage >= 0xE0) {
        uint8_t bit = (uint8_t)(1 << (usage - 0xE0));
        uint8_t nm = down ? (h->mods | bit) : (h->mods & ~bit);
        if (nm == h->mods) return;
        h->mods = nm;
    } else {
        int at = -1;
        for (int i = 0; i < 6; i++) if (h->keys[i] == usage) at = i;
        if (down) {
            if (at >= 0) return;
            for (int i = 0; i < 6; i++) if (!h->keys[i]) { h->keys[i] = (uint8_t)usage; at = i; break; }
            if (at < 0) {                                  /* rollover: shift out the oldest key */
                memmove(h->keys, h->keys + 1, 5);
                h->keys[5] = (uint8_t)usage;
            }
        } else {
            if (at < 0) return;
            memmove(h->keys + at, h->keys + at + 1, (size_t)(5 - at));
            h->keys[5] = 0;
        }
    }
    uint8_t r[8];
    hid_current(h, r);
    hid_push(h, r);
}

void wd_usb_tablet_event(wd_usbdev *d, int x, int y, int buttons, int wheel)
{
    hid_dev *h = (hid_dev *)d;
    if (!h || h->kind != 1) return;
    if (x < 0) x = 0;
    if (x > 32767) x = 32767;
    if (y < 0) y = 0;
    if (y > 32767) y = 32767;
    if (wheel > 127) wheel = 127;
    if (wheel < -127) wheel = -127;
    uint8_t r[8] = { (uint8_t)(buttons & 7), (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8),
                     (uint8_t)(int8_t)wheel, 0, 0 };
    /* coalesce pure motion into the newest unsent report */
    if (h->qlen && !wheel) {
        uint8_t *last = h->queue[(h->qhead + h->qlen - 1) % 64];
        if (last[0] == r[0] && last[5] == 0) {
            memcpy(last, r, 8);
            return;
        }
    }
    hid_push(h, r);
}

static void hid_reset(wd_usbdev *d)
{
    hid_dev *h = (hid_dev *)d;
    h->qhead = h->qlen = 0;
    h->protocol = 1;
    h->idle = 0;
    d->configuration = 0;
}

static void hid_destroy(wd_usbdev *d) { free(d); }

static wd_usbdev *hid_new(int kind)
{
    hid_dev *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->kind = kind;
    h->dev.speed = USB_SPEED_HIGH;
    h->dev.descs = kind ? &tablet_descs : &kbd_descs;
    h->dev.class_control = hid_class_control;
    h->dev.data = hid_data;
    h->dev.reset = hid_reset;
    h->dev.destroy = hid_destroy;
    h->rlen = kind ? 6 : 8;
    h->report_desc = kind ? tablet_report : kbd_report;
    h->report_desc_len = kind ? (int)sizeof tablet_report : (int)sizeof kbd_report;
    h->protocol = 1;
    return &h->dev;
}

wd_usbdev *wd_usb_keyboard_new(void) { return hid_new(0); }
wd_usbdev *wd_usb_tablet_new(void) { return hid_new(1); }

/* ASCII to HID usage (US layout); *shift set when Shift is needed. Returns 0 if unmapped. */
int wd_ascii_to_hid(int ch, int *shift)
{
    static const char plain[] = "1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static const char shifted[] = "!@#$%^&*()";
    static const char shifted2[] = "_+{}|~:\"~<>?";
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') return 0x04 + (ch - 'a');
    if (ch >= 'A' && ch <= 'Z') { *shift = 1; return 0x04 + (ch - 'A'); }
    if (ch == '\r') return 0x28;
    for (int i = 0; plain[i]; i++) {
        if (i == 20) continue;                         /* 0x32 is the non-US "#" key */
        if (plain[i] == ch) return 0x1E + i;
    }
    for (int i = 0; shifted[i]; i++) if (shifted[i] == ch) { *shift = 1; return 0x1E + i; }
    static const int usages2[] = { 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38 };
    for (int i = 0; shifted2[i]; i++) if (shifted2[i] == ch && i != 5) { *shift = 1; return usages2[i]; }
    return 0;
}
