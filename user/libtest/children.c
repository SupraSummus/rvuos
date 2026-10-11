/*
 * The library's test's children; see root.c.
 * Each starts with a0 at its page and keeps what it needs on its stack.
 */

#include "libtest.h"

#include "lib/libc.h"
#include "lib/say.h"
#include "lib/self.h"

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

/* The offset of the first byte of a ring's that is not zero, or the ring's bytes. */
static uint32_t ring_dirt(const struct ring *r)
{
    const volatile uint8_t *b = (const volatile uint8_t *)(uintptr_t)r->base;
    uint32_t bytes = ring_bytes(r->slots, r->slot_size);
    for (uint32_t k = 0; k < bytes; k++) {
        if (b[k] != 0) {
            return k;
        }
    }
    return bytes;
}

/*
 * A packet at a time, and each back before the next, once it found its channel zeroed;
 * then one more, whose echo it leaves in the ring, which the next client on its end must neither take nor read.
 */
void client_main(struct child_page *page)
{
    struct client_page *p = (struct client_page *)page;
    uint32_t at;
    child_report(page, CHILD_RUNNING);
    while (!chan_ready(&p->link)) {
        wait_any();
    }
    if ((at = ring_dirt(&p->link.rx)) != ring_bytes(p->link.rx.slots, p->link.rx.slot_size) ||
        (at = ring_dirt(&p->link.tx)) != ring_bytes(p->link.tx.slots, p->link.tx.slot_size)) {
        child_fail(page, CLIENT_STEP_DIRTY, at);
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
        if (!(p->named ? named_lock_try(&p->name) : lock_try(&p->lock))) {
            p->waited++;
            if ((p->named ? named_lock_take(&p->name) : lock_take(&p->lock)) != KERR_OK) {
                child_fail(page, LOCKER_STEP_TAKE, n);
            }
        }
        uint32_t was = *count;
        if (n % 2u == 0) {
            child_sleep(LOCK_SLEEP_US);
        }
        *count = was + 1u;
        if (p->named) {
            named_lock_give(&p->name);
        } else {
            lock_give(&p->lock);
        }
    }
    child_stop(page, LOCKER_DONE);
}

/* Sets the event each time the root task asks, once it has asked. */
void setter_main(struct child_page *page)
{
    struct setter_page *p = (struct setter_page *)page;
    volatile uint32_t *asked = (volatile uint32_t *)(uintptr_t)p->asked;
    child_report(page, CHILD_RUNNING);
    for (uint32_t n = 0; n < EVENT_SETS; n++) {
        while (*asked <= n) {
            child_sleep(EVENT_SLEEP_US);
        }
        event_set(&p->event);
    }
    child_stop(page, SETTER_DONE);
}

