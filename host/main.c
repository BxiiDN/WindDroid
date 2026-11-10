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
 *     -A DIR         write the fw_cfg ACPI files (tables, rsdp, table-loader) to DIR and exit
 *     -F FILE        save the framebuffer as a binary PPM image on exit
 *     -I             forward standard input to the serial port (interactive console)
 *     -k TEXT=INPUT  type INPUT on the serial port once TEXT has appeared (\\n, \\r, \\e escapes)
 *     -D FILE        attach a disk image (raw/IMG, VHDX, VHD) as an NVMe SSD; "ro:FILE" opens it read-only
 *     -c FILE        attach an ISO image as a USB DVD drive
 *     -u FILE        attach a disk image as a USB disk ("ro:FILE" read-only)
 *     -U             attach a USB keyboard and a USB tablet
 *     -X             general instruction decoder only (no fast paths), for testing
 *     -J             translate blocks to host code (AArch64 hosts only)
 *     -T             print a state checksum after every instruction / block (debugging)
 *     -K TEXT=INPUT  type INPUT on the USB keyboard once TEXT has appeared on the serial port
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
const uint8_t *wd_fwcfg_blob(wd_machine *m, const char *name, size_t *len);

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

#define MAX_DISKS 8
static const char *disks[MAX_DISKS];
static int disk_kind[MAX_DISKS];
static int ndisks, usb_hid, no_fast, use_jit;
static int key_usb[MAX_KEYS];
/* pending USB keystrokes: one key change per run slice so the guest sees every report */
static int usb_type_ev[4096][2];
static int usb_type_n, usb_type_pos;

static void usb_type_queue(const char *text, size_t n)
{
    for (size_t i = 0; i < n && usb_type_n + 4 <= 4096; i++) {
        int shift, u = wd_ascii_to_hid((unsigned char)text[i], &shift);
        if (!u) continue;
        if (shift) { usb_type_ev[usb_type_n][0] = 0xE1; usb_type_ev[usb_type_n++][1] = 1; }
        usb_type_ev[usb_type_n][0] = u; usb_type_ev[usb_type_n++][1] = 1;
        usb_type_ev[usb_type_n][0] = u; usb_type_ev[usb_type_n++][1] = 0;
        if (shift) { usb_type_ev[usb_type_n][0] = 0xE1; usb_type_ev[usb_type_n++][1] = 0; }
    }
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
    const char *input = NULL, *bios = NULL, *vars_path = NULL, *dtb_path = NULL, *fb_path = NULL, *acpi_dir = NULL;

    while ((opt = getopt(argc, argv, "m:n:ri:tqhb:v:d:s:k:IF:A:D:c:u:UK:XJT")) != -1) {
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
        case 'F': fb_path = optarg; break;
        case 'A': acpi_dir = optarg; break;
        case 'D': case 'c': case 'u':
            if (ndisks < MAX_DISKS) { disks[ndisks] = optarg; disk_kind[ndisks++] = opt; }
            break;
        case 'U': usb_hid = 1; break;
        case 'X': no_fast = 1; break;
        case 'J': use_jit = 1; break;
        case 'T': trace = 2; break;
        case 'K': {
            char *eq = strchr(optarg, '=');
            if (eq && nkeys < MAX_KEYS) { *eq = 0; key_after[nkeys] = optarg; key_input[nkeys] = eq + 1; key_usb[nkeys++] = 1; }
            usb_hid = 1;
            break;
        }
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
    if (no_fast) wd_set_option(m, "no_fast", 1);
    if (use_jit && wd_set_option(m, "jit", 1)) fprintf(stderr, "winddroid-vm: JIT not available on this host\n");
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
    for (int i = 0; i < ndisks; i++) {
        const char *path = disks[i];
        int ro = strncmp(path, "ro:", 3) == 0;
        if (ro) path += 3;
        if (disk_kind[i] == 'c') ro = 1;
        int fd = open(path, ro ? O_RDONLY : O_RDWR);
        if (fd < 0) { perror(path); return 2; }
        wd_disk d;
        char err[256];
        if (wd_disk_open_image(fd, ro, 1, &d, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", path, err);
            return 2;
        }
        int r = disk_kind[i] == 'D' ? wd_attach_nvme(m, &d) : wd_attach_usb_storage(m, &d, disk_kind[i] == 'c');
        if (r != 0) {
            fprintf(stderr, "%s: %s\n", path, wd_error(m));
            return 2;
        }
    }
    if (usb_hid && (wd_attach_usb_keyboard(m) || wd_attach_usb_tablet(m))) {
        fprintf(stderr, "USB: %s\n", wd_error(m));
        return 2;
    }
    if (acpi_dir) {
        const char *names[] = { "etc/acpi/tables", "etc/acpi/rsdp", "etc/table-loader" };
        const char *files[] = { "tables.bin", "rsdp.bin", "loader.bin" };
        for (int i = 0; i < 3; i++) {
            size_t fl;
            const uint8_t *d = wd_fwcfg_blob(m, names[i], &fl);
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", acpi_dir, files[i]);
            FILE *f = fopen(path, "wb");
            if (f && d) fwrite(d, 1, fl, f);
            if (f) fclose(f);
        }
        wd_destroy(m);
        return 0;
    }
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
        if (usb_type_pos < usb_type_n) {
            wd_key_event(m, usb_type_ev[usb_type_pos][0], usb_type_ev[usb_type_pos][1]);
            usb_type_pos++;
            if (chunk > 300000) chunk = 300000;
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
            if (key_usb[key_idx]) usb_type_queue(buf, n);
            else wd_uart_input(m, (const uint8_t *)buf, n);
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
    if (fb_path) {
        wd_fb_info fb;
        FILE *f = fopen(fb_path, "wb");
        if (f && wd_framebuffer(m, &fb)) {
            fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
            for (uint32_t y = 0; y < fb.height; y++)
                for (uint32_t x = 0; x < fb.width; x++) {
                    const uint8_t *p = fb.pixels + (size_t)y * fb.stride + 4 * x;
                    uint8_t rgb[3] = { p[2], p[1], p[0] };
                    fwrite(rgb, 1, 3, f);
                }
        }
        if (f) fclose(f);
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
