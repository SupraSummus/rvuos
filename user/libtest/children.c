/*
 * The library's test's children; see root.c.
 * Each starts with a0 at its page and keeps what it needs on its stack.
 */

#include "libtest.h"

#include "lib/say.h"

static void wait_any(void)
{
    uint32_t bits;
    if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
        rv_breakpoint();
    }
}

/* Waits whenever the ring is full, which only the other's take from it can end. */
static void send(const struct chan_end *e)
{
    for (uint32_t n = 0; n < PACKETS; n++) {
        uint8_t *slot;
        while ((slot = chan_put_begin(e)) == 0) {
            wait_any();
        }
        for (uint32_t i = 0; i < packet_len(n); i++) {
            slot[i] = packet_byte(n, i);
        }
        chan_put_end(e, packet_len(n));
    }
}

/* Waits whenever the ring is empty, which only the other's put into it can end. */
static void receive(struct child_page *page, const struct chan_end *e)
{
    for (uint32_t n = 0; n < PACKETS; n++) {
        uint32_t len;
        const uint8_t *in;
        while ((in = chan_get_begin(e, &len)) == 0) {
            wait_any();
        }
        if (len != packet_len(n)) {
            child_fail(page, PEER_STEP_LENGTH, n);
        }
        for (uint32_t i = 0; i < len; i++) {
            if (in[i] != packet_byte(n, i)) {
                child_fail(page, PEER_STEP_BYTES, n);
            }
        }
        chan_get_end(e);
    }
}

/* One way, then the other, so that each side only puts or only takes and its wakes have one cause each. */
void peer_main(struct child_page *page)
{
    struct peer_page *p = (struct peer_page *)page;
    struct out out = child_out(page);
    child_report(page, CHILD_RUNNING);
    if (p->sends_first) {
        send(&p->link);
        receive(page, &p->link);
    } else {
        receive(page, &p->link);
        send(&p->link);
    }
    say(&out, "peer: %u packets each way\n", PACKETS);
    child_stop(page, PEER_DONE);
}

void fault_main(struct child_page *page)
{
    struct fault_page *p = (struct fault_page *)page;
    struct out out = child_out(page);
    say(&out, "fault: a store to %x\n", p->address);
    child_report(page, CHILD_RUNNING);
    *(volatile uint32_t *)(uintptr_t)p->address = 1;
    child_stop(page, FAULT_BREACHED);
}
