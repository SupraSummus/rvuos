/*
 * The network process's side of its clients' sockets, user/wifi/sock.c, and the clock's messages, user/wifi/sntp.c,
 * on the host, under ASan and UBSan:
 * clients kept to their own ports whatever they ask, requests that lie about their length, answers in order,
 * and DNS and SNTP answers as servers send them, cut short at every length.
 * Built and run by `make wifi-test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../net.h"
#include "../sntp.h"
#include "../sock.h"
#include "frames.h"

static int failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "sock-test:%d: %s\n", __LINE__, #cond);                   \
            failures++;                                                               \
        }                                                                             \
    } while (0)

/* What the stack sent last, and how many. */
static uint8_t frame[NET_FRAME_MAX];
static uint32_t frame_len, frames;

static void capture(struct net *n, const uint8_t *f, uint32_t len)
{
    (void)n;
    memcpy(frame, f, len);
    frame_len = len;
    frames++;
}

/* The answers since the last request, as they would go into the clients' channels. */
struct answer {
    uint32_t client;
    struct sock_msg h;
    uint8_t data[NET_UDP_MAX];
};
static struct answer answers[8];
static uint32_t answer_count;

static void put(struct sock *s, uint32_t client, const struct sock_msg *h, const uint8_t *data)
{
    (void)s;
    CHECK(h->len <= NET_UDP_MAX && (h->len == 0 || data != 0));
    if (answer_count < 8 && h->len <= NET_UDP_MAX) {
        struct answer *a = &answers[answer_count];
        a->client = client;
        a->h = *h;
        if (h->len != 0) {
            memcpy(a->data, data, h->len);
        }
    }
    answer_count++;
}

/* A request from client, as its ring hands it over: how many answers it brought; the first is in answers[0]. */
static uint32_t ask(struct sock *s, uint32_t client, uint32_t op, uint16_t port, uint32_t ip, uint16_t peer_port,
                    const void *data, uint32_t len)
{
    static uint8_t msg[SOCK_MSG_MAX];
    answer_count = 0;
    sock_request(s, client, msg, sock_ask(msg, op, port, ip, peer_port, data, len));
    return answer_count;
}

/* The status of the one answer a request brought, or 0xff if it brought none or more. */
static uint32_t status_of(uint32_t answered)
{
    return answered == 1 ? answers[0].h.status : 0xffu;
}

static void on_own(struct net *n, void *arg, const struct net_datagram *d)
{
    (void)n;
    (void)arg;
    (void)d;
}

