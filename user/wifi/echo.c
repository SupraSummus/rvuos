/*
 * The echo: a client of the network process, in a process of its own, that sends every datagram to UDP port 7 back.
 *
 * It starts with a0 at its page and waits for the root task to connect its channel, then binds its port.
 * A datagram that reads "fault" makes it store where it has no region, as a broken client would,
 * and one that reads "hang" makes it spin, as a client caught in a loop would,
 * which show what each costs the rest of the system: the root task takes it down and builds it again,
 * once it faulted or once it did not answer a check, and the network process gives its port back meanwhile.
 */

#include "lib/libc.h"
#include "lib/say.h"
#include "wifi.h"

#define STEP_BIND 1u /* the port was refused; detail is why, a SOCK_ status */

/* Waits for anything, and answers the root task's check, which may be what came. */
static void wait_any(struct child_page *page)
{
    uint32_t bits;
    if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
        rv_breakpoint();
    }
    child_answer(page);
}

static void answer(struct echo_page *p, const struct sock_msg *h, const uint8_t *data)
{
    if (h->op == SOCK_BIND) {
        if (h->status != SOCK_OK) {
            child_fail(&p->c.c, STEP_BIND, h->status);
        }
        child_report(&p->c.c, ECHO_SERVING);
    } else if (h->op == SOCK_RECV && h->port == ECHO_PORT) {
        if (h->len == 5 && memcmp(data, "fault", 5) == 0) {
            *(volatile uint32_t *)0 = 0;
        }
        if (h->len == 4 && memcmp(data, "hang", 4) == 0) {
            for (;;) {
            }
        }
        /* Back from the port it came to, to where it came from; a full channel drops it, as a network may. */
        if (client_ask(&p->c.net, SOCK_SEND, ECHO_PORT, h->ip, h->peer_port, data, h->len) == 0) {
            p->echoed++;
        }
    }
}

__attribute__((noreturn)) void echo_main(struct child_page *page)
{
    struct echo_page *p = (struct echo_page *)page;
    struct out out = child_out();
    child_report(page, CHILD_RUNNING);
    while (!chan_ready(&p->c.net)) {
        wait_any(page);
    }
    client_ask(&p->c.net, SOCK_BIND, ECHO_PORT, 0, 0, 0, 0);
    say(&out, "echo: up\n");
    for (;;) {
        uint32_t len;
        const uint8_t *in;
        while ((in = chan_get_begin(&p->c.net, &len)) != 0) {
            struct sock_msg h;
            const uint8_t *data;
            if (sock_answer(in, len, &h, &data)) {
                answer(p, &h, data);
            }
            chan_get_end(&p->c.net);
        }
        wait_any(page);
    }
}
