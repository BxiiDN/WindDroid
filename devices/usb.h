/* WindDroid - USB device model shared by the host controller and devices.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WD_USB_H
#define WD_USB_H

#include "../core/internal.h"

#define USB_RET_NAK   (-1)       /* no data yet: keep the transfer pending */
#define USB_RET_STALL (-2)

enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };

typedef struct wd_usbdev wd_usbdev;

typedef struct {
    const uint8_t *device;              /* 18 bytes */
    const uint8_t *qualifier;           /* 10 bytes, high-speed devices */
    const uint8_t *config;              /* full configuration descriptor */
    int config_len;
    const char *strings[8];             /* index 1.. */
} wd_usb_descs;

struct wd_usbdev {
    int speed;
    int configuration;
    int alt_setting;
    int halted_in[16], halted_out[16];
    const wd_usb_descs *descs;
    /* class/vendor control requests (after the standard ones); IN: fill data, return length;
     * OUT: data holds wLength bytes, return 0; or USB_RET_STALL */
    int (*class_control)(wd_usbdev *d, const uint8_t *setup, uint8_t *data);
    /* bulk/interrupt transfer on endpoint 1..15: returns bytes moved, USB_RET_NAK or USB_RET_STALL */
    int (*data)(wd_usbdev *d, int ep, int in, uint8_t *buf, int len);
    void (*reset)(wd_usbdev *d);
    void (*destroy)(wd_usbdev *d);
    /* set by the host controller */
    struct wd_machine *m;
    void *hc;
    int port;
};

/* standard requests, then class_control */
int wd_usb_control(wd_usbdev *d, const uint8_t *setup, uint8_t *data);

/* the host controller: called by a device when an IN endpoint has new data */
void wd_xhci_wakeup(wd_usbdev *d, int ep);
int wd_xhci_attach(struct wd_machine *m, wd_usbdev *d);

wd_usbdev *wd_usb_keyboard_new(void);
wd_usbdev *wd_usb_tablet_new(void);
wd_usbdev *wd_usb_storage_new(const wd_disk *disk, int cdrom);

/* input into the HID devices */
void wd_usb_keyboard_event(wd_usbdev *d, int usage, int down);
void wd_usb_tablet_event(wd_usbdev *d, int x, int y, int buttons, int wheel);

#endif