static void test_sockets(void)
{
    static struct net n;
    static struct sock s;
    static uint8_t f[NET_FRAME_MAX];
    const uint32_t me = net_ip(192, 168, 1, 50), peer = net_ip(192, 168, 1, 7), router_ip = net_ip(192, 168, 1, 1);
    net_init(&n, pico, capture, 0);
    n.ip = me;
    n.mask = net_ip(255, 255, 255, 0);
    n.gateway = router_ip;
    n.dns = router_ip;
    sock_init(&s, &n, put, 0);
    CHECK(net_udp_bind(&n, 7777, on_own, 0) == 0);

    /* A port each client may hold, and none another holds, the server's own and DHCP's among them. */
    CHECK(status_of(ask(&s, 0, SOCK_BIND, 7, 0, 0, 0, 0)) == SOCK_OK && answers[0].h.op == SOCK_BIND);
    CHECK(answers[0].h.port == 7 && answers[0].client == 0 && answers[0].h.len == 0);
    CHECK(status_of(ask(&s, 1, SOCK_BIND, 7, 0, 0, 0, 0)) == SOCK_IN_USE);
    CHECK(status_of(ask(&s, 0, SOCK_BIND, 7, 0, 0, 0, 0)) == SOCK_IN_USE);
    CHECK(status_of(ask(&s, 1, SOCK_BIND, 7777, 0, 0, 0, 0)) == SOCK_IN_USE);
    CHECK(status_of(ask(&s, 1, SOCK_BIND, 68, 0, 0, 0, 0)) == SOCK_IN_USE);

    /* Port 0 asks the server for one, past any bound already; a client holds two at most. */
    CHECK(net_udp_bind(&n, SOCK_EPHEMERAL, on_own, 0) == 0);
    CHECK(status_of(ask(&s, 0, SOCK_BIND, 0, 0, 0, 0, 0)) == SOCK_OK && answers[0].h.port == SOCK_EPHEMERAL + 1);
    uint16_t picked = answers[0].h.port;
    CHECK(status_of(ask(&s, 0, SOCK_BIND, 0, 0, 0, 0, 0)) == SOCK_FULL);
    CHECK(status_of(ask(&s, 0, SOCK_BIND, 9, 0, 0, 0, 0)) == SOCK_FULL && !net_udp_bound(&n, 9));
    net_udp_unbind(&n, SOCK_EPHEMERAL);

    /* A datagram to a client's port goes to that client alone, with where it came from. */
    answer_count = 0;
    net_input(&n, f, udp_frame(f, pico, peer, me, 40000, 7, (const uint8_t *)"hello", 5));
    CHECK(answer_count == 1 && answers[0].client == 0 && answers[0].h.op == SOCK_RECV && answers[0].h.port == 7);
    CHECK(answers[0].h.ip == peer && answers[0].h.peer_port == 40000 && answers[0].h.len == 5);
    CHECK(memcmp(answers[0].data, "hello", 5) == 0 && s.delivered == 1);

    /* A client sends from the ports it holds and from no other; a datagram that went is not answered. */
    frames = 0;
    CHECK(status_of(ask(&s, 1, SOCK_SEND, 7, peer, 40000, "spoof", 5)) == SOCK_NOT_HELD && frames == 0);
    CHECK(status_of(ask(&s, 1, SOCK_SEND, 0, peer, 40000, "spoof", 5)) == SOCK_NOT_HELD && frames == 0);
    CHECK(status_of(ask(&s, 1, SOCK_SEND, 7777, peer, 40000, "spoof", 5)) == SOCK_NOT_HELD && frames == 0);
    CHECK(ask(&s, 0, SOCK_SEND, 7, peer, 40000, "hello back", 10) == 0 && frames == 1);
    CHECK(be16(frame + 34) == 7 && be16(frame + 36) == 40000 && memcmp(frame + 42, "hello back", 10) == 0);
    CHECK(memcmp(frame, router, 6) == 0 && checksum(frame + 34, 18, pseudo_sum(me, peer, 18)) == 0);

    /* Requests that lie: shorter than a header, a length that is not the message's, too long, unknown, or not a client's. */
    static uint8_t msg[SOCK_MSG_MAX + 8];
    uint32_t len = sock_ask(msg, SOCK_SEND, 7, peer, 40000, "abc", 3);
    answer_count = 0;
    sock_request(&s, 0, msg, 5);
    CHECK(answer_count == 1 && answers[0].h.status == SOCK_BAD && answers[0].h.len == 0);
    frames = 0;
    for (uint32_t cut = 0; cut < len; cut++) {
        answer_count = 0;
        sock_request(&s, 0, msg, cut);
        CHECK(answer_count == 1 && answers[0].h.status == SOCK_BAD);
    }
    answer_count = 0;
    sock_request(&s, 0, msg, len + 1);
    CHECK(answer_count == 1 && answers[0].h.status == SOCK_BAD && frames == 0);
    struct sock_msg big = { SOCK_SEND, 0, 7, peer, 40000, (uint16_t)(NET_UDP_MAX + 1u) };
    memcpy(msg, &big, sizeof(big));
    memset(msg + sizeof(big), 'x', NET_UDP_MAX + 1u);
    answer_count = 0;
    sock_request(&s, 0, msg, sizeof(big) + NET_UDP_MAX + 1u);
    CHECK(answer_count == 1 && answers[0].h.status == SOCK_BAD && frames == 0);
    CHECK(status_of(ask(&s, 0, 99, 7, 0, 0, 0, 0)) == SOCK_BAD);
    CHECK(status_of(ask(&s, 0, SOCK_RECV, 7, peer, 40000, "x", 1)) == SOCK_BAD);
    CHECK(ask(&s, SOCK_CLIENTS, SOCK_BIND, 8, 0, 0, 0, 0) == 0 && !net_udp_bound(&n, 8));

    /* A port back by asking, and every port of a client that is gone; another client may take them then. */
    CHECK(status_of(ask(&s, 1, SOCK_CLOSE, picked, 0, 0, 0, 0)) == SOCK_NOT_HELD && net_udp_bound(&n, picked));
    CHECK(status_of(ask(&s, 0, SOCK_CLOSE, 0, 0, 0, 0, 0)) == SOCK_NOT_HELD);
    CHECK(status_of(ask(&s, 0, SOCK_CLOSE, picked, 0, 0, 0, 0)) == SOCK_OK && !net_udp_bound(&n, picked));
    sock_drop(&s, 0);
    CHECK(!net_udp_bound(&n, 7) && net_udp_bound(&n, 7777));
    CHECK(status_of(ask(&s, 1, SOCK_BIND, 7, 0, 0, 0, 0)) == SOCK_OK);
    answer_count = 0;
    net_input(&n, f, udp_frame(f, pico, peer, me, 40000, 7, (const uint8_t *)"again", 5));
    CHECK(answer_count == 1 && answers[0].client == 1 && answers[0].h.op == SOCK_RECV);
    frames = 0;
    CHECK(status_of(ask(&s, 0, SOCK_SEND, 7, peer, 40000, "gone", 4)) == SOCK_NOT_HELD && frames == 0);

    /* As many ports as the stack keeps, past the server's own, whichever clients ask, and no more. */
    sock_drop(&s, 1);
    uint32_t taken = 0;
    for (uint32_t c = 0; c < SOCK_CLIENTS; c++) {
        for (uint32_t i = 0; i < SOCK_CLIENT_PORTS; i++) {
            taken += status_of(ask(&s, c, SOCK_BIND, 0, 0, 0, 0, 0)) == SOCK_OK;
        }
    }
    CHECK(taken == NET_UDP_PORTS - 1 && answers[0].h.status == SOCK_FULL);
    for (uint32_t c = 0; c < SOCK_CLIENTS; c++) {
        sock_drop(&s, c);
    }
    CHECK(net_udp_bound(&n, 7777) && !net_udp_bound(&n, 7));

    /* With no address, nothing goes; with the gateway unknown, an ARP request goes, and the client is told to send again. */
    CHECK(status_of(ask(&s, 2, SOCK_BIND, 5353, 0, 0, 0, 0)) == SOCK_OK);
    CHECK(status_of(ask(&s, 2, SOCK_SEND, 5353, 0, 53, "q", 1)) == SOCK_NO_ROUTE);
    static struct net bare;
    static struct sock t;
    net_init(&bare, pico, capture, 0);
    sock_init(&t, &bare, put, 0);
    CHECK(status_of(ask(&t, 0, SOCK_BIND, 5353, 0, 0, 0, 0)) == SOCK_OK);
    frames = 0;
    CHECK(status_of(ask(&t, 0, SOCK_SEND, 5353, net_ip(8, 8, 8, 8), 53, "q", 1)) == SOCK_NO_ROUTE && frames == 0);
    bare.ip = me;
    bare.mask = net_ip(255, 255, 255, 0);
    bare.gateway = router_ip;
    CHECK(status_of(ask(&t, 0, SOCK_SEND, 5353, net_ip(8, 8, 8, 8), 53, "q", 1)) == SOCK_RESOLVING);
    CHECK(frames == 1 && be16(frame + 12) == 0x0806);
}

