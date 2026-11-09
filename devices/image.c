/*
 * WindDroid - disk image formats behind a file descriptor:
 *
 *   raw / IMG   sector image, also used for ISO 9660 (CD/DVD) images
 *   VHDX        Hyper-V virtual hard disk v2 ([MS-VHDX]): dynamic and fixed,
 *               512 or 4096-byte logical sectors, log replay, block allocation
 *   VHD         Virtual PC / Hyper-V v1 (fixed and dynamic)
 *
 * Nothing is loaded into RAM besides the block allocation tables, so
 * images of any size work on a phone. Differencing images (with a parent)
 * are refused with a clear message.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../core/internal.h"

#define KiB 1024ULL
#define MiB (1024ULL * 1024ULL)

static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) { memset(p, 0, len); return 0; }
        p += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return 0;
}

static int pwrite_all(int fd, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; off += (uint64_t)n; len -= (size_t)n;
    }
    return 0;
}

static int sync_fd(int fd)
{
    if (fdatasync(fd) == 0) return 0;
    return errno == EINVAL || errno == EROFS ? 0 : -1;
}

static uint64_t file_size(int fd)
{
    struct stat st;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end > 0) return (uint64_t)end;
    return fstat(fd, &st) == 0 && st.st_size > 0 ? (uint64_t)st.st_size : 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put_be32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* CRC-32C (Castagnoli), reflected, as used by VHDX */
static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void random_guid(uint8_t g[16])
{
    static uint64_t state;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, g, 16);
        close(fd);
        if (n == 16) goto done;
    }
    state ^= (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL + (uint64_t)clock();
    for (int i = 0; i < 16; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        g[i] = (uint8_t)state;
    }
done:
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);     /* version 4 (Data3 high nibble, little-endian layout) */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

/* GUID in the on-disk (little-endian Data1..3) layout */
#define GUID(d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7) \
    { (d1) & 0xFF, ((d1) >> 8) & 0xFF, ((d1) >> 16) & 0xFF, ((d1) >> 24) & 0xFF, \
      (d2) & 0xFF, ((d2) >> 8) & 0xFF, (d3) & 0xFF, ((d3) >> 8) & 0xFF, b0, b1, b2, b3, b4, b5, b6, b7 }

/* ================================================================== raw */

int wd_image_probe(int fd)
{
    uint8_t b[512];
    if (pread_all(fd, b, 8, 0)) return WD_IMG_RAW;
    if (!memcmp(b, "vhdxfile", 8)) return WD_IMG_VHDX;
    if (!memcmp(b, "conectix", 8)) return WD_IMG_VHD;            /* dynamic disks keep a footer copy at 0 */
    uint64_t size = file_size(fd);
    if (size >= 512 && !pread_all(fd, b, 512, size - 512) && !memcmp(b, "conectix", 8)) return WD_IMG_VHD;
    if (size >= 511 && !pread_all(fd, b, 8, size - 511) && !memcmp(b, "conectix", 8)) return WD_IMG_VHD;
    if (!pread_all(fd, b, 5, 0x8001) && !memcmp(b, "CD001", 5)) return WD_IMG_ISO;
    return WD_IMG_RAW;
}

/* ================================================================== VHDX */

static const uint8_t G_BAT[16]   = GUID(0x2DC27766, 0xF623, 0x4200, 0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08);
static const uint8_t G_META[16]  = GUID(0x8B7CA206, 0x4790, 0x4B9A, 0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E);
static const uint8_t G_FPARAM[16] = GUID(0xCAA16737, 0xFA36, 0x4D43, 0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B);
static const uint8_t G_VSIZE[16] = GUID(0x2FA54224, 0xCD1B, 0x4876, 0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8);
static const uint8_t G_DISKID[16] = GUID(0xBECA12AB, 0xB2E6, 0x4523, 0x93, 0xEF, 0xC3, 0x09, 0xE0, 0x00, 0xC7, 0x46);
static const uint8_t G_LSS[16]   = GUID(0x8141BF1D, 0xA96F, 0x4709, 0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F);
static const uint8_t G_PSS[16]   = GUID(0xCDA348C7, 0x445D, 0x4471, 0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56);
static const uint8_t G_PARENT[16] = GUID(0xA8D35F2D, 0xB30B, 0x454D, 0xAB, 0xF7, 0xD3, 0xD8, 0x48, 0x34, 0xAB, 0x0C);

enum { PAYLOAD_NOT_PRESENT = 0, PAYLOAD_UNDEFINED = 1, PAYLOAD_ZERO = 2, PAYLOAD_UNMAPPED = 3,
       PAYLOAD_FULLY_PRESENT = 6, PAYLOAD_PARTIALLY_PRESENT = 7 };

