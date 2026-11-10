/*
 * WindDroid - USB mass storage (Bulk-Only Transport) with a SCSI block
 * device (SBC) or a DVD/CD-ROM drive (MMC). The CD drive lets the guest
 * boot and install from an ISO image, the way a USB DVD drive would.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "usb.h"

#define VID 0x1209
#define RESP_MAX 4096

enum { PH_CBW, PH_IN, PH_OUT, PH_CSW };

typedef struct {
    wd_usbdev dev;
    wd_disk disk;
    int cdrom, readonly;
    uint32_t bs;                  /* logical block size */
    uint64_t nblocks;
    int phase;
    uint32_t tag, host_len, done; /* bytes the host announced / bytes moved in the data phase */
    uint32_t dev_len;             /* bytes the device has for (or wants from) the host */
    int status;                   /* CSW status */
    uint8_t resp[RESP_MAX];       /* small responses */
    uint64_t ncmd, nfail;         /* diagnostics */
    struct { uint8_t op, status; uint32_t lba, count, host_len; } last[8];
    int stream;                   /* data phase moves blocks of the disk */
    uint64_t pos;                 /* byte offset on the disk for the stream */
    uint8_t *stage;               /* partial block of an OUT stream */
    uint32_t staged;
    uint8_t sense_key, asc, ascq;
    int media_event;
} msd;

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

static const uint8_t msd_device_tpl[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, VID & 0xFF, VID >> 8, 0x03, 0x00,
                                            0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t msd_qualifier[10] = { 10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0 };
static const uint8_t msd_config[] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 250,              /* configuration, bus powered, 500 mA */
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,                 /* interface: mass storage, SCSI, bulk-only */
    7, 5, 0x81, 2, 0x00, 0x02, 0,                 /* bulk IN 512 */
    7, 5, 0x02, 2, 0x00, 0x02, 0,                 /* bulk OUT 512 */
};
static const wd_usb_descs disk_descs = {
    msd_device_tpl, msd_qualifier, msd_config, sizeof msd_config,
    { NULL, "WindDroid", "WindDroid USB Disk", "WDUSBDISK0001" },
};
static const wd_usb_descs cd_descs = {
    msd_device_tpl, msd_qualifier, msd_config, sizeof msd_config,
    { NULL, "WindDroid", "WindDroid USB DVD-ROM", "WDUSBDVD0001" },
};

/* ---------------------------------------------------------------- SCSI */

static int fail(msd *s, uint8_t key, uint8_t asc, uint8_t ascq)
{
    s->sense_key = key; s->asc = asc; s->ascq = ascq;
    s->status = 1;
    return 0;
}

static int mode_pages(msd *s, int page, int pc, uint8_t *p)
{
    int n = 0;
    if (page == 0x08 || page == 0x3F) {                   /* caching */
        uint8_t *q = p + n;
        memset(q, 0, 20);
        q[0] = 0x08; q[1] = 0x12;
        if (!s->cdrom && pc != 1) q[2] = 0x04;            /* write cache enabled (we honour SYNCHRONIZE CACHE) */
        n += 20;
    }
    if (s->cdrom && (page == 0x2A || page == 0x3F)) {     /* CD/DVD capabilities */
        uint8_t *q = p + n;
        memset(q, 0, 22);
        q[0] = 0x2A; q[1] = 0x14;
        if (pc != 1) {
            q[2] = 0x3B; q[3] = 0; q[4] = 0x7F; q[5] = 0xFF; q[6] = 0x2D; q[7] = 0;
            be16(q + 8, 50 * 176); be16(q + 10, 2); be16(q + 12, 2048); be16(q + 14, 16 * 176);
        }
        n += 22;
    }
    if (page == 0x1C || page == 0x3F) {                   /* informational exceptions control */
        uint8_t *q = p + n;
        memset(q, 0, 12);
        q[0] = 0x1C; q[1] = 0x0A;
        n += 12;
    }
    if (!n && page != 0) return -1;
    return n;
}

static void inquiry_std(msd *s, uint8_t *r)
{
    memset(r, 0, 36);
    r[0] = s->cdrom ? 0x05 : 0x00;
    r[1] = s->cdrom ? 0x80 : 0x00;                        /* removable medium */
    r[2] = 0x05;                                          /* SPC-3 */
    r[3] = 0x02;
    r[4] = 31;
    memcpy(r + 8, "WindDrd ", 8);
    memcpy(r + 16, s->cdrom ? "USB DVD-ROM     " : "USB Disk        ", 16);
    memcpy(r + 32, "1.0 ", 4);
}

