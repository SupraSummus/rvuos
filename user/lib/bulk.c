/*
 * Bulk data, lent; see bulk.h.
 *
 * A connection's block both write holds the lender's ring of references, then the borrower's,
 * then the two words that say each side asked for room, the lender's first, then the buffers lent to fill;
 * its block lent to read holds the buffers lent to read.
 * A reference is three words in a ring's slot, as bulk.h says.
 * Each side reads a reference once, into memory of its own, and checks it there against what its end says,
 * so a side can be handed nothing outside its connection's buffers, and the lender nothing it did not lend.
 *
 * Waking: a side counts the references it put, and how many it had put when it last woke the other.
 * Once the other has taken that many it may be waiting, so the next reference with bytes wakes it;
 * room wakes it only if its word says it asked.
 * A side that asks writes its word before it looks at its ring a last time,
 * and a side that puts writes its reference before it reads the word, so one of the two sees the other.
 */

#include "lib/bulk.h"

#include "lib/child.h"
#include "lib/libc.h"

#define REF_WORDS  (BULK_REF_BYTES / 4u)
#define REF_BYTES  BULK_REF_BYTES
#define OFF_BITS   BULK_OFF_BITS
#define WANT_BYTES 8u  /* the two words that say a side asked for room */
#define BLOCK_MIN  SELF_REGION_MIN /* the smallest block, a region on every board */

_Static_assert(BULK_SIZE_MAX <= 1u << OFF_BITS, "an offset fits below the buffer");
_Static_assert(BULK_BUFFERS <= 1u << (32u - OFF_BITS), "a buffer's number fits above the offset");
_Static_assert(BULK_BUFFERS % 32u == 0, "the buffers fill the words that say who holds them");

/* What a reference handed to a side is to it. */
enum verdict {
    REFUSED, /* names nothing this side could be handed */
    ROOM,    /* a buffer given back empty, or lent empty to fill */
    BYTES,   /* a buffer handed over with bytes */
};

static int has(const uint32_t *map, uint32_t n)
{
    return (map[n / 32u] >> (n % 32u)) & 1u;
}

static void mark(uint32_t *map, uint32_t n)
{
    map[n / 32u] |= 1u << (n % 32u);
}

static void unmark(uint32_t *map, uint32_t n)
{
    map[n / 32u] &= ~(1u << (n % 32u));
}

static uint32_t buffers(const struct bulk_end *e)
{
    return e->reads + e->fills;
}

/* Where r's bytes lie, and whether this side may write them, from its end alone. */
static void place(const struct bulk_end *e, struct bulk_ref *r)
{
    uint32_t base = r->buf < e->reads ? e->read_base + r->buf * e->size : e->fill_base + (r->buf - e->reads) * e->size;
    uint8_t *bytes = (uint8_t *)(uintptr_t)(base + r->off);
    r->bytes = bytes;
    r->room = e->lender || r->buf >= e->reads ? bytes : 0;
}

/* Whether r names bytes of one of the connection's buffers. */
static int fits(const struct bulk_end *e, const struct bulk_ref *r)
{
    return r->buf < buffers(e) && r->off <= e->size && r->len <= e->size - r->off;
}

/* The word that says whether the lender, or a borrower, asked for room. */
static volatile uint32_t *want(const struct bulk_end *e, uint32_t lender)
{
    return (volatile uint32_t *)(uintptr_t)e->wants + (lender ? 0 : 1);
}

/*
 * A reference into this side's ring, and the other side woken if it may wait for it:
 * for bytes, once it has taken every reference it was last woken for; for room, if it asked for room.
 * 0, or -1 if the ring is full or the end not connected.
 */
static int send_ref(struct bulk_end *e, uint32_t buf, uint32_t off, uint32_t len, uint32_t tag, int bytes)
{
    uint8_t *slot = chan_put_begin(&e->refs);
    if (slot == 0) {
        return -1;
    }
    const uint32_t words[REF_WORDS] = { buf << OFF_BITS | off, len, tag };
    memcpy(slot, words, sizeof(words));
    ring_put_end(&e->refs.tx, sizeof(words)); /* whose fence puts the reference before the reads below */
    e->sent++;
    if (bytes ? (int32_t)(ring_taken(&e->refs.tx) - e->woke) >= 0 : *want(e, !e->lender) != 0) {
        e->woke = e->sent;
        rv_signal(e->refs.peer, NOTIFY_ALL_BITS);
    }
    return 0;
}