typedef struct {
    int fd, own, readonly;
    uint8_t header[4096];       /* current header */
    int cur_slot;               /* 0: 64 KiB, 1: 128 KiB */
    uint64_t bat_off;
    uint32_t bat_len;
    uint64_t *bat;
    uint32_t bat_entries;
    uint32_t block_size, lss, chunk_ratio;
    uint64_t vsize;
    uint64_t file_end;
    int written;                /* headers updated for this session */
} vhdx;

static int vhdx_header_valid(const uint8_t *h)
{
    if (memcmp(h, "head", 4)) return 0;
    uint8_t tmp[4096];
    memcpy(tmp, h, 4096);
    memset(tmp + 4, 0, 4);
    return crc32c(0, tmp, 4096) == le32(h + 4) && le16(h + 66) == 1;   /* version 1 */
}

/* Write a new header (sequence + 1) to both slots, the older one first. */
static int vhdx_write_header(vhdx *v, const uint8_t *log_guid, int new_write_guids)
{
    uint8_t h[4096];
    memcpy(h, v->header, 4096);
    put_le64(h + 8, le64(v->header + 8) + 1);
    if (new_write_guids) {
        random_guid(h + 16);                 /* FileWriteGuid */
        random_guid(h + 32);                 /* DataWriteGuid */
    }
    memcpy(h + 48, log_guid, 16);
    memset(h + 4, 0, 4);
    put_le32(h + 4, crc32c(0, h, 4096));
    int other = v->cur_slot ^ 1;
    if (pwrite_all(v->fd, h, 4096, (uint64_t)(other + 1) * 64 * KiB) || sync_fd(v->fd)) return -1;
    memcpy(v->header, h, 4096);
    v->cur_slot = other;
    /* second update so both copies are current (spec 2.2.2.1) */
    put_le64(h + 8, le64(h + 8) + 1);
    memset(h + 4, 0, 4);
    put_le32(h + 4, crc32c(0, h, 4096));
    other = v->cur_slot ^ 1;
    if (pwrite_all(v->fd, h, 4096, (uint64_t)(other + 1) * 64 * KiB) || sync_fd(v->fd)) return -1;
    memcpy(v->header, h, 4096);
    v->cur_slot = other;
    return 0;
}

/* ---------------------------------------------------------------- log replay */

typedef struct { uint64_t off, seq; uint32_t len, tail; } log_ent;

/* Read the log entry at 'pos' (offset within the log); returns 1 if valid. */
static int log_entry_at(vhdx *v, uint64_t log_off, uint32_t log_len, uint32_t pos, uint8_t **out, log_ent *e)
{
    uint8_t h[64];
    if (pread_all(v->fd, h, 64, log_off + pos)) return 0;
    if (memcmp(h, "loge", 4)) return 0;
    uint32_t len = le32(h + 8);
    if (!len || len % 4096 || len > log_len) return 0;
    uint64_t seq = le64(h + 16);
    if (!seq || memcmp(h + 32, v->header + 48, 16)) return 0;           /* LogGuid must match */
    uint32_t ndesc = le32(h + 24);
    if ((uint64_t)ndesc * 32 + 64 > len) return 0;
    uint8_t *buf = malloc(len);
    if (!buf) return 0;
    for (uint32_t done = 0; done < len; done += 4096) {                 /* the log is circular */
        uint32_t p = (pos + done) % log_len;
        if (pread_all(v->fd, buf + done, 4096, log_off + p)) { free(buf); return 0; }
    }
    uint32_t crc = le32(buf + 4);
    memset(buf + 4, 0, 4);
    int ok = crc32c(0, buf, len) == crc;
    put_le32(buf + 4, crc);
    /* descriptors must belong to this entry */
    uint32_t desc_sectors = (ndesc + 2 + 127) / 128, data_used = 0;
    for (uint32_t i = 0; ok && i < ndesc; i++) {
        const uint8_t *d = buf + 64 + 32 * i;
        if (le64(d + 24) != seq || le64(d + 16) % 4096) ok = 0;
        else if (!memcmp(d, "desc", 4)) data_used++;
        else if (memcmp(d, "zero", 4) || le64(d + 8) % 4096) ok = 0;
    }
    if (ok && (desc_sectors + data_used) * 4096ULL > len) ok = 0;
    if (!ok) { free(buf); return 0; }
    e->off = pos; e->seq = seq; e->len = len; e->tail = le32(h + 12);
    if (out) *out = buf; else free(buf);
    return 1;
}