/* Execute a command block; for a data-in command fill resp/dev_len or set up a stream. */
static int scsi_exec(msd *s, const uint8_t *cb)
{
    uint8_t *r = s->resp;
    uint32_t bs = s->bs;
    uint64_t lba = 0, count = 0;
    int write = 0;
    s->status = 0;
    s->stream = 0;
    s->dev_len = 0;
    memset(r, 0, 64);
    switch (cb[0]) {
    case 0x00:                                            /* TEST UNIT READY */
    case 0x1E:                                            /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x1D:                                            /* SEND DIAGNOSTIC */
    case 0xBB:                                            /* SET CD SPEED */
        return 0;
    case 0x1B:                                            /* START STOP UNIT */
        return 0;
    case 0x03: {                                          /* REQUEST SENSE */
        r[0] = 0x70; r[2] = s->sense_key; r[7] = 10; r[12] = s->asc; r[13] = s->ascq;
        s->dev_len = 18;
        s->sense_key = s->asc = s->ascq = 0;
        return 0;
    }
    case 0x12: {                                          /* INQUIRY */
        uint8_t type = s->cdrom ? 0x05 : 0x00;
        if (!(cb[1] & 1)) {
            if (cb[2]) return fail(s, 0x05, 0x24, 0);
            inquiry_std(s, r);
            s->dev_len = 36;
            return 0;
        }
        r[0] = type;
        r[1] = cb[2];
        switch (cb[2]) {
        case 0x00: r[3] = 3; r[4] = 0x00; r[5] = 0x80; r[6] = 0x83; s->dev_len = 7; return 0;
        case 0x80: {
            const char *sn = s->dev.descs->strings[3];
            r[3] = (uint8_t)strlen(sn);
            memcpy(r + 4, sn, strlen(sn));
            s->dev_len = 4 + (uint32_t)strlen(sn);
            return 0;
        }
        case 0x83: {
            const char *sn = s->dev.descs->strings[3];
            uint8_t *d = r + 4;
            d[0] = 0x02; d[1] = 0x01; d[2] = 0;           /* ASCII, T10 vendor identification */
            memcpy(d + 4, "WindDrd ", 8);
            memcpy(d + 12, sn, strlen(sn));
            d[3] = (uint8_t)(8 + strlen(sn));
            be16(r + 2, (uint32_t)(4 + d[3]));
            s->dev_len = 8 + d[3];
            return 0;
        }
        default: return fail(s, 0x05, 0x24, 0);
        }
    }
    case 0x1A: case 0x5A: {                               /* MODE SENSE (6) / (10) */
        int ten = cb[0] == 0x5A, page = cb[2] & 0x3F, pc = cb[2] >> 6;
        int hdr = ten ? 8 : 4;
        int n = mode_pages(s, page, pc, r + hdr);
        if (n < 0 || pc == 3) return fail(s, 0x05, 0x24, 0);
        uint8_t wp = (s->readonly || s->cdrom) ? 0x80 : 0;
        if (ten) { be16(r, (uint32_t)(hdr + n - 2)); r[3] = wp; }
        else { r[0] = (uint8_t)(hdr + n - 1); r[2] = wp; }
        s->dev_len = (uint32_t)(hdr + n);
        return 0;
    }
    case 0x15: case 0x55:                                 /* MODE SELECT: accepted, ignored */
        return 0;
    case 0x25: {                                          /* READ CAPACITY (10) */
        be32(r, s->nblocks - 1 > 0xFFFFFFFEULL ? 0xFFFFFFFFu : (uint32_t)(s->nblocks - 1));
        be32(r + 4, bs);
        s->dev_len = 8;
        return 0;
    }
    case 0x9E:                                            /* SERVICE ACTION IN: READ CAPACITY (16) */
        if ((cb[1] & 0x1F) != 0x10) return fail(s, 0x05, 0x24, 0);
        memset(r, 0, 32);
        be64(r, s->nblocks - 1);
        be32(r + 8, bs);
        if (bs == 4096 && s->disk.block_size == 4096) r[13] = 0;
        s->dev_len = 32;
        return 0;
    case 0x23:                                            /* READ FORMAT CAPACITIES */
        r[3] = 8;
        be32(r + 4, s->nblocks > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)s->nblocks);
        r[8] = 0x02;
        r[9] = (uint8_t)(bs >> 16); r[10] = (uint8_t)(bs >> 8); r[11] = (uint8_t)bs;
        s->dev_len = 12;
        return 0;
    case 0xA0:                                            /* REPORT LUNS */
        memset(r, 0, 16);
        r[3] = 8;
        s->dev_len = 16;
        return 0;
    case 0x35: case 0x91:                                 /* SYNCHRONIZE CACHE */
        if (s->disk.flush && s->disk.flush(s->disk.opaque)) return fail(s, 0x03, 0x0C, 0x00);
        return 0;
    case 0x2F: case 0x8F:                                 /* VERIFY */
        return 0;
    case 0x28: lba = rd32(cb + 2); count = (uint32_t)(cb[7] << 8 | cb[8]); goto rw;
    case 0xA8: lba = rd32(cb + 2); count = rd32(cb + 6); goto rw;
    case 0x88: lba = rd64(cb + 2); count = rd32(cb + 10); goto rw;
    case 0x2A: write = 1; lba = rd32(cb + 2); count = (uint32_t)(cb[7] << 8 | cb[8]); goto rw;
    case 0xAA: write = 1; lba = rd32(cb + 2); count = rd32(cb + 6); goto rw;
    case 0x8A: write = 1; lba = rd64(cb + 2); count = rd32(cb + 10); goto rw;
    case 0xBE:                                            /* READ CD: user data only */
        if (!s->cdrom) return fail(s, 0x05, 0x20, 0);
        lba = rd32(cb + 2);
        count = (uint32_t)(cb[6] << 16 | cb[7] << 8 | cb[8]);
        if (cb[9] == 0) return 0;
        if ((cb[9] & 0xF8) != 0x10) return fail(s, 0x05, 0x24, 0);
        goto rw;
    rw:
        if (write && (s->readonly || s->cdrom || !s->disk.write)) return fail(s, 0x07, 0x27, 0x00);
        if (lba + count > s->nblocks || lba + count < lba) return fail(s, 0x05, 0x21, 0x00);
        if (count * bs > 0xFFFFFFFFULL) return fail(s, 0x05, 0x24, 0);
        s->stream = 1;
        s->pos = lba * bs;
        s->dev_len = (uint32_t)(count * bs);
        s->staged = 0;
        return 0;
    /* ---- MMC */
    case 0x43: {                                          /* READ TOC/PMA/ATIP */
        if (!s->cdrom) return fail(s, 0x05, 0x20, 0);
        int msf = cb[1] & 2, format = cb[2] & 0xF, start = cb[6];
        if (format == 0) {
            if (start > 1 && start != 0xAA) return fail(s, 0x05, 0x24, 0);
            uint8_t *q = r + 2;
            *q++ = 1; *q++ = 1;
            uint64_t ends[2] = { 0, s->nblocks };
            int tracks[2] = { 1, 0xAA }, ctl[2] = { 0x14, 0x16 };
            for (int i = start == 0xAA ? 1 : 0; i < 2; i++) {
                *q++ = 0; *q++ = (uint8_t)ctl[i]; *q++ = (uint8_t)tracks[i]; *q++ = 0;
                if (msf) {
                    uint64_t f = ends[i] + 150;
                    *q++ = 0; *q++ = (uint8_t)(f / (75 * 60)); *q++ = (uint8_t)((f / 75) % 60); *q++ = (uint8_t)(f % 75);
                } else {
                    be32(q, (uint32_t)ends[i]);
                    q += 4;
                }
            }
            s->dev_len = (uint32_t)(q - r);
            be16(r, s->dev_len - 2);
            return 0;
        }
        if (format == 1) {
            memset(r, 0, 12);
            r[1] = 0x0A; r[2] = 1; r[3] = 1; r[5] = 0x14; r[6] = 1;
            s->dev_len = 12;
            return 0;
        }
        return fail(s, 0x05, 0x24, 0);
    }
    case 0x46: {                                          /* GET CONFIGURATION */
        if (!s->cdrom) return fail(s, 0x05, 0x20, 0);
        int dvd = s->nblocks * 2048ULL > 800ULL * 1024 * 1024;
        uint16_t cur = dvd ? 0x0010 : 0x0008;
        memset(r, 0, 40);
        be32(r, 36);
        be16(r + 6, cur);
        r[10] = 0x03; r[11] = 8;
        be16(r + 12, 0x0010); r[14] = dvd;
        be16(r + 16, 0x0008); r[18] = !dvd;
        be16(r + 20, 1); r[22] = 0x0B; r[23] = 8; be32(r + 24, 8); r[28] = 1;    /* core: USB */
        be16(r + 32, 3); r[34] = 0x0B; r[35] = 4; r[36] = 0x29;               /* removable medium */
        s->dev_len = 40;
        return 0;
    }
    case 0x4A: {                                          /* GET EVENT STATUS NOTIFICATION */
        if (!s->cdrom || !(cb[1] & 1)) return fail(s, 0x05, 0x24, 0);
        r[3] = 0x10;
        if (cb[4] & 0x10) {
            be16(r, 6);
            r[2] = 4;
            r[4] = s->media_event ? 2 : 0;                /* new media */
            r[5] = 0x02;                                  /* media present, tray closed */
            s->media_event = 0;
            s->dev_len = 8;
        } else {
            be16(r, 2);
            r[2] = 0x80;
            s->dev_len = 4;
        }
        return 0;
    }
    case 0x51:                                            /* READ DISC INFORMATION */
        if (!s->cdrom || (cb[1] & 7)) return fail(s, 0x05, 0x24, 0);
        memset(r, 0, 34);
        r[1] = 32; r[2] = 0x0E; r[3] = 1; r[4] = 1; r[5] = 1; r[6] = 1; r[7] = 0x20;
        s->dev_len = 34;
        return 0;
    case 0x52:                                            /* READ TRACK INFORMATION */
        if (!s->cdrom) return fail(s, 0x05, 0x20, 0);
        if ((cb[1] & 3) == 1 && rd32(cb + 2) > 1) return fail(s, 0x05, 0x24, 0);
        memset(r, 0, 36);
        r[1] = 34; r[2] = 1; r[3] = 1; r[5] = 0x04; r[6] = 0x01;
        be32(r + 24, (uint32_t)s->nblocks);
        s->dev_len = 36;
        return 0;
    case 0xBD:                                            /* MECHANISM STATUS */
        if (!s->cdrom) return fail(s, 0x05, 0x20, 0);
        memset(r, 0, 8);
        r[5] = 1;
        s->dev_len = 8;
        return 0;
    case 0xAD: {                                          /* READ DVD STRUCTURE */
        if (!s->cdrom || cb[1] != 0) return fail(s, 0x05, 0x24, 0);
        int format = cb[7];
        if (format == 0xFF) {
            memset(r, 0, 16);
            r[4] = 0x00; r[5] = 0x40; be16(r + 6, 2048 + 4);
            r[8] = 0x01; r[9] = 0x40; be16(r + 10, 4 + 4);
            r[12] = 0x04; r[13] = 0x40; be16(r + 14, 2048 + 4);
            be16(r, 14);
            s->dev_len = 16;
            return 0;
        }
        if (s->nblocks * 2048ULL <= 800ULL * 1024 * 1024) return fail(s, 0x05, 0x30, 0x02);   /* CD medium */
        if (format == 0) {
            if (cb[6]) return fail(s, 0x05, 0x24, 0);
            memset(r, 0, 2052);
            r[4] = 1; r[5] = 0x0F; r[6] = 1;
            be32(r + 12, (uint32_t)(s->nblocks >> 2) - 1);
            be32(r + 16, (uint32_t)(s->nblocks >> 2) - 1);
            be16(r, 2050);
            s->dev_len = 2052;
            return 0;
        }
        if (format == 1) { memset(r, 0, 8); be16(r, 6); s->dev_len = 8; return 0; }
        if (format == 4) { memset(r, 0, 2052); be16(r, 2050); s->dev_len = 2052; return 0; }
        return fail(s, 0x05, 0x24, 0);
    }
    default:
        return fail(s, 0x05, 0x20, 0x00);                 /* invalid command operation code */
    }
}

