/*
 * The USB network adapter and the user-mode NAT, from the guest side:
 * a polling xHCI driver, a CDC NCM driver (NTB16), and just enough of an
 * IPv4 stack for DHCP, ARP, ICMP echo, DNS, UDP and TCP.
 *
 * Run with -N -i "T<tcp port> U<udp port> C<closed tcp port>\n": the test
 * harness listens on 127.0.0.1 (an echo service for TCP and UDP), which the
 * guest reaches as 10.0.2.2.
 */
#include "rt.h"

#define ECAM     0x3F000000UL
#define XHCI_BAR 0x10010000UL

static uint64_t dcbaa[16] __attribute__((aligned(64)));
static uint32_t cmdring[64 * 4] __attribute__((aligned(64)));
static uint32_t evring[64 * 4] __attribute__((aligned(64)));
static uint64_t erst[2] __attribute__((aligned(64)));
static uint8_t inctx[33 * 32] __attribute__((aligned(64)));
static uint8_t devctx[32 * 32] __attribute__((aligned(1024)));
static uint32_t rings[6][64 * 4] __attribute__((aligned(64)));   /* by DCI (1 = EP0, 3 = int IN, 4 = bulk OUT, 5 = bulk IN) */
static uint8_t ctl[512] __attribute__((aligned(64)));
static uint8_t notif[64] __attribute__((aligned(64)));
static uint8_t ntb_out[8192] __attribute__((aligned(64)));
static uint8_t ntb_in[16384] __attribute__((aligned(64)));

static uintptr_t op, rt, db;
static int slot, ev_i, ev_cyc = 1, cmd_i, cmd_cyc = 1;
static int ring_i[6], ring_cyc[6];
static volatile int cmd_done, cmd_cc, cmd_slot;
static int done_cc[6], done_res[6], done_flag[6];

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void poll_events(void)
{
    for (;;) {
        uint32_t *e = evring + 4 * ev_i;
        if ((int)(e[3] & 1) != ev_cyc) return;
        int type = (int)((e[3] >> 10) & 0x3F);
        if (type == 33) { cmd_cc = (int)(e[2] >> 24); cmd_slot = (int)(e[3] >> 24); cmd_done = 1; }
        else if (type == 32) {
            int dci = (int)((e[3] >> 16) & 0x1F);
            if (dci < 6) { done_cc[dci] = (int)(e[2] >> 24); done_res[dci] = (int)(e[2] & 0xFFFFFF); done_flag[dci] = 1; }
        }
        if (++ev_i == 64) { ev_i = 0; ev_cyc ^= 1; }
        mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)(evring + 4 * ev_i) | 8);
    }
}

static int wait_flag(volatile int *f, uint64_t limit)
{
    uint64_t t0 = icount();
    while (!*f) {
        poll_events();
        if (icount() - t0 > limit) return 0;
    }
    return 1;
}

static int command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    if (cmd_i == 63) {                                    /* link back to the start */
        uint32_t *l = cmdring + 4 * 63;
        l[0] = (uint32_t)(uintptr_t)cmdring; l[1] = 0; l[2] = 0;
        dsb();
        l[3] = (6u << 10) | 2 | (uint32_t)cmd_cyc;
        cmd_i = 0;
        cmd_cyc ^= 1;
    }
    uint32_t *c = cmdring + 4 * cmd_i++;
    c[0] = d0; c[1] = d1; c[2] = d2;
    dsb();
    c[3] = (d3 & ~1u) | (uint32_t)cmd_cyc;
    dsb();
    cmd_done = 0;
    mmio_w32(db, 0);
    if (!wait_flag(&cmd_done, 50000000)) return -1;
    return cmd_cc;
}

static void ring_put(int dci, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    uint32_t *r = rings[dci];
    if (ring_i[dci] == 63) {
        uint32_t *l = r + 4 * 63;
        l[0] = (uint32_t)(uintptr_t)r; l[1] = 0; l[2] = 0;
        dsb();
        l[3] = (6u << 10) | 2 | (uint32_t)ring_cyc[dci];
        ring_i[dci] = 0;
        ring_cyc[dci] ^= 1;
    }
    uint32_t *t = r + 4 * ring_i[dci]++;
    t[0] = d0; t[1] = d1; t[2] = d2;
    dsb();
    t[3] = (d3 & ~1u) | (uint32_t)ring_cyc[dci];
}

/* control transfer on EP0; returns bytes transferred or -1 */
static int control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *data)
{
    int in = type & 0x80;
    uint32_t setup0 = (uint32_t)type | (uint32_t)req << 8 | (uint32_t)value << 16;
    uint32_t setup1 = (uint32_t)index | (uint32_t)len << 16;
    if (!in && len) memcpy(ctl, data, len);
    ring_put(1, setup0, setup1, 8, (2u << 10) | (1u << 6) | ((len ? (in ? 3u : 2u) : 0u) << 16));
    if (len) ring_put(1, (uint32_t)(uintptr_t)ctl, 0, len, (3u << 10) | (in ? 1u << 16 : 0));
    ring_put(1, 0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : 1u << 16));
    dsb();
    done_flag[1] = 0;
    mmio_w32(db + 4 * (uintptr_t)slot, 1);
    if (!wait_flag(&done_flag[1], 50000000) || (done_cc[1] != 1 && done_cc[1] != 13)) return -1;
    if (in && len) {
        memcpy(data, ctl, len);
        /* the residual is reported on the status stage event: read it from the data stage instead */
    }
    return len;
}

