/*
 * What the Wi-Fi system's root tasks share; see system.h.
 */

#include "system.h"

#include "lib/libc.h"
#include "rvuos.h"
#include "sntp.h"
#include "wifi.h"

/* Each child's table and data: its page at the base, its state and two packets' buffers on its stack. */
#define NET_TABLE        16u
#define NET_DATA_SIZE    0x2000u
#define CLIENT_TABLE     8u
#define CLIENT_DATA_SIZE 0x2000u
#define CLIENT_RESTARTS  3u

/* The watchdog's deadline, three of the checks it is fed at. */
#define WATCHDOG_US 3000000u

/* The units of the first core's time each earns. */
#define NET_UNITS    12u
#define CLIENT_UNITS 4u

static const struct {
    const char *name;
    void (*entry)(struct child_page *page);
} kinds[CLIENTS] = {
    [CLIENT_ECHO] = { "the echo", echo_main },
    [CLIENT_CLOCK] = { "the clock", clock_main },
    [CLIENT_LOGGER] = { "the logger", logger_main },
};

void system_halt(uint32_t code)
{
    rv_halt(BOOT_CAP_DEBUG, code);
}

void system_must(struct system *s, const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(s->out, "root: %s failed", what);
        if (s->self->what != 0) {
            say(s->out, " at %s", s->self->what);
        }
        say(s->out, ": %x\n", status);
        system_halt(SYSTEM_BROKEN);
    }
}

void system_net_build(struct system *s, struct chan_end *driver_link, const uint8_t mac[6])
{
    struct self *self = s->self;
    struct child *n = &s->network;
    system_must(s, "build the network process", child_new(self, n, "the network", NET_TABLE, NET_DATA_SIZE));
    struct net_page *np = (struct net_page *)n->page;
    memcpy(np->mac, mac, 6);
    np->board = s->board;
    system_must(s, "the link", chan_new(self, &s->link, LINK_SIZE, LINK_SLOT));
    system_must(s, "connect the driver and the network",
                chan_connect(self, &s->link, &s->driver, driver_link, n, &np->link));
    /*
     * The hub's channels lie in one frame, so a power of two of them, as many as the clients wanted need;
     * the network process finds the ends past them closed.
     */
    uint32_t ends = 1;
    while ((s->clients_wanted >> ends) != 0) {
        ends *= 2;
    }
    system_must(s, "the clients' hub",
                chan_hub_new(self, &s->hub, n, np->clients, ends, CLIENT_CHAN_SIZE, CLIENT_SLOT));
    /*
     * The bytes of its page it publishes the network's addresses in, carved from the room, for the clock;
     * the network process lives as long as the system, and a teardown of it would revoke the carve first.
     */
    uint32_t config_at = (uint32_t)(uintptr_t)np->config - self->room.base;
    system_must(s, "a slot for the network's addresses", slot_new(self, &s->config));
    system_must(s, "carve the network's addresses",
                rv_frame_carve(self->room.made, config_at, NET_CONFIG_SIZE, s->config));
    system_must(s, "a bit to wake itself", child_bit(self, n, &np->more_bit));
    system_must(s, "its inbox to wake itself", child_give_bits(self, n, n->inbox, np->more_bit, &np->more));
}

void system_net_start(struct system *s)
{
    system_must(s, "start the network process", child_start(s->self, &s->network, net_main, NET_UNITS));
    s->net_started = 1;
}

/*
 * A client of the network process, on end i of the hub, which is idle;
 * the logger is given the kernel's log too, and the clock the network's addresses, read only.
 */
static void client_build(struct system *s, uint32_t i)
{
    struct client *cl = &s->clients[i];
    struct child *c = &cl->child;
    uint32_t region;
    system_must(s, "build a client", child_new(s->self, c, kinds[i].name, CLIENT_TABLE, CLIENT_DATA_SIZE));
    system_must(s, "connect a client", chan_hub_connect(s->self, &s->hub, i, c, &((struct client_page *)c->page)->net));
    if (i == CLIENT_LOGGER) {
        system_must(s, "give the logger the log", child_map(s->self, c, BOOT_CAP_LOG, RIGHT_R | RIGHT_W, &region));
        ((struct logger_page *)c->page)->log_base = s->log_base;
    }
    if (i == CLIENT_CLOCK) {
        system_must(s, "give the clock the addresses", child_map(s->self, c, s->config, RIGHT_R, &region));
        ((struct clock_page *)c->page)->config = net_config((const struct net_page *)s->network.page);
    }
    system_must(s, "start a client", child_start(s->self, c, kinds[i].entry, CLIENT_UNITS));
    cl->down = 0;
}

/* A client that faulted or failed: its channel closed, taken down, and built again later, a few times at most. */
static void client_down(struct system *s, uint32_t i)
{
    struct client *cl = &s->clients[i];
    system_must(s, "close a client's channel", chan_hub_close(s->self, &s->hub, i));
    system_must(s, "take a client down", child_free(s->self, &cl->child));
    cl->down = ++cl->restarts <= CLIENT_RESTARTS;
    say(s->out, "root: %s is taken down%s\n", kinds[i].name, cl->down ? ", to be built again" : " for good");
}

