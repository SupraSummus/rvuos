/*
 * The logger: a client of the network process, in a process of its own, that carries the kernel's log to a host
 * while the system runs; wifi.h says what the two send each other.
 *
 * It reads the log through the frame the root task installed for it, which lets it rewrite the log too,
 * as the log's reader always could.
 * It serves the first host that asks until that host is quiet for five seconds, since the log has one reader.
 * It sends every 100 ms, so that the lines of a burst go out together, and as the host's word comes back,
 * at most a window ahead of what the host has; what the host lacks half a second after it was sent is sent again.
 */

#include "bytes.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "wifi.h"

#define TICK_US   100000u
#define CHUNK     1024u /* the most bytes of the log in one datagram */
#define WINDOW    2048u /* the most bytes sent that the host has not said it has */
#define RESEND_MS 500u
#define QUIET_MS  5000u

#define STEP_BIND 1u /* the port was refused; detail is why, a SOCK_ status */

struct lg {
    struct logger_page *page;
    const struct chan_end *net;
    struct out out;
    volatile struct rvuos_log *log;
    const volatile uint8_t *ring;
    uint32_t size;      /* of the ring, as the kernel said at the start */
    uint32_t now;       /* ms since it started, by its timer */
    uint32_t host;      /* the host it serves, network order, or 0 */
    uint16_t host_port;
    uint32_t heard;     /* when the host last said anything */
    uint32_t sent;      /* the offset of the next byte to send */
    uint32_t since;     /* when the oldest byte sent that the host has not said it has went */
    int held;           /* a send did not go: nothing more until the next tick */
    uint8_t msg[4u + CHUNK];
};

/* The log's taken, moved up to the oldest byte the ring still holds if the kernel wrote over the ones before. */
static uint32_t taken(struct lg *g, uint32_t head)
{
    uint32_t t = g->log->taken;
    if (head - t > g->size) {
        t = head - g->size;
        g->log->taken = t;
    }
    return t;
}

/* What the host lacks, as far ahead of what it said it has as the window lets, while the channel takes it. */
static void pump(struct lg *g)
{
    for (;;) {
        uint32_t head = g->log->head;
        uint32_t t = taken(g, head);
        if (g->sent - t > head - t || (g->sent != t && g->now - g->since >= RESEND_MS)) {
            g->sent = t; /* before what the ring holds, or not had in time: again from the first byte the host lacks */
        }
        if (g->sent == t) {
            g->since = g->now;
        }
        uint32_t ahead = g->sent - t;
        if (g->sent == head || ahead >= WINDOW) {
            return;
        }
        uint32_t n = head - g->sent;
        n = n < CHUNK ? n : CHUNK;
        n = n < WINDOW - ahead ? n : WINDOW - ahead;
        put_be32(g->msg, g->sent);
        for (uint32_t i = 0; i < n; i++) {
            g->msg[4u + i] = g->ring[(g->sent + i) % g->size];
        }
        if (g->log->head - g->sent > g->size) {
            continue; /* the kernel wrote over them meanwhile */
        }
        if (client_ask(g->net, SOCK_SEND, LOGGER_PORT, g->host, g->host_port, g->msg, 4u + n) != 0) {
            return;
        }
        g->sent += n;
    }
}

/* A host's word: the offset of the first byte it lacks; from the first host to speak, its asking for the log. */
static void heard(struct lg *g, const struct sock_msg *h, const uint8_t *data)
{
    if (h->len != 4u) {
        return;
    }
    if (g->host == 0) {
        g->host = h->ip;
        g->host_port = h->peer_port;
        g->sent = g->log->taken;
        say(&g->out, "logger: to %I, port %u\n", h->ip, h->peer_port);
    } else if (h->ip != g->host || h->peer_port != g->host_port) {
        return;
    }
    g->heard = g->now;
    uint32_t have = be32(data);
    uint32_t head = g->log->head;
    uint32_t t = g->log->taken;
    if (have - t > head - t) {
        g->sent = t; /* it lacks bytes before the ones it was sent, or speaks of another boot */
        return;
    }
    if (have != t) {
        g->log->taken = have;
        g->page->carried += have - t;
        g->since = g->now;
    }
    if (g->sent - t < have - t) {
        g->sent = have;
    }
}

static void answer(struct lg *g, const struct sock_msg *h, const uint8_t *data)
{
    if (h->op == SOCK_BIND) {
        if (h->status != SOCK_OK) {
            child_fail(&g->page->c.c, STEP_BIND, h->status);
        }
        child_report(&g->page->c.c, LOGGER_SERVING);
    } else if (h->op == SOCK_RECV && h->port == LOGGER_PORT) {
        heard(g, h, data);
    } else if (h->op == SOCK_SEND) {
        /* A datagram that did not go, while the next hop is found or the network has no address. */
        g->sent = g->log->taken;
        g->held = 1;
    }
}

__attribute__((noreturn)) void logger_main(struct child_page *page)
{
    struct lg lg, *g = &lg;
    memset(g, 0, sizeof(*g));
    g->page = (struct logger_page *)page;
    g->net = &g->page->c.net;
    g->out = child_out();
    g->log = (volatile struct rvuos_log *)(uintptr_t)g->page->log_base;
    g->ring = (const volatile uint8_t *)(uintptr_t)(g->page->log_base + RVUOS_LOG_HEADER);
    g->size = g->log->size;
    child_report(page, CHILD_RUNNING);
    uint32_t skipped;
    rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
    int asked = 0;
    for (;;) {
        uint32_t bits = 0;
        if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
        child_answer(page);
        if (bits & CHILD_BIT_TIMER) {
            rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
            g->now += (skipped + 1u) * (TICK_US / 1000u);
            g->held = 0;
        }
        if (!chan_ready(g->net)) {
            continue;
        }
        if (!asked) {
            asked = 1;
            client_ask(g->net, SOCK_BIND, LOGGER_PORT, 0, 0, 0, 0);
            say(&g->out, "logger: up\n");
        }
        uint32_t len;
        const uint8_t *in;
        while ((in = chan_get_begin(g->net, &len)) != 0) {
            struct sock_msg h;
            const uint8_t *data;
            if (sock_answer(in, len, &h, &data)) {
                answer(g, &h, data);
            }
            chan_get_end(g->net);
        }
        if (g->host != 0 && g->now - g->heard >= QUIET_MS) {
            say(&g->out, "logger: %I said nothing for %u s; the log waits for a host\n", g->host, QUIET_MS / 1000u);
            g->host = 0;
        }
        if (g->host != 0 && !g->held) {
            pump(g);
        }
    }
}