/* ---------------------------------------------------------------- Bulk-Only Transport */

static void out_stream_write(msd *s, const uint8_t *buf, uint32_t n)
{
    /* writes arrive in arbitrary pieces; the disk takes whole blocks */
    while (n) {
        if (!s->staged && n >= s->bs) {
            uint32_t whole = n - n % s->bs;
            if (s->status == 0 && s->disk.write(s->disk.opaque, s->pos, buf, whole)) fail(s, 0x03, 0x0C, 0x00);
            s->pos += whole; buf += whole; n -= whole;
            continue;
        }
        uint32_t take = s->bs - s->staged < n ? s->bs - s->staged : n;
        memcpy(s->stage + s->staged, buf, take);
        s->staged += take; buf += take; n -= take;
        if (s->staged == s->bs) {
            if (s->status == 0 && s->disk.write(s->disk.opaque, s->pos, s->stage, s->bs)) fail(s, 0x03, 0x0C, 0x00);
            s->pos += s->bs;
            s->staged = 0;
        }
    }
}

static int msd_data(wd_usbdev *d, int ep, int in, uint8_t *buf, int len)
{
    msd *s = (msd *)d;
    if (in && ep != 1) return USB_RET_STALL;
    if (!in && ep != 2) return USB_RET_STALL;
    if (in ? d->halted_in[1] : d->halted_out[2]) return USB_RET_STALL;
    switch (s->phase) {
    case PH_CBW: {
        if (in) return USB_RET_NAK;
        if (len != 31 || buf[0] != 'U' || buf[1] != 'S' || buf[2] != 'B' || buf[3] != 'C' ||
            buf[13] != 0 || buf[14] < 1 || buf[14] > 16) {
            d->halted_in[1] = d->halted_out[2] = 1;      /* invalid CBW: reset recovery needed */
            return USB_RET_STALL;
        }
        s->tag = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
        s->host_len = (uint32_t)buf[8] | (uint32_t)buf[9] << 8 | (uint32_t)buf[10] << 16 | (uint32_t)buf[11] << 24;
        s->done = 0;
        scsi_exec(s, buf + 15);
        {
            const uint8_t *cb = buf + 15;
            int i = (int)(s->ncmd++ % 8);
            s->last[i].op = cb[0];
            s->last[i].status = (uint8_t)s->status;
            s->last[i].lba = (cb[0] == 0x28 || cb[0] == 0xA8 || cb[0] == 0x2A || cb[0] == 0xAA || cb[0] == 0xBE) ? rd32(cb + 2) : 0;
            s->last[i].count = cb[0] == 0x28 || cb[0] == 0x2A ? (uint32_t)(cb[7] << 8 | cb[8]) : cb[0] == 0xA8 || cb[0] == 0xAA ? rd32(cb + 6) : 0;
            s->last[i].host_len = s->host_len;
            if (s->status) s->nfail++;
        }
        int host_in = buf[12] & 0x80;
        if (s->dev_len && s->stream && (host_in ? 0 : 1) != (buf[15] == 0x2A || buf[15] == 0xAA || buf[15] == 0x8A))
            s->status = 2;                                /* direction mismatch */
        if (s->status == 2) { s->dev_len = 0; s->stream = 0; }
        if (s->status == 0 && s->dev_len > s->host_len && s->stream) s->status = 2;   /* host expects less: phase error */
        if (s->host_len == 0) s->phase = PH_CSW;
        else s->phase = host_in ? PH_IN : PH_OUT;
        if (!host_in && !s->stream) s->dev_len = 0;       /* data-out for non-write commands is discarded */
        return len;
    }
    case PH_IN: {
        if (!in) return USB_RET_STALL;
        uint32_t want = s->host_len - s->done;
        if ((uint32_t)len < want) want = (uint32_t)len;
        uint32_t have = s->done < s->dev_len ? s->dev_len - s->done : 0;
        uint32_t n = have < want ? have : want;
        if (n) {
            if (s->stream) {
                if (s->status == 0 && s->disk.read(s->disk.opaque, s->pos, buf, n)) { fail(s, 0x03, 0x11, 0x00); memset(buf, 0, n); }
                s->pos += n;
            } else {
                memcpy(buf, s->resp + s->done, n);
            }
        }
        memset(buf + n, 0, want - n);                     /* pad up to what the host announced */
        s->done += want;
        if (s->done >= s->host_len) s->phase = PH_CSW;
        return (int)want;
    }
    case PH_OUT: {
        if (in) return USB_RET_NAK;
        uint32_t want = s->host_len - s->done;
        uint32_t n = (uint32_t)len < want ? (uint32_t)len : want;
        uint32_t use = s->done < s->dev_len ? s->dev_len - s->done : 0;
        if (use > n) use = n;
        if (s->stream && use) out_stream_write(s, buf, use);
        s->done += n;
        if (s->done >= s->host_len) s->phase = PH_CSW;
        return (int)n;
    }
    case PH_CSW: {
        if (!in) return USB_RET_STALL;
        if (len < 13) return USB_RET_STALL;
        uint32_t moved = s->done < s->dev_len ? s->done : s->dev_len;
        uint32_t residue = s->host_len - moved;
        memcpy(buf, "USBS", 4);
        buf[4] = (uint8_t)s->tag; buf[5] = (uint8_t)(s->tag >> 8); buf[6] = (uint8_t)(s->tag >> 16); buf[7] = (uint8_t)(s->tag >> 24);
        buf[8] = (uint8_t)residue; buf[9] = (uint8_t)(residue >> 8); buf[10] = (uint8_t)(residue >> 16); buf[11] = (uint8_t)(residue >> 24);
        buf[12] = (uint8_t)s->status;
        s->phase = PH_CBW;
        return 13;
    }
    }
    return USB_RET_STALL;
}

