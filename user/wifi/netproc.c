/*
 * The network process: the IP stack of net.c between the link and the UDP services; see wifi.h.
 *
 * It starts with a0 at the page it shares with the root task, keeps its state on its stack,
 * and waits on one notification for everything: the link, which says it has frames from the driver or room for more,
 * its timer, which ticks every 100 ms for DHCP's retries, and the root task.
 * It asks for an address by DHCP once started, and answers on two UDP ports:
 * the echo, and a line that says what the system has done.
 */

#include "lib/libc.h"
#include "lib/say.h"
#include "net.h"
#include "wifi.h"

#define TICK_US 100000u

struct netp {
    struct net net; /* first, so that the stack's callbacks find the process */
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

static void on_echo(struct net *n, uint32_t from_ip, uint16_t from_port, const uint8_t *data, uint32_t len)
{
    struct netp *p = (struct netp *)n;
    p->page->datagrams++;
    net_udp_send(n, from_ip, from_port, NET_PORT_ECHO, data, len);
}

/* Text into a buffer, for the status line. */
struct line {
    char buf[160];
    uint32_t len;
};

static void line_put(void *to, char c)
{
    struct line *l = to;
    if (l->len < sizeof(l->buf)) {
        l->buf[l->len++] = c;
    }
}

static void on_status(struct net *n, uint32_t from_ip, uint16_t from_port, const uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
    struct netp *p = (struct netp *)n;
    struct line l = { .len = 0 };
    p->page->datagrams++;
    say(&(const struct out){ line_put, &l }, "rvuos on a Pico 2 W: up %u s, frames in %u, out %u, pings %u, datagrams %u\n",
        p->now / 1000u, n->rx_frames, n->tx_frames, n->pings, p->page->datagrams);
    net_udp_send(n, from_ip, from_port, NET_PORT_STATUS, (const uint8_t *)l.buf, l.len);
}

__attribute__((noreturn)) void net_main(struct child_page *c)
{
    struct net_page *page = (struct net_page *)c;
    struct netp np, *p = &np;
    memset(p, 0, sizeof(*p));
    p->page = page;
    p->out = child_out(c);
    net_init(&p->net, page->mac, np_send, p);
    net_udp_bind(&p->net, NET_PORT_ECHO, on_echo);
    net_udp_bind(&p->net, NET_PORT_STATUS, on_status);

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
        if (bits & CHILD_BIT_TIMER) {
            rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
            p->now += (skipped + 1u) * (TICK_US / 1000u);
            net_tick(&p->net, p->now);
        }
        uint32_t len;
        const uint8_t *frame;
        while ((frame = chan_get_begin(&page->link, &len)) != 0) {
            net_input(&p->net, frame, len);
            page->rx_frames++;
            chan_get_end(&page->link);
        }
        page->pings = p->net.pings;
        if (!bound && p->net.dhcp_state == DHCP_BOUND) {
            bound = 1;
            page->ip = p->net.ip;
            page->mask = p->net.mask;
            page->gateway = p->net.gateway;
            say(&p->out, "net: address %I, gateway %I\n", p->net.ip, p->net.gateway);
            child_report(c, NET_BOUND);
        } else if (bound && p->net.dhcp_state != DHCP_BOUND) {
            bound = 0;
            child_report(c, NET_WAITING);
        }
    }
}
