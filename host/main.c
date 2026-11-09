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
#include <unistd.h>

#include "winddroid.h"

static void tx(void *opaque, uint8_t ch)
{
    (void)opaque;
    fputc(ch, stdout);
    if (ch == '\n') fflush(stdout);
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
    int realtime = 0, trace = 0, quiet = 0, opt;
    const char *input = NULL;

    while ((opt = getopt(argc, argv, "m:n:ri:tqh")) != -1) {
        switch (opt) {
        case 'm': ram_mb = strtoull(optarg, NULL, 0); break;
        case 'n': limit = strtoull(optarg, NULL, 0); break;
        case 'r': realtime = 1; break;
        case 'i': input = optarg; break;
        case 't': trace = 1; break;
        case 'q': quiet = 1; break;
        default:
            fprintf(stderr, "usage: %s [-m MB] [-n insns] [-r] [-i text] [-t] [-q] image\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "winddroid-vm %s: no image given\n", WD_VERSION);
        return 2;
    }

    size_t len;
    uint8_t *img = read_file(argv[optind], &len);
    if (!img) { perror(argv[optind]); return 2; }

    wd_config cfg = { .ram_size = ram_mb << 20, .realtime = realtime };
    wd_machine *m = wd_create(&cfg);
    if (!m) { fprintf(stderr, "out of memory\n"); return 2; }
    wd_set_uart_tx(m, tx, NULL);
    wd_set_trace(m, trace);
    if (wd_load_image(m, img, len) != 0) {
        fprintf(stderr, "load failed: %s\n", wd_error(m));
        return 2;
    }
    free(img);
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
    uint64_t executed = 0;
    for (;;) {
        uint64_t chunk = 1000000;
        if (limit) {
            if (executed >= limit) { status = 124; why = "instruction limit"; break; }
            if (limit - executed < chunk) chunk = limit - executed;
        }
        wd_run_result r = wd_run(m, chunk);
        if (r == WD_RUN_BUDGET) { executed += chunk; continue; }
        if (r == WD_RUN_SHUTDOWN) { status = wd_exit_code(m); why = "shutdown"; break; }
        if (r == WD_RUN_RESET) { status = 0; why = "reset requested"; break; }
        if (r == WD_RUN_IDLE) {
            uint64_t ns = wd_idle_ns(m);
            if (realtime && ns != UINT64_MAX) {
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
        char st[2048];
        wd_dump_state(m, st, sizeof st);
        fputs(st, stderr);
    }
    wd_destroy(m);
    return status;
}