static int msd_class_control(wd_usbdev *d, const uint8_t *setup, uint8_t *data)
{
    msd *s = (msd *)d;
    switch (setup[0] << 8 | setup[1]) {
    case 0xA1FE: data[0] = 0; return 1;                   /* GET MAX LUN */
    case 0x21FF:                                          /* BULK-ONLY MASS STORAGE RESET */
        s->phase = PH_CBW;
        return 0;
    default: return USB_RET_STALL;
    }
}

static void msd_reset(wd_usbdev *d)
{
    msd *s = (msd *)d;
    s->phase = PH_CBW;
    d->configuration = 0;
    memset(d->halted_in, 0, sizeof d->halted_in);
    memset(d->halted_out, 0, sizeof d->halted_out);
}

static void msd_destroy(wd_usbdev *d)
{
    msd *s = (msd *)d;
    if (s->disk.close) s->disk.close(s->disk.opaque);
    free(s->stage);
    free(s);
}

static int msd_diag(wd_usbdev *d, char *buf, int len)
{
    msd *s = (msd *)d;
    int n = snprintf(buf, (size_t)len, "    %s: %llu blocks of %u, phase %d, %llu commands (%llu failed), last:",
                     s->cdrom ? "DVD" : "disk", (unsigned long long)s->nblocks, s->bs, s->phase,
                     (unsigned long long)s->ncmd, (unsigned long long)s->nfail);
    for (uint64_t k = s->ncmd > 8 ? s->ncmd - 8 : 0; k < s->ncmd && n > 0 && n < len; k++) {
        int i = (int)(k % 8);
        n += snprintf(buf + n, (size_t)(len - n), " %02x/%u+%u/%u%s", s->last[i].op, s->last[i].lba, s->last[i].count,
                      s->last[i].host_len, s->last[i].status ? "!" : "");
    }
    if (n > 0 && n < len) n += snprintf(buf + n, (size_t)(len - n), "\n");
    return n < 0 ? 0 : (n < len ? n : len - 1);
}

wd_usbdev *wd_usb_storage_new(const wd_disk *disk, int cdrom)
{
    msd *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->disk = *disk;
    s->cdrom = cdrom;
    s->readonly = cdrom || !disk->write;
    s->bs = cdrom ? 2048 : (disk->block_size == 4096 ? 4096 : 512);
    s->nblocks = disk->size / s->bs;
    s->stage = malloc(s->bs);
    if (!s->stage || !s->nblocks) { free(s->stage); free(s); return NULL; }
    s->media_event = cdrom;
    s->dev.speed = USB_SPEED_HIGH;
    s->dev.descs = cdrom ? &cd_descs : &disk_descs;
    s->dev.class_control = msd_class_control;
    s->dev.data = msd_data;
    s->dev.diag = msd_diag;
    s->dev.reset = msd_reset;
    s->dev.destroy = msd_destroy;
    return &s->dev;
}
