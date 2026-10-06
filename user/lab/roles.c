/* The laboratory's roles, each a child of the root task; NOTES.md says what each stands for. */

#include "lab.h"
#include "lib/libc.h"

uint32_t lab_work(uint32_t iterations, uint32_t seed)
{
    uint32_t x = seed | 1u;
    for (uint32_t i = 0; i < iterations; i++) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
    }
    return x;
}

uint32_t lab_draw(uint32_t seed)
{
    uint32_t x = seed * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    return x ^ (x >> 13);
}

static uint32_t wait_inbox(void)
{
    uint32_t bits = 0;
    if (rv_wait(CHILD_INBOX, &bits) != KERR_OK) {
        rv_breakpoint();
    }
    return bits;
}

/* The clock's low word, enough for differences shorter than its wrap. */
static uint32_t now(const struct lab_page *p)
{
    uint64_t t;
    uint32_t hz, counter;
    if (rv_clock_read(p->clock, &t, &hz, &counter) != KERR_OK) {
        rv_breakpoint();
    }
    return (uint32_t)t;
}

/* An ask that went at the clock's low word t was answered now. */
static void record(struct lab_page *p, uint32_t t)
{
    struct lab_stats *st = &p->st;
    uint32_t counts = now(p) - t, n = st->n;
    if (__atomic_load_n(&p->phase, __ATOMIC_ACQUIRE) != LAB_MEASURE) {
        return;
    }
    if (n < LAB_SAMPLES) {
        st->sample[n] = counts;
    }
    if (counts > st->worst) {
        st->worst = counts;
    }
    __atomic_store_n(&st->n, n + 1u, __ATOMIC_RELEASE);
}

/* Copies a packet of len bytes into a struct of size, zeroing what it does not reach. */
static void take(void *to, uint32_t size, const uint8_t *in, uint32_t len)
{
    memset(to, 0, size);
    memcpy(to, in, len < size ? len : size);
}

/*
 * Waits for the next period, keeping in *pending the bits that came meanwhile,
 * then works up to the page's jitter, drawn afresh, so that its ask falls anywhere in a tick, as events do,
 * rather than always at the tick that woke it.
 */
static uint32_t next_period(const struct lab_page *p, uint32_t *pending, uint32_t seq)
{
    uint32_t skipped;
    while (!(*pending & CHILD_BIT_TIMER)) {
        *pending |= wait_inbox();
    }
    *pending &= ~CHILD_BIT_TIMER;
    rv_timer_period(CHILD_TIMER, CHILD_BIT_TIMER, p->period_us, &skipped);
    return p->jitter != 0 ? lab_work(lab_draw(seq) % p->jitter, seq) : 0;
}

/* Asks the server for cost's work and waits for the answer; returns the other bits that came meanwhile. */
static uint32_t ask(struct lab_page *p, uint32_t seq, uint32_t cost)
{
    struct ask a = { seq, cost, 0 };
    uint32_t other = 0;
    while (chan_send(&p->up, &a, sizeof(a)) != 0) {
        other |= wait_inbox();
    }
    for (;;) {
        uint32_t len;
        const uint8_t *in = chan_get_begin(&p->up, &len);
        if (in == 0) {
            other |= wait_inbox();
            continue;
        }
        struct answer an;
        take(&an, sizeof(an), in, len);
        chan_get_end(&p->up);
        if (an.seq == seq) {
            return other;
        }
    }
}

/* Its parent connected or closed some of its ends: says it saw, as a server of a hub must. */
static void ends_changed(struct lab_page *p)
{
    for (uint32_t i = 0; i < LAB_ENDS; i++) {
        uint32_t gen;
        if (chan_changed(&p->ends[i], &gen)) {
            chan_seen(&p->ends[i], gen);
        }
    }
}