static int vhdx_apply_entry(vhdx *v, const uint8_t *buf, uint64_t *max_end)
{
    uint32_t ndesc = le32(buf + 24);
    uint32_t data_sector = (ndesc + 2 + 127) / 128;
    uint8_t sec[4096];
    for (uint32_t i = 0; i < ndesc; i++) {
        const uint8_t *d = buf + 64 + 32 * i;
        uint64_t off = le64(d + 16), seq = le64(d + 24);
        if (!memcmp(d, "zero", 4)) {
            uint64_t n = le64(d + 8);
            memset(sec, 0, sizeof sec);
            for (uint64_t k = 0; k < n; k += 4096)
                if (pwrite_all(v->fd, sec, 4096, off + k)) return -1;
            if (off + n > *max_end) *max_end = off + n;
        } else {
            const uint8_t *s = buf + 4096ULL * data_sector++;
            if (memcmp(s, "data", 4) || ((uint64_t)le32(s + 4) << 32 | le32(s + 4092)) != seq) return -1;
            memcpy(sec, d + 8, 8);                    /* leading bytes */
            memcpy(sec + 8, s + 8, 4084);
            memcpy(sec + 4092, d + 4, 4);             /* trailing bytes */
            if (pwrite_all(v->fd, sec, 4096, off)) return -1;
            if (off + 4096 > *max_end) *max_end = off + 4096;
        }
    }
    uint64_t last = le64(buf + 56);
    if (last > *max_end) *max_end = last;
    return 0;
}