static void test_dns(void)
{
    uint8_t q[DNS_MSG_MAX];
    uint32_t len = dns_query(q, 0x1234, "pool.ntp.org");
    static const uint8_t want[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'p', 'o', 'o', 'l',
                                    3,    'n',  't',  'p',  3, 'o', 'r', 'g', 0, 0, 1, 0, 1 };
    CHECK(len == sizeof(want) && memcmp(q, want, sizeof(want)) == 0);
    char long_label[70], long_name[300];
    memset(long_label, 'a', 64);
    long_label[64] = '\0';
    for (uint32_t i = 0; i < sizeof(long_name) - 1; i++) {
        long_name[i] = i % 10 == 9 ? '.' : 'b';
    }
    long_name[sizeof(long_name) - 1] = '\0';
    CHECK(dns_query(q, 1, "") == 0 && dns_query(q, 1, "a..b") == 0 && dns_query(q, 1, ".a") == 0);
    CHECK(dns_query(q, 1, long_label) == 0 && dns_query(q, 1, long_name) == 0);
    CHECK(dns_query(q, 1, "a.") != 0);

    /* An answer: the question again, a CNAME by a pointer to it, then the address, as a resolver sends them. */
    uint8_t a[DNS_MSG_MAX];
    memcpy(a, want, sizeof(want));
    put_be16(a + 2, 0x8180);
    put_be16(a + 6, 2);
    uint32_t at = sizeof(want);
    static const uint8_t cname[] = { 0xc0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'n', 't', 'p', 0xc0, 17 };
    memcpy(a + at, cname, sizeof(cname));
    at += sizeof(cname);
    static const uint8_t addr[] = { 0xc0, 42, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 213, 222, 217, 10 };
    memcpy(a + at, addr, sizeof(addr));
    at += sizeof(addr);
    CHECK(dns_answer(a, at, 0x1234) == net_ip(213, 222, 217, 10));
    CHECK(dns_answer(a, at, 0x1235) == 0);
    for (uint32_t cut = 0; cut < at; cut++) {
        CHECK(dns_answer(a, cut, 0x1234) == 0);
    }
    put_be16(a + 2, 0x8183); /* no such name */
    CHECK(dns_answer(a, at, 0x1234) == 0);
    put_be16(a + 2, 0x0100); /* a question, not an answer */
    CHECK(dns_answer(a, at, 0x1234) == 0);
    put_be16(a + 2, 0x8180);

    /* Counts and lengths that point past the end, and every byte of the answer garbled, read nothing outside it. */
    put_be16(a + 4, 0xffff);
    CHECK(dns_answer(a, at, 0x1234) == 0);
    put_be16(a + 4, 1);
    for (uint32_t i = 2; i < at; i++) {
        uint8_t keep = a[i];
        for (uint32_t v = 0; v < 256; v += 51) {
            a[i] = (uint8_t)v;
            (void)dns_answer(a, at, 0x1234);
        }
        a[i] = keep;
    }
    CHECK(dns_answer(a, at, 0x1234) == net_ip(213, 222, 217, 10));
}