/* one TD on a data endpoint: post and (if wait) wait; returns the bytes moved */
static void post(int dci, void *buf, int len)
{
    ring_put(dci, (uint32_t)(uintptr_t)buf, 0, (uint32_t)len, (1u << 10) | (1u << 5) | (dci & 1 ? 1u << 2 : 0));   /* normal, IOC, ISP */
    dsb();
    done_flag[dci] = 0;
    mmio_w32(db + 4 * (uintptr_t)slot, (uint32_t)dci);
}

static int wait_td(int dci, int len, uint64_t limit)
{
    if (!wait_flag(&done_flag[dci], limit)) return -1;
    if (done_cc[dci] != 1 && done_cc[dci] != 13) return -2;
    return len - done_res[dci];
}

/* ---------------------------------------------------------------- NCM */

static void w16le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32le(uint8_t *p, uint32_t v) { w16le(p, v); w16le(p + 2, v >> 16); }
static uint32_t r16le(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t r32le(const uint8_t *p) { return r16le(p) | r16le(p + 2) << 16; }

static void send_frame(const uint8_t *f, int len)
{
    /* NTH16, datagram at 16, NDP16 after it */
    memset(ntb_out, 0, 16);
    memcpy(ntb_out + 16, f, (size_t)len);
    int ndp = (16 + len + 3) & ~3;
    w32le(ntb_out, 0x484D434E); w16le(ntb_out + 4, 12); w16le(ntb_out + 6, 0);
    w16le(ntb_out + 8, (uint32_t)(ndp + 16)); w16le(ntb_out + 10, (uint32_t)ndp);
    memset(ntb_out + 16 + len, 0, (size_t)(ndp - 16 - len));
    w32le(ntb_out + ndp, 0x304D434E); w16le(ntb_out + ndp + 4, 16); w16le(ntb_out + ndp + 6, 0);
    w16le(ntb_out + ndp + 8, 16); w16le(ntb_out + ndp + 10, (uint32_t)len);
    w16le(ntb_out + ndp + 12, 0); w16le(ntb_out + ndp + 14, 0);
    post(4, ntb_out, ndp + 16);
    int r = wait_td(4, ndp + 16, 50000000);
    CHECK_EQ(r, ndp + 16);
}

/* received frames, from the NTBs of bulk IN */
static uint8_t rxf[64][1600];
static int rxl[64], rx_head, rx_n, in_posted, ntb_count, max_dgrams;

static void rx_service(void)
{
    if (!in_posted) { post(5, ntb_in, sizeof ntb_in); in_posted = 1; }
    poll_events();
    if (!done_flag[5]) return;
    in_posted = 0;
    int got = (int)sizeof ntb_in - done_res[5];
    CHECK(done_cc[5] == 1 || done_cc[5] == 13);
    const uint8_t *b = ntb_in;
    if (got < 12 || r32le(b) != 0x484D434E || r16le(b + 4) != 12 || (int)r16le(b + 8) != got) {
        test_failures++;
        printf_("bad NTB: %d bytes\n", got);
        return;
    }
    ntb_count++;
    int ndp = (int)r16le(b + 10), n = 0;
    CHECK_EQ(r32le(b + ndp), 0x304D434E);
    CHECK_EQ(ndp & 3, 0);
    for (int e = ndp + 8; e + 4 <= ndp + (int)r16le(b + ndp + 4); e += 4) {
        int idx = (int)r16le(b + e), l = (int)r16le(b + e + 2);
        if (!idx) break;
        CHECK_EQ(idx & 3, 0);
        if (rx_n < 64 && l <= 1600 && idx + l <= got) {
            int slotn = (rx_head + rx_n) % 64;
            memcpy(rxf[slotn], b + idx, (size_t)l);
            rxl[slotn] = l;
            rx_n++;
        }
        n++;
    }
    if (n > max_dgrams) max_dgrams = n;
}

/* next frame of the given ethertype (others are skipped), 0 on timeout */
static int recv_frame(uint8_t *out, int ethertype, uint64_t limit)
{
    uint64_t t0 = icount();
    for (;;) {
        while (rx_n) {
            int l = rxl[rx_head];
            const uint8_t *f = rxf[rx_head];
            rx_head = (rx_head + 1) % 64;
            rx_n--;
            if ((f[12] << 8 | f[13]) == ethertype) { memcpy(out, f, (size_t)l); return l; }
        }
        rx_service();
        if (icount() - t0 > limit) return 0;
    }
}

/* ---------------------------------------------------------------- IPv4 */

static const uint8_t my_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t gw_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
static uint32_t my_ip;
#define GW  0x0A000202u
#define DNS 0x0A000203u
static uint8_t pkt[1600], in[1600];
static uint16_t ipid = 1;

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v >> 16); w16(p + 2, v); }
static uint32_t r16(const uint8_t *p) { return (uint32_t)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) { return r16(p) << 16 | r16(p + 2); }

static uint32_t sum(uint32_t s, const uint8_t *p, int n)
{
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}