/* The next reference of this side's ring, read once into r, and what it is; 0 if the ring is empty. */
static int peek_ref(struct bulk_end *e, struct bulk_ref *r, enum verdict *v)
{
    uint32_t len, words[REF_WORDS] = { 0, 0, 0 };
    const uint8_t *slot = chan_get_begin(&e->refs, &len);
    if (slot == 0) {
        return 0;
    }
    memcpy(words, slot, len < sizeof(words) ? len : sizeof(words));
    *r = (struct bulk_ref){ words[0] >> OFF_BITS, words[0] & ((1u << OFF_BITS) - 1u), words[1], words[2], 0, 0 };
    /* The lender is handed back what it lent; a borrower is handed what it neither holds nor has spare. */
    if (len != sizeof(words) || !fits(e, r) ||
        !(e->lender ? has(e->away, r->buf) : !has(e->held, r->buf) && !has(e->spare, r->buf))) {
        *v = REFUSED;
    } else {
        *v = (r->buf < e->reads) == (e->lender != 0) ? ROOM : BYTES;
    }
    return 1;
}

/* What a reference peeked at is to this side, taken in: room among what it may take, bytes among what it holds. */
static void take_in(struct bulk_end *e, struct bulk_ref *r, enum verdict v)
{
    ring_get_end(&e->refs.rx);
    if (v == REFUSED) {
        e->refused++;
        return;
    }
    if (e->lender) {
        unmark(e->away, r->buf);
    }
    if (v == ROOM) {
        if (!e->lender) {
            mark(e->spare, r->buf);
        }
        *want(e, e->lender) = 0;
        return;
    }
    mark(e->held, r->buf);
    place(e, r);
}

/* A buffer this side may take: the lender's to read that no one holds, a borrower's spare to fill. */
static int find(const struct bulk_end *e, uint32_t *buf)
{
    for (uint32_t b = e->lender ? 0 : e->reads, end = e->lender ? e->reads : buffers(e); b < end; b++) {
        if (e->lender ? !has(e->held, b) && !has(e->away, b) : has(e->spare, b)) {
            *buf = b;
            return 1;
        }
    }
    return 0;
}

int bulk_ready(const struct bulk_end *e)
{
    return chan_ready(&e->refs);
}

int bulk_take(struct bulk_end *e, struct bulk_ref *r)
{
    uint32_t buf;
    if (!bulk_ready(e)) {
        return -1;
    }
    if (!find(e, &buf)) {
        /* Room asked for, then the room already in the ring taken in, as far as the first reference with bytes. */
        *want(e, e->lender) = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (uint32_t n = ring_ready(&e->refs.rx); n != 0; n--) {
            struct bulk_ref in;
            enum verdict v;
            if (peek_ref(e, &in, &v) == 0 || v == BYTES) {
                break;
            }
            take_in(e, &in, v);
        }
        if (!find(e, &buf)) {
            return -1;
        }
        *want(e, e->lender) = 0;
    }
    if (!e->lender) {
        unmark(e->spare, buf);
    }
    mark(e->held, buf);
    *r = (struct bulk_ref){ buf, 0, e->size, 0, 0, 0 };
    place(e, r);
    return 0;
}

int bulk_hand(struct bulk_end *e, const struct bulk_ref *r)
{
    /* The lender hands over buffers to read, a borrower buffers to fill, each one it took. */
    if (r->buf >= buffers(e) || (r->buf < e->reads) != (e->lender != 0) || !has(e->held, r->buf)) {
        return -1;
    }
    if (!fits(e, r) || send_ref(e, r->buf, r->off, r->len, r->tag, 1) != 0) {
        bulk_put(e, r->buf);
        return -1;
    }
    unmark(e->held, r->buf);
    if (e->lender) {
        mark(e->away, r->buf);
    }
    return 0;
}

struct bulk_turn bulk_turn(struct bulk_end *e)
{
    return (struct bulk_turn){ e, bulk_ready(e) ? ring_ready(&e->refs.rx) : 0 };
}

int bulk_next(struct bulk_turn *t, struct bulk_ref *r)
{
    while (t->left != 0) {
        enum verdict v;
        if (peek_ref(t->e, r, &v) == 0) {
            break;
        }
        t->left--;
        take_in(t->e, r, v);
        if (v == BYTES) {
            return 1;
        }
    }
    t->left = 0;
    return 0;
}

int bulk_more(const struct bulk_turn *t)
{
    return bulk_ready(t->e) && ring_ready(&t->e->refs.rx) != 0;
}

