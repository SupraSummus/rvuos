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

/* Waits until its parent tells it something. */
static void wait_parent(void)
{
    uint32_t bits = 0;
    while (!(bits & CHILD_BIT_PARENT)) {
        if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
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
    struct out out = child_out();
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
    struct out out = child_out();
    say(&out, "fault: a store to %x\n", p->address);
    child_report(page, CHILD_RUNNING);
    *(volatile uint32_t *)(uintptr_t)p->address = 1;
    child_stop(page, FAULT_BREACHED);
}

/* Answers each check as its parent asks it, and does nothing else. */
void answer_main(struct child_page *page)
{
    child_report(page, CHILD_RUNNING);
    for (;;) {
        child_answer(page);
        wait_any();
    }
}

/* Comes round no loop, so it answers no check, and its parent must find it so. */
void spin_main(struct child_page *page)
{
    child_report(page, CHILD_RUNNING);
    for (;;) {
    }
}

/* Each packet back on the channel it came on; a client's end the root task closed is let go of, and nothing kept of it. */
void server_main(struct child_page *page)
{
    struct server_page *p = (struct server_page *)page;
    child_report(page, CHILD_RUNNING);
    for (;;) {
        for (uint32_t i = 0; i < HUB_CLIENTS; i++) {
            struct chan_end *e = &p->ends[i];
            uint32_t gen, len;
            const uint8_t *in;
            if (chan_changed(e, &gen)) {
                p->let_go += !(gen & 1u);
                chan_seen(e, gen);
            }
            while ((in = chan_get_begin(e, &len)) != 0) {
                chan_send(e, in, len);
                chan_get_end(e);
            }
        }
        wait_any();
    }
}

/*
 * A packet at a time, and each back before the next;
 * then one more, whose echo it leaves in the ring, which the next client on its end must not see.
 */
void client_main(struct child_page *page)
{
    struct client_page *p = (struct client_page *)page;
    child_report(page, CHILD_RUNNING);
    while (!chan_ready(&p->link)) {
        wait_any();
    }
    for (uint32_t n = 0; n < PACKETS; n++) {
        uint8_t *slot;
        uint32_t len;
        const uint8_t *in;
        while ((slot = chan_put_begin(&p->link)) == 0) {
            wait_any();
        }
        for (uint32_t i = 0; i < packet_len(n); i++) {
            slot[i] = packet_byte(n + p->seed, i);
        }
        chan_put_end(&p->link, packet_len(n));
        while ((in = chan_get_begin(&p->link, &len)) == 0) {
            wait_any();
        }
        if (len != packet_len(n)) {
            child_fail(page, PEER_STEP_LENGTH, n);
        }
        for (uint32_t i = 0; i < len; i++) {
            if (in[i] != packet_byte(n + p->seed, i)) {
                child_fail(page, PEER_STEP_BYTES, n);
            }
        }
        chan_get_end(&p->link);
    }
    chan_send(&p->link, "left", 4);
    child_stop(page, CLIENT_DONE);
}

void locker_main(struct child_page *page)
{
    struct locker_page *p = (struct locker_page *)page;
    volatile uint32_t *count = (volatile uint32_t *)(uintptr_t)p->count;
    child_report(page, CHILD_RUNNING);
    for (uint32_t n = 0; n < LOCK_ROUNDS; n++) {
        if (!lock_try(&p->lock)) {
            p->waited++;
            if (lock_take(&p->lock) != KERR_OK) {
                child_fail(page, LOCKER_STEP_TAKE, n);
            }
        }
        uint32_t was = *count;
        if (n % 2u == 0) {
            child_sleep(LOCK_SLEEP_US);
        }
        *count = was + 1u;
        lock_give(&p->lock);
    }
    child_stop(page, LOCKER_DONE);
}

/* The root task tells it once for each pass it reported, so it reads after the wait that took the tell's bit. */
void passer_main(struct child_page *page)
{
    struct passer_page *p = (struct passer_page *)page;
    volatile uint32_t *buffer = (volatile uint32_t *)(uintptr_t)p->buffer;
    child_report(page, CHILD_RUNNING);
    for (;;) {
        wait_parent();
        uint32_t pass = p->pass;
        if (pass == PASS_GONE) {
            *buffer = PASS_GONE;
            child_stop(page, PASSER_BREACHED);
        }
        if (pass > 0 && *buffer != pass - 1u) {
            child_fail(page, PASSER_STEP_BUFFER, *buffer);
        }
        *buffer = pass;
        child_report(page, PASSER_HELD + pass);
    }
}