static uint16_t fold(uint32_t s)
{
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint32_t pseudo(uint32_t a, uint32_t b, int proto, int len)
{
    uint8_t ph[12];
    w32(ph, a); w32(ph + 4, b); ph[8] = 0; ph[9] = (uint8_t)proto; w16(ph + 10, (uint32_t)len);
    return sum(0, ph, 12);
}

/* payload at pkt + 34; fills Ethernet + IP and the transport checksum */
static void send_ip(uint32_t src, uint32_t dst, int proto, int tlen, const uint8_t *dmac)
{
    memcpy(pkt, dmac, 6); memcpy(pkt + 6, my_mac, 6); w16(pkt + 12, 0x0800);
    uint8_t *ip = pkt + 14, *t = ip + 20;
    ip[0] = 0x45; ip[1] = 0; w16(ip + 2, (uint32_t)(20 + tlen)); w16(ip + 4, ipid++); w16(ip + 6, 0);
    ip[8] = 128; ip[9] = (uint8_t)proto; w16(ip + 10, 0); w32(ip + 12, src); w32(ip + 16, dst);
    w16(ip + 10, fold(sum(0, ip, 20)));
    if (proto == 17) { w16(t + 6, 0); uint16_t c = fold(sum(pseudo(src, dst, 17, tlen), t, tlen)); w16(t + 6, c ? c : 0xFFFF); }
    if (proto == 6) { w16(t + 16, 0); w16(t + 16, fold(sum(pseudo(src, dst, 6, tlen), t, tlen))); }
    if (proto == 1) { w16(t + 2, 0); w16(t + 2, fold(sum(0, t, tlen))); }
    send_frame(pkt, 34 + tlen);
}

static void send_udp(uint32_t src, uint16_t sp, uint32_t dst, uint16_t dp, const void *d, int len, const uint8_t *dmac)
{
    uint8_t *u = pkt + 34;
    w16(u, sp); w16(u + 2, dp); w16(u + 4, (uint32_t)(8 + len));
    memcpy(u + 8, d, (size_t)len);
    send_ip(src, dst, 17, 8 + len, dmac);
}

/* next IPv4 packet with this protocol and destination port (0: any); checks the checksums */
static int recv_ip(int proto, uint16_t port, uint64_t limit)
{
    uint64_t t0 = icount();
    while (icount() - t0 < limit) {
        int l = recv_frame(in, 0x0800, limit);
        if (!l) return 0;
        const uint8_t *ip = in + 14;
        int tot = (int)r16(ip + 2);
        CHECK(memcmp(in + 6, gw_mac, 6) == 0);
        if (fold(sum(0, ip, 20)) != 0) { test_failures++; printf_("bad IP checksum\n"); continue; }
        if (ip[9] != proto) continue;
        const uint8_t *t = ip + 20;
        int tl = tot - 20;
        if (proto == 6 && fold(sum(pseudo(r32(ip + 12), r32(ip + 16), 6, tl), t, tl)) != 0) { test_failures++; printf_("bad TCP checksum\n"); continue; }
        if (proto == 17 && fold(sum(pseudo(r32(ip + 12), r32(ip + 16), 17, tl), t, tl)) != 0) { test_failures++; printf_("bad UDP checksum\n"); continue; }
        if (proto == 1 && fold(sum(0, t, tl)) != 0) { test_failures++; printf_("bad ICMP checksum\n"); continue; }
        if (port && (proto == 6 || proto == 17) && r16(t + 2) != port) continue;
        return tl;
    }
    return 0;
}

#define T(p) (in + 34)

/* ---------------------------------------------------------------- tests */

static void test_dhcp(void)
{
    uint8_t d[300];
    for (int round = 0; round < 2; round++) {
        memset(d, 0, sizeof d);
        d[0] = 1; d[1] = 1; d[2] = 6; w32(d + 4, 0x1234ABCD + (uint32_t)round);
        w16(d + 10, 0x8000);                              /* broadcast flag */
        memcpy(d + 28, my_mac, 6);
        w32(d + 236, 0x63825363u);
        int o = 240;
        d[o++] = 53; d[o++] = 1; d[o++] = round ? 3 : 1;  /* DISCOVER, then REQUEST */
        if (round) { d[o++] = 50; d[o++] = 4; w32(d + o, my_ip); o += 4; d[o++] = 54; d[o++] = 4; w32(d + o, GW); o += 4; }
        d[o++] = 55; d[o++] = 3; d[o++] = 1; d[o++] = 3; d[o++] = 6;
        d[o++] = 255;
        static const uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        send_udp(0, 68, 0xFFFFFFFFu, 67, d, o, bc);
        int tl = recv_ip(17, 68, 100000000);
        CHECK(tl > 248);
        if (tl <= 248) return;
        const uint8_t *b = T(0) + 8;
        CHECK_EQ(r32(in + 14 + 12), GW);
        CHECK_EQ(b[0], 2);
        CHECK_EQ(r32(b + 4), 0x1234ABCD + (uint32_t)round);
        CHECK(memcmp(b + 28, my_mac, 6) == 0);
        my_ip = r32(b + 16);
        CHECK_EQ(my_ip, 0x0A00020Fu);
        int type = 0;
        uint32_t router = 0, dns = 0, mask = 0, lease = 0;
        for (int p = 240; p < tl - 8 && b[p] != 255;) {
            if (b[p] == 0) { p++; continue; }
            if (b[p] == 53) type = b[p + 2];
            if (b[p] == 3) router = r32(b + p + 2);
            if (b[p] == 6) dns = r32(b + p + 2);
            if (b[p] == 1) mask = r32(b + p + 2);
            if (b[p] == 51) lease = r32(b + p + 2);
            p += 2 + b[p + 1];
        }
        CHECK_EQ(type, round ? 5 : 2);                    /* OFFER, ACK */
        CHECK_EQ(router, GW);
        CHECK_EQ(dns, DNS);
        CHECK_EQ(mask, 0xFFFFFF00u);
        CHECK(lease >= 3600);
    }
}

static void test_arp(void)
{
    static const uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint32_t targets[2] = { my_ip, GW };                  /* the first must stay unanswered */
    /* both requests in one NTB: two datagrams, the second at an aligned offset after the first */
    static uint8_t two[84];
    for (int k = 0; k < 2; k++) {
        uint8_t *f = two + 42 * k;
        memcpy(f, bc, 6); memcpy(f + 6, my_mac, 6); w16(f + 12, 0x0806);
        uint8_t *a = f + 14;
        w16(a, 1); w16(a + 2, 0x0800); a[4] = 6; a[5] = 4; w16(a + 6, 1);
        memcpy(a + 8, my_mac, 6); w32(a + 14, k ? my_ip : 0); memset(a + 18, 0, 6); w32(a + 24, targets[k]);
    }
    memset(ntb_out, 0, 256);
    memcpy(ntb_out + 16, two, 42);
    memcpy(ntb_out + 60, two + 42, 42);
    w32le(ntb_out, 0x484D434E); w16le(ntb_out + 4, 12); w16le(ntb_out + 8, 124); w16le(ntb_out + 10, 104);
    w32le(ntb_out + 104, 0x304D434E); w16le(ntb_out + 108, 20);
    w16le(ntb_out + 112, 16); w16le(ntb_out + 114, 42); w16le(ntb_out + 116, 60); w16le(ntb_out + 118, 42);
    post(4, ntb_out, 124);
    CHECK_EQ(wait_td(4, 124, 50000000), 124);
    int l = recv_frame(in, 0x0806, 50000000);
    CHECK(l >= 42);
    const uint8_t *a = in + 14;
    CHECK_EQ(r16(a + 6), 2);
    CHECK_EQ(r32(a + 14), GW);                            /* the reply is for the gateway */
    CHECK(memcmp(a + 8, gw_mac, 6) == 0);
    CHECK(memcmp(in, my_mac, 6) == 0);
    CHECK_EQ(r32(a + 24), my_ip);
}

static void test_ping(void)
{
    uint8_t *t = pkt + 34;
    t[0] = 8; t[1] = 0; w16(t + 4, 0x4242); w16(t + 6, 7);
    for (int i = 0; i < 56; i++) t[8 + i] = (uint8_t)(i * 3);
    send_ip(my_ip, GW, 1, 64, gw_mac);
    int tl = recv_ip(1, 0, 50000000);
    CHECK_EQ(tl, 64);
    const uint8_t *r = T(0);
    CHECK_EQ(r[0], 0);
    CHECK_EQ(r16(r + 4), 0x4242);
    CHECK_EQ(r16(r + 6), 7);
    int same = 1;
    for (int i = 0; i < 56; i++) same &= r[8 + i] == (uint8_t)(i * 3);
    CHECK(same);
    CHECK_EQ(r32(in + 14 + 12), GW);
}

static int dns_query(const char *name, int qtype, uint16_t id, uint32_t *addr, int *rcode)
{
    uint8_t q[128];
    memset(q, 0, 12);
    w16(q, id); w16(q + 2, 0x0100); w16(q + 4, 1);
    int o = 12;
    for (const char *s = name; *s;) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        q[o++] = (uint8_t)(dot - s);
        memcpy(q + o, s, (size_t)(dot - s));
        o += (int)(dot - s);
        s = *dot ? dot + 1 : dot;
    }
    q[o++] = 0;
    w16(q + o, (uint32_t)qtype); w16(q + o + 2, 1);
    o += 4;
    int qlen = o;
    send_udp(my_ip, 5353, DNS, 53, q, qlen, gw_mac);
    int tl = recv_ip(17, 5353, 200000000);
    if (tl < 8 + qlen) return -1;
    const uint8_t *r = T(0) + 8;
    CHECK_EQ(r32(in + 14 + 12), DNS);
    CHECK_EQ(r16(T(0)), 53);
    CHECK_EQ(r16(r), id);
    CHECK(r[2] & 0x80);
    CHECK(memcmp(r + 12, q + 12, (size_t)(qlen - 12)) == 0);   /* question echoed */
    *rcode = r[3] & 15;
    int an = (int)r16(r + 6), found = 0;
    int p = qlen;
    for (int i = 0; i < an && p + 12 <= tl - 8; i++) {
        CHECK_EQ(r16(r + p), 0xC00C);
        int type = (int)r16(r + p + 2), rdl = (int)r16(r + p + 10);
        if (type == 1 && rdl == 4 && !found) { *addr = r32(r + p + 12); found = 1; }
        p += 12 + rdl;
    }
    return an;
}

