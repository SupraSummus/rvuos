/*
 * The network process: the IP stack of net.c between the link and its clients; see wifi.h.
 *
 * It starts with a0 at the page it shares with the root task, keeps its state on its stack,
 * and waits on one notification for everything: the link, which says the driver lent it frames or took some back,
 * a bit for each client's connection, its timer, which ticks every 100 ms for DHCP's retries, and the root task,
 * which says when it connected or closed a client's connection.
 * It asks for an address by DHCP once started, publishes the network's addresses as they change, lib/seqlock.h,
 * serves its clients' sockets, sock.h,
 * and answers on UDP port 7777 itself, with a line that says what the system has done.
 *
 * It trusts the driver and not its clients, lib/bulk.h: the driver lends it the link, so it reads each frame where it
 * lies, which nothing but the driver writes; it lends each client a connection, so it copies a datagram once into the
 * client's buffers, as clients may not read each other's, and reads a client's request once, sock.h.
 * From each it takes a turn for each wake, a ring's worth at most,
 * so a client that keeps handing it references cannot keep it from the link and the other clients,
 * and wakes itself for what it left.
 */

#include "lib/libc.h"
#include "lib/say.h"
#include "net.h"
#include "wifi.h"

#define TICK_US 100000u

struct netp {
    struct net net; /* first, so that the stack's callbacks find the process */
    struct sock sock;
    struct sock_config published; /* the network's addresses, as it published them last */
    struct net_page *page;
    struct out out;
    uint32_t now; /* ms since the process started, by its timer */
    uint32_t tx_full;
};

/* A frame to the driver, in a buffer the driver lent it to fill; dropped if none is free. */
static void np_send(struct net *n, const uint8_t *frame, uint32_t len)
{
    struct netp *p = (struct netp *)n;
    struct bulk_end *e = &p->page->link;
    struct bulk_ref r;
    if (len > e->size || bulk_take(e, &r) != 0) {
        p->tx_full++;
        return;
    }
    memcpy(r.room, frame, len);
    r.len = len;
    if (bulk_hand(e, &r) != 0) {
        p->tx_full++;
        return;
    }
    p->page->tx_frames++;
}

/* A message to a client, in a buffer of its connection lent to read; dropped if none is free, which costs it alone. */
static void np_put(struct sock *s, uint32_t client, const struct sock_msg *h, const uint8_t *data)
{
    struct netp *p = s->owner;
    struct bulk_end *e = &p->page->clients[client];
    struct bulk_ref r;
    if (sizeof(*h) + h->len > e->size || bulk_take(e, &r) != 0) {
        return;
    }
    memcpy(r.room, h, sizeof(*h));
    if (h->len != 0) {
        memcpy(r.room + sizeof(*h), data, h->len);
    }
    r.len = sizeof(*h) + h->len;
    bulk_hand(e, &r);
}

/* Text into a buffer, for the status line. */
struct line {
    char buf[160];
    uint32_t len;
};

static void line_write(void *to, const char *s, uint32_t n)
{
    struct line *l = to;
    for (uint32_t i = 0; i < n && l->len < sizeof(l->buf); i++) {
        l->buf[l->len++] = s[i];
    }
}

static void on_status(struct net *n, void *arg, const struct net_datagram *d)
{
    struct netp *p = arg;
    struct line l = { .len = 0 };
    p->page->datagrams++;
    say(&(const struct out){ line_write, &l },
        "rvuos on %s: up %u s, frames in %u, out %u, pings %u, datagrams %u, to clients %u\n", p->page->board,
        p->now / 1000u, n->rx_frames, n->tx_frames, n->pings, p->page->datagrams, p->sock.delivered);
    net_udp_send(n, d->from_ip, d->from_port, NET_PORT_STATUS, (const uint8_t *)l.buf, l.len);
}

/* A turn of the link: each frame the driver lent read where it lies by the stack, and given back; 1 if it had more. */
static int link_turn(struct netp *p)
{
    struct bulk_end *e = &p->page->link;
    struct bulk_turn t = bulk_turn(e);
    struct bulk_ref r;
    while (bulk_next(&t, &r)) {
        net_input(&p->net, r.bytes, r.len);
        p->page->rx_frames++;
        bulk_put(e, r.buf);
    }
    return bulk_more(&t);
}

/*
 * A turn of client i's connection: each request read once where it lies, sock.h, and its buffer given back to fill
 * again; 1 if it had more.
 */
static int client_turn(struct netp *p, uint32_t i)
{
    struct bulk_end *e = &p->page->clients[i];
    struct bulk_turn t = bulk_turn(e);
    struct bulk_ref r;
    while (bulk_next(&t, &r)) {
        sock_request(&p->sock, i, r.bytes, r.len);
        bulk_put(e, r.buf);
    }
    return bulk_more(&t);
}

/* The clients' connections the root task connected or closed: a closed one's ports go, and so does a new one's last. */
static void clients_changed(struct netp *p)
{
    for (uint32_t i = 0; i < SOCK_CLIENTS; i++) {
        struct bulk_end *e = &p->page->clients[i];
        uint32_t gen;
        if (bulk_changed(e, &gen)) {
            sock_drop(&p->sock, i);
            say(&p->out, "net: client %u %s\n", i, (gen & 1u) ? "connected" : "gone");
            bulk_seen(e, gen);
        }
    }
}

__attribute__((noreturn)) void net_main(struct child_page *c)
{
    struct net_page *page = (struct net_page *)c;
    struct netp np, *p = &np;
    memset(p, 0, sizeof(*p));
    p->page = page;
    p->out = child_out();
    net_init(&p->net, page->mac, np_send, p);
    sock_init(&p->sock, &p->net, np_put, p);
    net_udp_bind(&p->net, NET_PORT_STATUS, on_status, p);
    struct seqlock config = net_config(page);
    seqlock_init(&config);

    say(&p->out, "net: up\n");
    child_report(c, NET_WAITING);
    uint32_t skipped;
    rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
    net_dhcp_start(&p->net);

    uint32_t bound = 0;
    for (;;) {
        uint32_t bits = 0;
        if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
        child_answer(c);
        if (bits & CHILD_BIT_TIMER) {
            rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
            p->now += (skipped + 1u) * (TICK_US / 1000u);
            net_tick(&p->net, p->now);
        }
        if (bits & CHILD_BIT_PARENT) {
            clients_changed(p);
        }
        int more = link_turn(p);
        for (uint32_t i = 0; i < SOCK_CLIENTS; i++) {
            more |= client_turn(p, i);
        }
        if (more) {
            rv_signal(page->more, NOTIFY_ALL_BITS);
        }
        page->pings = p->net.pings;
        struct sock_config now = sock_config(&p->sock);
        if (memcmp(&now, &p->published, sizeof(now)) != 0) {
            p->published = now;
            seqlock_write(&config, &now);
        }
        if (!bound && p->net.dhcp_state == DHCP_BOUND) {
            bound = 1;
            page->ip = p->net.ip;
            say(&p->out, "net: address %I, gateway %I, lease %u s\n", p->net.ip, p->net.gateway, p->net.lease);
            child_report(c, NET_BOUND);
        } else if (bound && p->net.dhcp_state != DHCP_BOUND) {
            bound = 0;
            child_report(c, NET_WAITING);
        }
    }
}
