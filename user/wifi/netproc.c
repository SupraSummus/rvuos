/*
 * The network process: the IP stack of net.c between the link and its clients; see wifi.h.
 *
 * It starts with a0 at the page it shares with the root task, keeps its state on its stack,
 * and waits on one notification for everything: the link, which says it has frames from the driver or room for more,
 * a bit for each client's channel, its timer, which ticks every 100 ms for DHCP's retries, and the root task,
 * which says when it connected or closed a client's channel.
 * It asks for an address by DHCP once started, serves its clients' sockets, sock.h,
 * and answers on UDP port 7777 itself, with a line that says what the system has done.
 *
 * It trusts the driver and not its clients: from each channel it takes as many packets as a ring holds for each wake,
 * so a client that keeps its ring full, or makes it seem so, cannot keep it from the link and the other clients,
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
    struct net_page *page;
    struct out out;
    uint32_t now; /* ms since the process started, by its timer */
    uint32_t tx_full;
};

/* A frame to the driver; dropped if the link is full. */
static void np_send(struct net *n, const uint8_t *frame, uint32_t len)
{
    struct netp *p = (struct netp *)n;
    if (chan_send(&p->page->link, frame, len) != 0) {
        p->tx_full++;
        return;
    }
    p->page->tx_frames++;
}

/* A message to a client; dropped if its channel is full, which costs that client alone. */
static void np_put(struct sock *s, uint32_t client, const struct sock_msg *h, const uint8_t *data)
{
    struct netp *p = s->owner;
    const struct chan_end *e = &p->page->clients[client];
    uint8_t *slot = chan_put_begin(e);
    if (slot == 0 || sizeof(*h) + h->len > e->tx.slot_size) {
        return;
    }
    memcpy(slot, h, sizeof(*h));
    if (h->len != 0) {
        memcpy(slot + sizeof(*h), data, h->len);
    }
    chan_put_end(e, sizeof(*h) + h->len);
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
        "rvuos on a Pico 2 W: up %u s, frames in %u, out %u, pings %u, datagrams %u, to clients %u\n", p->now / 1000u,
        n->rx_frames, n->tx_frames, n->pings, p->page->datagrams, p->sock.delivered);
    net_udp_send(n, d->from_ip, d->from_port, NET_PORT_STATUS, (const uint8_t *)l.buf, l.len);
}

/*
 * At most as many packets as the channel's ring holds, which is as many as an honest peer can have put meanwhile,
 * the link's to the stack and a client's to its sockets; 1 if it had more.
 */
static int take(struct netp *p, const struct chan_end *e, uint32_t client)
{
    uint32_t len;
    const uint8_t *in;
    for (uint32_t n = 0; n < e->rx.slots; n++) {
        if ((in = chan_get_begin(e, &len)) == 0) {
            return 0;
        }
        if (e == &p->page->link) {
            net_input(&p->net, in, len);
            p->page->rx_frames++;
        } else {
            sock_request(&p->sock, client, in, len);
        }
        chan_get_end(e);
    }
    return chan_get_begin(e, &len) != 0;
}

/* The clients' channels the root task connected or closed: a closed one's ports go, and so does a new one's last. */
static void clients_changed(struct netp *p)
{
    for (uint32_t i = 0; i < SOCK_CLIENTS; i++) {
        struct chan_end *e = &p->page->clients[i];
        uint32_t gen;
        if (chan_changed(e, &gen)) {
            sock_drop(&p->sock, i);
            say(&p->out, "net: client %u %s\n", i, (gen & 1u) ? "connected" : "gone");
            chan_seen(e, gen);
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
        int more = take(p, &page->link, 0);
        for (uint32_t i = 0; i < SOCK_CLIENTS; i++) {
            more |= take(p, &page->clients[i], i);
        }
        if (more) {
            rv_signal(page->more, NOTIFY_ALL_BITS);
        }
        page->pings = p->net.pings;
        if (!bound && p->net.dhcp_state == DHCP_BOUND) {
            bound = 1;
            page->ip = p->net.ip;
            page->mask = p->net.mask;
            page->gateway = p->net.gateway;
            say(&p->out, "net: address %I, gateway %I, lease %u s\n", p->net.ip, p->net.gateway, p->net.lease);
            child_report(c, NET_BOUND);
        } else if (bound && p->net.dhcp_state != DHCP_BOUND) {
            bound = 0;
            child_report(c, NET_WAITING);
        }
    }
}
