/*
 * WindDroid - user-mode NAT (see nat.h).
 *
 * TCP connections from the guest are terminated here: a guest SYN makes a
 * non-blocking connect() on the host, the SYN-ACK goes back once that
 * succeeded (an RST if it failed), and from then on bytes are copied between
 * the host socket and a small TCP sender/receiver facing the guest. The link
 * to the guest does not lose frames, so the sender is simple: no window
 * scaling, MSS 1460, go-back-N retransmission on a timeout.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "nat.h"

#define MAX_TCP   256
#define MAX_UDP   64
#define MAX_ICMP  16
#define MAX_DNS   32
#define SBUF_MAX  (256 * 1024)            /* host -> guest bytes not yet acknowledged */
#define RBUF_MAX  (64 * 1024 - 1)         /* guest -> host bytes not yet written (= our window) */
#define OUR_MSS   1460
#define RTO_MS    400
#define UDP_IDLE_MS  120000
#define ICMP_IDLE_MS 10000
#define CONNECT_TIMEOUT_MS 30000

static const uint8_t gw_mac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

enum { T_FREE, T_CONNECTING, T_SYNACK, T_OPEN };

typedef struct {
    int state, fd;
    uint32_t gip, rip;                    /* guest address, remote address as the guest sees it */
    uint16_t gport, rport;
    uint32_t iss, sbase, snd_nxt, rcv_nxt, fin_seq;
    uint32_t wnd;                         /* guest's receive window */
    uint16_t mss;
    uint8_t *sbuf, *rbuf;
    int slen, rlen;
    int host_eof, fin_sent, fin_acked, guest_fin, shut_wr, last_wnd;
    uint64_t t_progress, t_start;
} tcp_conn;

typedef struct {
    int fd;
    uint32_t gip;
    uint16_t gport;
    uint64_t last;
} udp_conn;

typedef struct {
    int fd;
    uint32_t gip, dip;
    uint16_t gid;
    uint64_t last;
} icmp_conn;

/* DNS lookups run on their own threads; the queue outlives the NAT if a lookup is slow */
typedef struct dns_job {
    struct dns_job *next;
    uint8_t q[512];
    int qlen, qend;                       /* query length, end of the question section */
    uint32_t gip;
    uint16_t gport;
    char name[256];
    int done, rcode, naddr;
    uint32_t addr[8];
} dns_job;

typedef struct {
    pthread_mutex_t lock;
    int refs, dead;
    dns_job *done;
    int pending;
} dns_queue;

