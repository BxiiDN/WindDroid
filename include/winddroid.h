/*
 * WindDroid - public API of the virtual machine engine.
 *
 * The engine emulates a single-core AArch64 machine that follows the
 * memory map of QEMU's "virt" board (GICv3, PL011 UART, PL031 RTC,
 * ARM generic timer, PSCI over HVC/SMC).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef WINDDROID_H
#define WINDDROID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_VERSION "0.3.2"

typedef struct wd_machine wd_machine;

/* Physical memory map (same addresses as QEMU virt). */
#define WD_FLASH_BASE    0x00000000ULL
#define WD_GICD_BASE     0x08000000ULL
#define WD_GITS_BASE     0x08080000ULL  /* ITS: control frame + translation frame */
#define WD_GITS_TRANSLATER (WD_GITS_BASE + 0x10040ULL)
#define WD_GICR_BASE     0x080A0000ULL
#define WD_UART_BASE     0x09000000ULL
#define WD_RTC_BASE      0x09010000ULL
#define WD_FWCFG_BASE    0x09020000ULL
#define WD_CTL_BASE      0x09100000ULL  /* WindDroid control device */
#define WD_FLASH_SIZE    0x04000000ULL  /* per bank; bank 1 at WD_FLASH_SIZE */
#define WD_PCIE_MMIO     0x10000000ULL
#define WD_PCIE_MMIO_SZ  0x2EFF0000ULL
#define WD_PCIE_PIO      0x3EFF0000ULL
#define WD_PCIE_ECAM     0x3F000000ULL
#define WD_PCIE_ECAM_SZ  0x01000000ULL
#define WD_PCIE_MMIO_HI     0x8000000000ULL
#define WD_PCIE_MMIO_HI_SZ  0x8000000000ULL
#define WD_VIRTIO_BASE   0x0A000000ULL
#define WD_RAM_BASE      0x40000000ULL

/* Interrupt numbers (INTID). */
#define WD_IRQ_UART      33   /* SPI 1 */
#define WD_IRQ_RTC       34   /* SPI 2 */
#define WD_PPI_VTIMER    27
#define WD_PPI_PTIMER    30

typedef enum {
    WD_RUN_BUDGET = 0,   /* instruction budget used up, call again */
    WD_RUN_IDLE,         /* CPU waits in WFI with nothing to wake it */
    WD_RUN_SHUTDOWN,     /* guest powered off (PSCI SYSTEM_OFF or ctl) */
    WD_RUN_RESET,        /* guest requested a reset */
    WD_RUN_ERROR         /* emulator internal error, see wd_error() */
} wd_run_result;

typedef void (*wd_uart_tx_fn)(void *opaque, uint8_t ch);

typedef struct {
    uint64_t ram_size;   /* bytes, multiple of 4 KiB */
    int realtime;        /* 1: counter follows host clock, 0: counter = instructions */
    int ncpu;            /* virtual CPUs, 1..8 (0 means 1); they share one host thread */
} wd_config;

wd_machine *wd_create(const wd_config *cfg);
void        wd_destroy(wd_machine *m);
void        wd_set_uart_tx(wd_machine *m, wd_uart_tx_fn fn, void *opaque);

/* Loading. Return 0 on success, -1 on error (see wd_error). */
int  wd_load_elf(wd_machine *m, const uint8_t *data, size_t len);
int  wd_load_raw(wd_machine *m, const uint8_t *data, size_t len, uint64_t paddr);
void wd_set_entry(wd_machine *m, uint64_t pc);
int  wd_load_image(wd_machine *m, const uint8_t *data, size_t len); /* ELF or raw at RAM base */

void wd_uart_input(wd_machine *m, const uint8_t *data, size_t len);

/* UEFI firmware boot: code goes to flash bank 0, the variable store to bank 1
 * (vars may be NULL for an empty store). Builds the device tree and fw_cfg. */
int  wd_load_firmware(wd_machine *m, const uint8_t *code, size_t code_len, const uint8_t *vars, size_t vars_len);
/* Framebuffer configured by the guest (ramfb). Pixels are XRGB8888 in guest
 * RAM: byte order B, G, R, X. Returns 0 while no mode has been set. */
typedef struct {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    uint32_t generation;      /* changes whenever the mode changes */
} wd_fb_info;
int wd_framebuffer(wd_machine *m, wd_fb_info *info);

/* Block devices. A disk is a set of callbacks over a byte-addressed image;
 * the engine owns it after a successful attach and calls close() on destroy. */
