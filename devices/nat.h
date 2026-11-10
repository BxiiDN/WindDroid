/* WindDroid - user-mode NAT: the guest's Ethernet frames in, host sockets out.
 *
 * The guest sees the network 10.0.2.0/24 (as with QEMU's user networking):
 *   10.0.2.2   gateway, DHCP server; TCP/UDP to it reach the host's 127.0.0.1
 *   10.0.2.3   DNS server (answered with the host resolver)
 *   10.0.2.15  the address handed to the guest by DHCP
 * IPv4 only: ARP, DHCP, DNS, ICMP echo, UDP and TCP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WD_NAT_H
#define WD_NAT_H

#include <stdint.h>

#define NAT_NET      0x0A000200u          /* 10.0.2.0 */
#define NAT_GW       0x0A000202u
#define NAT_DNS      0x0A000203u
#define NAT_GUEST    0x0A00020Fu

typedef struct wd_nat wd_nat;

typedef struct {
    /* a frame for the guest; return 0 if it was queued, -1 if there is no room */
    int (*send)(void *opaque, const uint8_t *frame, int len);
    /* how many more frames the guest side can take now */
    int (*room)(void *opaque);
    void *opaque;
} wd_nat_ops;

wd_nat *wd_nat_new(const wd_nat_ops *ops);
void wd_nat_free(wd_nat *n);
/* an Ethernet frame (without FCS) sent by the guest */
void wd_nat_input(wd_nat *n, const uint8_t *frame, int len);
/* services the host sockets and timers; call often (it never blocks) */
void wd_nat_poll(wd_nat *n);
/* the guest side has room again */
void wd_nat_kick(wd_nat *n);

typedef struct {
    int tcp, udp, icmp, dns_pending;      /* open entries */
    uint64_t frames_in, frames_out;
} wd_nat_stats;
void wd_nat_get_stats(wd_nat *n, wd_nat_stats *s);

#endif