static void test_dns(void)
{
    uint32_t a = 0;
    int rc = -1;
    int an = dns_query("localhost", 1, 0x5151, &a, &rc);
    CHECK(an >= 1);
    CHECK_EQ(rc, 0);
    CHECK_EQ(a, 0x7F000001u);
    an = dns_query("localhost", 28, 0x5152, &a, &rc);      /* AAAA: an empty answer */
    CHECK_EQ(an, 0);
    CHECK_EQ(rc, 0);
}

static void test_udp(uint16_t port)
{
    char msg[200];
    for (int k = 0; k < 3; k++) {
        int n = snprintf_(msg, sizeof msg, "datagram %d to the host echo service %s", k, "....................");
        send_udp(my_ip, (uint16_t)(40000 + k), GW, port, msg, n, gw_mac);
        int tl = recv_ip(17, (uint16_t)(40000 + k), 200000000);
        CHECK_EQ(tl, 8 + n);
        if (tl != 8 + n) continue;
        CHECK_EQ(r16(T(0)), port);                         /* from 10.0.2.2:port */
        CHECK_EQ(r32(in + 14 + 12), GW);
        CHECK(memcmp(T(0) + 8, msg, (size_t)n) == 0);
    }
}

/* ---- TCP: a small client */

enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };

static void tcp_out(uint16_t sp, uint16_t dp, uint32_t seq, uint32_t ack, int flags, const uint8_t *d, int len, uint32_t wnd)
{
    uint8_t *t = pkt + 34;
    int hl = flags & SYN ? 24 : 20;
    w16(t, sp); w16(t + 2, dp); w32(t + 4, seq); w32(t + 8, ack);
    t[12] = (uint8_t)((hl / 4) << 4); t[13] = (uint8_t)flags; w16(t + 14, wnd); w16(t + 16, 0); w16(t + 18, 0);
    if (flags & SYN) { t[20] = 2; t[21] = 4; w16(t + 22, 1460); }
    if (len) memcpy(t + hl, d, (size_t)len);
    send_ip(my_ip, GW, 6, hl + len, gw_mac);
}