typedef struct wd_disk {
    void *opaque;
    uint64_t size;            /* bytes; a multiple of 512 is used */
    int readonly;
    uint32_t block_size;      /* logical block size: 512 (0 means 512), 2048 (CD) or 4096 */
    int format;               /* WD_IMG_* the image was opened as */
    int (*read)(void *opaque, uint64_t off, void *buf, size_t len);         /* 0 = ok */
    int (*write)(void *opaque, uint64_t off, const void *buf, size_t len);  /* NULL if read-only */
    int (*flush)(void *opaque);
    void (*close)(void *opaque);
} wd_disk;

enum { WD_IMG_RAW = 0, WD_IMG_ISO, WD_IMG_VHD, WD_IMG_VHDX };
/* Detect the format of an image file from its contents. */
int wd_image_probe(int fd);

/* Raw image behind a file descriptor (pread/pwrite, nothing is loaded into RAM). */
int wd_disk_from_fd(int fd, int readonly, int take_ownership, wd_disk *out);
/* Zero-filled in-memory disk. */
int wd_disk_memory(uint64_t size, wd_disk *out);
/* Image file of any supported format (raw/IMG, ISO, VHDX, VHD), detected from its contents. */
int wd_disk_open_image(int fd, int readonly, int take_ownership, wd_disk *out, char *err, size_t errlen);

/* Create an empty dynamic VHDX of 'size' bytes (block_size/sector_size 0 = 32 MiB / 512). */
int wd_vhdx_create(int fd, uint64_t size, uint32_t block_size, uint32_t sector_size);

/* Attach an NVMe SSD on the PCIe bus. Returns 0 on success. */
int wd_attach_nvme(wd_machine *m, const wd_disk *disk);

/* USB (xHCI controller on PCIe, created on first use). The keyboard takes
 * HID usage codes (USB HID Usage Tables, page 0x07; modifiers 0xE0..0xE7);
 * the tablet takes absolute coordinates 0..32767 and buttons bit0 left,
 * bit1 right, bit2 middle. A CD drive takes ISO images (2048-byte blocks). */
int  wd_usb_add_xhci(wd_machine *m);
int  wd_attach_usb_keyboard(wd_machine *m);
int  wd_attach_usb_tablet(wd_machine *m);
int  wd_attach_usb_storage(wd_machine *m, const wd_disk *disk, int cdrom);
/* USB network adapter (CDC NCM, inbox driver in Windows 11) on a user-mode NAT:
 * the guest gets 10.0.2.15 by DHCP, 10.0.2.2 is the host (127.0.0.1), 10.0.2.3 DNS.
 * mac NULL: 52:54:00:12:34:56. */
int  wd_attach_usb_net(wd_machine *m, const uint8_t mac[6]);
void wd_key_event(wd_machine *m, int hid_usage, int down);
void wd_pointer_event(wd_machine *m, int x, int y, int buttons, int wheel);
/* US layout: HID usage for an ASCII character (0 if none), *shift set when Shift is needed. */
int  wd_ascii_to_hid(int ch, int *shift);

/* Current variable store (to persist it); returns NULL if not booting firmware. The
 * length covers the initial store and everything the firmware wrote. */
const uint8_t *wd_firmware_vars(wd_machine *m, size_t *len, int *dirty);
/* The host has stored the variable store: clears the dirty flag. */
void wd_firmware_vars_saved(wd_machine *m);

wd_run_result wd_run(wd_machine *m, uint64_t max_insns);

/* When wd_run returned WD_RUN_IDLE in realtime mode: nanoseconds the host
 * may sleep before calling wd_run again (UINT64_MAX = until input). */
uint64_t wd_idle_ns(wd_machine *m);

int         wd_exit_code(wd_machine *m);
uint64_t    wd_icount(wd_machine *m);
uint64_t    wd_pc(wd_machine *m);
const char *wd_error(wd_machine *m);
void        wd_set_trace(wd_machine *m, int on);
/* Engine options: "no_fast" (general decoder only), "jit" (block translation, on by
 * default on AArch64 hosts). Returns 0 if the option is known and could be set. */
int         wd_set_option(wd_machine *m, const char *name, int value);
/* 1 if blocks are translated to host code (AArch64 host with executable memory). */
int         wd_jit_active(wd_machine *m);
void        wd_dump_state(wd_machine *m, char *buf, size_t len);
/* a text report for bug reports: hot code, exceptions, CPU and USB state */
size_t      wd_diag_report(wd_machine *m, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
