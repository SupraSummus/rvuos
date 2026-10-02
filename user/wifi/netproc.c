/*
 * The network process: the IP stack of net.c between the link's rings and the UDP services; see wifi.h.
 *
 * It starts with a0 at the page it shares with the root task, keeps its state at the base of its data frame,
 * and waits on one notification for everything: frames from the driver, room in the ring to it,
 * its timer, which ticks every 100 ms for DHCP's retries, and the root task.
 * It asks for an address by DHCP once started, and answers on two UDP ports:
 * the echo, and a line that says what the system has done.
 */

#include <stddef.h>

#include "lib.h"
#include "net.h"
#include "ring.h"
#include "rvuos.h"
#include "wifi.h"

#define TICK_US 100000u

struct netp {
    struct net net; /* first, so that the stack's callbacks find the process */
    struct net_page *page;
    struct out out;
    uint32_t now; /* ms since the process started, by its timer */
    uint32_t tx_full;
};

static void np_put(void *to, char c)
{
    if (plog_put(&((struct netp *)to)->page->c.log, c)) {
        rv_signal(CHILD_ROOT, ROOT_BIT_PAGE(CHILD_NET));
    }
}

/* A frame into the ring to the driver, which hears of it if the ring was empty; dropped if the ring is full. */
static void np_send(struct net *n, const uint8_t *frame, uint32_t len)
{
    struct netp *p = (struct netp *)n;
    struct ring *tx = LINK_TX(p->page->link_base);
    uint8_t *slot = ring_put_begin(tx);
    if (slot == 0 || len > LINK_SLOT) {
        p->tx_full++;
        return;
    }
    memcpy(slot, frame, len);
    p->page->tx_frames++;
    if (ring_put_end(tx, len)) {
        rv_signal(NET_DRIVER, DRV_BIT_TX);
    }
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
    struct line l;
    l.len = 0;
    const struct out o = { line_put, &l };
    p->page->datagrams++;
    print(&o, "rvuos on a Pico 2 W: up ");
    print_dec(&o, p->now / 1000u);
    print(&o, " s, frames in ");
    print_dec(&o, n->rx_frames);
    print(&o, ", out ");
    print_dec(&o, n->tx_frames);
    print(&o, ", pings ");
    print_dec(&o, n->pings);
    print(&o, ", datagrams ");
    print_dec(&o, p->page->datagrams);
    print(&o, "\n");
    net_udp_send(n, from_ip, from_port, NET_PORT_STATUS, (const uint8_t *)l.buf, l.len);
}

static void report(struct netp *p, uint32_t state)
{
    p->page->c.state = state;
    rv_signal(CHILD_ROOT, ROOT_BIT_PAGE(CHILD_NET));
}

void net_main(struct net_page *page);
__attribute__((noreturn)) void net_main(struct net_page *page)
{
    struct netp *p = (struct netp *)page->c.data_base;
    memset(p, 0, sizeof(*p));
    p->page = page;
    p->out = (struct out){ np_put, p };
    net_init(&p->net, page->mac, np_send, p);
    net_udp_bind(&p->net, NET_PORT_ECHO, on_echo);
    net_udp_bind(&p->net, NET_PORT_STATUS, on_status);

    print(&p->out, "net: up\n");
    report(p, NET_WAITING);
    uint32_t skipped;
    rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
    net_dhcp_start(&p->net);

    struct ring *rx = LINK_RX(page->link_base);
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
        while ((frame = ring_get_begin(rx, &len)) != 0) {
            net_input(&p->net, frame, len);
            page->rx_frames++;
            if (ring_get_end(rx)) {
                rv_signal(NET_DRIVER, DRV_BIT_RXSPACE);
            }
        }
        page->pings = p->net.pings;
        if (!bound && p->net.dhcp_state == DHCP_BOUND) {
            bound = 1;
            char text[16];
            page->ip = p->net.ip;
            page->mask = p->net.mask;
            page->gateway = p->net.gateway;
            print(&p->out, "net: address ");
            print(&p->out, net_ip_text(p->net.ip, text));
            print(&p->out, ", gateway ");
            print(&p->out, net_ip_text(p->net.gateway, text));
            print(&p->out, "\n");
            report(p, NET_BOUND);
        } else if (bound && p->net.dhcp_state != DHCP_BOUND) {
            bound = 0;
            report(p, NET_WAITING);
        }
    }
}
