/* The benchmark's children; see root.c. */

#include "bench.h"
#include "lib/libc.h"

static void wait_inbox(void)
{
    uint32_t bits;
    rv_wait(CHILD_INBOX, &bits);
}

/* Answers each signal of its parent's with one of its own. */
void pong_main(struct child_page *page)
{
    (void)page;
    for (;;) {
        wait_inbox();
        rv_signal(CHILD_PARENT, NOTIFY_ALL_BITS);
    }
}

/* Wants the core all the time. */
void spin_main(struct child_page *page)
{
    (void)page;
    for (;;) {
        __asm__ volatile("");
    }
}

/* Sends back each packet as it came. */
void echo_main(struct child_page *page)
{
    struct ping_page *p = (struct ping_page *)page;
    for (;;) {
        uint32_t len;
        const uint8_t *packet;
        while ((packet = chan_get_begin(&p->link, &len)) != 0) {
            uint8_t *slot = chan_put_begin(&p->link);
            if (slot != 0) {
                memcpy(slot, packet, len);
                chan_put_end(&p->link, len);
            }
            chan_get_end(&p->link);
        }
        wait_inbox();
    }
}

/* A packet out, and waits until it is back. */
static void round_trip(struct ping_page *p, const uint8_t *packet)
{
    if (chan_send(&p->link, packet, PACKET) != 0) {
        child_fail(&p->c, 1, 0);
    }
    for (;;) {
        uint32_t len;
        if (chan_get_begin(&p->link, &len) != 0) {
            chan_get_end(&p->link);
            return;
        }
        wait_inbox();
    }
}

/* Sends a packet and waits for it back, a few times to start and then the page's rounds, timed. */
void ping_main(struct child_page *page)
{
    struct ping_page *p = (struct ping_page *)page;
    uint8_t packet[PACKET];
    memset(packet, 0x5a, sizeof(packet));
    while (!chan_ready(&p->link)) {
        wait_inbox();
    }
    for (uint32_t i = 0; i < 10; i++) {
        round_trip(p, packet);
    }
    uint64_t start, end;
    uint32_t hz, counter;
    rv_clock_read(p->clock, &start, &hz, &counter);
    for (uint32_t i = 0; i < p->rounds; i++) {
        round_trip(p, packet);
    }
    rv_clock_read(p->clock, &end, &hz, &counter);
    p->counts = (uint32_t)(end - start);
    p->hz = hz;
    child_stop(&p->c, PING_DONE);
}