static int vhdx_replay_log(vhdx *v, char *err, size_t errlen)
{
    uint64_t log_off = le64(v->header + 72);
    uint32_t log_len = le32(v->header + 68);
    if (!log_len || log_len % MiB || log_off % MiB) { snprintf(err, errlen, "VHDX log location is invalid"); return -1; }
    log_ent *ents = NULL;
    size_t n = 0;
    for (uint32_t pos = 0; pos < log_len; pos += 4096) {
        log_ent e;
        if (!log_entry_at(v, log_off, log_len, pos, NULL, &e)) continue;
        log_ent *ne = realloc(ents, (n + 1) * sizeof *ents);
        if (!ne) { free(ents); snprintf(err, errlen, "out of memory"); return -1; }
        ents = ne;
        ents[n++] = e;
    }
    /* the active sequence ends at the newest entry whose tail chain is intact */
    int64_t best = -1;
    for (;;) {
        int64_t head = -1;
        for (size_t i = 0; i < n; i++)
            if (ents[i].seq && (head < 0 || ents[i].seq > ents[head].seq)) head = (int64_t)i;
        if (head < 0) break;
        /* walk from the head's tail to the head */
        uint32_t pos = ents[head].tail;
        uint64_t expect = 0;
        int ok = 0;
        for (size_t steps = 0; steps <= n; steps++) {
            log_ent e;
            if (!log_entry_at(v, log_off, log_len, pos, NULL, &e)) break;
            if (expect && e.seq != expect) break;
            if (e.off == ents[head].off && e.seq == ents[head].seq) { ok = 1; break; }
            expect = e.seq + 1;
            pos = (pos + e.len) % log_len;
        }
        if (ok) { best = head; break; }
        ents[head].seq = 0;                              /* not usable, try the next newest */
    }
    if (best < 0) {
        free(ents);
        snprintf(err, errlen, "VHDX log is dirty but has no valid entries");
        return -1;
    }
    uint64_t max_end = 0;
    uint32_t pos = ents[best].tail;
    for (;;) {
        uint8_t *buf;
        log_ent e;
        if (!log_entry_at(v, log_off, log_len, pos, &buf, &e)) { free(ents); snprintf(err, errlen, "VHDX log read failed"); return -1; }
        uint64_t flushed = le64(buf + 48);
        if (flushed > file_size(v->fd)) {
            free(buf); free(ents);
            snprintf(err, errlen, "VHDX file is truncated (log flushed offset beyond end of file)");
            return -1;
        }
        int r = vhdx_apply_entry(v, buf, &max_end);
        free(buf);
        if (r) { free(ents); snprintf(err, errlen, "VHDX log replay failed"); return -1; }
        if (e.off == ents[best].off) break;
        pos = (pos + e.len) % log_len;
    }
    free(ents);
    uint64_t size = file_size(v->fd);
    if (max_end > size) {
        uint64_t ns = (max_end + MiB - 1) & ~(MiB - 1);
        if (ftruncate(v->fd, (off_t)ns)) { snprintf(err, errlen, "cannot grow the VHDX file"); return -1; }
    }
    if (sync_fd(v->fd)) { snprintf(err, errlen, "flush failed"); return -1; }
    static const uint8_t zero[16];
    if (vhdx_write_header(v, zero, 0)) { snprintf(err, errlen, "cannot write the VHDX header"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- I/O */

static uint64_t vhdx_bat_index(const vhdx *v, uint64_t block)
{
    return block + block / v->chunk_ratio;
}

static int vhdx_read(void *o, uint64_t off, void *buf, size_t len)
{
    vhdx *v = o;
    uint8_t *p = buf;
    while (len) {
        uint64_t block = off / v->block_size, in = off % v->block_size;
        size_t n = v->block_size - in < len ? (size_t)(v->block_size - in) : len;
        uint64_t e = v->bat[vhdx_bat_index(v, block)];
        int state = (int)(e & 7);
        if (state == PAYLOAD_FULLY_PRESENT || state == PAYLOAD_PARTIALLY_PRESENT) {
            if (pread_all(v->fd, p, n, (e >> 20) * MiB + in)) return -1;
        } else {
            memset(p, 0, n);
        }
        p += n; off += n; len -= n;
    }
    return 0;
}

static int vhdx_begin_write(vhdx *v)
{
    if (v->written) return 0;
    static const uint8_t zero[16];
    if (vhdx_write_header(v, zero, 1)) return -1;
    v->written = 1;
    return 0;
}

static int vhdx_write(void *o, uint64_t off, const void *buf, size_t len)
{
    vhdx *v = o;
    const uint8_t *p = buf;
    if (vhdx_begin_write(v)) return -1;
    while (len) {
        uint64_t block = off / v->block_size, in = off % v->block_size;
        size_t n = v->block_size - in < len ? (size_t)(v->block_size - in) : len;
        uint64_t idx = vhdx_bat_index(v, block);
        uint64_t e = v->bat[idx];
        int state = (int)(e & 7);
        if (state != PAYLOAD_FULLY_PRESENT) {
            if (all_zero(p, n)) { p += n; off += n; len -= n; continue; }   /* reads back as zero anyway */
            uint64_t at = (v->file_end + MiB - 1) & ~(MiB - 1);
            if (ftruncate(v->fd, (off_t)(at + v->block_size))) return -1;  /* new block reads as zeros */
            if (pwrite_all(v->fd, p, n, at + in) || sync_fd(v->fd)) return -1;
            uint8_t le[8];
            e = (at / MiB) << 20 | PAYLOAD_FULLY_PRESENT;
            put_le64(le, e);
            if (pwrite_all(v->fd, le, 8, v->bat_off + idx * 8)) return -1;
            v->bat[idx] = e;
            v->file_end = at + v->block_size;
        } else if (pwrite_all(v->fd, p, n, (e >> 20) * MiB + in)) {
            return -1;
        }
        p += n; off += n; len -= n;
    }
    return 0;
}

static int vhdx_flush(void *o) { return sync_fd(((vhdx *)o)->fd); }

static void vhdx_close(void *o)
{
    vhdx *v = o;
    if (v->own) close(v->fd);
    free(v->bat);
    free(v);
}

static int vhdx_open(int fd, int readonly, int own, wd_disk *out, char *err, size_t errlen)
{
    vhdx *v = calloc(1, sizeof *v);
    if (!v) { snprintf(err, errlen, "out of memory"); return -1; }
    v->fd = fd; v->own = own; v->readonly = readonly;
    uint8_t h[2][4096];
    if (pread_all(fd, h[0], 4096, 64 * KiB) || pread_all(fd, h[1], 4096, 128 * KiB)) goto io;
    int ok0 = vhdx_header_valid(h[0]), ok1 = vhdx_header_valid(h[1]);
    if (!ok0 && !ok1) { snprintf(err, errlen, "VHDX headers are corrupt"); goto fail; }
    v->cur_slot = ok0 && ok1 ? (le64(h[1] + 8) > le64(h[0] + 8)) : ok1;
    memcpy(v->header, h[v->cur_slot], 4096);

    static const uint8_t zero[16];
    if (memcmp(v->header + 48, zero, 16)) {
        if (readonly) {
            snprintf(err, errlen, "VHDX image has a log that must be replayed; open it read-write once");
            goto fail;
        }
        if (vhdx_replay_log(v, err, errlen)) goto fail;
    }

    /* region table */
    uint8_t rt[64 * 1024];
    int have = 0;
    for (int copy = 0; copy < 2 && !have; copy++) {
        if (pread_all(fd, rt, sizeof rt, (192 + 64 * copy) * KiB)) goto io;
        if (memcmp(rt, "regi", 4)) continue;
        uint32_t crc = le32(rt + 4);
        memset(rt + 4, 0, 4);
        if (crc32c(0, rt, sizeof rt) == crc && le32(rt + 8) <= 2047) have = 1;
    }
    if (!have) { snprintf(err, errlen, "VHDX region table is corrupt"); goto fail; }
    uint64_t meta_off = 0;
    uint32_t meta_len = 0;
    for (uint32_t i = 0; i < le32(rt + 8); i++) {
        const uint8_t *e = rt + 16 + 32 * i;
        if (!memcmp(e, G_BAT, 16)) { v->bat_off = le64(e + 16); v->bat_len = le32(e + 24); }
        else if (!memcmp(e, G_META, 16)) { meta_off = le64(e + 16); meta_len = le32(e + 24); }
        else if (le32(e + 28) & 1) { snprintf(err, errlen, "VHDX uses an unknown required region"); goto fail; }
    }
    if (!v->bat_off || !meta_off || meta_len < 64 * KiB) { snprintf(err, errlen, "VHDX is missing its BAT or metadata"); goto fail; }

    /* metadata */
    uint8_t *meta = malloc(meta_len);
    if (!meta) goto io;
    if (pread_all(fd, meta, meta_len, meta_off) || memcmp(meta, "metadata", 8)) {
        free(meta);
        snprintf(err, errlen, "VHDX metadata is corrupt");
        goto fail;
    }
    int has_parent = 0;
    uint32_t pss = 512;
    for (uint32_t i = 0; i < le16(meta + 10) && 32 + 32 * i < 64 * KiB; i++) {
        const uint8_t *e = meta + 32 + 32 * i;
        uint32_t o = le32(e + 16), l = le32(e + 20), flags = le32(e + 24);
        if (o + (uint64_t)l > meta_len) continue;
        const uint8_t *d = meta + o;
        if (!memcmp(e, G_FPARAM, 16) && l >= 8) { v->block_size = le32(d); has_parent = (le32(d + 4) >> 1) & 1; }
        else if (!memcmp(e, G_VSIZE, 16) && l >= 8) v->vsize = le64(d);
        else if (!memcmp(e, G_LSS, 16) && l >= 4) v->lss = le32(d);
        else if (!memcmp(e, G_PSS, 16) && l >= 4) pss = le32(d);
        else if (!memcmp(e, G_PARENT, 16)) has_parent = 1;
        else if (!memcmp(e, G_DISKID, 16)) { }
        else if (flags & 4) { free(meta); snprintf(err, errlen, "VHDX uses an unknown required metadata item"); goto fail; }
    }
    free(meta);
    (void)pss;
    if (has_parent) { snprintf(err, errlen, "differencing VHDX (with a parent disk) is not supported; merge it first"); goto fail; }
    if (v->block_size < MiB || v->block_size > 256 * MiB || (v->block_size & (v->block_size - 1)) ||
        (v->lss != 512 && v->lss != 4096) || !v->vsize || v->vsize % v->lss) {
        snprintf(err, errlen, "VHDX parameters are invalid");
        goto fail;
    }
    v->chunk_ratio = (uint32_t)(((uint64_t)1 << 23) * v->lss / v->block_size);
    uint64_t blocks = (v->vsize + v->block_size - 1) / v->block_size;
    uint64_t entries = blocks + (blocks - 1) / v->chunk_ratio;
    if (entries * 8 > v->bat_len || entries > (1u << 28)) { snprintf(err, errlen, "VHDX BAT is too small"); goto fail; }
    v->bat_entries = (uint32_t)entries;
    v->bat = malloc(entries * 8);
    uint8_t *raw = malloc(entries * 8);
    if (!v->bat || !raw) { free(raw); goto io; }
    if (pread_all(fd, raw, entries * 8, v->bat_off)) { free(raw); goto io; }
    uint64_t fend = file_size(fd);
    v->file_end = fend;
    for (uint64_t i = 0; i < entries; i++) {
        v->bat[i] = le64(raw + 8 * i);
        int state = (int)(v->bat[i] & 7);
        if ((i + 1) % (v->chunk_ratio + 1) == 0) continue;   /* sector bitmap entry */
        if (state == PAYLOAD_PARTIALLY_PRESENT) { free(raw); snprintf(err, errlen, "VHDX has partially present blocks"); goto fail; }
        if (state == PAYLOAD_FULLY_PRESENT) {
            uint64_t at = (v->bat[i] >> 20) * MiB;
            if (at + v->block_size > v->file_end) v->file_end = at + v->block_size;
            if (at + v->block_size > fend + v->block_size) { free(raw); snprintf(err, errlen, "VHDX block lies beyond the end of the file"); goto fail; }
        }
    }
    free(raw);

    memset(out, 0, sizeof *out);
    out->opaque = v;
    out->size = v->vsize;
    out->readonly = readonly;
    out->block_size = v->lss;
    out->format = WD_IMG_VHDX;
    out->read = vhdx_read;
    out->write = readonly ? NULL : vhdx_write;
    out->flush = vhdx_flush;
    out->close = vhdx_close;
    return 0;
io:
    snprintf(err, errlen, "read error");
fail:
    free(v->bat);
    free(v);
    return -1;
}

/* Create an empty dynamic VHDX (1 MiB header section, 1 MiB log, metadata, BAT). */
int wd_vhdx_create(int fd, uint64_t size, uint32_t block_size, uint32_t sector_size)
{
    if (!block_size) block_size = 32 * MiB;
    if (!sector_size) sector_size = 512;
    if (block_size < MiB || block_size > 256 * MiB || (block_size & (block_size - 1)) ||
        (sector_size != 512 && sector_size != 4096) || !size || size % sector_size || size > 64ULL * 1024 * 1024 * MiB)
        return -1;
    uint32_t chunk_ratio = (uint32_t)(((uint64_t)1 << 23) * sector_size / block_size);
    uint64_t blocks = (size + block_size - 1) / block_size;
    uint64_t entries = blocks + (blocks - 1) / chunk_ratio;
    uint64_t bat_len = (entries * 8 + MiB - 1) & ~(MiB - 1);
    const uint64_t log_off = 1 * MiB, meta_off = 2 * MiB, bat_off = 3 * MiB;
    if (ftruncate(fd, 0) || ftruncate(fd, (off_t)(bat_off + bat_len))) return -1;

    uint8_t *b = calloc(1, 64 * KiB);
    if (!b) return -1;
    int r = 0;
    /* file type identifier */
    memcpy(b, "vhdxfile", 8);
    const char *creator = "WindDroid " WD_VERSION;
    for (int i = 0; creator[i]; i++) b[8 + 2 * i] = (uint8_t)creator[i];
    r |= pwrite_all(fd, b, 64 * KiB, 0);
    /* headers */
    for (int slot = 0; slot < 2; slot++) {
        memset(b, 0, 64 * KiB);
        memcpy(b, "head", 4);
        put_le64(b + 8, (uint64_t)slot + 1);
        random_guid(b + 16);
        random_guid(b + 32);
        b[66] = 1;                                         /* version 1, log version 0 */
        put_le32(b + 68, (uint32_t)MiB);
        put_le64(b + 72, log_off);
        put_le32(b + 4, crc32c(0, b, 4096));
        r |= pwrite_all(fd, b, 64 * KiB, (uint64_t)(slot + 1) * 64 * KiB);
    }
    /* region tables */
    memset(b, 0, 64 * KiB);
    memcpy(b, "regi", 4);
    put_le32(b + 8, 2);
    memcpy(b + 16, G_BAT, 16);  put_le64(b + 32, bat_off);  put_le32(b + 40, (uint32_t)bat_len); put_le32(b + 44, 1);
    memcpy(b + 48, G_META, 16); put_le64(b + 64, meta_off); put_le32(b + 72, (uint32_t)MiB);   put_le32(b + 76, 1);
    put_le32(b + 4, crc32c(0, b, 64 * KiB));
    r |= pwrite_all(fd, b, 64 * KiB, 192 * KiB);
    r |= pwrite_all(fd, b, 64 * KiB, 256 * KiB);
    /* metadata: table, then the items from 64 KiB on */
    memset(b, 0, 64 * KiB);
    memcpy(b, "metadata", 8);
    b[10] = 5;
    struct { const uint8_t *guid; uint32_t len, flags; } items[5] = {
        { G_FPARAM, 8, 4 }, { G_VSIZE, 8, 6 }, { G_LSS, 4, 6 }, { G_PSS, 4, 6 }, { G_DISKID, 16, 6 },
    };
    uint8_t data[64];
    memset(data, 0, sizeof data);
    put_le32(data + 0, block_size);                         /* file parameters (flags 0) */
    put_le64(data + 8, size);
    put_le32(data + 16, sector_size);
    put_le32(data + 24, 4096);
    random_guid(data + 32);
    uint32_t doff[5] = { 0, 8, 16, 24, 32 };
    for (int i = 0; i < 5; i++) {
        uint8_t *e = b + 32 + 32 * i;
        memcpy(e, items[i].guid, 16);
        put_le32(e + 16, (uint32_t)(64 * KiB) + doff[i]);
        put_le32(e + 20, items[i].len);
        put_le32(e + 24, items[i].flags);
    }
    r |= pwrite_all(fd, b, 64 * KiB, meta_off);
    r |= pwrite_all(fd, data, sizeof data, meta_off + 64 * KiB);
    free(b);
    /* the BAT is all zero (blocks not present) thanks to ftruncate */
    if (r || sync_fd(fd)) return -1;
    return 0;
}

/* ================================================================== VHD */

typedef struct {
    int fd, own;
    uint64_t size;
    int dynamic;
    uint8_t footer[512];
    uint64_t footer_off;          /* where the trailing footer lives */
    uint32_t *bat;                /* host order */
    uint32_t max_entries, block_size, bitmap_bytes;
    uint64_t bat_off;
    struct { uint32_t block; uint8_t *bits; } cache[8];
    int cache_next;
} vhd;

static uint32_t vhd_checksum(const uint8_t *p, size_t n, size_t skip)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) if (i < skip || i >= skip + 4) s += p[i];
    return ~s;
}

static uint8_t *vhd_bitmap(vhd *v, uint32_t block)
{
    for (int i = 0; i < 8; i++)
        if (v->cache[i].bits && v->cache[i].block == block) return v->cache[i].bits;
    int slot = v->cache_next++ & 7;
    if (!v->cache[slot].bits && !(v->cache[slot].bits = malloc(v->bitmap_bytes))) return NULL;
    v->cache[slot].block = UINT32_MAX;
    if (pread_all(v->fd, v->cache[slot].bits, v->bitmap_bytes, (uint64_t)v->bat[block] * 512)) return NULL;
    v->cache[slot].block = block;
    return v->cache[slot].bits;
}

static int vhd_read(void *o, uint64_t off, void *buf, size_t len)
{
    vhd *v = o;
    uint8_t *p = buf;
    if (!v->dynamic) return pread_all(v->fd, buf, len, off);
    while (len) {
        uint32_t block = (uint32_t)(off / v->block_size);
        uint64_t in = off % v->block_size;
        size_t n = 512 - (in & 511);
        if (n > len) n = len;
        if (block >= v->max_entries || v->bat[block] == UINT32_MAX) {
            memset(p, 0, n);
        } else {
            uint8_t *bits = vhd_bitmap(v, block);
            if (!bits) return -1;
            uint32_t sec = (uint32_t)(in / 512);
            if (bits[sec / 8] & (0x80 >> (sec % 8))) {
                /* read as many consecutive present sectors as possible */
                size_t span = n;
                while (span < len && in + span < v->block_size) {
                    uint32_t s2 = (uint32_t)((in + span) / 512);
                    if (!(bits[s2 / 8] & (0x80 >> (s2 % 8)))) break;
                    span += len - span < 512 ? len - span : 512;
                }
                n = span;
                if (pread_all(v->fd, p, n, (uint64_t)v->bat[block] * 512 + v->bitmap_bytes + in)) return -1;
            } else {
                memset(p, 0, n);
            }
        }
        p += n; off += n; len -= n;
    }
    return 0;
}

static int vhd_write(void *o, uint64_t off, const void *buf, size_t len)
{
    vhd *v = o;
    const uint8_t *p = buf;
    if (!v->dynamic) return pwrite_all(v->fd, buf, len, off);
    while (len) {
        uint32_t block = (uint32_t)(off / v->block_size);
        uint64_t in = off % v->block_size;
        size_t n = v->block_size - in < len ? (size_t)(v->block_size - in) : len;
        if (block >= v->max_entries) return -1;
        if (v->bat[block] == UINT32_MAX) {
            if (all_zero(p, n)) { p += n; off += n; len -= n; continue; }
            /* new block replaces the trailing footer: bitmap (all present), zero data, footer */
            uint64_t at = v->footer_off;
            uint8_t *bits = malloc(v->bitmap_bytes);
            if (!bits) return -1;
            memset(bits, 0xFF, v->bitmap_bytes);
            int r = pwrite_all(v->fd, bits, v->bitmap_bytes, at);
            free(bits);
            uint64_t data = at + v->bitmap_bytes, nf = data + v->block_size;
            if (r || ftruncate(v->fd, (off_t)(nf + 512)) || pwrite_all(v->fd, v->footer, 512, nf) ||
                pwrite_all(v->fd, p, n, data + in) || sync_fd(v->fd)) return -1;
            uint8_t be[4];
            put_be32(be, (uint32_t)(at / 512));
            if (pwrite_all(v->fd, be, 4, v->bat_off + 4ULL * block)) return -1;
            v->bat[block] = (uint32_t)(at / 512);
            v->footer_off = nf;
            for (int i = 0; i < 8; i++) if (v->cache[i].block == block) v->cache[i].block = UINT32_MAX;
        } else {
            uint8_t *bits = vhd_bitmap(v, block);
            if (!bits) return -1;
            int changed = 0;
            for (uint64_t s = in / 512; s <= (in + n - 1) / 512; s++)
                if (!(bits[s / 8] & (0x80 >> (s % 8)))) {
                    /* a sector becoming present: bytes of it outside this write must read as zero */
                    uint64_t sec_off = (uint64_t)v->bat[block] * 512 + v->bitmap_bytes + s * 512;
                    static const uint8_t z[512];
                    if (pwrite_all(v->fd, z, 512, sec_off)) return -1;
                    bits[s / 8] |= (uint8_t)(0x80 >> (s % 8));
                    changed = 1;
                }
            if (pwrite_all(v->fd, p, n, (uint64_t)v->bat[block] * 512 + v->bitmap_bytes + in)) return -1;
            if (changed && pwrite_all(v->fd, bits, v->bitmap_bytes, (uint64_t)v->bat[block] * 512)) return -1;
        }
        p += n; off += n; len -= n;
    }
    return 0;
}

static int vhd_flush(void *o) { return sync_fd(((vhd *)o)->fd); }

static void vhd_close(void *o)
{
    vhd *v = o;
    if (v->own) close(v->fd);
    for (int i = 0; i < 8; i++) free(v->cache[i].bits);
    free(v->bat);
    free(v);
}

static int vhd_open(int fd, int readonly, int own, wd_disk *out, char *err, size_t errlen)
{
    vhd *v = calloc(1, sizeof *v);
    if (!v) { snprintf(err, errlen, "out of memory"); return -1; }
    v->fd = fd; v->own = own;
    uint64_t fsize = file_size(fd);
    uint8_t *f = v->footer;
    int found = 0;
    if (fsize >= 512 && !pread_all(fd, f, 512, fsize - 512) && !memcmp(f, "conectix", 8)) { found = 1; v->footer_off = fsize - 512; }
    if (!found && fsize >= 511 && !pread_all(fd, f, 511, fsize - 511) && !memcmp(f, "conectix", 8)) { found = 1; f[511] = 0; v->footer_off = fsize - 511; }
    if (!found && !pread_all(fd, f, 512, 0) && !memcmp(f, "conectix", 8)) { found = 1; v->footer_off = fsize - 512; }
    if (!found) { snprintf(err, errlen, "VHD footer not found"); goto fail; }
    if (vhd_checksum(f, 512, 64) != be32(f + 64)) { snprintf(err, errlen, "VHD footer checksum mismatch"); goto fail; }
    uint32_t type = be32(f + 60);
    v->size = be64(f + 48);
    if (type == 4) { snprintf(err, errlen, "differencing VHD (with a parent disk) is not supported; merge it first"); goto fail; }
    if (type == 2) {
        if (v->size > v->footer_off) { snprintf(err, errlen, "fixed VHD is truncated"); goto fail; }
    } else if (type == 3) {
        uint8_t dh[1024];
        uint64_t dh_off = be64(f + 16);
        if (pread_all(fd, dh, 1024, dh_off) || memcmp(dh, "cxsparse", 8)) { snprintf(err, errlen, "VHD dynamic header not found"); goto fail; }
        if (vhd_checksum(dh, 1024, 36) != be32(dh + 36)) { snprintf(err, errlen, "VHD dynamic header checksum mismatch"); goto fail; }
        v->dynamic = 1;
        v->bat_off = be64(dh + 16);
        v->max_entries = be32(dh + 28);
        v->block_size = be32(dh + 32);
        if (v->block_size < 512 || (v->block_size & 511) || (v->block_size & (v->block_size - 1)) ||
            (uint64_t)v->max_entries * v->block_size < v->size) {
            snprintf(err, errlen, "VHD dynamic header is invalid");
            goto fail;
        }
        v->bitmap_bytes = ((v->block_size / 512 / 8) + 511) & ~511u;
        v->bat = malloc((size_t)v->max_entries * 4);
        uint8_t *raw = malloc((size_t)v->max_entries * 4);
        if (!v->bat || !raw || pread_all(fd, raw, (size_t)v->max_entries * 4, v->bat_off)) {
            free(raw);
            snprintf(err, errlen, "cannot read the VHD block table");
            goto fail;
        }
        for (uint32_t i = 0; i < v->max_entries; i++) {
            v->bat[i] = be32(raw + 4 * i);
            if (v->bat[i] != UINT32_MAX) {
                uint64_t end = (uint64_t)v->bat[i] * 512 + v->bitmap_bytes + v->block_size;
                if (end > v->footer_off + 512) {
                    free(raw);
                    snprintf(err, errlen, "VHD block lies beyond the end of the file");
                    goto fail;
                }
            }
        }
        free(raw);
    } else {
        snprintf(err, errlen, "unknown VHD disk type %u", type);
        goto fail;
    }
    for (int i = 0; i < 8; i++) v->cache[i].block = UINT32_MAX;
    memset(out, 0, sizeof *out);
    out->opaque = v;
    out->size = v->size & ~511ULL;
    out->readonly = readonly;
    out->block_size = 512;
    out->format = WD_IMG_VHD;
    out->read = vhd_read;
    out->write = readonly ? NULL : vhd_write;
    out->flush = vhd_flush;
    out->close = vhd_close;
    return 0;
fail:
    free(v->bat);
    free(v);
    return -1;
}

/* ================================================================== entry point */

int wd_disk_open_image(int fd, int readonly, int take_ownership, wd_disk *out, char *err, size_t errlen)
{
    char dummy[8];
    if (!err) { err = dummy; errlen = sizeof dummy; }
    int kind = wd_image_probe(fd);
    int r;
    switch (kind) {
    case WD_IMG_VHDX: r = vhdx_open(fd, readonly, take_ownership, out, err, errlen); break;
    case WD_IMG_VHD:  r = vhd_open(fd, readonly, take_ownership, out, err, errlen); break;
    default:
        r = wd_disk_from_fd(fd, readonly, take_ownership, out);
        if (r) snprintf(err, errlen, "cannot read the image size");
        else {
            out->format = kind;
            if (kind == WD_IMG_ISO) out->block_size = 2048;
        }
        break;
    }
    return r;
}