static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }

#define TCP_BYTES 300000
#define WND 8192                                          /* our receive window: the NAT must respect it */

static void test_tcp(uint16_t port)
{
    uint16_t sp = 50001;
    uint32_t iss = 1000000, snd_una = iss + 1, snd_nxt = iss + 1, rcv_nxt = 0, peer_wnd = 0;
    tcp_out(sp, port, iss, 0, SYN, NULL, 0, 65535);
    int tl = recv_ip(6, sp, 200000000);
    CHECK(tl >= 24);
    if (tl < 24) return;
    const uint8_t *t = T(0);
    CHECK_EQ(t[13] & (SYN | ACK), SYN | ACK);
    CHECK_EQ(r32(t + 8), iss + 1);
    CHECK_EQ(t[12] >> 4, 6);                              /* MSS option */
    CHECK_EQ(t[20], 2);
    CHECK_EQ(r16(t + 22), 1460);
    rcv_nxt = r32(t + 4) + 1;
    peer_wnd = r16(t + 14);
    CHECK(peer_wnd >= 1460);
    tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, NULL, 0, WND);

    /* stream TCP_BYTES to the echo service while reading the echo back */
    static uint8_t seg[1460];
    uint32_t recvd = 0, sent_fin = 0, got_fin = 0, idle = 0, mismatch = 0, beyond = 0, dropped = 0, resent = 0;
    uint32_t acked_to_peer = rcv_nxt;
    uint64_t t0 = icount();
    while (!(got_fin && snd_una == iss + 1 + TCP_BYTES + 1)) {
        if (icount() - t0 > 3000000000ULL) break;
        /* send while the window allows */
        uint32_t off = snd_nxt - (iss + 1);
        if (off < TCP_BYTES && snd_nxt - snd_una + 1460 <= peer_wnd) {
            int n = TCP_BYTES - (int)off < 1460 ? TCP_BYTES - (int)off : 1460;
            for (int i = 0; i < n; i++) seg[i] = pattern(off + (uint32_t)i);
            tcp_out(sp, port, snd_nxt, rcv_nxt, ACK | PSH, seg, n, WND);
            acked_to_peer = rcv_nxt;
            snd_nxt += (uint32_t)n;
            continue;
        }
        if (off == TCP_BYTES && recvd == TCP_BYTES && !sent_fin) {   /* everything echoed: close */
            tcp_out(sp, port, snd_nxt, rcv_nxt, FIN | ACK, NULL, 0, WND);
            acked_to_peer = rcv_nxt;
            snd_nxt++;
            sent_fin = 1;
        }
        tl = recv_ip(6, sp, 3000000);
        if (!tl) {
            if (++idle > 20) {                             /* nothing for a while: go back */
                snd_nxt = snd_una;
                if (sent_fin && snd_una != iss + 1 + TCP_BYTES + 1) sent_fin = 0;
                idle = 0;
            }
            continue;
        }
        idle = 0;
        t = T(0);
        int hl = (t[12] >> 4) * 4, dl = tl - hl, fl = t[13];
        if (fl & RST) { test_failures++; printf_("unexpected RST\n"); return; }
        uint32_t a = r32(t + 8);
        if ((fl & ACK) && (int32_t)(a - snd_una) > 0 && (int32_t)(a - snd_nxt) <= 0) snd_una = a;
        peer_wnd = r16(t + 14);
        uint32_t seq = r32(t + 4);
        if (dl > 0 && (int32_t)(seq + (uint32_t)dl - (acked_to_peer + WND)) > 0) beyond++;
        if (dl > 0 && seq == rcv_nxt && recvd >= 100000 && !dropped) {
            dropped = 1;                                   /* lose one segment: the NAT must send it again */
            continue;
        }
        if (dl > 0 && dropped == 1 && seq != rcv_nxt) continue;   /* the rest of that flight: dropped too */
        if (dl > 0 && dropped == 1 && seq == rcv_nxt) { dropped = 2; resent = 1; }
        if (dl > 0 || (fl & FIN)) {
            if (seq == rcv_nxt) {
                for (int i = 0; i < dl; i++) mismatch += t[hl + i] != pattern(recvd + (uint32_t)i);
                recvd += (uint32_t)dl;
                rcv_nxt += (uint32_t)dl;
                if (fl & FIN) { rcv_nxt++; got_fin = 1; }
            }
            tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, NULL, 0, WND);
            acked_to_peer = rcv_nxt;
        }
    }
    CHECK_EQ(beyond, 0);
    CHECK_EQ(resent, 1);
    CHECK_EQ(recvd, TCP_BYTES);
    CHECK_EQ(mismatch, 0);
    CHECK(got_fin);
    CHECK_EQ(snd_una, iss + 1 + TCP_BYTES + 1);           /* our data and FIN acknowledged */
    printf_("tcp: %u bytes echoed\n", recvd);
}

