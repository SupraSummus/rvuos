/*
 * The clock: a client of the network process, in a process of its own, that learns the time and tells it.
 *
 * It starts with a0 at its page and waits for the root task to connect its channel.
 * Then each step is a question and its answer, asked again every second until it comes:
 * the network's addresses from the network process, a port of its own, the address of a server of pool.ntp.org
 * from the network's DNS server, and the time from that server by SNTP.
 * It keeps the time by its timer from then on, and answers any datagram to UDP port 13 with it as text, RFC 867.
 */

#include "lib/libc.h"
#include "lib/say.h"
#include "sntp.h"
#include "wifi.h"

#define TICK_US  100000u
#define RETRY_MS 1000u
#define RESEND_MS 100u /* after a send that went nowhere, while the next hop's address is asked for */

enum step {
    ASK_CONFIG, /* the network's addresses */
    ASK_PORT,   /* a port of its own to ask from */
    ASK_NAME,   /* the server's address */
    ASK_TIME,   /* the time */
    KNOWS,
};

struct clk {
    struct clock_page *page;
    const struct chan_end *net;
    struct out out;
    uint32_t step;
    uint32_t now, asked; /* ms since it started, by its timer, and when it last asked */
    struct sock_config config;
    uint16_t port, id;
    uint32_t server, nonce;
    uint32_t seconds, at; /* the time it learned, and when, by now */
    uint8_t msg[DNS_MSG_MAX];
};

/* Asks what the step needs, again if need be. */
static void ask(struct clk *k)
{
    uint32_t len;
    k->asked = k->now;
    switch (k->step) {
    case ASK_CONFIG:
        client_ask(k->net, SOCK_CONFIG, 0, 0, 0, 0, 0);
        break;
    case ASK_PORT:
        client_ask(k->net, SOCK_BIND, 0, 0, 0, 0, 0);
        break;
    case ASK_NAME:
        k->id = (uint16_t)(k->id * 31u + k->now + 1u);
        len = dns_query(k->msg, k->id, CLOCK_SERVER);
        client_ask(k->net, SOCK_SEND, k->port, k->config.dns, DNS_PORT, k->msg, len);
        break;
    case ASK_TIME:
        k->nonce = k->nonce * 1103515245u + 12345u + k->now;
        ntp_query(k->msg, k->nonce);
        client_ask(k->net, SOCK_SEND, k->port, k->server, NTP_PORT, k->msg, NTP_MSG);
        break;
    }
}

static void next(struct clk *k, uint32_t step)
{
    k->step = step;
    ask(k);
}

/* The time as text, to whoever asked on port 13. */
static void tell(struct clk *k, const struct sock_msg *h)
{
    char text[32];
    uint32_t len = sizeof("the time is not known yet\n") - 1u;
    if (k->step == KNOWS) {
        utc_format(text, k->seconds + (k->now - k->at) / 1000u);
        len = strlen(text);
        text[len++] = '\n';
    } else {
        memcpy(text, "the time is not known yet\n", len);
    }
    client_ask(k->net, SOCK_SEND, CLOCK_PORT, h->ip, h->peer_port, text, len);
}

static void answer(struct clk *k, const struct sock_msg *h, const uint8_t *data)
{
    switch (h->op) {
    case SOCK_CONFIG:
        if (k->step == ASK_CONFIG && h->status == SOCK_OK && h->len == sizeof(k->config)) {
            memcpy(&k->config, data, sizeof(k->config));
            if (k->config.ip != 0 && k->config.dns != 0) {
                say(&k->out, "clock: asking %I for %s\n", k->config.dns, CLOCK_SERVER);
                client_ask(k->net, SOCK_BIND, CLOCK_PORT, 0, 0, 0, 0);
                next(k, ASK_PORT);
            }
        }
        break;
    case SOCK_BIND:
        if (h->status != SOCK_OK) {
            say(&k->out, "clock: port %u refused, %u\n", h->port, h->status);
        } else if (h->port != CLOCK_PORT && k->step == ASK_PORT) {
            k->port = h->port;
            next(k, ASK_NAME);
        }
        break;
    case SOCK_SEND:
        /* A datagram that did not go: again soon, if the next hop is being found, else at the next try. */
        if (h->status == SOCK_RESOLVING) {
            k->asked = k->now - RETRY_MS + RESEND_MS;
        }
        break;
    case SOCK_RECV:
        if (h->port == CLOCK_PORT) {
            tell(k, h);
        } else if (k->step == ASK_NAME && h->ip == k->config.dns && h->peer_port == DNS_PORT) {
            uint32_t ip = dns_answer(data, h->len, k->id);
            if (ip != 0) {
                k->server = ip;
                say(&k->out, "clock: asking %I for the time\n", ip);
                next(k, ASK_TIME);
            }
        } else if (k->step == ASK_TIME && h->ip == k->server && h->peer_port == NTP_PORT) {
            uint32_t seconds = ntp_answer(data, h->len, k->nonce);
            if (seconds != 0) {
                k->seconds = seconds;
                k->at = k->now;
                k->step = KNOWS;
                k->page->server = k->server;
                k->page->seconds = seconds;
                child_report(&k->page->c.c, CLOCK_SET);
            }
        }
        break;
    }
}

__attribute__((noreturn)) void clock_main(struct child_page *page)
{
    struct clk clk, *k = &clk;
    memset(k, 0, sizeof(*k));
    k->page = (struct clock_page *)page;
    k->net = &k->page->c.net;
    k->out = child_out();
    child_report(page, CHILD_RUNNING);
    uint32_t skipped;
    rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
    int asked = 0;
    for (;;) {
        uint32_t bits = 0;
        if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
        if (bits & CHILD_BIT_TIMER) {
            rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, TICK_US, &skipped);
            k->now += (skipped + 1u) * (TICK_US / 1000u);
        }
        if (!chan_ready(k->net)) {
            continue;
        }
        if (!asked) {
            asked = 1;
            say(&k->out, "clock: up\n");
            ask(k);
        }
        uint32_t len;
        const uint8_t *in;
        while ((in = chan_get_begin(k->net, &len)) != 0) {
            struct sock_msg h;
            const uint8_t *data;
            if (sock_answer(in, len, &h, &data)) {
                answer(k, &h, data);
            }
            chan_get_end(k->net);
        }
        if (k->step != KNOWS && k->now - k->asked >= RETRY_MS) {
            ask(k);
        }
    }
}