void bulk_put(struct bulk_end *e, uint32_t buf)
{
    if (buf >= buffers(e) || !has(e->held, buf)) {
        return;
    }
    unmark(e->held, buf);
    if ((buf < e->reads) == (e->lender != 0)) {
        /* One it took and did not hand: the lender's to read is free again, a borrower's to fill spare again. */
        if (!e->lender) {
            mark(e->spare, buf);
        }
        return;
    }
    /* Room back: the lender's buffer to fill, whole, to fill again; a borrower's buffer to read, read. */
    if (send_ref(e, buf, 0, e->lender ? e->size : 0, 0, 0) == 0 && e->lender) {
        mark(e->away, buf);
    }
}

int bulk_changed(const struct bulk_end *e, uint32_t *gen)
{
    return chan_changed(&e->refs, gen);
}

void bulk_seen(struct bulk_end *e, uint32_t gen)
{
    if (!(gen & 1u)) {
        memset(e->held, 0, sizeof(e->held));
        memset(e->away, 0, sizeof(e->away));
        memset(e->spare, 0, sizeof(e->spare));
    }
    chan_seen(&e->refs, gen);
}

/* --- The parent's side. --- */

static uint32_t at_least(uint32_t bytes)
{
    uint32_t size = BLOCK_MIN;
    while (size < bytes) {
        size *= 2u;
    }
    return size;
}

/*
 * The slots of each ring of a connection: two for every buffer, so that honest sides fill a ring half at most,
 * and a take never finds a ring full that it would have to wake the other side for.
 */
static uint32_t ring_slots(const struct bulk_shape *shape)
{
    uint32_t slots = 1;
    while (slots < 2u * (shape->reads + shape->fills)) {
        slots *= 2u;
    }
    return slots;
}

/* Connection i's block both write, and its block lent to read. */
static uint32_t both_base(const struct bulk *b, uint32_t i)
{
    return b->frame.base + b->both_at + i * b->both_size;
}

static uint32_t read_base(const struct bulk *b, uint32_t i)
{
    return b->frame.base + b->read_at + i * b->read_size;
}

uint32_t bulk_new(struct self *s, struct bulk *b, struct child *lender, struct bulk_end *ends, uint32_t count,
                  const struct bulk_shape *shape)
{
    uint32_t region;
    if (count == 0 || count > BULK_CONNECTIONS || shape->reads + shape->fills == 0 ||
        shape->reads + shape->fills > BULK_BUFFERS || shape->size == 0 || shape->size % 8u != 0 ||
        shape->size >= BULK_SIZE_MAX) {
        s->what = "connections and buffers of a shape a lender can lend";
        return KERR_INVALID_ARG;
    }
    memset(b, 0, sizeof(*b));
    b->lender = lender;
    b->ends = ends;
    b->count = count;
    b->shape = *shape;
    uint32_t slots = ring_slots(shape), ring = ring_bytes(slots, REF_BYTES);
    b->both_size = at_least(2u * ring + WANT_BYTES + shape->fills * shape->size);
    b->read_size = shape->reads != 0 ? at_least(shape->reads * shape->size) : 0;
    /* The larger blocks first, so that the smaller begin on a multiple of their size too. */
    if (b->read_size >= b->both_size) {
        b->both_at = count * b->read_size;
    } else {
        b->read_at = count * b->both_size;
    }
    PASS(mem_frame(s, at_least(count * (b->read_size + b->both_size)), &b->frame));
    PASS(child_map(s, lender, b->frame.made, RIGHT_R | RIGHT_W, &region));
    for (uint32_t i = 0; i < count; i++) {
        struct bulk_end *e = &ends[i];
        uint32_t both = both_base(b, i);
        memset(e, 0, sizeof(*e));
        PASS(child_bit(s, lender, &e->refs.bit));
        PASS(child_slot(s, lender, &e->refs.peer));
        e->refs.tx = ring_shape(both, ring, REF_BYTES);
        e->refs.rx = ring_shape(both + ring, ring, REF_BYTES);
        e->lender = 1;
        e->reads = shape->reads;
        e->fills = shape->fills;
        e->size = shape->size;
        e->read_base = read_base(b, i);
        e->wants = both + 2u * ring;
        e->fill_base = e->wants + WANT_BYTES;
    }
    return KERR_OK;
}

int bulk_idle(const struct bulk *b, uint32_t i)
{
    const struct chan_end *e = &b->ends[i].refs;
    uint32_t gen = __atomic_load_n(&e->gen, __ATOMIC_RELAXED);
    return !(gen & 1u) && __atomic_load_n(&e->seen, __ATOMIC_ACQUIRE) == gen;
}

/*
 * Bytes of the frame carved, zeroed, so that its rings are empty, and installed in the borrower,
 * with rights, in a region *region says; the carve deleted then, so that the install hangs below the frame.
 */
