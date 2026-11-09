/*
 * disktool - exercise the disk image layer from the command line (tests).
 *   disktool info IMG
 *   disktool open-rw IMG                       open read-write (replays a VHDX log) and close
 *   disktool check IMG OFFSET LENGTH BYTE      every byte must equal BYTE
 *   disktool fill IMG OFFSET LENGTH BYTE       write BYTE over the range
 *   disktool dump IMG [OFFSET LENGTH]          raw contents to stdout
 *   disktool load IMG OFFSET < data            write stdin at OFFSET
 *   disktool create OUT.vhdx SIZE [BLOCK [SECTOR]]
 *   disktool convert IN OUT.vhdx               copy any image into a new VHDX
 *   disktool fuzz IMG SEED OPS [SPAN]          random writes/reads against a RAM mirror of the
 *                                              first SPAN bytes (default 96M), then reopen and compare
 * Sizes accept K/M/G suffixes. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "winddroid.h"

static uint64_t num(const char *s)
{
    char *e;
    uint64_t v = strtoull(s, &e, 0);
    if (*e == 'K' || *e == 'k') v <<= 10;
    else if (*e == 'M' || *e == 'm') v <<= 20;
    else if (*e == 'G' || *e == 'g') v <<= 30;
    return v;
}

static int open_disk(const char *path, int rw, wd_disk *d)
{
    int fd = open(path, rw ? O_RDWR : O_RDONLY);
    if (fd < 0) { perror(path); exit(2); }
    char err[256];
    if (wd_disk_open_image(fd, !rw, 1, d, err, sizeof err)) { fprintf(stderr, "%s: %s\n", path, err); exit(3); }
    return 0;
}

#define CHUNK (4u << 20)

static uint64_t rnd(uint64_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    return *x;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    const char *cmd = argv[1];
    wd_disk d;
    static uint8_t buf[CHUNK];
    if (!strcmp(cmd, "info")) {
        open_disk(argv[2], 0, &d);
        static const char *names[] = { "raw", "iso", "vhd", "vhdx" };
        printf("format=%s size=%llu block=%u\n", names[d.format], (unsigned long long)d.size, d.block_size ? d.block_size : 512);
        d.close(d.opaque);
        return 0;
    }
    if (!strcmp(cmd, "open-rw")) {
        open_disk(argv[2], 1, &d);
        d.close(d.opaque);
        return 0;
    }
    if ((!strcmp(cmd, "check") || !strcmp(cmd, "fill")) && argc == 6) {
        int fill = !strcmp(cmd, "fill");
        open_disk(argv[2], fill, &d);
        uint64_t off = num(argv[3]), len = num(argv[4]);
        int byte = (int)strtol(argv[5], NULL, 0);
        if (off + len > d.size) { fprintf(stderr, "range beyond the disk\n"); return 4; }
        while (len) {
            size_t n = len < CHUNK ? (size_t)len : CHUNK;
            if (fill) {
                memset(buf, byte, n);
                if (d.write(d.opaque, off, buf, n)) { fprintf(stderr, "write error at %llu\n", (unsigned long long)off); return 5; }
            } else {
                if (d.read(d.opaque, off, buf, n)) { fprintf(stderr, "read error at %llu\n", (unsigned long long)off); return 5; }
                for (size_t i = 0; i < n; i++)
                    if (buf[i] != byte) {
                        fprintf(stderr, "mismatch at %llu: %02x != %02x\n", (unsigned long long)(off + i), buf[i], byte);
                        return 1;
                    }
            }
            off += n; len -= n;
        }
        if (fill && d.flush(d.opaque)) return 5;
        d.close(d.opaque);
        return 0;
    }
    if (!strcmp(cmd, "dump")) {
        open_disk(argv[2], 0, &d);
        uint64_t off = argc > 3 ? num(argv[3]) : 0, len = argc > 4 ? num(argv[4]) : d.size - off;
        while (len) {
            size_t n = len < CHUNK ? (size_t)len : CHUNK;
            if (d.read(d.opaque, off, buf, n)) return 5;
            fwrite(buf, 1, n, stdout);
            off += n; len -= n;
        }
        d.close(d.opaque);
        return 0;
    }
    if (!strcmp(cmd, "load") && argc == 4) {
        open_disk(argv[2], 1, &d);
        uint64_t off = num(argv[3]);
        size_t n;
        while ((n = fread(buf, 1, CHUNK, stdin)) > 0) {
            if (d.write(d.opaque, off, buf, n)) return 5;
            off += n;
        }
        if (d.flush(d.opaque)) return 5;
        d.close(d.opaque);
        return 0;
    }
    if (!strcmp(cmd, "create") && argc >= 4) {
        int fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror(argv[2]); return 2; }
        if (wd_vhdx_create(fd, num(argv[3]), argc > 4 ? (uint32_t)num(argv[4]) : 0, argc > 5 ? (uint32_t)num(argv[5]) : 0)) {
            fprintf(stderr, "create failed\n");
            return 5;
        }
        close(fd);
        return 0;
    }
    if (!strcmp(cmd, "convert") && argc == 4) {
        wd_disk in, out;
        open_disk(argv[2], 0, &in);
        int fd = open(argv[3], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || wd_vhdx_create(fd, in.size, 0, in.block_size == 4096 ? 4096 : 512)) { fprintf(stderr, "create failed\n"); return 5; }
        close(fd);
        open_disk(argv[3], 1, &out);
        for (uint64_t off = 0; off < in.size; off += CHUNK) {
            size_t n = in.size - off < CHUNK ? (size_t)(in.size - off) : CHUNK;
            if (in.read(in.opaque, off, buf, n)) return 5;
            size_t z = 0;
            while (z < n && !buf[z]) z++;
            if (z == n) continue;                      /* the new image reads as zero already */
            if (out.write(out.opaque, off, buf, n)) return 5;
        }
        if (out.flush(out.opaque)) return 5;
        in.close(in.opaque);
        out.close(out.opaque);
        return 0;
    }
    if (!strcmp(cmd, "fuzz") && argc >= 5) {
        uint64_t seed = num(argv[3]), ops = num(argv[4]);
        open_disk(argv[2], 1, &d);
        uint64_t span = argc > 5 ? num(argv[5]) : 96u << 20;
        if (span > d.size) span = d.size;
        span &= ~511ULL;
        uint8_t *mirror = malloc(span), *tmp = malloc(span);
        if (!mirror || !tmp) return 6;
        for (uint64_t off = 0; off < span; off += CHUNK)
            if (d.read(d.opaque, off, mirror + off, span - off < CHUNK ? span - off : CHUNK)) return 5;
        uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
        for (uint64_t i = 0; i < ops; i++) {
            uint64_t sectors = span / 512;
            uint64_t s0 = rnd(&x) % sectors;
            uint64_t n = 1 + rnd(&x) % (rnd(&x) % 4 == 0 ? 8192 : 64);
            if (s0 + n > sectors) n = sectors - s0;
            uint64_t off = s0 * 512, len = n * 512;
            int kind = (int)(rnd(&x) % 4);
            if (kind < 2) {
                uint8_t pat = (uint8_t)rnd(&x);
                for (uint64_t k = 0; k < len; k++) tmp[k] = kind == 0 ? (uint8_t)(pat + k * 7 + (k >> 9)) : 0;
                if (d.write(d.opaque, off, tmp, len)) { fprintf(stderr, "write error\n"); return 5; }
                memcpy(mirror + off, tmp, len);
            } else {
                if (d.read(d.opaque, off, tmp, len)) { fprintf(stderr, "read error\n"); return 5; }
                if (memcmp(tmp, mirror + off, len)) { fprintf(stderr, "op %llu: read mismatch at %llu+%llu\n", (unsigned long long)i, (unsigned long long)off, (unsigned long long)len); return 1; }
            }
        }
        if (d.flush(d.opaque)) return 5;
        d.close(d.opaque);
        open_disk(argv[2], 0, &d);
        for (uint64_t off = 0; off < span; off += CHUNK) {
            size_t n = span - off < CHUNK ? (size_t)(span - off) : CHUNK;
            if (d.read(d.opaque, off, tmp, n)) return 5;
            if (memcmp(tmp, mirror + off, n)) { fprintf(stderr, "after reopen: mismatch near %llu\n", (unsigned long long)off); return 1; }
        }
        d.close(d.opaque);
        free(mirror); free(tmp);
        return 0;
    }
    fprintf(stderr, "bad command\n");
    return 2;
}
