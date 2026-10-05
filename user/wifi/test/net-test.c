/*
 * The Wi-Fi system's IP stack, user/wifi/net.c, on the host, under ASan and UBSan:
 * a DHCP exchange, ARP, a ping and UDP, against frames a server and a peer would send.
 * Built and run by `make wifi-test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net.h"
#include "frames.h"

static uint8_t sent[8][NET_FRAME_MAX];
static uint32_t sent_len[8], sent_count;
static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "net-test:%d: %s\n", __LINE__, #cond);                    \
            failures++;                                                               \
        }                                                                             \
    } while (0)

static void capture(struct net *n, const uint8_t *frame, uint32_t len)
{
    (void)n;
    if (sent_count < 8) {
        memcpy(sent[sent_count], frame, len);
        sent_len[sent_count] = len;
    }
    sent_count++;
}

static uint32_t got_from_ip, got_len;
static uint16_t got_from_port, got_port;
static uint8_t got[64];
static void *got_arg;

static void on_udp(struct net *n, void *arg, const struct net_datagram *d)
{
    (void)n;
    got_arg = arg;
    got_from_ip = d->from_ip;
    got_from_port = d->from_port;
    got_port = d->port;
    got_len = d->len < sizeof(got) ? d->len : sizeof(got);
    memcpy(got, d->data, got_len);
}

/* A DHCP answer of a type, from the router, offering .50: to our address and .50, or by broadcast. */
static uint32_t dhcp_answer(uint8_t *f, const uint8_t *discover, uint32_t type, int unicast)
{
    uint8_t d[300];
    memset(d, 0, sizeof(d));
    d[0] = 2;
    d[1] = 1;
    d[2] = 6;
    memcpy(d + 4, discover + 4, 4); /* xid */
    uint32_t yiaddr = net_ip(192, 168, 1, 50);
    memcpy(d + 16, &yiaddr, 4);
    memcpy(d + 28, pico, 6);
    const uint8_t cookie[4] = { 99, 130, 83, 99 };
    memcpy(d + 236, cookie, 4);
    uint8_t opts[] = {
        53, 1, (uint8_t)type, 54, 4, 192, 168, 1, 1, 1, 4, 255, 255, 255, 0, 3, 4, 192, 168, 1, 1,
        6, 4, 192, 168, 1, 1, 51, 4, 0, 0, 0x0e, 0x10, 255,
    };
    memcpy(d + 240, opts, sizeof(opts));
    const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return udp_frame(f, unicast ? pico : all, net_ip(192, 168, 1, 1), unicast ? yiaddr : 0xffffffffu, 67, 68, d,
                     sizeof(d));
}