static void put_be32(uint8_t *p, uint32_t v)
{
    put_be16(p, v >> 16);
    put_be16(p + 2, v);
}

static void test_ntp(void)
{
    uint8_t q[NTP_MSG], a[NTP_MSG];
    ntp_query(q, 0xcafe0123u);
    CHECK(q[0] == 0x23 && be16(q + 40) == 0xcafe && be16(q + 42) == 0x0123);

    /* A server's answer: its mode, a stratum, our transmit time back as the origin, and its own. */
    const uint32_t ntp_unix = 2208988800u, noon = 1791028800u; /* 2026-10-03 12:00:00 UTC */
    memset(a, 0, sizeof(a));
    a[0] = 0x24;
    a[1] = 2;
    memcpy(a + 24, q + 40, 8);
    put_be32(a + 40, noon + ntp_unix);
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0123u) == noon);
    CHECK(ntp_answer(a, sizeof(a) - 1, 0xcafe0123u) == 0);
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0124u) == 0);
    a[1] = 0; /* a kiss of death */
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0123u) == 0);
    a[1] = 2;
    a[0] = 0xe4; /* not synchronised */
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0123u) == 0);
    a[0] = 0x23; /* a client's */
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0123u) == 0);
    a[0] = 0x24;
    /* Past NTP's wrap in 2036, the count starts again and the difference wraps with it. */
    put_be32(a + 40, 1000u);
    CHECK(ntp_answer(a, sizeof(a), 0xcafe0123u) == 2085979496u);

    char text[UTC_TEXT];
    utc_format(text, 0);
    CHECK(strcmp(text, "1970-01-01 00:00:00 UTC") == 0);
    utc_format(text, 951782400u);
    CHECK(strcmp(text, "2000-02-29 00:00:00 UTC") == 0);
    utc_format(text, noon);
    CHECK(strcmp(text, "2026-10-03 12:00:00 UTC") == 0);
    utc_format(text, 1798761599u);
    CHECK(strcmp(text, "2026-12-31 23:59:59 UTC") == 0);
    utc_format(text, 2085979496u);
    CHECK(strcmp(text, "2036-02-07 06:44:56 UTC") == 0);
    utc_format(text, 0xffffffffu);
    CHECK(strcmp(text, "2106-02-07 06:28:15 UTC") == 0);
}

int main(void)
{
    test_sockets();
    test_dns();
    test_ntp();
    if (failures) {
        fprintf(stderr, "sock-test: %d failed\n", failures);
        return 1;
    }
    printf("sock-test: PASS\n");
    return 0;
}
