/*
 * winddroid-vm - command line front end of the WindDroid engine.
 *
 *   winddroid-vm [options] image.{elf,bin}
 *     -m MB          RAM size in MiB (default 256)
 *     -n N           stop after N instructions (default: unlimited)
 *     -r             realtime counter (default: counter = instructions)
 *     -i TEXT        feed TEXT to the UART receiver before running
 *     -t             trace every instruction to stderr
 *     -q             do not print the summary line
 *     -b FILE        boot UEFI firmware (QEMU_EFI.fd) from flash; the image argument is then optional
 *     -v FILE        UEFI variable store (created if missing, written back on exit)
 *     -d FILE        write the generated device tree blob to FILE
 *     -s TEXT        stop with exit status 0 as soon as TEXT appears on the serial port
 *     -I             forward standard input to the serial port (interactive console)
 *     -k TEXT=INPUT  type INPUT on the serial port once TEXT has appeared (\\n, \\r, \\e escapes)
 *
 * Exit status: guest exit code (control device), 0 after PSCI SYSTEM_OFF,
 * 124 when the instruction limit is reached, 125 when the guest is idle
 * forever, 126 on emulator error.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

#include "winddroid.h"

const uint8_t *wd_get_dtb(wd_machine *m, size_t *len);

static volatile sig_atomic_t interrupted;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static const char *stop_text;
static size_t stop_pos;
static int stop_hit;

/* serial input scheduled after a given output text appears ("-k TEXT=INPUT", repeatable, in order) */
#define MAX_KEYS 16
static const char *key_after[MAX_KEYS], *key_input[MAX_KEYS];
static int nkeys, key_idx;
static size_t key_pos;
static int key_due;