/* A server: one ask from each client in turn, until none is left, so every client is served alike. */
void store_main(struct child_page *c)
{
    struct lab_page *p = (struct lab_page *)c;
    for (;;) {
        if (wait_inbox() & CHILD_BIT_PARENT) {
            ends_changed(p);
        }
        for (int more = 1; more;) {
            more = 0;
            for (uint32_t i = 0; i < LAB_ENDS; i++) {
                uint32_t len;
                uint8_t *slot = chan_put_begin(&p->ends[i]);
                const uint8_t *in = chan_get_begin(&p->ends[i], &len);
                if (slot == 0 || in == 0) {
                    continue;
                }
                struct ask a;
                take(&a, sizeof(a), in, len);
                chan_get_end(&p->ends[i]);
                struct answer out = { a.seq, a.tag, lab_work(a.cost, a.seq) };
                memcpy(slot, &out, sizeof(out));
                chan_put_end(&p->ends[i], sizeof(out));
                more = 1;
            }
        }
    }
}

/*
 * A server in the middle: its clients' asks to its own server, tagged by end, and the answers back,
 * for which a client's ring has room, since it has one ask out at a time.
 */
void gate_main(struct child_page *c)
{
    struct lab_page *p = (struct lab_page *)c;
    for (;;) {
        if (wait_inbox() & CHILD_BIT_PARENT) {
            ends_changed(p);
        }
        for (int more = 1; more;) {
            more = 0;
            for (uint32_t i = 0; i < LAB_ENDS; i++) {
                uint32_t len;
                uint8_t *slot = chan_put_begin(&p->up);
                const uint8_t *in = chan_get_begin(&p->ends[i], &len);
                if (slot != 0 && in != 0) {
                    struct ask a;
                    take(&a, sizeof(a), in, len);
                    chan_get_end(&p->ends[i]);
                    a.tag = i;
                    memcpy(slot, &a, sizeof(a));
                    chan_put_end(&p->up, sizeof(a));
                    more = 1;
                }
            }
            uint32_t len;
            const uint8_t *in = chan_get_begin(&p->up, &len);
            if (in != 0) {
                struct answer an;
                take(&an, sizeof(an), in, len);
                chan_get_end(&p->up);
                if (an.tag < LAB_ENDS && chan_send(&p->ends[an.tag], &an, sizeof(an)) == 0) {
                    more = 1;
                }
            }
        }
    }
}

/* A client that every period asks for a little work and wants it before the next, or does it itself. */
void ui_main(struct child_page *c)
{
    struct lab_page *p = (struct lab_page *)c;
    volatile uint32_t sink = 0;
    uint32_t pending = CHILD_BIT_TIMER;
    for (uint32_t seq = 1;; seq++) {
        sink = next_period(p, &pending, seq);
        uint32_t t = now(p);
        if (p->local) {
            sink = lab_work(p->cost, seq);
        } else {
            pending |= ask(p, seq, p->cost);
        }
        record(p, t);
    }
    (void)sink;
}

/* A client that asks for much work, one ask after another. */
void bulk_main(struct child_page *c)
{
    struct lab_page *p = (struct lab_page *)c;
    for (uint32_t seq = 1;; seq++) {
        uint32_t t = now(p);
        ask(p, seq, p->cost);
        record(p, t);
    }
}

/* Wants the core all the time. */
void hog_main(struct child_page *c)
{
    (void)c;
    volatile uint32_t sink = 0;
    for (;;) {
        sink = lab_work(10000u, sink);
    }
}

/* Takes the lock it shares, or the mutex: KERR_OK, or the status of a wait that failed, holding nothing. */
static uint32_t take_lock(const struct lab_page *p)
{
    return p->mutex != 0 ? rv_mutex_lock(p->mutex, p->lend) : lock_take(&p->lock);
}

static void give_lock(const struct lab_page *p)
{
    if (p->mutex != 0) {
        rv_mutex_unlock(p->mutex);
    } else {
        lock_give(&p->lock);
    }
}

/* Holds the lock it shares for cost's work, every period, or with none again after gap's work outside it. */
void locker_main(struct child_page *c)
{
    struct lab_page *p = (struct lab_page *)c;
    volatile uint32_t sink = 0;
    uint32_t pending = CHILD_BIT_TIMER;
    for (uint32_t seq = 1;; seq++) {
        if (p->period_us != 0) {
            sink = next_period(p, &pending, seq);
        }
        uint32_t t = now(p);
        if (take_lock(p) != KERR_OK) {
            child_fail(c, 1, 0);
        }
        record(p, t);
        sink = lab_work(p->cost, seq);
        give_lock(p);
        sink = lab_work(p->gap, seq);
    }
    (void)sink;
}
