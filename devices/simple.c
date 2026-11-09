/*
 * WindDroid - simple devices: PL011 UART, PL031 RTC and the WindDroid
 * control device (exit code, instruction counter, host clock).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <time.h>

#include "../core/internal.h"

static const uint8_t pl011_id[8] = { 0x11, 0x10, 0x14, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };
static const uint8_t pl031_id[8] = { 0x31, 0x10, 0x04, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };

/* ---------------------------------------------------------------- PL011 */

#define UART_RXRIS (1u << 4)
#define UART_TXRIS (1u << 5)
#define UART_RTRIS (1u << 6)

void wd_uart_reset(wd_uart *u)
{
    memset(u, 0, sizeof *u);
    u->cr = 0x300;
    u->ifls = 0x12;
}

static uint32_t uart_ris(wd_uart *u)
{
    uint32_t r = UART_TXRIS | (u->ris & ~(UART_RXRIS | UART_RTRIS | UART_TXRIS));
    if (u->rx_count) r |= UART_RXRIS | UART_RTRIS;
    return r;
}

void wd_uart_update(wd_machine *m)
{
    wd_uart *u = &m->uart;
    wd_gic_set_line(m, WD_IRQ_UART, (uart_ris(u) & u->imsc) != 0);
}

uint64_t wd_uart_read(wd_machine *m, uint64_t off)
{
    wd_uart *u = &m->uart;
    if (off >= 0xFE0 && off < 0x1000) return pl011_id[(off - 0xFE0) / 4];
    switch (off) {
    case 0x000: {
        uint32_t c = 0;
        if (u->rx_count) {
            c = u->rx[u->rx_head];
            u->rx_head = (u->rx_head + 1) % UART_FIFO;
            u->rx_count--;
            wd_uart_update(m);
        }
        return c;
    }
    case 0x004: return 0;
    case 0x018: {
        uint32_t fr = 0x80;                     /* TXFE */
        if (!u->rx_count) fr |= 0x10;           /* RXFE */
        if (u->rx_count == UART_FIFO) fr |= 0x40;
        return fr;
    }
    case 0x020: return 0;                       /* ILPR */
    case 0x024: return u->ibrd;
    case 0x028: return u->fbrd;
    case 0x02C: return u->lcr_h;
    case 0x030: return u->cr;
    case 0x034: return u->ifls;
    case 0x038: return u->imsc;
    case 0x03C: return uart_ris(u);
    case 0x040: return uart_ris(u) & u->imsc;
    case 0x048: return u->dmacr;
    default: return 0;
    }
}

void wd_uart_write(wd_machine *m, uint64_t off, uint64_t val)
{
    wd_uart *u = &m->uart;
    switch (off) {
    case 0x000:
        if (m->uart_tx) m->uart_tx(m->uart_opaque, (uint8_t)val);
        break;
    case 0x004: break;
    case 0x024: u->ibrd = (uint32_t)val & 0xFFFF; break;
    case 0x028: u->fbrd = (uint32_t)val & 0x3F; break;
    case 0x02C: u->lcr_h = (uint32_t)val & 0xFF; break;
    case 0x030: u->cr = (uint32_t)val & 0xFFFF; break;
    case 0x034: u->ifls = (uint32_t)val & 0x3F; break;
    case 0x038: u->imsc = (uint32_t)val & 0x7FF; break;
    case 0x044: u->ris &= ~(uint32_t)val; break;
    case 0x048: u->dmacr = (uint32_t)val & 7; break;
    default: break;
    }
    wd_uart_update(m);
}

void wd_uart_input(wd_machine *m, const uint8_t *data, size_t len)
{
    wd_uart *u = &m->uart;
    for (size_t i = 0; i < len && u->rx_count < UART_FIFO; i++) {
        u->rx[(u->rx_head + u->rx_count) % UART_FIFO] = data[i];
        u->rx_count++;
    }
    wd_uart_update(m);
    m->wfi = 0;
}

/* ---------------------------------------------------------------- PL031 */

static uint32_t rtc_lr_offset;
static uint32_t rtc_imsc, rtc_mr;

uint64_t wd_rtc_read(wd_machine *m, uint64_t off)
{
    WD_UNUSED(m);
    if (off >= 0xFE0 && off < 0x1000) return pl031_id[(off - 0xFE0) / 4];
    switch (off) {
    case 0x000: return (uint32_t)time(NULL) + rtc_lr_offset;
    case 0x004: return rtc_mr;
    case 0x008: return (uint32_t)time(NULL) + rtc_lr_offset;
    case 0x00C: return 1;
    case 0x010: return rtc_imsc;
    default: return 0;
    }
}

void wd_rtc_write(wd_machine *m, uint64_t off, uint64_t val)
{
    WD_UNUSED(m);
    switch (off) {
    case 0x004: rtc_mr = (uint32_t)val; break;
    case 0x008: rtc_lr_offset = (uint32_t)val - (uint32_t)time(NULL); break;
    case 0x010: rtc_imsc = (uint32_t)val & 1; break;
    default: break;
    }
}

/* ---------------------------------------------------------------- control device */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t wd_ctl_read(wd_machine *m, uint64_t off)
{
    switch (off) {
    case 0x00: return 0x54434457;               /* "WDCT" */
    case 0x08: return m->cpu.icount;
    case 0x10: return mono_ns();
    case 0x18: return m->ram_size;
    case 0x20: return 0x000101;                 /* 0.1.1 */
    default: return 0;
    }
}

void wd_ctl_write(wd_machine *m, uint64_t off, uint64_t val)
{
    if (off == 0x00) {
        m->exit_code = (int)(uint32_t)val;
        m->stop = WD_RUN_SHUTDOWN;
    }
}
