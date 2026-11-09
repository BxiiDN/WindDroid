/*
 * Host-side unit tests for engine pieces that are easiest to check directly:
 * the logical-immediate decoder (cross-checked against the LLVM assembler's
 * encodings in tests/bitmask_vectors.h) and the ELF loader's error handling.
 */
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"
#include "../devices/usb.h"

int wd_decode_bitmask(int sf, int N, int imms, int immr, uint64_t *out);

#include "bitmask_vectors.h"

static int failures;
#define T(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---------------------------------------------------------------- USB devices */

static int ctl(wd_usbdev *d, int type, int req, int value, int index, int len, uint8_t *data)
{
    uint8_t setup[8] = { (uint8_t)type, (uint8_t)req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index,
                         (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    return wd_usb_control(d, setup, data);
}

static int cbw(wd_usbdev *d, uint32_t tag, uint32_t len, int in, const uint8_t *cb, int cblen)
{
    uint8_t b[31] = { 'U', 'S', 'B', 'C' };
    memcpy(b + 4, &tag, 4);
    memcpy(b + 8, &len, 4);
    b[12] = in ? 0x80 : 0;
    b[14] = (uint8_t)cblen;
    memcpy(b + 15, cb, (size_t)cblen);
    return d->data(d, 2, 0, b, 31);
}

static int csw(wd_usbdev *d, uint32_t tag, uint32_t *residue)
{
    uint8_t b[64];
    int n = d->data(d, 1, 1, b, 64);
    if (n != 13 || memcmp(b, "USBS", 4)) return -1;
    uint32_t t, r;
    memcpy(&t, b + 4, 4);
    memcpy(&r, b + 8, 4);
    if (t != tag) return -2;
    if (residue) *residue = r;
    return b[12];
}

static void usb_tests(void)
{
    uint8_t buf[8192];
    /* keyboard: descriptors */
    wd_usbdev *k = wd_usb_keyboard_new();
    T(ctl(k, 0x80, 6, 0x0100, 0, 64, buf) == 18 && buf[1] == 1 && buf[2] == 0x00 && buf[3] == 0x02);
    T(ctl(k, 0x80, 6, 0x0100, 0, 8, buf) == 8);
    int cl = ctl(k, 0x80, 6, 0x0200, 0, 255, buf);
    T(cl > 9 && (buf[2] | buf[3] << 8) == cl);
    int pos = 0, saw_if = 0, saw_ep = 0, rdlen = 0;
    while (pos < cl && buf[pos]) {
        if (buf[pos + 1] == 4) saw_if = buf[pos + 5] == 3 && buf[pos + 6] == 1 && buf[pos + 7] == 1;
        if (buf[pos + 1] == 0x21) rdlen = buf[pos + 7] | buf[pos + 8] << 8;
        if (buf[pos + 1] == 5) saw_ep = buf[pos + 2] == 0x81 && (buf[pos + 3] & 3) == 3;
        pos += buf[pos];
    }
    T(pos == cl && saw_if && saw_ep);
    T(ctl(k, 0x81, 6, 0x2200, 0, 255, buf) == rdlen);
    T(ctl(k, 0x80, 6, 0x0300, 0, 255, buf) == 4 && buf[2] == 0x09 && buf[3] == 0x04);
    T(ctl(k, 0x80, 6, 0x0302, 0, 255, buf) > 2 && buf[2] == 'W' && buf[1] == 3);
    T(ctl(k, 0x80, 6, 0x0600, 0, 10, buf) == 10);
    T(ctl(k, 0x00, 9, 1, 0, 0, buf) == 0 && ctl(k, 0x80, 8, 0, 0, 1, buf) == 1 && buf[0] == 1);
    T(ctl(k, 0x80, 6, 0x0900, 0, 64, buf) == USB_RET_STALL);
    T(ctl(k, 0x21, 0x0B, 0, 0, 0, buf) == 0 && ctl(k, 0xA1, 3, 0, 0, 1, buf) == 1 && buf[0] == 0);
    /* keyboard: reports */
    T(k->data(k, 1, 1, buf, 8) == USB_RET_NAK);
    wd_usb_keyboard_event(k, 0xE1, 1);
    wd_usb_keyboard_event(k, 0x04, 1);
    wd_usb_keyboard_event(k, 0x04, 1);                    /* repeated press: no new report */
    wd_usb_keyboard_event(k, 0x04, 0);
    wd_usb_keyboard_event(k, 0xE1, 0);
    uint8_t r1[8] = { 2 }, r2[8] = { 2, 0, 4 }, r3[8] = { 2 }, r4[8] = { 0 };
    T(k->data(k, 1, 1, buf, 8) == 8 && !memcmp(buf, r1, 8));
    T(k->data(k, 1, 1, buf, 8) == 8 && !memcmp(buf, r2, 8));
    T(k->data(k, 1, 1, buf, 8) == 8 && !memcmp(buf, r3, 8));
    T(k->data(k, 1, 1, buf, 8) == 8 && !memcmp(buf, r4, 8));
    T(k->data(k, 1, 1, buf, 8) == USB_RET_NAK);
    for (int u = 4; u < 11; u++) wd_usb_keyboard_event(k, u, 1);
    for (int i = 0; i < 6; i++) k->data(k, 1, 1, buf, 8);
    T(k->data(k, 1, 1, buf, 8) == 8 && buf[2] == 5 && buf[7] == 10);   /* 7th key: oldest rolls out */
    wd_usb_keyboard_event(k, 0x70, 1);                    /* outside the report descriptor range */
    T(k->data(k, 1, 1, buf, 8) == USB_RET_NAK);
    T(k->data(k, 2, 0, buf, 8) == USB_RET_STALL);
    k->destroy(k);

    /* tablet */
    wd_usbdev *t = wd_usb_tablet_new();
    wd_usb_tablet_event(t, 100, 200, 0, 0);
    wd_usb_tablet_event(t, 40000, -5, 0, 0);              /* clamped, coalesced with the previous motion */
    T(t->data(t, 1, 1, buf, 8) == 6 && buf[0] == 0 && (buf[1] | buf[2] << 8) == 32767 && (buf[3] | buf[4] << 8) == 0);
    T(t->data(t, 1, 1, buf, 8) == USB_RET_NAK);
    wd_usb_tablet_event(t, 10, 10, 1, 0);
    wd_usb_tablet_event(t, 10, 10, 0, 0);
    wd_usb_tablet_event(t, 10, 10, 0, -3);
    T(t->data(t, 1, 1, buf, 8) == 6 && buf[0] == 1);
    T(t->data(t, 1, 1, buf, 8) == 6 && buf[0] == 0 && buf[5] == 0);
    T(t->data(t, 1, 1, buf, 8) == 6 && (int8_t)buf[5] == -3);
    t->destroy(t);

    /* mass storage: a 1 MiB disk */
    wd_disk disk;
    T(wd_disk_memory(1 << 20, &disk) == 0);
    for (int i = 0; i < 4096; i++) buf[i] = (uint8_t)(i * 7 + 3);
    disk.write(disk.opaque, 512 * 10, buf, 4096);
    wd_usbdev *s = wd_usb_storage_new(&disk, 0);
    uint32_t res;
    T(ctl(s, 0xA1, 0xFE, 0, 0, 1, buf) == 1 && buf[0] == 0);
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    T(cbw(s, 1, 36, 1, inq, 6) == 31);
    T(s->data(s, 1, 1, buf, 36) == 36 && buf[0] == 0 && !memcmp(buf + 8, "WindDrd ", 8));
    T(csw(s, 1, &res) == 0 && res == 0);
    uint8_t rc10[10] = { 0x25 };
    cbw(s, 2, 8, 1, rc10, 10);
    T(s->data(s, 1, 1, buf, 8) == 8 && buf[3] == 0xFF && buf[2] == 0x07 && buf[6] == 2 && buf[7] == 0);   /* 2047, 512 */
    T(csw(s, 2, NULL) == 0);
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 10, 0, 0, 8, 0 };   /* READ(10) LBA 10, 8 blocks */
    cbw(s, 3, 4096, 1, rd, 10);
    uint8_t got[4096];
    T(s->data(s, 1, 1, got, 1000) == 1000 && s->data(s, 1, 1, got + 1000, 4096) == 3096);   /* split TDs */
    int okread = 1;
    for (int i = 0; i < 4096; i++) if (got[i] != (uint8_t)(i * 7 + 3)) okread = 0;
    T(okread);
    T(csw(s, 3, &res) == 0 && res == 0);
    uint8_t wr[10] = { 0x2A, 0, 0, 0, 0, 100, 0, 0, 2, 0 };  /* WRITE(10) LBA 100, 2 blocks */
    cbw(s, 4, 1024, 0, wr, 10);
    memset(buf, 0xA5, 1024);
    T(s->data(s, 2, 0, buf, 300) == 300 && s->data(s, 2, 0, buf + 300, 724) == 724);   /* partial blocks */
    T(csw(s, 4, NULL) == 0);
    disk.read(disk.opaque, 51200, got, 1024);
    int okw = 1;
    for (int i = 0; i < 1024; i++) if (got[i] != 0xA5) okw = 0;
    T(okw);
    uint8_t bad[6] = { 0xC7 };                            /* unsupported opcode */
    cbw(s, 5, 0, 0, bad, 6);
    T(csw(s, 5, NULL) == 1);
    uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    cbw(s, 6, 18, 1, sense, 6);
    T(s->data(s, 1, 1, buf, 18) == 18 && buf[0] == 0x70 && buf[2] == 5 && buf[12] == 0x20);
    T(csw(s, 6, NULL) == 0);
    cbw(s, 7, 64, 1, inq, 6);                             /* host asks for more than INQUIRY returns */
    T(s->data(s, 1, 1, buf, 64) == 64);
    T(csw(s, 7, &res) == 0 && res == 28);
    uint8_t oob[10] = { 0x28, 0, 0, 0, 0x08, 0, 0, 0, 1, 0 };   /* LBA 2048: past the end */
    cbw(s, 8, 512, 1, oob, 10);
    T(s->data(s, 1, 1, buf, 512) == 512);
    T(csw(s, 8, NULL) == 1);
    uint8_t junk[31] = { 'X' };
    T(s->data(s, 2, 0, junk, 31) == USB_RET_STALL);       /* invalid CBW: both pipes halt */
    T(s->data(s, 1, 1, buf, 13) == USB_RET_STALL);
    T(ctl(s, 0x21, 0xFF, 0, 0, 0, buf) == 0);             /* reset recovery */
    T(ctl(s, 0x02, 1, 0, 0x81, 0, buf) == 0 && ctl(s, 0x02, 1, 0, 0x02, 0, buf) == 0);
    T(cbw(s, 9, 36, 1, inq, 6) == 31 && s->data(s, 1, 1, buf, 36) == 36 && csw(s, 9, NULL) == 0);
    s->destroy(s);

    /* CD-ROM */
    T(wd_disk_memory(4 << 20, &disk) == 0);
    wd_usbdev *cd = wd_usb_storage_new(&disk, 1);
    cbw(cd, 1, 36, 1, inq, 6);
    T(cd->data(cd, 1, 1, buf, 36) == 36 && buf[0] == 5 && buf[1] == 0x80);
    csw(cd, 1, NULL);
    cbw(cd, 2, 8, 1, rc10, 10);
    T(cd->data(cd, 1, 1, buf, 8) == 8 && buf[2] == 0x07 && buf[3] == 0xFF && buf[6] == 0x08);  /* 2047 x 2048 */
    csw(cd, 2, NULL);
    uint8_t toc[10] = { 0x43, 0, 0, 0, 0, 0, 0, 0, 12 + 8, 0 };
    cbw(cd, 3, 20, 1, toc, 10);
    T(cd->data(cd, 1, 1, buf, 20) == 20 && buf[1] == 18 && buf[6] == 1 && buf[14] == 0xAA &&
      buf[16] == 0 && buf[17] == 0 && buf[18] == 0x08 && buf[19] == 0x00);
    T(csw(cd, 3, NULL) == 0);
    uint8_t gc[10] = { 0x46, 0, 0, 0, 0, 0, 0, 0, 40, 0 };
    cbw(cd, 4, 40, 1, gc, 10);
    T(cd->data(cd, 1, 1, buf, 40) == 40 && buf[6] == 0 && buf[7] == 0x08);   /* small medium: CD-ROM profile */
    T(csw(cd, 4, NULL) == 0);
    uint8_t cdw[10] = { 0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    cbw(cd, 5, 2048, 0, cdw, 10);
    T(cd->data(cd, 2, 0, buf, 2048) == 2048);
    T(csw(cd, 5, NULL) == 1);
    cbw(cd, 6, 18, 1, sense, 6);
    T(cd->data(cd, 1, 1, buf, 18) == 18 && buf[2] == 7 && buf[12] == 0x27);   /* write protected */
    csw(cd, 6, NULL);
    cd->destroy(cd);
}

int main(void)
{
    usb_tests();
    /* every encodable 64-bit and 32-bit logical immediate */
    unsigned n64 = 0, n32 = 0;
    for (unsigned i = 0; i < sizeof bitmask_vectors / sizeof bitmask_vectors[0]; i++) {
        const struct bm *v = &bitmask_vectors[i];
        uint64_t got = 0;
        int ok = wd_decode_bitmask(v->sf, v->n, v->imms, v->immr, &got);
        if (!ok || got != v->value) {
            failures++;
            if (failures < 10)
                printf("FAIL bitmask sf=%d N=%d immr=%d imms=%d: got %llx want %llx\n", v->sf, v->n, v->immr,
                       v->imms, (unsigned long long)got, (unsigned long long)v->value);
        }
        if (v->sf) n64++; else n32++;
    }
    T(n64 == 7680);
    T(n32 == 992);
    /* and nothing else is accepted */
    unsigned accepted = 0;
    for (int sf = 0; sf < 2; sf++)
        for (int N = 0; N < 2; N++)
            for (int immr = 0; immr < 64; immr++)
                for (int imms = 0; imms < 64; imms++) {
                    if (!sf && (immr > 31 || imms > 31)) continue;
                    uint64_t v;
                    accepted += (unsigned)wd_decode_bitmask(sf, N, imms, immr, &v);
                }
    T(accepted == n64 + n32);
    /* reserved encodings */
    uint64_t dummy;
    T(!wd_decode_bitmask(1, 0, 0x3F, 0, &dummy));     /* all ones element */
    T(!wd_decode_bitmask(0, 1, 0, 0, &dummy));        /* N=1 for 32-bit */
    T(!wd_decode_bitmask(1, 1, 0x3F, 0, &dummy));

    /* loader rejects bad input */
    wd_config cfg = { .ram_size = 16 << 20 };
    wd_machine *m = wd_create(&cfg);
    T(m != NULL);
    uint8_t junk[100] = { 0x7f, 'E', 'L', 'F', 2, 1 };
    T(wd_load_elf(m, junk, sizeof junk) == -1);
    T(wd_load_raw(m, junk, sizeof junk, 0x1000) == -1);
    T(wd_load_raw(m, junk, sizeof junk, WD_RAM_BASE + (16 << 20) - 50) == -1);
    T(wd_load_raw(m, junk, sizeof junk, WD_RAM_BASE) == 0);

    /* a tiny raw program: mov x0,#7 ; str w0,[ctl] -> exit(7) */
    uint32_t prog[] = {
        0xD28000E0,             /* mov x0, #7 */
        0xD2A12201,             /* mov x1, #0x09100000 */
        0xB9000020,             /* str w0, [x1] */
        0x14000000,             /* b . */
    };
    T(wd_load_image(m, (const uint8_t *)prog, sizeof prog) == 0);
    T(wd_run(m, 100) == WD_RUN_SHUTDOWN);
    T(wd_exit_code(m) == 7);
    T(wd_icount(m) == 3);
    wd_destroy(m);

    printf("unit tests: %d failures (%u + %u bitmask vectors)\n", failures, n64, n32);
    return failures ? 1 : 0;
}