/* What a client's new state says, in the log. */
static void client_state(struct system *s, uint32_t i, uint32_t state)
{
    struct client *cl = &s->clients[i];
    const char *name = kinds[i].name;
    if (state == CHILD_FAILED) {
        say(s->out, "root: %s failed at step %u, read %x\n", name, cl->child.page->step, cl->child.page->detail);
        client_down(s, i);
    } else if (i == CLIENT_ECHO && state == ECHO_SERVING) {
        say(s->out, "root: %s answers on UDP port %u\n", name, ECHO_PORT);
    } else if (i == CLIENT_LOGGER && state == LOGGER_SERVING) {
        say(s->out, "root: %s sends the log to whoever asks on UDP port %u\n", name, LOGGER_PORT);
    } else if (i == CLIENT_CLOCK && state == CLOCK_SET) {
        const struct clock_page *p = (const struct clock_page *)cl->child.page;
        char text[UTC_TEXT];
        utc_format(text, p->seconds);
        say(s->out, "root: %s says it is %s, by %I, and tells it on UDP port %u\n", name, text, p->server, CLOCK_PORT);
    }
}

/* A child's fault, as the kernel tells it to the parent. */
static void say_fault(struct system *s, struct child *c)
{
    uint32_t cause, pc, addr, status;
    if (rv_thread_fault(c->thread, &cause, &pc, &addr, &status) == KERR_OK) {
        say(s->out, "root: %s faulted, cause %x at %x, address %x\n", c->name, cause, pc, addr);
    } else {
        say(s->out, "root: a thread of %s's faulted; the kernel's report says where\n", c->name);
    }
}

uint32_t system_heard(struct system *s, uint32_t bits)
{
    struct child *n = &s->network;
    uint32_t state;
    if (bits & s->driver.bit_fault) {
        say_fault(s, &s->driver);
        return SYSTEM_FAULTED;
    }
    if (n->page != 0 && (bits & n->bit_fault)) {
        say_fault(s, n);
        return SYSTEM_FAULTED;
    }
    if (n->page != 0 && child_poll(n, &state)) {
        if (state == CHILD_FAILED) {
            say(s->out, "root: %s failed at step %u, read %x\n", n->name, n->page->step, n->page->detail);
            return SYSTEM_FAILED;
        }
        if (state == NET_BOUND && !s->clients_built) {
            s->clients_built = 1;
            say(s->out, "root: the system answers at %I: ping it, or send a datagram to UDP port %u for its status\n",
                ((const struct net_page *)n->page)->ip, NET_PORT_STATUS);
            for (uint32_t i = 0; i < CLIENTS; i++) {
                if (s->clients_wanted & CLIENT_BIT(i)) {
                    client_build(s, i);
                }
            }
        }
    }
    for (uint32_t i = 0; i < CLIENTS; i++) {
        struct client *cl = &s->clients[i];
        struct child *c = &cl->child;
        if (c->page == 0) {
            if (cl->down && chan_hub_idle(&s->hub, i)) {
                client_build(s, i);
            }
            continue;
        }
        if (bits & c->bit_fault) {
            say_fault(s, c);
            client_down(s, i);
        } else if (child_poll(c, &state)) {
            client_state(s, i, state);
        }
    }
    return 0;
}

uint32_t system_check(struct system *s, int driver_serves)
{
    struct child *const serving[] = { driver_serves ? &s->driver : 0, s->net_started ? &s->network : 0 };
    for (uint32_t i = 0; i < sizeof(serving) / sizeof(serving[0]); i++) {
        if (serving[i] != 0 && !child_check(serving[i])) {
            say(s->out, "root: %s stopped answering\n", serving[i]->name);
            return SYSTEM_SILENT;
        }
    }
    for (uint32_t i = 0; i < CLIENTS; i++) {
        if (s->clients[i].child.page != 0 && !child_check(&s->clients[i].child)) {
            say(s->out, "root: %s stopped answering\n", kinds[i].name);
            client_down(s, i);
        }
    }
    system_must(s, "feed the watchdog", rv_clock_watchdog(BOOT_CAP_CLOCK, WATCHDOG_US));
    return 0;
}

void system_summary(struct system *s)
{
    if (s->net_started) {
        const struct net_page *n = (const struct net_page *)s->network.page;
        say(s->out, "; network frames in %u, out %u, pings %u, datagrams %u", n->rx_frames, n->tx_frames, n->pings,
            n->datagrams);
    }
    const struct echo_page *e = (const struct echo_page *)s->clients[CLIENT_ECHO].child.page;
    if (e != 0) {
        say(s->out, "; echoed %u", e->echoed);
    }
    const struct logger_page *l = (const struct logger_page *)s->clients[CLIENT_LOGGER].child.page;
    if (l != 0) {
        say(s->out, "; %u bytes of the log carried", l->carried);
    }
    say(s->out, "; %u bytes and %u slots unused\n", mem_unused(s->self), slots_unused(s->self));
}

void system_scanned(struct system *s, const struct drv_net *nets, uint32_t count)
{
    say(s->out, "root: %u access points heard\n", count);
    for (uint32_t i = 0; i < count; i++) {
        const struct drv_net *n = &nets[i];
        char ssid[sizeof(n->ssid)];
        uint32_t j = 0;
        for (; j + 1 < sizeof(ssid) && n->ssid[j] != 0; j++) {
            ssid[j] = n->ssid[j] >= 0x20 && n->ssid[j] < 0x7f ? n->ssid[j] : '?';
        }
        ssid[j] = 0;
        say(s->out, "  %M  ch %u  rssi %d  %s\n", n->bssid, n->channel, n->rssi, j ? ssid : "(hidden)");
    }
}