/* Copies the snapshot out until the root task says stop, failing at a copy that is not whole or goes back. */
void snap_reader_main(struct child_page *page)
{
    struct snap_page *p = (struct snap_page *)page;
    uint32_t data[SNAP_WORDS], version, last = 0;
    child_report(page, CHILD_RUNNING);
    while (!p->stop) {
        uint32_t halfway = p->writing;
        if (!seqlock_read(&p->lock, data, &version)) {
            p->again++;
            continue;
        }
        for (uint32_t i = 0; i < SNAP_WORDS; i++) {
            if (data[i] != version) {
                child_fail(page, SNAP_STEP_TORN, version);
            }
        }
        if (version < last) {
            child_fail(page, SNAP_STEP_OLDER, version);
        }
        last = version;
        p->halfway += halfway;
        p->reads++;
    }
    child_stop(page, SNAP_READ);
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

/* A connection's requests back and checked, held until a buffer to answer in is free. */
struct asking {
    struct bulk_ref held[BULK_FILLS];
    uint32_t count;
};

/*
 * Hands a buffer the lender took to connection i's borrower: 1, or 0 once the root task has closed the connection,
 * which may come at any moment, and which the lender acts on at its next look, letting go of all of it.
 */
static int lend(struct lender_page *p, uint32_t i, const struct bulk_ref *r)
{
    if (bulk_hand(&p->ends[i], r) == 0) {
        return 1;
    }
    if (bulk_ready(&p->ends[i])) {
        child_fail(&p->c, LENDER_STEP_LEND, i);
    }
    return 0;
}

/*
 * A connection connected anew: every buffer to read free, as nothing of a connection the lender let go of is held,
 * and a hand of one it does not hold refused;
 * then one kept for itself, as a driver keeps one for frames it reads itself.
 */
static void ask(struct lender_page *p, uint32_t i)
{
    struct bulk_end *e = &p->ends[i];
    struct bulk_ref r;
    uint32_t taken = 0;
    while (bulk_take(e, &r) == 0) {
        taken++;
    }
    for (uint32_t buf = 0; buf < taken; buf++) {
        bulk_put(e, buf);
    }
    struct bulk_ref free = { 1, 0, 1, 0, 0, 0 };
    if (taken != BULK_READS || bulk_hand(e, &free) == 0 || bulk_take(e, &r) != 0) {
        if (bulk_ready(e)) {
            child_fail(&p->c, LENDER_STEP_BOOKS, i);
        }
        return;
    }
    p->asked[i] = 0;
    p->answered[i] = 0;
}

/* A turn of what connection i handed back with bytes: requests, each checked where it lies and held; 1 if it had more. */
static int take_back(struct lender_page *p, uint32_t i, struct asking *a)
{
    struct bulk_end *e = &p->ends[i];
    struct bulk_turn t = bulk_turn(e);
    struct bulk_ref r;
    while (bulk_next(&t, &r)) {
        uint32_t n = r.tag;
        int same = r.buf >= e->reads && r.off == request_off(n) && r.len == request_len(n);
        for (uint32_t k = 0; same && k < r.len; k++) {
            same = r.bytes[k] == bulk_byte(REQUEST_SEED(p->seed[i]), n, k);
        }
        if (!same) {
            child_fail(&p->c, LENDER_STEP_BYTES, i);
        }
        p->asked[i]++;
        a->held[a->count++] = r;
    }
    return bulk_more(&t);
}

/* Each request held answered while a buffer to read is free, and its buffer given back to fill again. */
static void answer(struct lender_page *p, uint32_t i, struct asking *a)
{
    struct bulk_end *e = &p->ends[i];
    struct bulk_ref ans;
    while (a->count != 0 && bulk_take(e, &ans) == 0) {
        struct bulk_ref req = a->held[--a->count];
        uint32_t n = req.tag;
        ans.off = answer_off(n);
        ans.len = answer_len(n);
        ans.tag = n;
        for (uint32_t k = 0; k < ans.len; k++) {
            ans.room[ans.off + k] = bulk_byte(p->seed[i], n, k);
        }
        if (!lend(p, i, &ans)) {
            return;
        }
        p->answered[i]++;
        bulk_put(e, req.buf);
    }
}

/* Asks each borrower and answers it; a connection the root task closed is let go of, and nothing kept of it. */
void lender_main(struct child_page *page)
{
    struct lender_page *p = (struct lender_page *)page;
    struct asking asking[BULK_PEERS];
    memset(asking, 0, sizeof(asking));
    child_report(page, CHILD_RUNNING);
    for (;;) {
        int more = 0;
        for (uint32_t i = 0; i < BULK_PEERS; i++) {
            uint32_t gen;
            if (bulk_changed(&p->ends[i], &gen)) {
                p->let_go += !(gen & 1u);
                bulk_seen(&p->ends[i], gen);
                asking[i] = (struct asking){ .count = 0 };
                if (gen & 1u) {
                    ask(p, i);
                }
            }
            more |= take_back(p, i, &asking[i]);
            answer(p, i, &asking[i]);
        }
        if (!more) {
            wait_any();
        }
    }
}

/* The offset of the first of len bytes at base that is not zero, or len. */
static uint32_t first_set(uint32_t base, uint32_t len)
{
    const volatile uint8_t *b = (const volatile uint8_t *)(uintptr_t)base;
    for (uint32_t k = 0; k < len; k++) {
        if (b[k] != 0) {
            return k;
        }
    }
    return len;
}

/* A reference it does not hold, written straight into its ring, of bytes bytes, as a borrower that forges would. */
static void forge(const struct bulk_end *e, uint32_t buf, uint32_t off, uint32_t len, uint32_t bytes)
{
    uint8_t *slot = chan_put_begin(&e->refs);
    const uint32_t words[BULK_REF_BYTES / 4u] = { buf << BULK_OFF_BITS | off, len, 0 };
    if (slot != 0) {
        memcpy(slot, words, sizeof(words));
        chan_put_end(&e->refs, bytes);
    }
}

/*
 * Asks its requests in order in each buffer to fill it can take, and reads each answer where it lies and gives it back,
 * once it has found its blocks zeroed; a forger hands over what it does not hold too,
 * and one told to breach stores into its first answer.
 */
void borrower_main(struct child_page *page)
{
    struct borrower_page *p = (struct borrower_page *)page;
    struct bulk_end *e = &p->link;
    child_report(page, CHILD_RUNNING);
    while (!bulk_ready(e)) {
        wait_any();
    }
    if (p->flood) {
        *(volatile uint32_t *)(uintptr_t)e->refs.tx.base += 0x40000000u; /* the head, see lib/ring.c */
        rv_signal(e->refs.peer, NOTIFY_ALL_BITS);
        child_stop(page, BORROWER_FLOODED);
    }
    struct bulk_ref none = { e->reads, 0, 1, 0, 0, 0 };
    if (bulk_hand(e, &none) == 0) {
        child_fail(page, BORROWER_STEP_HAND, none.buf);
    }
    /* The answers, the requests and both rings, which the lender writes nothing of before it is asked. */
    uint32_t at, rings = 2u * ring_bytes(e->refs.rx.slots, e->refs.rx.slot_size);
    if ((at = first_set(e->read_base, e->reads * e->size)) != e->reads * e->size ||
        (at = first_set(e->fill_base, e->fills * e->size)) != e->fills * e->size ||
        (at = first_set(e->refs.rx.base, rings)) != rings) {
        child_fail(page, BORROWER_STEP_DIRTY, at);
    }
    uint32_t next = 0;
    int forged = !p->forge;
    while (p->filled < BULK_ROUNDS || p->read < BULK_ROUNDS) {
        struct bulk_turn t = bulk_turn(e);
        struct bulk_ref r;
        while (bulk_next(&t, &r)) {
            uint32_t n = r.tag;
            if (r.room != 0) {
                child_fail(page, BORROWER_STEP_ROOM, r.buf);
            }
            if (r.off != answer_off(n) || r.len != answer_len(n)) {
                child_fail(page, BORROWER_STEP_BYTES, n);
            }
            for (uint32_t k = 0; k < r.len; k++) {
                if (r.bytes[k] != bulk_byte(p->seed, n, k)) {
                    child_fail(page, BORROWER_STEP_BYTES, n);
                }
            }
            if (p->breach) {
                *(volatile uint8_t *)(uintptr_t)r.bytes = 0;
                child_stop(page, BORROWER_BREACHED);
            }
            bulk_put(e, r.buf);
            p->read++;
        }
        while (next < BULK_ROUNDS && bulk_take(e, &r) == 0) {
            if (r.room == 0) {
                child_fail(page, BORROWER_STEP_ROOM, r.buf);
            }
            /* Once it holds a request, before it hands anything, so that no answer is lent yet. */
            if (!forged) {
                forge(e, 1, 0, 1, BULK_REF_BYTES);                  /* an answer, none of which is lent yet */
                forge(e, 200, 0, 1, BULK_REF_BYTES);                /* no buffer of the connection's */
                forge(e, r.buf, 0, 1, 4);                           /* the request it holds, cut short */
                forge(e, r.buf, 0, BULK_SIZE + 1u, BULK_REF_BYTES); /* the same, past the end of its buffer */
                forged = 1;
            }
            for (uint32_t k = 0; k < request_len(next); k++) {
                r.room[request_off(next) + k] = bulk_byte(REQUEST_SEED(p->seed), next, k);
            }
            r.off = request_off(next);
            r.len = request_len(next);
            r.tag = next++;
            if (bulk_hand(e, &r) != 0) {
                child_fail(page, BORROWER_STEP_HAND, r.buf);
            }
            p->filled++;
        }
        if (!bulk_more(&t) && (p->filled < BULK_ROUNDS || p->read < BULK_ROUNDS)) {
            wait_any();
        }
    }
    child_stop(page, BORROWER_DONE);
}

/*
 * Moves its table to one twice as large, a notification it made in a slot of its own among what moves,
 * then makes another in the first slot past the old end, signals it there and deletes it through s.table,
 * which the old table, too small for that slot, would refuse; the second table slot is empty again.
 */
void mover_main(struct child_page *page)
{
    struct mover_page *p = (struct mover_page *)page;
    struct self s;
    uint32_t status, near, bits = 0;
    child_self(&s, &p->own);
    child_report(page, CHILD_RUNNING);
    if ((status = slot_new(&s, &near)) != KERR_OK ||
        (status = rv_pool_alloc(s.pool, CAP_NOTIFICATION, near, 0)) != KERR_OK) {
        child_fail(page, MOVER_STEP_MOVE, status);
    }
    uint32_t end = s.slot_end, unused = slots_unused(&s);
    if ((status = self_migrate(&s, s.pool, 2 * end)) != KERR_OK || slots_unused(&s) != unused + end ||
        (status = rv_signal(near, 0x1u)) != KERR_OK || (status = rv_wait(near, &bits)) != KERR_OK || bits != 0x1u ||
        (status = slot_free(&s, near)) != KERR_OK || rv_signal(SLOT_IN(1, s.inbox), 0x1u) != KERR_INVALID_CAP) {
        child_fail(page, MOVER_STEP_MOVE, status);
    }
    if ((status = rv_pool_alloc(s.pool, CAP_NOTIFICATION, end, 0)) != KERR_OK ||
        (status = rv_signal(end, 0x1u)) != KERR_OK || (status = rv_wait(end, &bits)) != KERR_OK || bits != 0x1u ||
        (status = rv_cap_delete(s.table, end)) != KERR_OK) {
        child_fail(page, MOVER_STEP_FAR, status);
    }
    child_report(page, MOVER_MOVED);
    for (;;) {
        child_answer(page);
        wait_any();
    }
}

/* The builder's thread, with a0 at the builder's page: a sleep on its own timer, then a breakpoint. */
static __attribute__((noreturn)) void worker_main(struct builder_page *p)
{
    uint32_t bits = 0;
    rv_timer_set(p->timer, 0x1u, OWN_SLEEP_US);
    while (rv_wait(p->note, &bits) == KERR_OK && !(bits & 0x1u)) {
    }
    p->worked = bits & 0x1u;
    for (;;) {
        rv_breakpoint();
    }
}

/* Builds the thread from its own account, through lib/self.h as a root task does; checks what the account refuses. */
void builder_main(struct child_page *page)
{
    struct builder_page *p = (struct builder_page *)page;
    struct self s;
    uint32_t thread, unit, time, line, status;
    child_self(&s, &p->own);
    child_report(page, CHILD_RUNNING);
    if ((status = slot_new(&s, &p->note)) != KERR_OK ||
        (status = rv_pool_alloc(s.pool, CAP_NOTIFICATION, p->note, 0)) != KERR_OK ||
        (status = timer_bind(&s, s.pool, p->note, &p->timer)) != KERR_OK ||
        (status = slot_new(&s, &thread)) != KERR_OK ||
        (status = rv_pool_alloc(s.pool, CAP_THREAD, thread, s.process)) != KERR_OK ||
        (status = rv_thread_watch(thread, p->own.watch, NOTIFY_ALL_BITS)) != KERR_OK ||
        (status = rv_thread_configure(thread, (uint32_t)(uintptr_t)worker_main,
                                      (uint32_t)(uintptr_t)(p->stack + sizeof(p->stack)), (uint32_t)(uintptr_t)p)) !=
            KERR_OK ||
        (status = units_take(&s, OWN_UNITS, &unit)) != KERR_OK || (status = slot_new(&s, &time)) != KERR_OK ||
        (status = rv_time_carve(s.time, unit, OWN_UNITS, time)) != KERR_OK ||
        (status = rv_time_bind(time, thread, 0, OWN_UNITS)) != KERR_OK || (status = slot_free(&s, time)) != KERR_OK) {
        child_fail(page, BUILDER_STEP_BUILD, status);
    }
    if (timer_bind(&s, s.pool, p->note, &line) != KERR_OK) {
        child_fail(page, BUILDER_STEP_LIMIT, 1);
    }
    if (timer_bind(&s, s.pool, p->note, &line) != KERR_LIMIT) {
        child_fail(page, BUILDER_STEP_LIMIT, 2);
    }
    if (units_take(&s, 1, &unit) != KERR_LIMIT) {
        child_fail(page, BUILDER_STEP_LIMIT, 3);
    }
    if ((status = rv_thread_resume(thread)) != KERR_OK) {
        child_fail(page, BUILDER_STEP_BUILD, status);
    }
    child_report(page, BUILDER_BUILT);
    for (;;) {
        child_answer(page);
        wait_any();
    }
}
