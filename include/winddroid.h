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

#define WD_VERSION "0.1.1"

typedef struct wd_machine wd_machine;

/* Physical memory map (same addresses as QEMU virt). */
#define WD_FLASH_BASE    0x00000000ULL
#define WD_GICD_BASE     0x08000000ULL
#define WD_GICR_BASE     0x080A0000ULL
#define WD_UART_BASE     0x09000000ULL
#define WD_RTC_BASE      0x09010000ULL
#define WD_CTL_BASE      0x09100000ULL  /* WindDroid control device */
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

wd_run_result wd_run(wd_machine *m, uint64_t max_insns);

/* When wd_run returned WD_RUN_IDLE in realtime mode: nanoseconds the host
 * may sleep before calling wd_run again (UINT64_MAX = until input). */
uint64_t wd_idle_ns(wd_machine *m);

int         wd_exit_code(wd_machine *m);
uint64_t    wd_icount(wd_machine *m);
uint64_t    wd_pc(wd_machine *m);
const char *wd_error(wd_machine *m);
void        wd_set_trace(wd_machine *m, int on);
void        wd_dump_state(wd_machine *m, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