static void test_tcp_refused(uint16_t port)
{
    tcp_out(50002, port, 777, 0, SYN, NULL, 0, 65535);
    int tl = recv_ip(6, 50002, 200000000);
    CHECK(tl >= 20);
    if (tl < 20) return;
    CHECK_EQ(T(0)[13] & (RST | ACK), RST | ACK);
    CHECK_EQ(r32(T(0) + 8), 778);
    /* a stray segment for no connection gets a reset too */
    tcp_out(50003, port, 5, 99, ACK, NULL, 0, 65535);
    tl = recv_ip(6, 50003, 50000000);
    CHECK(tl >= 20);
    if (tl >= 20) {
        CHECK_EQ(T(0)[13] & (RST | ACK), RST);
        CHECK_EQ(r32(T(0) + 4), 99);
    }
}

/* to a reader that starts late: the NAT's window closes, opens again with a
 * window update, and a probe sent into the closed window must not be taken */
static void test_tcp_slow(uint16_t port)
{
    uint16_t sp = 50010;
    uint32_t iss = 5000000, snd_una = iss + 1, snd_nxt = iss + 1, rcv_nxt = 0, peer_wnd = 0;
    tcp_out(sp, port, iss, 0, SYN, NULL, 0, 65535);
    int tl = recv_ip(6, sp, 200000000);
    CHECK(tl >= 24);
    if (tl < 24) return;
    const uint8_t *t = T(0);
    rcv_nxt = r32(t + 4) + 1;
    peer_wnd = r16(t + 14);
    tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, NULL, 0, 65535);
    static uint8_t seg[1460];
    char reply[64];
    int rlen = 0, got_fin = 0, sent_fin = 0, zero_seen = 0, probes = 0, idle = 0;
    uint64_t t0 = icount();
    while (!got_fin && icount() - t0 < 20000000000ULL) {
        uint32_t off = snd_nxt - (iss + 1);
        uint32_t inflight = snd_nxt - snd_una;
        if (off < TCP_BYTES && inflight + 1460 <= peer_wnd) {
            int n = TCP_BYTES - (int)off < 1460 ? TCP_BYTES - (int)off : 1460;
            for (int i = 0; i < n; i++) seg[i] = pattern(off + (uint32_t)i);
            tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, seg, n, 65535);
            snd_nxt += (uint32_t)n;
            continue;
        }
        if (snd_una - (iss + 1) == TCP_BYTES && !sent_fin) {
            tcp_out(sp, port, snd_nxt, rcv_nxt, FIN | ACK, NULL, 0, 65535);
            snd_nxt++;
            sent_fin = 1;
        }
        tl = recv_ip(6, sp, 3000000);
        if (!tl) {
            if (++idle > 10 && off < TCP_BYTES && snd_nxt == snd_una) {
                /* window (nearly) closed: probe with a full segment beyond it */
                int n = TCP_BYTES - (int)off < 1460 ? TCP_BYTES - (int)off : 1460;
                for (int i = 0; i < n; i++) seg[i] = pattern(off + (uint32_t)i);
                tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, seg, n, 65535);
                probes++;
                idle = 0;
            } else if (idle > 40) {
                snd_nxt = snd_una;                         /* go back */
                if (sent_fin && snd_una != iss + 1 + TCP_BYTES + 1) sent_fin = 0;
                idle = 0;
            }
            continue;
        }
        idle = 0;
        t = T(0);
        int hl = (t[12] >> 4) * 4, dl = tl - hl, fl = t[13];
        if (fl & RST) { test_failures++; printf_("slow: unexpected RST\n"); return; }
        uint32_t a = r32(t + 8);
        if ((fl & ACK) && (int32_t)(a - snd_una) > 0 && (int32_t)(a - (iss + 1 + TCP_BYTES + 1)) <= 0) snd_una = a;
        if ((int32_t)(snd_nxt - snd_una) < 0) snd_nxt = snd_una;
        peer_wnd = r16(t + 14);
        if (peer_wnd < 1460 && snd_una - (iss + 1) < TCP_BYTES) zero_seen = 1;
        uint32_t seq = r32(t + 4);
        if (dl > 0 || (fl & FIN)) {
            if (seq == rcv_nxt) {
                for (int i = 0; i < dl && rlen < 63; i++) reply[rlen++] = (char)t[hl + i];
                rcv_nxt += (uint32_t)dl;
                if (fl & FIN) { rcv_nxt++; got_fin = 1; }
            }
            tcp_out(sp, port, snd_nxt, rcv_nxt, ACK, NULL, 0, 65535);
        }
    }
    reply[rlen] = 0;
    CHECK(zero_seen);
    CHECK(probes > 0);
    CHECK(got_fin);
    CHECK(strcmp(reply, "GOOD 300000") == 0);
    if (strcmp(reply, "GOOD 300000") == 0) printf_("tcp: slow reader got all %d bytes (%d probes)\n", TCP_BYTES, probes);
    else printf_("slow reader: '%s'\n", reply);
}