struct wd_nat {
    wd_nat_ops ops;
    uint8_t guest_mac[6];
    int have_mac;
    uint16_t ip_id;
    uint32_t iss_seed;
    int sndbuf;                           /* SO_SNDBUF for host TCP sockets (tests: WINDDROID_NAT_SNDBUF) */
    tcp_conn tcp[MAX_TCP];
    udp_conn udp[MAX_UDP];
    icmp_conn icmp[MAX_ICMP];
    dns_queue *dq;
    uint64_t frames_in, frames_out;
    uint8_t frame[1600];
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint32_t csum_add(uint32_t sum, const uint8_t *p, int len)
{
    for (int i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t pseudo(uint32_t src, uint32_t dst, int proto, int len)
{
    uint8_t ph[12];
    wr32(ph, src); wr32(ph + 4, dst); ph[8] = 0; ph[9] = (uint8_t)proto; wr16(ph + 10, (uint32_t)len);
    return csum_add(0, ph, 12);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* the address the guest uses -> the host address to connect to (0: not reachable) */
static uint32_t map_out(uint32_t ip)
{
    if (ip == NAT_GW) return 0x7F000001u;
    if ((ip & 0xFFFFFF00u) == NAT_NET) return 0;
    if (ip == 0 || ip == 0xFFFFFFFFu || (ip >> 28) == 0xE) return 0;   /* broadcast, multicast */
    return ip;
}

/* a host address seen in a reply -> what the guest should see */
static uint32_t map_in(uint32_t ip)
{
    return (ip >> 24) == 127 ? NAT_GW : ip;
}

/* ---------------------------------------------------------------- output */

static int room(wd_nat *n) { return n->ops.room ? n->ops.room(n->ops.opaque) : 1; }

/* frame = 14-byte Ethernet header + payload already in n->frame */
static int send_frame(wd_nat *n, const uint8_t *dst_mac, int ethertype, int len)
{
    memcpy(n->frame, dst_mac, 6);
    memcpy(n->frame + 6, gw_mac, 6);
    wr16(n->frame + 12, (uint32_t)ethertype);
    if (n->ops.send(n->ops.opaque, n->frame, len) != 0) return -1;
    n->frames_out++;
    return 0;
}

/* IPv4 packet: the transport header and data are already at n->frame + 34; computes the checksums */
static int send_ip(wd_nat *n, uint32_t src, uint32_t dst, int proto, int tlen)
{
    uint8_t *ip = n->frame + 14, *t = ip + 20;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, (uint32_t)(20 + tlen));
    wr16(ip + 4, n->ip_id++);
    wr16(ip + 6, 0x4000);                 /* don't fragment */
    ip[8] = 64; ip[9] = (uint8_t)proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, src); wr32(ip + 16, dst);
    wr16(ip + 10, csum_fold(csum_add(0, ip, 20)));
    if (proto == 6) {
        wr16(t + 16, 0);
        wr16(t + 16, csum_fold(csum_add(pseudo(src, dst, 6, tlen), t, tlen)));
    } else if (proto == 17) {
        wr16(t + 6, 0);
        uint16_t c = csum_fold(csum_add(pseudo(src, dst, 17, tlen), t, tlen));
        wr16(t + 6, c ? c : 0xFFFF);
    } else if (proto == 1) {
        wr16(t + 2, 0);
        wr16(t + 2, csum_fold(csum_add(0, t, tlen)));
    }
    return send_frame(n, dst == 0xFFFFFFFFu || !n->have_mac ? bcast_mac : n->guest_mac, 0x0800, 34 + tlen);
}

static int send_udp(wd_nat *n, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int len)
{
    if (len > 1472) return -1;
    uint8_t *u = n->frame + 34;
    wr16(u, sport); wr16(u + 2, dport); wr16(u + 4, (uint32_t)(8 + len));
    memmove(u + 8, data, (size_t)len);
    return send_ip(n, src, dst, 17, 8 + len);
}

/* ---------------------------------------------------------------- TCP */

enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };

static int our_window(const tcp_conn *c) { return RBUF_MAX - c->rlen; }

static int tcp_send(wd_nat *n, tcp_conn *c, uint32_t seq, int flags, const uint8_t *data, int len)
{
    uint8_t *t = n->frame + 34;
    int hl = flags & F_SYN ? 24 : 20;
    wr16(t, c->rport); wr16(t + 2, c->gport);
    wr32(t + 4, seq);
    wr32(t + 8, flags & F_ACK ? c->rcv_nxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = (uint8_t)flags;
    int w = our_window(c);
    c->last_wnd = w;
    wr16(t + 14, (uint32_t)w);
    wr16(t + 16, 0); wr16(t + 18, 0);
    if (flags & F_SYN) { t[20] = 2; t[21] = 4; wr16(t + 22, OUR_MSS); }
    if (len) memcpy(t + hl, data, (size_t)len);
    return send_ip(n, c->rip, c->gip, 6, hl + len);
}

/* RST for a segment that belongs to no connection */
static void tcp_reset_reply(wd_nat *n, uint32_t gip, uint16_t gport, uint32_t rip, uint16_t rport,
                            uint32_t seq, uint32_t ack, int ack_valid)
{
    tcp_conn tmp = { 0 };
    tmp.gip = gip; tmp.gport = gport; tmp.rip = rip; tmp.rport = rport;
    tmp.rcv_nxt = ack;
    tmp.rlen = RBUF_MAX;                  /* window 0 */
    tcp_send(n, &tmp, seq, F_RST | (ack_valid ? F_ACK : 0), NULL, 0);
}

static void tcp_free(tcp_conn *c)
{
    if (c->fd >= 0) close(c->fd);
    free(c->sbuf);
    free(c->rbuf);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->state = T_FREE;
}

static void tcp_abort(wd_nat *n, tcp_conn *c)
{
    tcp_send(n, c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
    tcp_free(c);
}

/* send what the window allows, the FIN when the host side is done */
static void tcp_output(wd_nat *n, tcp_conn *c, uint64_t now)
{
    if (c->state != T_OPEN) return;
    if ((c->snd_nxt != c->sbase || (c->fin_sent && !c->fin_acked)) && now - c->t_progress > RTO_MS) {
        c->snd_nxt = c->sbase;            /* go back: resend everything not acknowledged */
        if (!c->fin_acked) c->fin_sent = 0;
        c->t_progress = now;
    }
    for (;;) {
        int sent = (int)(c->snd_nxt - c->sbase);
        int left = c->slen - sent;
        int allowed = (int)c->wnd - sent;
        int len = left < c->mss ? left : c->mss;
        if (len > allowed) len = allowed;
        if (len <= 0) break;
        if (room(n) < 2) return;
        int last = len == left;
        if (tcp_send(n, c, c->snd_nxt, F_ACK | (last ? F_PSH : 0), c->sbuf + sent, len)) return;
        if (c->snd_nxt == c->sbase) c->t_progress = now;
        c->snd_nxt += (uint32_t)len;
    }
    if (c->host_eof && !c->fin_sent && (int)(c->snd_nxt - c->sbase) == c->slen && room(n) >= 1) {
        if (tcp_send(n, c, c->snd_nxt, F_FIN | F_ACK, NULL, 0) == 0) {
            if (c->snd_nxt == c->sbase) c->t_progress = now;
            c->fin_seq = c->snd_nxt;
            c->fin_sent = 1;
        }
    }
}

static void tcp_flush_to_host(wd_nat *n, tcp_conn *c)
{
    while (c->rlen > 0) {
        ssize_t w = send(c->fd, c->rbuf, (size_t)c->rlen, MSG_NOSIGNAL);
        if (w <= 0) break;
        memmove(c->rbuf, c->rbuf + w, (size_t)(c->rlen - w));
        c->rlen -= (int)w;
    }
    if (c->guest_fin && c->rlen == 0 && !c->shut_wr) {
        shutdown(c->fd, SHUT_WR);
        c->shut_wr = 1;
    }
    /* window update once a full segment fits again */
    if (c->last_wnd < OUR_MSS && our_window(c) >= OUR_MSS && c->state == T_OPEN)
        tcp_send(n, c, c->snd_nxt, F_ACK, NULL, 0);
}

static void tcp_maybe_close(tcp_conn *c)
{
    if (c->state == T_OPEN && c->fin_acked && c->guest_fin && c->rlen == 0) tcp_free(c);
}

static tcp_conn *tcp_find(wd_nat *n, uint32_t gip, uint16_t gport, uint32_t rip, uint16_t rport)
{
    for (int i = 0; i < MAX_TCP; i++) {
        tcp_conn *c = &n->tcp[i];
        if (c->state != T_FREE && c->gip == gip && c->gport == gport && c->rip == rip && c->rport == rport) return c;
    }
    return NULL;
}

static void tcp_input(wd_nat *n, uint32_t src, uint32_t dst, const uint8_t *t, int len)
{
    if (len < 20) return;
    int hl = (t[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint16_t sport = rd16(t), dport = rd16(t + 2);
    uint32_t seq = rd32(t + 4), ack = rd32(t + 8);
    int flags = t[13];
    uint32_t wnd = rd16(t + 14);
    const uint8_t *data = t + hl;
    int dlen = len - hl;
    uint64_t now = now_ms();
    tcp_conn *c = tcp_find(n, src, sport, dst, dport);

    if (!c) {
        if (flags & F_RST) return;
        if ((flags & (F_SYN | F_ACK)) != F_SYN) {
            tcp_reset_reply(n, src, sport, dst, dport, flags & F_ACK ? ack : 0,
                            seq + (uint32_t)dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0), !(flags & F_ACK));
            return;
        }
        uint32_t host = map_out(dst);
        tcp_conn *f = NULL;
        for (int i = 0; i < MAX_TCP && !f; i++) if (n->tcp[i].state == T_FREE) f = &n->tcp[i];
        int fd = host && f ? socket(AF_INET, SOCK_STREAM, 0) : -1;
        if (fd < 0) {
            tcp_reset_reply(n, src, sport, dst, dport, 0, seq + 1, 1);
            return;
        }
        set_nonblock(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (n->sndbuf > 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n->sndbuf, sizeof n->sndbuf);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(dport), .sin_addr.s_addr = htonl(host) };
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 && errno != EINPROGRESS) {
            close(fd);
            tcp_reset_reply(n, src, sport, dst, dport, 0, seq + 1, 1);
            return;
        }
        memset(f, 0, sizeof *f);
        f->fd = fd;
        f->state = T_CONNECTING;
        f->gip = src; f->gport = sport; f->rip = dst; f->rport = dport;
        f->rcv_nxt = seq + 1;
        f->iss = n->iss_seed += 0x01000193u * 64 + (uint32_t)now;
        f->sbase = f->snd_nxt = f->iss + 1;
        f->mss = 536;
        for (int o = 20; o < hl;) {               /* options: only the MSS is used */
            int kind = t[o];
            if (kind == 0) break;
            if (kind == 1) { o++; continue; }
            if (o + 1 >= hl || t[o + 1] < 2) break;
            if (kind == 2 && t[o + 1] == 4 && o + 4 <= hl) f->mss = rd16(t + o + 2);
            o += t[o + 1];
        }
        if (f->mss > OUR_MSS) f->mss = OUR_MSS;
        if (f->mss < 64) f->mss = 64;
        f->wnd = wnd;
        f->sbuf = malloc(SBUF_MAX);
        f->rbuf = malloc(RBUF_MAX);
        if (!f->sbuf || !f->rbuf) { tcp_free(f); tcp_reset_reply(n, src, sport, dst, dport, 0, seq + 1, 1); return; }
        f->t_start = f->t_progress = now;
        return;
    }

    if (flags & F_RST) {
        tcp_free(c);
        return;
    }
    if (flags & F_SYN) {                          /* retransmitted SYN */
        if (c->state == T_SYNACK) tcp_send(n, c, c->iss, F_SYN | F_ACK, NULL, 0);
        return;
    }
    if (c->state == T_CONNECTING) return;
    if (!(flags & F_ACK)) return;
    if (c->state == T_SYNACK) {
        if (ack != c->iss + 1) {
            tcp_reset_reply(n, src, sport, dst, dport, ack, 0, 0);
            return;
        }
        c->state = T_OPEN;
        c->t_progress = now;
    }
    /* acknowledgement */
    uint32_t acked = ack - c->sbase;
    if (c->fin_sent && ack == c->fin_seq + 1) {
        c->fin_acked = 1;
        acked = (uint32_t)c->slen;
    }
    if (acked > 0 && acked <= (uint32_t)c->slen) {
        memmove(c->sbuf, c->sbuf + acked, (size_t)(c->slen - (int)acked));
        c->slen -= (int)acked;
        c->sbase += acked;
        c->t_progress = now;
    }
    if ((int32_t)(c->snd_nxt - c->sbase) < 0) c->snd_nxt = c->sbase;
    c->wnd = wnd;
    /* data and FIN */
    if (dlen > 0 || (flags & F_FIN)) {
        if (seq == c->rcv_nxt && !c->guest_fin) {
            int take = dlen;
            if (take > RBUF_MAX - c->rlen) take = RBUF_MAX - c->rlen;
            memcpy(c->rbuf + c->rlen, data, (size_t)take);
            c->rlen += take;
            c->rcv_nxt += (uint32_t)take;
            if ((flags & F_FIN) && take == dlen) {
                c->guest_fin = 1;
                c->rcv_nxt++;
            }
        }
        tcp_send(n, c, c->snd_nxt, F_ACK, NULL, 0);
        tcp_flush_to_host(n, c);
    }
    tcp_output(n, c, now);
    tcp_maybe_close(c);
}

static void tcp_poll(wd_nat *n, struct pollfd *p, tcp_conn **who, int *np, uint64_t now)
{
    for (int i = 0; i < MAX_TCP; i++) {
        tcp_conn *c = &n->tcp[i];
        if (c->state == T_FREE) continue;
        short ev = 0;
        if (c->state == T_CONNECTING) ev = POLLOUT;
        else if (c->state == T_OPEN) {
            if (!c->host_eof && c->slen < SBUF_MAX) ev |= POLLIN;
            if (c->rlen > 0) ev |= POLLOUT;
        }
        if (c->state == T_CONNECTING && now - c->t_start > CONNECT_TIMEOUT_MS) {
            tcp_reset_reply(n, c->gip, c->gport, c->rip, c->rport, 0, c->rcv_nxt, 1);
            tcp_free(c);
            continue;
        }
        if (c->state == T_SYNACK && now - c->t_progress > RTO_MS) {
            tcp_send(n, c, c->iss, F_SYN | F_ACK, NULL, 0);
            c->t_progress = now;
        }
        if (ev) { p[*np].fd = c->fd; p[*np].events = ev; p[*np].revents = 0; who[(*np)++] = c; }
    }
}

static void tcp_ready(wd_nat *n, tcp_conn *c, short rev, uint64_t now)
{
    if (c->state == T_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err || (rev & (POLLERR | POLLHUP) && !(rev & POLLOUT))) {
            tcp_reset_reply(n, c->gip, c->gport, c->rip, c->rport, 0, c->rcv_nxt, 1);
            tcp_free(c);
            return;
        }
        if (!(rev & POLLOUT)) return;
        c->state = T_SYNACK;
        c->t_progress = now;
        tcp_send(n, c, c->iss, F_SYN | F_ACK, NULL, 0);
        return;
    }
    if (rev & (POLLIN | POLLHUP | POLLERR)) {
        while (c->slen < SBUF_MAX && !c->host_eof) {
            ssize_t r = recv(c->fd, c->sbuf + c->slen, (size_t)(SBUF_MAX - c->slen), 0);
            if (r > 0) { c->slen += (int)r; continue; }
            if (r == 0) { c->host_eof = 1; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            tcp_abort(n, c);                      /* connection reset by the far end */
            return;
        }
    }
    if (rev & POLLOUT) tcp_flush_to_host(n, c);
    tcp_output(n, c, now);
    tcp_maybe_close(c);
}

/* ---------------------------------------------------------------- UDP */

static void udp_forward(wd_nat *n, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *d, int len)
{
    uint32_t host = map_out(dst);
    if (!host) return;
    uint64_t now = now_ms();
    udp_conn *c = NULL;
    for (int i = 0; i < MAX_UDP && !c; i++)
        if (n->udp[i].fd > 0 && n->udp[i].gip == src && n->udp[i].gport == sport) c = &n->udp[i];
    if (!c) {
        udp_conn *oldest = &n->udp[0];
        for (int i = 0; i < MAX_UDP && !c; i++) {
            if (n->udp[i].fd <= 0) c = &n->udp[i];
            else if (n->udp[i].last < oldest->last) oldest = &n->udp[i];
        }
        if (!c) { close(oldest->fd); c = oldest; }
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { c->fd = 0; return; }
        set_nonblock(fd);
        c->fd = fd;
        c->gip = src;
        c->gport = sport;
    }
    c->last = now;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(dport), .sin_addr.s_addr = htonl(host) };
    sendto(c->fd, d, (size_t)len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void udp_ready(wd_nat *n, udp_conn *c)
{
    uint8_t buf[1472];
    for (int k = 0; k < 64 && room(n) > 0; k++) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        ssize_t r = recvfrom(c->fd, buf, sizeof buf, MSG_TRUNC, (struct sockaddr *)&sa, &sl);
        if (r < 0) break;
        if (r > (ssize_t)sizeof buf) continue;    /* would need IP fragments: dropped */
        c->last = now_ms();
        send_udp(n, map_in(ntohl(sa.sin_addr.s_addr)), ntohs(sa.sin_port), c->gip, c->gport, buf, (int)r);
    }
}

/* ---------------------------------------------------------------- DHCP */

static void dhcp_input(wd_nat *n, const uint8_t *d, int len)
{
    if (len < 240 || d[0] != 1 || d[1] != 1 || d[2] != 6 || rd32(d + 236) != 0x63825363u) return;
    int type = 0;
    uint32_t req_ip = 0;
    for (int o = 240; o < len;) {
        int code = d[o];
        if (code == 255) break;
        if (code == 0) { o++; continue; }
        if (o + 1 >= len || o + 2 + d[o + 1] > len) break;
        if (code == 53 && d[o + 1] == 1) type = d[o + 2];
        if (code == 50 && d[o + 1] == 4) req_ip = rd32(d + o + 2);
        o += 2 + d[o + 1];
    }
    int reply;
    if (type == 1) reply = 2;                     /* DISCOVER -> OFFER */
    else if (type == 3) reply = req_ip && req_ip != NAT_GUEST && rd32(d + 12) != NAT_GUEST ? 6 : 5;   /* REQUEST -> ACK/NAK */
    else if (type == 8) reply = 5;                /* INFORM -> ACK */
    else return;
    uint8_t r[300];
    memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, d + 4, 4);                      /* xid */
    memcpy(r + 10, d + 10, 2);                    /* flags */
    if (reply != 6 && type != 8) wr32(r + 16, NAT_GUEST);
    if (type == 8) memcpy(r + 12, d + 12, 4);
    wr32(r + 20, NAT_GW);
    memcpy(r + 28, d + 28, 16);                   /* chaddr */
    wr32(r + 236, 0x63825363u);
    uint8_t *o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)reply;
    *o++ = 54; *o++ = 4; wr32(o, NAT_GW); o += 4;
    if (reply != 6) {
        if (type != 8) {
            *o++ = 51; *o++ = 4; wr32(o, 86400); o += 4;
            *o++ = 58; *o++ = 4; wr32(o, 43200); o += 4;
            *o++ = 59; *o++ = 4; wr32(o, 75600); o += 4;
        }
        *o++ = 1; *o++ = 4; wr32(o, 0xFFFFFF00u); o += 4;
        *o++ = 3; *o++ = 4; wr32(o, NAT_GW); o += 4;
        *o++ = 6; *o++ = 4; wr32(o, NAT_DNS); o += 4;
        *o++ = 28; *o++ = 4; wr32(o, NAT_NET | 0xFF); o += 4;
        *o++ = 26; *o++ = 2; wr16(o, 1500); o += 2;
    }
    *o++ = 255;
    send_udp(n, NAT_GW, 67, 0xFFFFFFFFu, 68, r, (int)(o - r));
}

/* ---------------------------------------------------------------- DNS */

static void dq_unref(dns_queue *q)
{
    pthread_mutex_lock(&q->lock);
    int last = --q->refs == 0;
    pthread_mutex_unlock(&q->lock);
    if (!last) return;
    while (q->done) { dns_job *j = q->done; q->done = j->next; free(j); }
    pthread_mutex_destroy(&q->lock);
    free(q);
}

typedef struct { dns_queue *q; dns_job *j; } dns_arg;

static void *dns_thread(void *p)
{
    dns_arg *a = p;
    dns_job *j = a->j;
    dns_queue *q = a->q;
    free(a);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int e = getaddrinfo(j->name, NULL, &hints, &res);
    if (e == 0) {
        for (struct addrinfo *ai = res; ai && j->naddr < 8; ai = ai->ai_next) {
            uint32_t v = ntohl(((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
            int dup = 0;
            for (int i = 0; i < j->naddr; i++) dup |= j->addr[i] == v;
            if (!dup) j->addr[j->naddr++] = v;
        }
        freeaddrinfo(res);
        j->rcode = 0;
    } else {
        j->rcode = e == EAI_NONAME
#ifdef EAI_NODATA
                   || e == EAI_NODATA
#endif
                   ? 3 : 2;                       /* NXDOMAIN / SERVFAIL */
    }
    pthread_mutex_lock(&q->lock);
    if (!q->dead) { j->next = q->done; q->done = j; j = NULL; }
    q->pending--;
    pthread_mutex_unlock(&q->lock);
    free(j);
    dq_unref(q);
    return NULL;
}

static void dns_reply(wd_nat *n, dns_job *j)
{
    uint8_t r[512];
    int len = j->qend;
    memcpy(r, j->q, (size_t)len);
    r[2] = (uint8_t)(0x80 | (j->q[2] & 0x01));    /* QR, RD from the query */
    r[3] = (uint8_t)(0x80 | j->rcode);            /* RA, rcode */
    wr16(r + 6, (uint32_t)j->naddr);
    wr16(r + 8, 0); wr16(r + 10, 0);
    for (int i = 0; i < j->naddr && len + 16 <= (int)sizeof r; i++) {
        wr16(r + len, 0xC00C); wr16(r + len + 2, 1); wr16(r + len + 4, 1);
        wr32(r + len + 6, 60); wr16(r + len + 10, 4); wr32(r + len + 12, j->addr[i]);
        len += 16;
    }
    send_udp(n, NAT_DNS, 53, j->gip, j->gport, r, len);
}

static void dns_input(wd_nat *n, uint32_t src, uint16_t sport, const uint8_t *d, int len)
{
    if (len < 12 || len > 512 || (d[2] & 0x80) || ((d[2] >> 3) & 15) != 0 || rd16(d + 4) != 1) return;
    dns_job *j = calloc(1, sizeof *j);
    if (!j) return;
    memcpy(j->q, d, (size_t)len);
    j->qlen = len;
    j->gip = src;
    j->gport = sport;
    int o = 12, nl = 0;
    while (o < len && d[o]) {
        int l = d[o];
        if (l > 63 || o + 1 + l > len || nl + l + 2 > (int)sizeof j->name) { free(j); return; }
        if (nl) j->name[nl++] = '.';
        memcpy(j->name + nl, d + o + 1, (size_t)l);
        nl += l;
        o += 1 + l;
    }
    if (o + 5 > len) { free(j); return; }
    j->name[nl] = 0;
    int qtype = rd16(d + o + 1), qclass = rd16(d + o + 3);
    j->qend = o + 5;
    wr16(j->q + 10, 0);                           /* the reply carries no additional records */
    if (qclass != 1 || qtype != 1 || nl == 0) {   /* only A records: the rest get an empty answer */
        j->rcode = 0;
        dns_reply(n, j);
        free(j);
        return;
    }
    dns_queue *q = n->dq;
    pthread_mutex_lock(&q->lock);
    int busy = q->pending >= MAX_DNS;
    if (!busy) { q->pending++; q->refs++; }
    pthread_mutex_unlock(&q->lock);
    if (busy) { j->rcode = 2; dns_reply(n, j); free(j); return; }
    dns_arg *a = malloc(sizeof *a);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (a) { a->q = q; a->j = j; }
    if (!a || pthread_create(&th, &at, dns_thread, a) != 0) {
        free(a);
        pthread_mutex_lock(&q->lock);
        q->pending--;
        pthread_mutex_unlock(&q->lock);
        dq_unref(q);
        j->rcode = 2;
        dns_reply(n, j);
        free(j);
    }
    pthread_attr_destroy(&at);
}

/* ---------------------------------------------------------------- ICMP */

static void icmp_input(wd_nat *n, uint32_t src, uint32_t dst, const uint8_t *d, int len)
{
    if (len < 8 || d[0] != 8 || len > 1472) return;   /* echo requests only */
    if (dst == NAT_GW || dst == NAT_DNS) {
        uint8_t *t = n->frame + 34;
        memmove(t, d, (size_t)len);
        t[0] = 0;
        send_ip(n, dst, src, 1, len);
        return;
    }
    uint32_t host = map_out(dst);
    if (!host) return;
    uint64_t now = now_ms();
    icmp_conn *c = NULL;
    uint16_t id = rd16(d + 4);
    for (int i = 0; i < MAX_ICMP && !c; i++)
        if (n->icmp[i].fd > 0 && n->icmp[i].gip == src && n->icmp[i].dip == dst && n->icmp[i].gid == id) c = &n->icmp[i];
    if (!c) {
        for (int i = 0; i < MAX_ICMP && !c; i++) if (n->icmp[i].fd <= 0) c = &n->icmp[i];
        if (!c) return;
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);   /* unprivileged "ping" socket */
        if (fd < 0) return;
        set_nonblock(fd);
        c->fd = fd; c->gip = src; c->dip = dst; c->gid = id;
    }
    c->last = now;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(host) };
    sendto(c->fd, d, (size_t)len, 0, (struct sockaddr *)&sa, sizeof sa);
}

static void icmp_ready(wd_nat *n, icmp_conn *c)
{
    uint8_t buf[1472];
    for (int k = 0; k < 16 && room(n) > 0; k++) {
        ssize_t r = recv(c->fd, buf, sizeof buf, 0);
        if (r < 8) break;
        if (buf[0] != 0) continue;
        wr16(buf + 4, c->gid);
        memcpy(n->frame + 34, buf, (size_t)r);
        send_ip(n, c->dip, c->gip, 1, (int)r);
    }
}

/* ---------------------------------------------------------------- input */

static void arp_input(wd_nat *n, const uint8_t *a, int len)
{
    if (len < 28 || rd16(a) != 1 || rd16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || rd16(a + 6) != 1) return;
    uint32_t spa = rd32(a + 14), tpa = rd32(a + 24);
    if ((tpa & 0xFFFFFF00u) != NAT_NET || tpa == spa || tpa == NAT_GUEST || (tpa & 0xFF) == 0xFF || (tpa & 0xFF) == 0)
        return;                                   /* the guest's own address (and probes for it) stay unanswered */
    uint8_t *r = n->frame + 14;
    wr16(r, 1); wr16(r + 2, 0x0800); r[4] = 6; r[5] = 4; wr16(r + 6, 2);
    memcpy(r + 8, gw_mac, 6); wr32(r + 14, tpa);
    memcpy(r + 18, a + 8, 6); wr32(r + 24, spa);
    send_frame(n, a + 8, 0x0806, 42);
}

void wd_nat_input(wd_nat *n, const uint8_t *f, int len)
{
    if (len < 14) return;
    n->frames_in++;
    if (!(f[6] & 1)) { memcpy(n->guest_mac, f + 6, 6); n->have_mac = 1; }
    int type = rd16(f + 12);
    if (type == 0x0806) { arp_input(n, f + 14, len - 14); return; }
    if (type != 0x0800 || len < 34) return;
    const uint8_t *ip = f + 14;
    int ihl = (ip[0] & 15) * 4, tot = rd16(ip + 2);
    if ((ip[0] >> 4) != 4 || ihl < 20 || tot < ihl || 14 + tot > len) return;
    if (rd16(ip + 6) & 0x3FFF) return;            /* fragments are not reassembled */
    if (csum_fold(csum_add(0, ip, ihl)) != 0) return;
    uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
    const uint8_t *t = ip + ihl;
    int tl = tot - ihl;
    switch (ip[9]) {
    case 6:
        if (csum_fold(csum_add(pseudo(src, dst, 6, tl), t, tl)) != 0) return;
        tcp_input(n, src, dst, t, tl);
        break;
    case 17: {
        if (tl < 8 || rd16(t + 4) < 8 || rd16(t + 4) > tl) return;
        tl = rd16(t + 4);
        if (rd16(t + 6) && csum_fold(csum_add(pseudo(src, dst, 17, tl), t, tl)) != 0) return;
        uint16_t sport = rd16(t), dport = rd16(t + 2);
        if (dport == 67) dhcp_input(n, t + 8, tl - 8);
        else if (dst == NAT_DNS && dport == 53) dns_input(n, src, sport, t + 8, tl - 8);
        else udp_forward(n, src, sport, dst, dport, t + 8, tl - 8);
        break;
    }
    case 1:
        if (csum_fold(csum_add(0, t, tl)) != 0) return;
        icmp_input(n, src, dst, t, tl);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- polling */

void wd_nat_poll(wd_nat *n)
{
    uint64_t now = now_ms();
    /* finished DNS lookups */
    dns_job *done;
    pthread_mutex_lock(&n->dq->lock);
    done = n->dq->done;
    n->dq->done = NULL;
    pthread_mutex_unlock(&n->dq->lock);
    while (done) {
        dns_job *j = done;
        done = j->next;
        dns_reply(n, j);
        free(j);
    }

    static __thread struct pollfd p[MAX_TCP + MAX_UDP + MAX_ICMP];
    static __thread void *who[MAX_TCP + MAX_UDP + MAX_ICMP];
    static __thread int kind[MAX_TCP + MAX_UDP + MAX_ICMP];
    int np = 0;
    tcp_poll(n, p, (tcp_conn **)who, &np, now);
    for (int i = 0; i < np; i++) kind[i] = 0;
    for (int i = 0; i < MAX_UDP; i++) {
        udp_conn *c = &n->udp[i];
        if (c->fd <= 0) continue;
        if (now - c->last > UDP_IDLE_MS) { close(c->fd); memset(c, 0, sizeof *c); continue; }
        p[np].fd = c->fd; p[np].events = POLLIN; p[np].revents = 0; who[np] = c; kind[np++] = 1;
    }
    for (int i = 0; i < MAX_ICMP; i++) {
        icmp_conn *c = &n->icmp[i];
        if (c->fd <= 0) continue;
        if (now - c->last > ICMP_IDLE_MS) { close(c->fd); memset(c, 0, sizeof *c); continue; }
        p[np].fd = c->fd; p[np].events = POLLIN; p[np].revents = 0; who[np] = c; kind[np++] = 2;
    }
    if (np && poll(p, (nfds_t)np, 0) > 0) {
        for (int i = 0; i < np; i++) {
            if (!p[i].revents) continue;
            if (kind[i] == 0) {
                tcp_conn *c = who[i];
                if (c->state != T_FREE && c->fd == p[i].fd) tcp_ready(n, c, p[i].revents, now);
            } else if (kind[i] == 1) {
                udp_ready(n, who[i]);
            } else {
                icmp_ready(n, who[i]);
            }
        }
    }
    /* retransmissions and data waiting for window or room */
    for (int i = 0; i < MAX_TCP; i++) {
        tcp_conn *c = &n->tcp[i];
        if (c->state == T_OPEN) {
            tcp_output(n, c, now);
            tcp_maybe_close(c);
        }
    }
}

void wd_nat_kick(wd_nat *n)
{
    uint64_t now = now_ms();
    for (int i = 0; i < MAX_TCP; i++) if (n->tcp[i].state == T_OPEN) tcp_output(n, &n->tcp[i], now);
}

wd_nat *wd_nat_new(const wd_nat_ops *ops)
{
    wd_nat *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->dq = calloc(1, sizeof *n->dq);
    if (!n->dq) { free(n); return NULL; }
    pthread_mutex_init(&n->dq->lock, NULL);
    n->dq->refs = 1;
    n->ops = *ops;
    n->iss_seed = (uint32_t)now_ms() * 2654435761u;
    const char *sb = getenv("WINDDROID_NAT_SNDBUF");
    n->sndbuf = sb ? atoi(sb) : 0;
    for (int i = 0; i < MAX_TCP; i++) n->tcp[i].fd = -1;
    return n;
}

void wd_nat_free(wd_nat *n)
{
    if (!n) return;
    for (int i = 0; i < MAX_TCP; i++) if (n->tcp[i].state != T_FREE) tcp_free(&n->tcp[i]);
    for (int i = 0; i < MAX_UDP; i++) if (n->udp[i].fd > 0) close(n->udp[i].fd);
    for (int i = 0; i < MAX_ICMP; i++) if (n->icmp[i].fd > 0) close(n->icmp[i].fd);
    pthread_mutex_lock(&n->dq->lock);
    n->dq->dead = 1;                              /* lookups still running free their own jobs */
    pthread_mutex_unlock(&n->dq->lock);
    dq_unref(n->dq);
    free(n);
}

void wd_nat_get_stats(wd_nat *n, wd_nat_stats *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < MAX_TCP; i++) s->tcp += n->tcp[i].state != T_FREE;
    for (int i = 0; i < MAX_UDP; i++) s->udp += n->udp[i].fd > 0;
    for (int i = 0; i < MAX_ICMP; i++) s->icmp += n->icmp[i].fd > 0;
    pthread_mutex_lock(&n->dq->lock);
    s->dns_pending = n->dq->pending;
    pthread_mutex_unlock(&n->dq->lock);
    s->frames_in = n->frames_in;
    s->frames_out = n->frames_out;
}