static void tx(void *opaque, uint8_t ch)
{
    (void)opaque;
    fputc(ch, stdout);
    if (ch == '\n') fflush(stdout);
    if (stop_text && !stop_hit) {
        if ((char)ch == stop_text[stop_pos]) {
            if (!stop_text[++stop_pos]) stop_hit = 1;
        } else stop_pos = (char)ch == stop_text[0] ? 1 : 0;
    }
    if (key_idx < nkeys && !key_due) {
        const char *k = key_after[key_idx];
        if ((char)ch == k[key_pos]) {
            if (!k[++key_pos]) key_due = 1;
        } else key_pos = (char)ch == k[0] ? 1 : 0;
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    uint64_t ram_mb = 256, limit = 0;
    int realtime = 0, trace = 0, quiet = 0, opt, interactive = 0;
    const char *input = NULL, *bios = NULL, *vars_path = NULL, *dtb_path = NULL;

    while ((opt = getopt(argc, argv, "m:n:ri:tqhb:v:d:s:k:I")) != -1) {
        switch (opt) {
        case 'm': ram_mb = strtoull(optarg, NULL, 0); break;
        case 'n': limit = strtoull(optarg, NULL, 0); break;
        case 'r': realtime = 1; break;
        case 'i': input = optarg; break;
        case 't': trace = 1; break;
        case 'q': quiet = 1; break;
        case 'b': bios = optarg; break;
        case 'v': vars_path = optarg; break;
        case 'd': dtb_path = optarg; break;
        case 's': stop_text = optarg; break;
        case 'I': interactive = 1; realtime = 1; break;
        case 'k': {
            char *eq = strchr(optarg, '=');
            if (eq && nkeys < MAX_KEYS) { *eq = 0; key_after[nkeys] = optarg; key_input[nkeys++] = eq + 1; }
            break;
        }
        default:
            fprintf(stderr, "usage: %s [-m MB] [-n insns] [-r] [-i text] [-t] [-q] image\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc && !bios) {
        fprintf(stderr, "winddroid-vm %s: no image given\n", WD_VERSION);
        return 2;
    }

    size_t len = 0;
    uint8_t *img = NULL;
    if (optind < argc) {
        img = read_file(argv[optind], &len);
        if (!img) { perror(argv[optind]); return 2; }
    }

    wd_config cfg = { .ram_size = ram_mb << 20, .realtime = realtime };
    wd_machine *m = wd_create(&cfg);
    if (!m) { fprintf(stderr, "out of memory\n"); return 2; }
    wd_set_uart_tx(m, tx, NULL);
    wd_set_trace(m, trace);
    if (bios) {
        size_t blen, vlen = 0;
        uint8_t *code = read_file(bios, &blen), *vars = NULL;
        if (!code) { perror(bios); return 2; }
        if (vars_path) vars = read_file(vars_path, &vlen);
        if (wd_load_firmware(m, code, blen, vars, vlen) != 0) {
            fprintf(stderr, "firmware load failed: %s\n", wd_error(m));
            return 2;
        }
        free(code);
        free(vars);
    } else if (wd_load_image(m, img, len) != 0) {
        fprintf(stderr, "load failed: %s\n", wd_error(m));
        return 2;
    }
    free(img);
    if (dtb_path) {
        size_t dl;
        const uint8_t *d = wd_get_dtb(m, &dl);
        FILE *f = fopen(dtb_path, "wb");
        if (f && d) { fwrite(d, 1, dl, f); fclose(f); }
    }
    if (input) {
        /* allow "\n" escapes in the argument */
        char buf[4096];
        size_t n = 0;
        for (const char *p = input; *p && n < sizeof buf; p++) {
            if (p[0] == '\\' && p[1] == 'n') { buf[n++] = '\n'; p++; }
            else if (p[0] == '\\' && p[1] == 'r') { buf[n++] = '\r'; p++; }
            else buf[n++] = *p;
        }
        wd_uart_input(m, (const uint8_t *)buf, n);
    }

    double t0 = now_s();
    int status = 0;
    const char *why = "";
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    uint64_t executed = 0;
    for (;;) {
        if (interrupted) { status = 130; why = "interrupted"; break; }
        if (interactive) {
            fd_set fds;
            struct timeval tv = { 0, 0 };
            FD_ZERO(&fds);
            FD_SET(0, &fds);
            if (select(1, &fds, NULL, NULL, &tv) > 0) {
                uint8_t ib[256];
                ssize_t got = read(0, ib, sizeof ib);
                if (got > 0) {
                    for (ssize_t i = 0; i < got; i++) if (ib[i] == '\n') ib[i] = '\r';
                    wd_uart_input(m, ib, (size_t)got);
                } else interactive = 0;
            }
        }
        uint64_t chunk = 1000000;
        if (limit) {
            if (executed >= limit) { status = 124; why = "instruction limit"; break; }
            if (limit - executed < chunk) chunk = limit - executed;
        }
        wd_run_result r = wd_run(m, chunk);
        if (stop_hit) { status = 0; why = "stop text seen"; break; }
        if (key_due) {
            char buf[512];
            size_t n = 0;
            for (const char *p = key_input[key_idx]; *p && n < sizeof buf; p++) {
                if (p[0] == '\\' && p[1] == 'n') { buf[n++] = '\n'; p++; }
                else if (p[0] == '\\' && p[1] == 'r') { buf[n++] = '\r'; p++; }
                else if (p[0] == '\\' && p[1] == 'e') { buf[n++] = 27; p++; }
                else buf[n++] = *p;
            }
            wd_uart_input(m, (const uint8_t *)buf, n);
            key_due = 0;
            key_pos = 0;
            key_idx++;
        }
        if (r == WD_RUN_BUDGET) { executed += chunk; continue; }
        if (r == WD_RUN_SHUTDOWN) { status = wd_exit_code(m); why = "shutdown"; break; }
        if (r == WD_RUN_RESET) { status = 0; why = "reset requested"; break; }
        if (r == WD_RUN_IDLE) {
            uint64_t ns = wd_idle_ns(m);
            if (realtime && (ns != UINT64_MAX || interactive)) {
                if (ns > 20000000ULL) ns = 20000000ULL;       /* stay responsive to input */
                struct timespec ts = { (time_t)(ns / 1000000000ULL), (long)(ns % 1000000000ULL) };
                nanosleep(&ts, NULL);
                continue;
            }
            status = 125; why = "idle forever (WFI with no wake-up source)";
            break;
        }
        status = 126;
        why = wd_error(m);
        break;
    }
    double dt = now_s() - t0;
    fflush(stdout);
    if (status >= 124 || !quiet) {
        fprintf(stderr, "\n[winddroid] %s: exit=%d, %llu instructions, %.2f s, %.1f MIPS\n", why, status,
                (unsigned long long)wd_icount(m), dt, dt > 0 ? (double)wd_icount(m) / dt / 1e6 : 0.0);
    }
    if (status >= 124) {
        char st[4096];
        wd_dump_state(m, st, sizeof st);
        fputs(st, stderr);
    }
    if (vars_path) {
        size_t vl;
        int dirty;
        const uint8_t *v = wd_firmware_vars(m, &vl, &dirty);
        if (v && dirty) {
            FILE *f = fopen(vars_path, "wb");
            if (f) { fwrite(v, 1, vl, f); fclose(f); }
        }
    }
    wd_destroy(m);
    return status;
}