int main(void)
{
    static struct net n;
    static uint8_t f[NET_FRAME_MAX];
    net_init(&n, pico, capture, 0);
    net_tick(&n, 1000);

    /* A discover: broadcast, from 0.0.0.0:68 to :67, type 1, our address in chaddr, the broadcast flag clear. */
    net_dhcp_start(&n);
    CHECK(sent_count == 1);
    const uint8_t *s = sent[0];
    CHECK(memcmp(s, "\xff\xff\xff\xff\xff\xff", 6) == 0 && be16(s + 12) == 0x0800);
    CHECK(s[14 + 9] == 17 && be16(s + 34) == 68 && be16(s + 36) == 67);
    CHECK(checksum(s + 14, 20, 0) == 0);
    const uint8_t *dhcp = s + 42;
    CHECK(dhcp[0] == 1 && memcmp(dhcp + 28, pico, 6) == 0 && dhcp[240] == 53 && dhcp[242] == 1);
    CHECK(be16(dhcp + 10) == 0);
    uint8_t discover[300];
    memcpy(discover, dhcp, sizeof(discover));

    /* Unanswered, it goes again in the same transaction after 4 s, then after 8. */
    net_tick(&n, 4999);
    CHECK(sent_count == 1);
    net_tick(&n, 5000);
    CHECK(sent_count == 2 && sent[1][42 + 242] == 1 && memcmp(sent[1] + 42 + 4, discover + 4, 4) == 0);
    net_tick(&n, 12999);
    CHECK(sent_count == 2);
    net_tick(&n, 13000);
    CHECK(sent_count == 3 && memcmp(sent[2] + 42 + 4, discover + 4, 4) == 0);

    /* An offer to our address and .50, as the server sends it, brings a request for .50 from the server .1. */
    net_input(&n, f, dhcp_answer(f, discover, 2, 1));
    CHECK(sent_count == 4 && n.dhcp_state == DHCP_REQUESTING && sent[3][42 + 242] == 3);

    /* Unanswered, the request goes again after 4 s and 8; after the third, a discover begins a transaction anew. */
    net_tick(&n, 17000);
    CHECK(sent_count == 5 && sent[4][42 + 242] == 3);
    net_tick(&n, 25000);
    CHECK(sent_count == 6 && sent[5][42 + 242] == 3);
    net_tick(&n, 40999);
    CHECK(sent_count == 6);
    net_tick(&n, 41000);
    CHECK(sent_count == 7 && sent[6][42 + 242] == 1 && memcmp(sent[6] + 42 + 4, discover + 4, 4) != 0);
    CHECK(n.dhcp_state == DHCP_SELECTING);
    memcpy(discover, sent[6] + 42, sizeof(discover));

    /* A broadcast offer is taken as well; the acknowledgement, to our address and .50, binds. */
    net_input(&n, f, dhcp_answer(f, discover, 2, 0));
    CHECK(sent_count == 8 && n.dhcp_state == DHCP_REQUESTING && sent[7][42 + 242] == 3);
    net_input(&n, f, dhcp_answer(f, discover, 5, 1));
    CHECK(n.dhcp_state == DHCP_BOUND);
    CHECK(n.ip == net_ip(192, 168, 1, 50) && n.mask == net_ip(255, 255, 255, 0) && n.gateway == net_ip(192, 168, 1, 1));
    CHECK(n.lease == 3600);

    /* Who has .50? tell .1: the answer goes to the router with our address. */
    sent_count = 0;
    memcpy(f, "\xff\xff\xff\xff\xff\xff", 6);
    memcpy(f + 6, router, 6);
    put_be16(f + 12, 0x0806);
    uint8_t *a = f + 14;
    put_be16(a, 1);
    put_be16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put_be16(a + 6, 1);
    memcpy(a + 8, router, 6);
    uint32_t r_ip = net_ip(192, 168, 1, 1), me = net_ip(192, 168, 1, 50);
    memcpy(a + 14, &r_ip, 4);
    memset(a + 18, 0, 6);
    memcpy(a + 24, &me, 4);
    net_input(&n, f, 42);
    CHECK(sent_count == 1 && be16(sent[0] + 12) == 0x0806 && be16(sent[0] + 20) == 2);
    CHECK(memcmp(sent[0], router, 6) == 0 && memcmp(sent[0] + 22, pico, 6) == 0);

    /* A ping from .7, on the link: the reply goes back to its sender's address, checksums right. */
    sent_count = 0;
    uint8_t *icmp = f + 34;
    icmp[0] = 8;
    icmp[1] = 0;
    put_be16(icmp + 2, 0);
    put_be16(icmp + 4, 0x1234);
    put_be16(icmp + 6, 1);
    memcpy(icmp + 8, "rvuos ping", 10);
    put_be16(icmp + 2, checksum(icmp, 18, 0));
    uint32_t peer = net_ip(192, 168, 1, 7);
    ip_frame(f, pico, peer, me, 1, 18);
    net_input(&n, f, 34 + 18);
    CHECK(sent_count == 1 && n.pings == 1);
    CHECK(memcmp(sent[0], router, 6) == 0 && sent[0][34] == 0 && checksum(sent[0] + 34, 18, 0) == 0);
    CHECK(checksum(sent[0] + 14, 20, 0) == 0 && memcmp(sent[0] + 14 + 16, &peer, 4) == 0);
    CHECK(memcmp(sent[0] + 42, "rvuos ping", 10) == 0);

    /* UDP to a bound port reaches it, with what it was bound with; an answer goes back with its checksum right. */
    CHECK(net_udp_bind(&n, 7, on_udp, &n) == 0);
    CHECK(net_udp_bind(&n, 7, on_udp, 0) == -1 && net_udp_bind(&n, 68, on_udp, 0) == -1);
    CHECK(net_udp_bind(&n, 0, on_udp, 0) == -1 && net_udp_bound(&n, 7) && net_udp_bound(&n, 68));
    sent_count = 0;
    net_input(&n, f, udp_frame(f, pico, peer, me, 40000, 7, (const uint8_t *)"hello", 5));
    CHECK(got_len == 5 && memcmp(got, "hello", 5) == 0 && got_from_ip == peer && got_from_port == 40000);
    CHECK(got_port == 7 && got_arg == &n);
    CHECK(net_udp_send(&n, peer, 40000, 7, (const uint8_t *)"hello back", 10) == 0);
    CHECK(sent_count == 1 && be16(sent[0] + 34) == 7 && be16(sent[0] + 36) == 40000);
    CHECK(checksum(sent[0] + 34, 18, pseudo_sum(me, peer, 18)) == 0);

    /* Unbound, the port takes nothing, and may be bound again. */
    net_udp_unbind(&n, 7);
    got_len = 0;
    net_input(&n, f, udp_frame(f, pico, peer, me, 40000, 7, (const uint8_t *)"hello", 5));
    CHECK(got_len == 0 && !net_udp_bound(&n, 7) && net_udp_bind(&n, 7, on_udp, &n) == 0);

    /* A corrupted datagram is dropped; one off the link goes to the gateway, whose address is known now. */
    got_len = 0;
    uint32_t len = udp_frame(f, pico, peer, me, 40000, 7, (const uint8_t *)"hello", 5);
    f[len - 1] ^= 0x55;
    net_input(&n, f, len);
    CHECK(got_len == 0 && n.rx_dropped == 1);
    sent_count = 0;
    CHECK(net_udp_send(&n, net_ip(8, 8, 8, 8), 53, 5353, (const uint8_t *)"x", 1) == 0);
    CHECK(sent_count == 1 && memcmp(sent[0], router, 6) == 0);

    /* With the gateway unknown, a send off the link asks for it instead. */
    static struct net m;
    net_init(&m, pico, capture, 0);
    m.ip = me;
    m.mask = net_ip(255, 255, 255, 0);
    m.gateway = r_ip;
    sent_count = 0;
    CHECK(net_udp_send(&m, net_ip(8, 8, 8, 8), 53, 5353, (const uint8_t *)"x", 1) == -1);
    CHECK(sent_count == 1 && be16(sent[0] + 12) == 0x0806 && be16(sent[0] + 20) == 1);

    if (failures) {
        fprintf(stderr, "net-test: %d failed\n", failures);
        return 1;
    }
    printf("net-test: PASS\n");
    return 0;
}