/* ---------------------------------------------------------------- setup */

static int read_ports(uint16_t *tp, uint16_t *up, uint16_t *cp, uint16_t *sp)
{
    char line[64];
    int n = 0;
    uint64_t t0 = icount();
    while (n < 63 && icount() - t0 < 100000000ULL) {
        int ch = getc_nb();
        if (ch < 0) continue;
        if (ch == '\n' || ch == '\r') break;
        line[n++] = (char)ch;
    }
    line[n] = 0;
    *tp = *up = *cp = *sp = 0;
    for (int i = 0; i < n; i++) {
        uint16_t *dst = line[i] == 'T' ? tp : line[i] == 'U' ? up : line[i] == 'C' ? cp : line[i] == 'S' ? sp : NULL;
        if (!dst) continue;
        uint32_t v = 0;
        while (i + 1 < n && line[i + 1] >= '0' && line[i + 1] <= '9') v = v * 10 + (uint32_t)(line[++i] - '0');
        *dst = (uint16_t)v;
    }
    return *tp && *up && *cp && *sp;
}

static int usb_setup(void)
{
    /* xHCI at slot 0 with BAR0 already assigned? assign it here */
    uintptr_t cfg = 0;
    for (int dev = 0; dev < 32 && !cfg; dev++) {
        uintptr_t c = ECAM + ((uintptr_t)dev << 15);
        if (mmio_r32(c) == (0x000Du << 16 | 0x1B36)) cfg = c;
    }
    CHECK(cfg != 0);
    if (!cfg) return 0;
    mmio_w32(cfg + 0x10, (uint32_t)XHCI_BAR);
    mmio_w32(cfg + 0x14, 0);
    mmio_w32(cfg + 0x04, 0x6);                            /* memory, bus master */
    op = XHCI_BAR + (mmio_r32(XHCI_BAR) & 0xFF);
    rt = XHCI_BAR + (mmio_r32(XHCI_BAR + 0x18) & ~0x1Fu);
    db = XHCI_BAR + (mmio_r32(XHCI_BAR + 0x14) & ~3u);
    mmio_w32(op, 2);
    while (mmio_r32(op) & 2) {}
    while (mmio_r32(op + 4) & (1u << 11)) {}
    mmio_w32(op + 0x38, 8);
    mmio_w64(op + 0x30, (uint64_t)(uintptr_t)dcbaa);
    mmio_w64(op + 0x18, (uint64_t)(uintptr_t)cmdring | 1);
    erst[0] = (uint64_t)(uintptr_t)evring;
    erst[1] = 64;
    mmio_w32(rt + 0x28, 1);
    mmio_w64(rt + 0x38, (uint64_t)(uintptr_t)evring);
    mmio_w64(rt + 0x30, (uint64_t)(uintptr_t)erst);
    mmio_w32(op, 1);                                      /* run, no interrupts: polling */
    while (mmio_r32(op + 4) & 1) {}

    int port = 0;
    for (int p = 1; p <= 8 && !port; p++) if (mmio_r32(op + 0x400 + 0x10 * (uintptr_t)(p - 1)) & 1) port = p;
    CHECK(port != 0);
    if (!port) return 0;
    uintptr_t ps = op + 0x400 + 0x10 * (uintptr_t)(port - 1);
    mmio_w32(ps, (mmio_r32(ps) & 0x0E00C3E0u) | (1u << 4));
    for (uint64_t t0 = icount(); !(mmio_r32(ps) & 2) && icount() - t0 < 10000000;) poll_events();
    CHECK(mmio_r32(ps) & 2);
    int speed = (int)((mmio_r32(ps) >> 10) & 15);
    CHECK_EQ(speed, 3);                                   /* high speed */
    mmio_w32(ps, (mmio_r32(ps) & 0x0E00C3E0u) | (1u << 21) | (1u << 17));
    poll_events();

    CHECK_EQ(command(0, 0, 0, 9u << 10), 1);              /* enable slot */
    slot = cmd_slot;
    dcbaa[slot] = (uint64_t)(uintptr_t)devctx;
    memset(inctx, 0, sizeof inctx);
    uint32_t *icc = (uint32_t *)inctx, *sc = (uint32_t *)(inctx + 32), *ep0 = (uint32_t *)(inctx + 64);
    icc[1] = 3;
    sc[0] = (1u << 27) | ((uint32_t)speed << 20);
    sc[1] = (uint32_t)port << 16;
    for (int i = 0; i < 6; i++) { ring_i[i] = 0; ring_cyc[i] = 1; memset(rings[i], 0, sizeof rings[i]); }
    ep0[1] = (4u << 3) | (3u << 1) | (64u << 16);
    ep0[2] = (uint32_t)(uintptr_t)rings[1] | 1;
    ep0[4] = 8;
    CHECK_EQ(command((uint32_t)(uintptr_t)inctx, 0, 0, (11u << 10) | ((uint32_t)slot << 24)), 1);

    uint8_t d[256];
    CHECK_EQ(control(0x80, 6, 0x0100, 0, 18, d), 18);
    CHECK_EQ(d[4], 0xEF);                                 /* IAD device class */
    CHECK_EQ(r16le(d + 8), 0x1209);
    int cl = control(0x80, 6, 0x0200, 0, 9, d);
    CHECK_EQ(cl, 9);
    int total = (int)r16le(d + 2);
    CHECK(total > 9 && total <= 256);
    control(0x80, 6, 0x0200, 0, (uint16_t)total, d);
    /* walk the configuration: IAD, NCM functional descriptor, endpoints */
    int iad = 0, ncmfd = 0, eth_imac = 0, eps = 0;
    for (int p = 0; p + 2 <= total && d[p];) {
        if (d[p + 1] == 0x0B && d[p + 4] == 2 && d[p + 5] == 0x0D) iad = 1;
        if (d[p + 1] == 0x24 && d[p + 2] == 0x1A) ncmfd = 1;
        if (d[p + 1] == 0x24 && d[p + 2] == 0x0F) eth_imac = d[p + 3];
        if (d[p + 1] == 5) eps++;
        p += d[p];
    }
    CHECK(iad && ncmfd);
    CHECK_EQ(eps, 3);
    CHECK(eth_imac != 0);
    uint8_t s[64];
    int sl = control(0x80, 6, (uint16_t)(0x0300 | eth_imac), 0x0409, 64, s);
    CHECK(sl > 0);
    static const char want_mac[] = "525400123456";
    int macok = s[0] == 26;
    for (int i = 0; i < 12; i++) macok &= s[2 + 2 * i] == (uint8_t)want_mac[i];
    CHECK(macok);

    CHECK_EQ(control(0x00, 9, 1, 0, 0, NULL), 0);         /* SET_CONFIGURATION */
    /* configure: DCI 3 interrupt IN 16, DCI 4 bulk OUT 512, DCI 5 bulk IN 512 */
    memset(inctx, 0, sizeof inctx);
    icc[1] = 1 | (1u << 3) | (1u << 4) | (1u << 5);
    sc[0] = (5u << 27) | ((uint32_t)speed << 20);
    sc[1] = (uint32_t)port << 16;
    static const uint32_t type[6] = { 0, 0, 0, 7, 2, 6 }, mps[6] = { 0, 0, 0, 16, 512, 512 };
    for (int dci = 3; dci <= 5; dci++) {
        uint32_t *e = (uint32_t *)(inctx + 32 * (dci + 1));
        e[0] = dci == 3 ? 8u << 16 : 0;                   /* interval */
        e[1] = (type[dci] << 3) | (3u << 1) | (mps[dci] << 16);
        e[2] = (uint32_t)(uintptr_t)rings[dci] | 1;
        e[4] = mps[dci];
    }
    CHECK_EQ(command((uint32_t)(uintptr_t)inctx, 0, 0, (12u << 10) | ((uint32_t)slot << 24)), 1);

    /* NCM class requests */
    uint8_t np[28];
    CHECK_EQ(control(0xA1, 0x80, 0, 0, 28, np), 28);      /* GET_NTB_PARAMETERS */
    CHECK_EQ(r16le(np), 28);
    CHECK_EQ(r16le(np + 2) & 1, 1);
    CHECK(r32le(np + 4) >= 16384);
    CHECK_EQ(r16le(np + 12), 4);                          /* alignment */
    uint8_t sz[4];
    w32le(sz, sizeof ntb_in);
    CHECK_EQ(control(0x21, 0x86, 0, 0, 4, sz), 4);        /* SET_NTB_INPUT_SIZE */
    memset(sz, 0, 4);
    CHECK_EQ(control(0xA1, 0x85, 0, 0, 4, sz), 4);
    CHECK_EQ(r32le(sz), sizeof ntb_in);
    CHECK_EQ(control(0x21, 0x84, 1, 0, 0, NULL), -1);     /* NTB32 not supported: stall */
    /* a stalled control endpoint must be reset before the next request (xHCI 4.6.8) */
    CHECK_EQ(command(0, 0, 0, (14u << 10) | ((uint32_t)slot << 24) | (1u << 16)), 1);   /* reset endpoint 1 */
    CHECK_EQ(command((uint32_t)(uintptr_t)(rings[1] + 4 * ring_i[1]) | (uint32_t)ring_cyc[1], 0, 0,
                     (16u << 10) | ((uint32_t)slot << 24) | (1u << 16)), 1);           /* set TR dequeue */
    CHECK_EQ(control(0x21, 0x43, 0x0C, 0, 0, NULL), 0);   /* packet filter: directed + broadcast */
    CHECK_EQ(control(0x01, 11, 1, 1, 0, NULL), 0);        /* SET_INTERFACE(1, 1): link up */
    uint8_t alt = 9;
    CHECK_EQ(control(0x81, 10, 0, 1, 1, &alt), 1);
    CHECK_EQ(alt, 1);

    /* notifications: speed change, then connected */
    post(3, notif, 16);
    CHECK_EQ(wait_td(3, 16, 100000000), 16);
    CHECK_EQ(notif[0], 0xA1);
    CHECK_EQ(notif[1], 0x2A);
    CHECK_EQ(r32le(notif + 8), 1000000000u);
    post(3, notif, 16);
    CHECK_EQ(wait_td(3, 16, 100000000), 8);
    CHECK_EQ(notif[1], 0x00);
    CHECK_EQ(r16le(notif + 2), 1);
    return 1;
}

int main(void)
{
    uint16_t tport, uport, cport, sport;
    CHECK(read_ports(&tport, &uport, &cport, &sport));
    if (usb_setup()) {
        test_dhcp();
        test_arp();
        test_ping();
        test_dns();
        test_udp(uport);
        test_tcp(tport);
        test_tcp_refused(cport);
        test_tcp_slow(sport);
    }
    printf_("net: %d failures (%d NTBs, up to %d datagrams in one)\n", test_failures, ntb_count, max_dgrams);
    return test_failures ? 1 : 0;
}