static uint32_t carve_clear(struct self *s, const struct bulk *b, uint32_t at, uint32_t size, struct child *borrower,
                            uint32_t rights, uint32_t *region)
{
    uint32_t carve;
    PASS(slot_new(s, &carve));
    uint32_t status = rv_frame_carve(b->frame.made, at, size, carve);
    if (status != KERR_OK) {
        s->what = "carve a connection";
        slot_back(s, carve);
        return status;
    }
    if ((status = frame_zero(s, carve, b->frame.base + at, size)) == KERR_OK) {
        status = child_map(s, borrower, carve, rights, region);
    }
    const char *what = s->what;
    PASS(slot_free(s, carve));
    s->what = what;
    return status;
}

/* Written last, once the rest of the end is, and odd: connected. */
static void open_end(struct bulk_end *e)
{
    __atomic_store_n(&e->refs.gen, e->refs.gen | 1u, __ATOMIC_RELEASE);
}

/* The parts of an end its side keeps, as a connection begins: nothing held or put, and every buffer to fill lent. */
static void fresh(struct bulk_end *e)
{
    memset(e->held, 0, sizeof(e->held));
    memset(e->away, 0, sizeof(e->away));
    memset(e->spare, 0, sizeof(e->spare));
    for (uint32_t buf = e->reads; buf < buffers(e); buf++) {
        mark(e->lender ? e->away : e->spare, buf);
    }
    e->sent = 0;
    e->woke = 0;
}

uint32_t bulk_connect(struct self *s, struct bulk *b, uint32_t i, struct child *borrower, struct bulk_end *be)
{
    if (i >= b->count || !bulk_idle(b, i) || (be->refs.gen & 1u)) {
        s->what = "an idle connection of a lender, and a borrower's end not connected";
        return KERR_STATE;
    }
    struct bulk_end *le = &b->ends[i];
    uint32_t *regions = b->regions[i];
    b->borrowers[i] = borrower;
    fresh(le);
    /* Nothing of the last borrower's for the next to read: a zeroed block is empty rings and buffers. */
    PASS(carve_clear(s, b, b->both_at + i * b->both_size, b->both_size, borrower, RIGHT_R | RIGHT_W, &regions[0]));
    if (b->read_size != 0) {
        PASS(carve_clear(s, b, b->read_at + i * b->read_size, b->read_size, borrower, RIGHT_R, &regions[1]));
    }
    PASS(child_bit(s, borrower, &be->refs.bit));
    PASS(child_give_bits(s, borrower, b->lender->inbox, le->refs.bit, &be->refs.peer));
    PASS(child_put_bits(s, b->lender, le->refs.peer, borrower->inbox, be->refs.bit));
    be->refs.tx = le->refs.rx;
    be->refs.rx = le->refs.tx;
    be->lender = 0;
    be->reads = le->reads;
    be->fills = le->fills;
    be->size = le->size;
    be->read_base = le->read_base;
    be->fill_base = le->fill_base;
    be->wants = le->wants;
    fresh(be);
    be->refused = 0;
    open_end(be);
    open_end(le);
    TRY(s, "tell a borrower of its connection", child_tell(borrower));
    TRY(s, "tell a lender of a borrower", child_tell(b->lender));
    return KERR_OK;
}

uint32_t bulk_close(struct self *s, struct bulk *b, uint32_t i)
{
    if (i >= b->count || !(b->ends[i].refs.gen & 1u)) {
        s->what = "a connected connection of a lender";
        return KERR_STATE;
    }
    struct bulk_end *le = &b->ends[i];
    /* The blocks out of the borrower, unless it was taken down already, which took its regions with it. */
    for (uint32_t k = 0; k < 2u; k++) {
        if (b->regions[i][k] != 0 && (b->borrowers[i]->regions & (1u << b->regions[i][k]))) {
            PASS(child_unmap(s, b->borrowers[i], b->regions[i][k]));
        }
        b->regions[i][k] = 0;
    }
    /* The borrower's inbox out of the lender's table, which a borrower taken down took with it already. */
    uint32_t status = rv_cap_delete(b->lender->table, le->refs.peer);
    if (status != KERR_OK && status != KERR_INVALID_CAP) {
        s->what = "take a borrower's inbox from its lender";
        return status;
    }
    __atomic_store_n(&le->refs.gen, le->refs.gen + 1u, __ATOMIC_RELEASE);
    TRY(s, "tell a lender a borrower is gone", child_tell(b->lender));
    return KERR_OK;
}

uint32_t bulk_free(struct self *s, struct bulk *b)
{
    return mem_give(s, &b->frame);
}
