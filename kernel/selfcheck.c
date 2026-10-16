/*
 * Kernel invariants, executable form.
 * The list mirrors DESIGN.md, "Properties"; keep the two in step.
 *
 * The checker trusts nothing the kernel computed except pool_list
 * and the boot grant table:
 * it walks pools and objects itself,
 * recomputes what PMP would grant from the region slots,
 * and compares against the image the kernel built
 * and the CSR values read back from the hardware.
 *
 * It runs after every system call in the host fuzzer
 * and, once OP_TEST_TRACE has been invoked, on the target as well.
 * It is freestanding so that the same file serves both.
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "pmp.h"
#include "sched.h"
#include "timer.h"

static __attribute__((noreturn)) void fail(const char *msg, uint32_t a, uint32_t b, uint32_t c)
{
    kputs("invariant violated: ");
    (kputs)(msg);
    kputc(' ');
    kput_hex(a);
    kputc(' ');
    kput_hex(b);
    kputc(' ');
    kput_hex(c);
    kputc('\n');
    selfcheck_fail();
}

/*
 * Rights the root task received at boot for a range,
 * or zero if the range is not within a single granted range.
 * No operation joins regions,
 * so every region a process can name lies within one of them.
 */
static uint8_t granted_rights(uint32_t base, uint32_t size)
{
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        const struct granted_range *g = &boot_granted[i];
        uint32_t off = base - g->base;
        if (g->size != 0 && base >= g->base && off < g->size && size <= g->size - off) {
            return g->rights;
        }
    }
    return 0;
}

static uint32_t aligned_size(const struct obj_header *o)
{
    return ((uint32_t)obj_size(o) + OBJ_ALIGN - 1) & ~(uint32_t)(OBJ_ALIGN - 1);
}

/* The walks of object.h, here so that the host fuzzer's coverage leaves them out with the self-check. */
struct obj_header *pool_first(struct pool *pool)
{
    return pool_next(pool, &pool->hdr);
}

struct obj_header *pool_next(struct pool *pool, struct obj_header *obj)
{
    paddr_t next = v2p(obj) + aligned_size(obj);
    if (next >= pool_base(pool) + pool->used) {
        return NULL;
    }
    return p2v(next);
}

struct obj_header *object_first(void)
{
    return pool_list != NULL ? &pool_list->hdr : NULL;
}

struct obj_header *object_next(struct obj_header *obj)
{
    struct pool *pool = obj_pool(obj);
    struct obj_header *next = pool_next(pool, obj);
    if (next != NULL) {
        return next;
    }
    struct pool *np = pool_next_pool(pool);
    return np != NULL ? &np->hdr : NULL;
}

struct obj_header *object_find(paddr_t p)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (v2p(o) == p) {
            return o;
        }
    }
    return NULL;
}

static void check_pools(void)
{
    /* The kernel's memory lies below its log, and nothing granted may reach it. */
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        const struct granted_range *g = &boot_granted[i];
        if (g->size && ranges_overlap(g->base, g->size, RAM_BASE, KLOG_BASE - RAM_BASE)) {
            fail("granted memory overlaps the kernel's", g->base, g->size, 0);
        }
    }

    paddr_t before = 0;
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        paddr_t base = v2p(p);
        if (p->hdr.type != CAP_POOL || p->hdr.pool != base || p->hdr.back != 0) {
            fail("pool descriptor is malformed", base, 0, 0);
        }
        if (p->prev != before) {
            fail("pool list is not linked back", base, p->prev, before);
        }
        before = base;
        /* The pool's own node, a capability to it that names the pool as its holder; the tree check reads its links. */
        if (p->node.type != CAP_POOL || p->node.a != base || p->node.b != base) {
            fail("pool's own node is malformed", base, p->node.type, p->node.a);
        }
        if (p->used < sizeof(*p) || p->used > p->size) {
            fail("pool used mark outside the pool", base, p->used, p->size);
        }
        if ((base | p->size) & (OBJ_ALIGN - 1)) {
            fail("pool is not aligned", base, p->size, 0);
        }
        /* The pool's own node is an original, so that every capability to the pool and its objects lies below it. */
        if (!p->node.orig) {
            fail("pool's own node is no original", base, 0, 0);
        }
        if (p->dying > 1 || (!p->dying && p->sweep != 0)) {
            fail("pool's destroy state is malformed", base, p->dying, p->sweep);
        }
        if (granted_rights(base, p->size) != RIGHT_ALL) {
            fail("pool lies outside memory granted with full rights", base, p->size, 0);
        }
        for (struct pool *q = pool_next_pool(p); q != NULL; q = pool_next_pool(q)) {
            if (ranges_overlap(base, p->size, pool_base(q), q->size)) {
                fail("pools overlap", base, pool_base(q), 0);
            }
        }

        /*
         * Objects tile the pool from the descriptor to the used mark,
         * each saying how far back the one before it lies, and the newest is the last.
         * This is the one walk that does not use object_first/object_next:
         * those cross pools by an object's own header,
         * which is what this loop is here to check.
         * The walk steps by obj_size and the allocator by obj_type_size, which OP_POOL_INFO tells,
         * so this holds the two to each other; what a describing call returns, and that it changes nothing,
         * relates a call to the state before it, which only host/history.c sees.
         */
        paddr_t at = base + aligned_size(&p->hdr);
        paddr_t prev = base;
        for (struct obj_header *o = pool_first(p); o != NULL; o = pool_next(p, o)) {
            if (v2p(o) != at) {
                fail("object walk skipped", base, at, v2p(o));
            }
            if (obj_pool(o) != p) {
                fail("object claims another pool", at, o->pool, base);
            }
            if ((uint32_t)o->back * OBJ_ALIGN != at - prev) {
                fail("object does not know where the one before it lies", at, o->back, prev);
            }
            if (o->type != CAP_CAPTABLE && o->type != CAP_PROCESS &&
                o->type != CAP_THREAD && o->type != CAP_NOTIFICATION &&
                o->type != CAP_IRQ) {
                fail("object has an unexpected type", at, o->type, 0);
            }
            prev = at;
            at += aligned_size(o);
            if (at > base + p->used) {
                fail("object runs past the used mark", v2p(o), at, base + p->used);
            }
        }
        if (at != base + p->used) {
            fail("objects do not end at the used mark", base, at, base + p->used);
        }
        if (p->last != prev) {
            fail("pool's newest object is not its last", base, p->last, prev);
        }
    }
}

/* Rights PMP grants at addr under a NAPOT image: the lowest matching entry wins. */
static uint8_t image_rights(const uint32_t *addr, const uint8_t *cfg, unsigned count, uint64_t at)
{
    for (unsigned i = 0; i < count; i++) {
        uint64_t base, size;
        pmp_napot_range(addr[i], &base, &size);
        if ((cfg[i] & 0x18) == PMP_A_NAPOT && at >= base && at - base < size) {
            return cfg[i] & (PMP_R | PMP_W | PMP_X);
        }
    }
    return 0;
}

static uint8_t slot_rights(const struct process *proc, uint64_t at)
{
    for (unsigned i = 0; i < proc->regions; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE && at >= block_base(s) && at < (uint64_t)block_base(s) + block_size(s)) {
            return rights_to_pmp(s->rights);
        }
    }
    return 0;
}

/* The slots and the image grant the same rights at an address, or the check fails. */
static void check_rights_at(const struct process *proc, const uint32_t *addr, const uint8_t *cfg, unsigned count,
                            bool csrs, uint64_t at)
{
    uint8_t want = slot_rights(proc, at);
    uint8_t got = image_rights(addr, cfg, count, at);
    if (want != got) {
        fail(csrs ? "PMP CSRs grant rights the slots do not" : "PMP image grants rights the slots do not",
             (uint32_t)at, got, want);
    }
}

/*
 * The PMP image and the region slots describe the same function
 * from address to rights.
 * Both are constant from each breakpoint, a region's or an entry's start or end, up to the next of either,
 * so comparing at zero and at every breakpoint is exact.
 * `csrs` says whether the image was read back from the CSRs,
 * for the report only.
 */
static void check_pmp_image(const struct process *proc, const uint32_t *addr,
                            const uint8_t *cfg, unsigned count, bool csrs)
{
    for (unsigned i = 0; i < count; i++) {
        uint8_t mode = cfg[i] & 0x18;
        if (mode != PMP_A_OFF && mode != PMP_A_NAPOT) {
            fail("PMP entry uses a mode other than OFF and NAPOT", i, mode, csrs);
        }
        if ((cfg[i] & PMP_W) && !(cfg[i] & PMP_R)) {
            fail("PMP entry uses the reserved encoding R=0 W=1", i, cfg[i], csrs);
        }
    }

    check_rights_at(proc, addr, cfg, count, csrs, 0);
    for (unsigned i = 0; i < proc->regions; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE) {
            check_rights_at(proc, addr, cfg, count, csrs, block_base(s));
            check_rights_at(proc, addr, cfg, count, csrs, (uint64_t)block_base(s) + block_size(s));
        }
    }
    for (unsigned i = 0; i < count; i++) {
        uint64_t base, size;
        pmp_napot_range(addr[i], &base, &size);
        check_rights_at(proc, addr, cfg, count, csrs, base);
        check_rights_at(proc, addr, cfg, count, csrs, base + size);
    }
}

static void check_process(const struct process *proc)
{
    /* As many region slots as its creator asked for, and no more than entries, so an install never runs out of them. */
    if (proc->regions == 0 || proc->regions > pmp_entry_count) {
        fail("process has no region slot, or more than the hardware has entries", v2p(proc), proc->regions,
             pmp_entry_count);
        return;
    }
    for (unsigned i = 0; i < proc->regions; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type == CAP_NONE) {
            continue;
        }
        /*
         * A region slot holds an installed region and nothing else, no original, a frame that names the process;
         * the tree check reads its links.
         */
        if (s->type != CAP_FRAME || s->rights == 0 || (s->rights & ~RIGHT_ALL) || s->orig) {
            fail("region slot holds something other than an installed region", v2p(proc), i, s->type);
        }
        if (node_holder(s) != v2p(proc)) {
            fail("installed region does not name its process", v2p(proc), i, s->b);
        }
        uint32_t base = block_base(s), size = block_size(s);
        uint8_t granted = granted_rights(base, size);
        if (size < region_min_size() || granted == 0) {
            fail("process maps memory outside what was granted", v2p(proc), base, size);
        }
        if (s->rights & ~granted) {
            fail("process maps memory with rights beyond the grant", v2p(proc), base, s->rights);
        }
        for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
            if (ranges_overlap(base, size, pool_base(p), p->size)) {
                fail("process maps a pool: isolation broken", v2p(proc), base, pool_base(p));
            }
        }
        for (unsigned j = i + 1; j < proc->regions; j++) {
            const struct cap *t = &proc->slots[j];
            if (t->type != CAP_NONE && ranges_overlap(base, size, block_base(t), block_size(t))) {
                fail("process has overlapping region slots", v2p(proc), i, j);
            }
#if PMP_SPLIT_STORE_AS_READ
            /* See PMP_SPLIT_STORE_AS_READ in board.h. */
            if (t->type != CAP_NONE) {
                const struct cap *lower = base < block_base(t) ? s : t, *upper = base < block_base(t) ? t : s;
                if ((uint64_t)block_base(lower) + block_size(lower) == block_base(upper) && (lower->rights & RIGHT_W) &&
                    (upper->rights & RIGHT_R) && !(upper->rights & RIGHT_W)) {
                    fail("process may write right below a region it may only read", v2p(proc), i, j);
                }
            }
#endif
        }
    }

    if (proc->pmp_count > proc->regions) {
        fail("PMP image has more entries than the process has region slots", v2p(proc), proc->pmp_count,
             proc->regions);
    }
    check_pmp_image(proc, process_pmp_addr(proc), process_pmp_cfg(proc), proc->pmp_count, false);

    /*
     * The sweep clears a table slot with the table, in whatever pool; the tree check reads its links.
     * Only a capability that may write a table makes it a process's, by an allocation or a mount.
     */
    for (unsigned i = 0; i < PROCESS_TABLES; i++) {
        const struct cap *tc = &proc->tables[i];
        if (tc->type != CAP_NONE) {
            struct obj_header *t = object_find(tc->a);
            if (tc->type != CAP_CAPTABLE || t == NULL || t->type != CAP_CAPTABLE) {
                fail("process's table slot names no live table", v2p(proc), i, tc->a);
            }
            if (tc->orig) {
                fail("process's table slot is an original", v2p(proc), i, tc->a);
            }
            if (!(tc->rights & RIGHT_W)) {
                fail("process's table slot without the right to write the table", v2p(proc), i, tc->rights);
            }
        }
    }
}

/*
 * Waiting threads and places that belong on a queue, and threads and places found on one; the two counts must agree.
 */
static uint32_t owed_threads, queued_threads;

/* The units the places the walk met are bound to. */
static uint32_t units_seen;

/* The threads found on their borrowers' rings of lenders, and the threads found lending; the two counts must agree. */
static uint32_t lends_seen, lends_named;

/* Whether it is the thread's turn, or it pays for the turn, either of which it has only on the core it names. */
static bool is_turn(const struct thread *t)
{
    return t->core < CORES && (cores[t->core].turn == t || cores[t->core].payer == t);
}

/*
 * The count a place's account is counted to:
 * the count of the core whose turn its thread has or pays for while it holds that core's time,
 * which lags the machine's while that core has not trapped since another counted, and the machine's otherwise.
 */
static uint32_t count_of(const struct place *p)
{
    const struct thread *t = place_thread(p);
    const struct cap *b = place_time(p);
    return is_turn(t) && b->type == CAP_TIME && unit_core(range_first(b)) == t->core ? cores[t->core].ticks : sched_ticks;
}

/* A place's account at the count, its stamp's balance and its tick's gain every tick since, held to the cap. */
static uint32_t account_now(const struct place *p)
{
    uint64_t balance = p->balance + (uint64_t)(count_of(p) - p->stamp) * tick_gain(p);
    uint32_t cap = account_cap(p);
    return balance > cap ? cap : (uint32_t)balance;
}

/*
 * The place a borrower's place runs on the time of while it has none:
 * that of the thread that has lent it time longest, if that one holds the time of the place's core.
 * check_lenders has vetted the ring.
 */
static const struct place *lender_of(const struct place *p)
{
    const struct thread *t = place_thread(p);
    const struct cap *b = place_time(p);
    if (t->lenders == 0 || b->type != CAP_TIME) {
        return NULL;
    }
    return thread_place_on(p2v(t->lenders), unit_core(range_first(b)));
}

/*
 * The queue a place belongs on: none unless its thread is ready, the place is bound, and its thread's turn is nowhere;
 * the run queue with a tick in its account or its lender's, else the spare queue with spare time,
 * else the spent queue with units of its own or its lender's, and none without.
 */
static uint8_t queue_owed(const struct place *p)
{
    const struct thread *t = place_thread(p);
    if (t->state != THREAD_READY || is_turn(t) || place_time(p)->type != CAP_TIME) {
        return QUEUE_NONE;
    }
    const struct place *l = lender_of(p);
    if (account_now(p) >= tick_parts() || (l != NULL && account_now(l) >= tick_parts())) {
        return QUEUE_RUN;
    }
    if (place_spare(p)) {
        return QUEUE_SPARE;
    }
    return place_units(p) != 0 || (l != NULL && place_units(l) != 0) ? QUEUE_SPENT : QUEUE_NONE;
}

/*
 * The threads that lend a thread their time are a ring of live threads that name it, each linked back to the one before.
 * The back links make the walk close at the first or fail, as a queue's do.
 */
static void check_lenders(const struct thread *t)
{
    if (t->lenders == 0) {
        return;
    }
    paddr_t at = t->lenders;
    do {
        struct obj_header *o = object_find(at);
        if (o == NULL || o->type != CAP_THREAD) {
            fail("a thread's lender is not a live thread", v2p(t), at, 0);
        }
        const struct thread *l = (const struct thread *)o;
        if (l->lend_to != v2p(t)) {
            fail("a thread's lender lends to another", v2p(t), at, l->lend_to);
        }
        struct obj_header *next = object_find(l->lend_next);
        if (next == NULL || next->type != CAP_THREAD || ((const struct thread *)next)->lend_prev != at) {
            fail("a thread's ring of lenders is not linked back", v2p(t), at, l->lend_next);
        }
        lends_seen++;
        at = l->lend_next;
    } while (at != t->lenders);
}

/* An account that gives p time once it holds a tick, its own or its lender's, reaches one no earlier than the release. */
static void check_release(const struct place *p, const struct place *a, const char *what)
{
    if (a == NULL || place_units(a) == 0) {
        return;
    }
    uint32_t missing = tick_parts() - account_now(a);
    uint32_t at = sched_ticks + (missing + tick_gain(a) - 1) / tick_gain(a);
    uint32_t release = cores[p->core].nearest_release;
    if ((int32_t)(at - release) < 0) {
        fail(what, v2p(p), at, release);
    }
}

/*
 * A place knows its thread and which of its places it is, its units are a leaf of the tree, which the tree check reads,
 * naming the thread that holds them, bound through a capability with the right to bind, whose rights they carry,
 * units of one core, which it alone earns,
 * and its account holds at most its cap and was last counted no later than the count.
 * It waits on the queue of its core its account says, and on none while its thread's turn is anywhere.
 * The cores whose time it holds go into *held, which must not hold its core already.
 */
static void check_place(const struct thread *t, uint32_t i, uint32_t *held)
{
    const struct place *p = thread_place(t, i);
    const struct cap *b = &t->time[i];
    if (p->thread != v2p(t) || p->index != i) {
        fail("a place does not know its thread", v2p(t), i, p->thread);
    }
    uint32_t first = range_first(b), count = range_count(b);
    if (b->type != CAP_NONE &&
        (b->type != CAP_TIME || (b->rights & RIGHT_W) == 0 || (b->rights & ~(RIGHT_W | RIGHT_X)) != 0 ||
         first >= MACHINE_UNITS || count > MACHINE_UNITS - first || node_holder(b) != v2p(t))) {
        fail("a place's units are malformed", v2p(t), i, b->a);
    }
    if (b->type != CAP_NONE && count != 0 && unit_core(first) != unit_core(first + count - 1)) {
        fail("a place's units are of two cores", v2p(t), first, count);
    }
    if (b->type != CAP_NONE) {
        uint32_t core = 1u << unit_core(first);
        if (*held & core) {
            fail("two places of a thread hold the time of one core", v2p(t), i, first);
        }
        *held |= core;
    }
    /* Only an original has anything below it. */
    if (b->type != CAP_NONE && b->orig) {
        fail("a place's units are an original", v2p(t), i, first);
    }
    for (uint32_t u = 0; b->type != CAP_NONE && u < count; u++) {
        if (unit_place[first + u] != v2p(p)) {
            fail("a unit a place is bound to names another", v2p(t), first + u, unit_place[first + u]);
        }
    }
    units_seen += place_units(p);
    if (p->balance > account_cap(p)) {
        fail("a place's account holds more than its cap", v2p(t), p->balance, place_units(p));
    }
    if ((int32_t)(count_of(p) - p->stamp) < 0) {
        fail("a place's account is stamped ahead of the count", v2p(t), p->stamp, count_of(p));
    }
    if (p->core >= CORES) {
        fail("a place names a core there is not", v2p(t), i, p->core);
    }
    /* A turn on one core takes a thread off the queues of every other until it ends; see turn_take. */
    if (is_turn(t) && p->queue != QUEUE_NONE) {
        fail("a place of a thread whose turn it is waits on a queue", v2p(t), i, p->queue);
    }
    uint8_t owed = queue_owed(p);
    if (p->queue != owed) {
        fail(owed == QUEUE_NONE ? "a place names a queue it is not for"
             : p->queue == QUEUE_NONE ? "a place of a ready thread that may run waits on no queue"
                                      : "a place waits on another queue than its account says",
             v2p(t), p->queue, owed);
    }
    if (owed != QUEUE_NONE && p->core != unit_core(first)) {
        fail("a place waits on another core than its units are of", v2p(t), p->core, first);
    }
    if (owed != QUEUE_NONE) {
        owed_threads++;
    } else if (p->queue_next != 0 || p->queue_prev != 0) {
        fail("a place on no queue is linked into one", v2p(t), i, p->queue_next);
    }
    /* One that waits without time has it again at the release, by its own account or its lender's, which waits on no queue. */
    if (owed == QUEUE_SPARE || owed == QUEUE_SPENT) {
        check_release(p, p, "a place's account reaches a tick before the release");
        check_release(p, lender_of(p), "a lender's account reaches a tick before the release");
    }
}

static void check_thread(const struct thread *t)
{
    /*
     * The sweep clears a thread's process with the process, in whatever pool,
     * and a thread without one is stopped; the tree check reads the node's links.
     */
    const struct cap *h = &t->proc;
    if (h->type != CAP_NONE) {
        struct obj_header *p = object_find(h->a);
        if (h->type != CAP_PROCESS || p == NULL || p->type != CAP_PROCESS || node_holder(h) != v2p(t)) {
            fail("thread's process slot names no live process", v2p(t), h->type, h->a);
        }
        if (h->orig) {
            fail("a thread's process is an original", v2p(t), h->a, 0);
        }
    } else if (t->state != THREAD_STOPPED) {
        fail("thread without a process is not stopped", v2p(t), t->state, 0);
    }
    if (t->flags & ~(uint8_t)(THREAD_UNTRACED | THREAD_FAULTED)) {
        fail("thread has unknown flags", v2p(t), t->flags, 0);
    }
    /* What stopped a thread at a fault is told only while it stays stopped there; see OP_THREAD_FAULT. */
    if ((t->flags & THREAD_FAULTED) && t->state != THREAD_STOPPED) {
        fail("thread runs with a fault to tell", v2p(t), t->state, 0);
    }
    if (t->core >= CORES) {
        fail("thread names a core there is not", v2p(t), t->core, 0);
    }
    /* A place for each core it may hold the time of at once, one at least, and no two holding one core's. */
    if (t->places == 0 || t->places > CORES) {
        fail("thread has an impossible count of places", v2p(t), t->places, 0);
    }
    uint32_t held = 0;
    for (uint32_t i = 0; i < t->places; i++) {
        check_place(t, i, &held);
    }
    /*
     * The sweep clears a thread's watch with its notification, in whatever pool; the tree check reads the node's links.
     * It was set through a capability with the right to signal, whose rights it carries, and it signals some bit.
     */
    const struct cap *w = &t->watch;
    if (w->type != CAP_NONE) {
        struct obj_header *n = object_find(w->a);
        if (w->type != CAP_NOTIFICATION || n == NULL || n->type != CAP_NOTIFICATION || (w->rights & RIGHT_W) == 0 ||
            w->b == 0) {
            fail("thread's watch names no live notification", v2p(t), w->type, w->a);
        }
        if (w->orig) {
            fail("a thread's watch is an original", v2p(t), w->a, 0);
        }
    }
    check_lenders(t);
    /*
     * A thread lends only while it waits, to another live thread, on whose ring it lies,
     * since every thread on a ring names the ring's thread and the rings hold as many as lend.
     */
    if (t->lend_to != 0) {
        struct obj_header *borrower = object_find(t->lend_to);
        if (t->state != THREAD_WAITING) {
            fail("a thread that does not wait lends", v2p(t), t->state, 0);
        }
        if (borrower == NULL || borrower->type != CAP_THREAD || t->lend_to == v2p(t)) {
            fail("a thread lends to something that is not another live thread", v2p(t), t->lend_to, 0);
        }
        lends_named++;
    } else if (t->lend_next != 0 || t->lend_prev != 0) {
        fail("a thread that lends nothing is linked into a ring of lenders", v2p(t), t->lend_next, t->lend_prev);
    }
    switch (t->state) {
    case THREAD_STOPPED:
    case THREAD_READY:
        if (t->waiting_on != 0) {
            fail("runnable thread waits on something", v2p(t), t->waiting_on, t->state);
        }
        if (t->queue_next != 0 || t->queue_prev != 0) {
            fail("thread that waits on nothing is linked into a notification's waiters", v2p(t), t->queue_next, t->state);
        }
        break;
    case THREAD_WAITING: {
        struct obj_header *n = object_find(t->waiting_on);
        if (n == NULL || n->type != CAP_NOTIFICATION) {
            fail("thread waits on something that is not a notification",
                 v2p(t), t->waiting_on, 0);
        }
        owed_threads++;
        break;
    }
    default:
        fail("thread is in an unknown state", v2p(t), t->state, 0);
    }
}

/*
 * A notification's waiters are a ring of live threads, each linked back to the one before, each waiting on it,
 * none of them a core's turn, though one may pay for a turn it lent its time to.
 * The back links make the walk close at the oldest or fail:
 * no thread can be entered twice from two different predecessors.
 */
static void check_waiters(const struct notification *ntfn)
{
    paddr_t head = ntfn->waiters;
    if (head == 0) {
        return;
    }
    paddr_t at = head;
    do {
        struct obj_header *o = object_find(at);
        if (o == NULL || o->type != CAP_THREAD) {
            fail("queue holds something that is not a thread", head, at, 0);
        }
        const struct thread *t = (const struct thread *)o;
        if (t->state != THREAD_WAITING || t->waiting_on != v2p(ntfn) || (t->core < CORES && cores[t->core].turn == t)) {
            fail("queue holds a thread it is not for", head, at, t->state);
        }
        struct obj_header *next = object_find(t->queue_next);
        if (next == NULL || next->type != CAP_THREAD ||
            ((const struct thread *)next)->queue_prev != at) {
            fail("queue is not linked back", head, at, t->queue_next);
        }
        queued_threads++;
        at = t->queue_next;
    } while (at != head);
}

/* The place at an address, if it is one of a live thread's, else NULL; a walk, as object_find is. */
static const struct place *place_at(paddr_t at)
{
    for (struct obj_header *t = object_first(); t != NULL; t = object_next(t)) {
        if (t->type != CAP_THREAD) {
            continue;
        }
        const struct thread *th = (const struct thread *)t;
        for (uint32_t i = 0; i < th->places; i++) {
            if (v2p(thread_place(th, i)) == at) {
                return thread_place(th, i);
            }
        }
    }
    return NULL;
}

/*
 * A core's queue is a ring of places of live threads, each linked back to the one before,
 * each owed that queue of that core, and so none of its thread's turn.
 * The back links make the walk close at the oldest or fail.
 */
static void check_queue(paddr_t head, uint8_t queue, uint32_t core)
{
    if (head == 0) {
        return;
    }
    paddr_t at = head;
    do {
        const struct place *p = place_at(at);
        if (p == NULL) {
            fail("a core's queue holds something that is not a place of a live thread", head, at, 0);
        }
        if (p->queue != queue || queue_owed(p) != queue) {
            fail("queue holds a place it is not for", head, at, p->queue);
        }
        if (p->core != core) {
            fail("a core's queue holds a place of another core", head, at, p->core);
        }
        const struct place *next = place_at(p->queue_next);
        if (next == NULL || next->queue_prev != at) {
            fail("queue is not linked back", head, at, p->queue_next);
        }
        queued_threads++;
        at = p->queue_next;
    } while (at != head);
}

/*
 * Whether nothing could ever run again, which the kernel stops the machine for rather than let a core idle:
 * no core has a turn or a thread on its run or spare queue, nor untraced on its spent queue,
 * and no Irq is armed on a line, or tracing is on, where nobody is left to tick.
 */
static bool nothing_runs(void)
{
    for (uint32_t i = 0; i < CORES; i++) {
        const struct core *c = &cores[i];
        if (c->turn != NULL || c->run_queue != 0 || c->spare_queue != 0 || (!debug_trace && c->spent_queue != 0)) {
            return false;
        }
    }
    return debug_trace || armed_sources == 0;
}

/*
 * A core is told nothing more of a thread than other cores told it,
 * so it runs user mode only for a ready thread whose regions it holds.
 */
static void check_core(uint32_t i)
{
    const struct core *c = &cores[i];
    const struct thread *current = c->current;
    if (current != NULL) {
        struct obj_header *o = object_find(v2p(current));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("a core's thread is not a live object", i, v2p(current), 0);
        }
    }
    /* A core whose turn it is runs the thread; one that has no turn idles, and may yet hold the last it ran. */
    if (c->turn != NULL && (c->turn != current || current->core != i)) {
        fail("it is not the running thread's turn", i, v2p(c->turn), v2p(current));
    }
    if (c->turn == NULL && nothing_runs()) {
        fail("a core idles where nothing can run again", i, 0, 0);
    }
    /* The core counts up to the machine's count as its traps begin, and charges its turn to the tick it counted. */
    if ((int32_t)(sched_ticks - c->ticks) < 0 || (c == core_self() && c->ticks != sched_ticks)) {
        fail("a core counted other ticks than the machine", i, c->ticks, sched_ticks);
    }
    /* The turn's place on the core, while it has one, which a bind or a revoke during the turn may empty. */
    const struct place *own = c->turn != NULL ? thread_place_on(c->turn, i) : NULL;
    if (own != NULL && own->stamp != c->ticks) {
        fail("the ticks of a turn were not charged to its thread", v2p(c->turn), own->stamp, c->ticks);
    }
    /* A turn on lent time is paid by another live thread named to the core, whose place there is charged as the turn's would be. */
    const struct thread *payer = c->payer;
    if (payer != NULL) {
        struct obj_header *o = object_find(v2p(payer));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("a core's turn is paid by something that is not a live thread", i, v2p(payer), 0);
        }
        if (c->turn == NULL || payer == c->turn || payer->core != i) {
            fail("a turn's payer is not another thread of its core", i, v2p(payer), payer->core);
        }
        const struct place *paid = thread_place_on(payer, i);
        if (paid != NULL && paid->stamp != c->ticks) {
            fail("the ticks of a turn were not charged to its payer", v2p(payer), paid->stamp, c->ticks);
        }
        /*
         * It pays from the wait in which it lent the turn's thread its time, the first of the thread's lenders,
         * and once woken until the turn ends.
         */
        if (payer->state == THREAD_WAITING && (c->turn == NULL || c->turn->lenders != v2p(payer))) {
            fail("a turn's payer that waits is not the turn's first lender", i, v2p(payer), payer->lend_to);
        }
    }
    /*
     * A core whose turn's thread another core stopped drops the trap it takes next, whatever the thread is by then:
     * the stopping core marks it as it shoots it, and the mark stays until that trap takes the lock,
     * so neither a core in user mode nor the one that runs this check, which took the lock, holds one;
     * it goes with the turn, which a destroy may take first.
     * Under tracing a thread the host cannot follow never runs, so only a core so marked has its turn for one:
     * a watcher may write a stopped thread's registers before that core's trap, and resume it.
     */
    if (c->drop_trap && (c == core_self() || c->in_user)) {
        fail("a core is to drop a trap it is not taking", i, c->turn != NULL ? v2p(c->turn) : 0, c->in_user);
    }
    if (c != core_self() && c->turn != NULL && c->turn->state != THREAD_READY && !c->drop_trap) {
        fail("a core would take a trap as the thread another core stopped", i, v2p(c->turn), c->turn->state);
    }
    if (debug_trace && c->turn != NULL && (c->turn->flags & THREAD_UNTRACED) && !c->drop_trap) {
        fail("a traced core has its turn for a thread the host cannot follow", i, v2p(c->turn), c->turn->flags);
    }
    /*
     * A turn on spare time is one no thread with time wants, so it ends as the trap that makes one ready does,
     * unless its thread stands at a call stopped for an interrupt, which it makes again first;
     * on another core that runs user mode it ends as that core traps, which whoever made the thread ready owes it.
     */
    if (!c->recall && c->run_queue != 0 && sched_spare_turn(c) &&
        (c == core_self() || (c->in_user && !c->interrupt_owed))) {
        fail("a turn on spare time runs while a thread with time waits", i, v2p(c->turn), c->run_queue);
    }
    if (debug_trace && c->turn_from != 0) {
        fail("a traced turn is charged from within a tick", i, c->turn_from, 0);
    }
    if ((int32_t)(c->nearest_release - sched_ticks) <= 0) {
        fail("the release is for a tick already past", c->nearest_release, sched_ticks, i);
    }
    if ((int32_t)(c->nearest_deadline - sched_ticks) <= 0) {
        fail("the timer wakes for a deadline already past", c->nearest_deadline, sched_ticks, i);
    }
    if (c->in_user && (c->turn == NULL || current->state != THREAD_READY)) {
        fail("a core runs user mode for a thread that may not run", i, current != NULL ? v2p(current) : 0, 0);
    }
    if (c->in_user && c->pmp_stale) {
        fail("a core runs user mode with regions its process no longer has", i, v2p(current), 0);
    }
    check_queue(c->run_queue, QUEUE_RUN, i);
    check_queue(c->spare_queue, QUEUE_SPARE, i);
    check_queue(c->spent_queue, QUEUE_SPENT, i);
}

/*
 * An armed watchdog lies ahead of the count, since the tick that reaches it halts the machine,
 * and no further than the longest delay a feed sets;
 * the core that fed it wakes for it, as for a timer line it armed.
 */
static void check_watchdog(void)
{
    if (!watchdog.armed) {
        return;
    }
    uint32_t ahead = watchdog.deadline - sched_ticks;
    if ((int32_t)ahead <= 0) {
        fail("the machine runs past the watchdog's deadline", watchdog.deadline, sched_ticks, 0);
    }
    if (ahead > WATCHDOG_US_MAX / TIMER_US_PER_TICK + 1) {
        fail("the watchdog is set further than the longest delay", watchdog.deadline, sched_ticks, 0);
    }
    if (watchdog.core >= CORES) {
        fail("the watchdog names a core there is not", watchdog.core, 0, 0);
    }
    uint32_t nearest = cores[watchdog.core].nearest_deadline;
    if ((int32_t)(watchdog.deadline - nearest) < 0) {
        fail("the watchdog comes due before the timer wakes for it", watchdog.deadline, nearest, watchdog.core);
    }
}

/*
 * Each unit names the live thread that earns it, or none, and check_thread has held every thread's units to the table,
 * so as many units named as units bound leaves no unit naming a thread that does not earn it.
 * Each core's queues hold the threads that name them and it, and check_thread counted those owed a place on one.
 * A core whose turn it is runs the thread, and its account is counted up to the core's count;
 * under tracing the clock is the tick count alone, so the turn owes nothing from before the tick.
 * The release and the nearest deadline lie ahead of the count.
 */
static void check_scheduler(void)
{
    /* Before the cores, so that a watchdog past its deadline is reported as itself and not as its core's deadline. */
    check_watchdog();
    for (uint32_t i = 0; i < CORES; i++) {
        check_core(i);
    }
    uint32_t named = 0;
    for (uint32_t i = 0; i < MACHINE_UNITS; i++) {
        if (unit_place[i] == 0) {
            continue;
        }
        if (place_at(unit_place[i]) == NULL) {
            fail("a unit names something that is not a place of a live thread", i, unit_place[i], 0);
        }
        named++;
    }
    if (named != units_seen) {
        fail("a unit names a place that does not earn it", named, units_seen, 0);
    }
}

/*
 * The timer interrupts only at a tick that could change what runs, and a trap counts the others.
 * The nearest deadline it wakes for is its core's, which check_core holds ahead of the count
 * and check_irq to every timer line armed there.
 * While a thread runs, every tick the timer lets pass would hand the processor back to that thread:
 * nobody is on the run queue, and it has time, or may run on spare time with nobody on the spare queue;
 * and none of them is the release, the nearest deadline,
 * or, while it could not go on alone on spare time, the tick that leaves its account less than a tick.
 * The tick the timer was last set for is one of those, a later one only after a change that marked it stale.
 * check_scheduler has made sure that it is the running thread's turn.
 * Only this core's timer is checked: another sets its own as its trap ends, told by whoever changed what it runs.
 */
static void check_wake(void)
{
    const struct core *core = core_self();
    if (core->turn == NULL) {
        return;
    }
    uint32_t wake = sched_wake_ticks();
    /* Unless a change since marked it stale, the tick the timer was set for still comes no later. */
    int32_t set = (int32_t)(core->wake_tick - sched_ticks);
    if (!core->wake_stale && (set < 1 || set > (int32_t)wake)) {
        fail("the timer is set past a tick an unmarked change brought forward", core->wake_tick, sched_ticks, wake);
    }
    if (wake <= 1) {
        if (wake == 0) {
            fail("the timer wakes for a tick already counted", 0, 0, 0);
        }
        return;
    }
    /* Each tick decides afresh who pays for a turn on lent time. */
    if (core->payer != NULL) {
        fail("the timer lets a tick pass while the turn runs on lent time", wake, v2p(core->turn), v2p(core->payer));
    }
    const struct thread *t = core->turn;
    /* A thread whose place here was emptied during its turn ends the turn at the next tick. */
    const struct place *p = thread_place_on(t, core_index());
    if (p == NULL) {
        fail("the timer lets the tick pass that ends a turn without the core's time", wake, v2p(t), 0);
    }
    uint32_t balance = account_now(p);
    bool time = balance >= tick_parts();
    bool alone = place_spare(p) && core->spare_queue == 0;
    if (core->run_queue != 0 || (!time && !alone)) {
        fail("the timer lets a tick pass that ends the turn", wake, v2p(t), balance);
    }
    /*
     * On the tick before the timer's the account still has a tick in it, unless it may go on alone on spare time:
     * the next tick it pays for the rest of this one, from turn_from, and gains its units, up to its cap,
     * and every tick after costs it a tick less the gain.
     */
    if (time && !alone) {
        uint32_t next = balance - COUNT_PARTS * (timer_tick_counts() - core->turn_from) + tick_gain(p);
        next = next < account_cap(p) ? next : account_cap(p);
        if (next < tick_parts() || (uint64_t)(wake - 2) * (tick_parts() - tick_gain(p)) > next - tick_parts()) {
            fail("the timer lets the tick pass that drains the turn's account", wake, balance, place_units(p));
        }
    }
    if (wake > core->nearest_release - sched_ticks) {
        fail("the timer lets the release pass", wake, core->nearest_release, sched_ticks);
    }
    if (wake > core->nearest_deadline - sched_ticks) {
        fail("the timer lets the nearest deadline pass", wake, core->nearest_deadline, sched_ticks);
    }
}

/* The Irqs the walk found armed on a device's line or a timer line, to hold armed_sources to. */
static uint32_t armed_seen;

static void check_irq(const struct irq *i)
{
    /*
     * The sweep clears an Irq's notification with the notification, in whatever pool,
     * and an Irq without one is disarmed; the tree check reads the node's links.
     * It was bound through a capability with the right to signal, whose rights and bits it carries,
     * and it is armed with no bit that capability may not signal.
     */
    const struct cap *s = &i->ntfn;
    if (s->type != CAP_NONE) {
        struct obj_header *n = object_find(s->a);
        if (!irq_held(s) || n == NULL || n->type != CAP_NOTIFICATION || (s->rights & RIGHT_W) == 0 || i->may == 0) {
            fail("irq's notification slot names no live notification", v2p(i), s->type, s->a);
        }
        if (s->orig) {
            fail("an irq's notification is an original", v2p(i), s->a, 0);
        }
        if (i->bits & ~i->may) {
            fail("irq is armed with bits its notification capability may not signal", v2p(i), i->bits, i->may);
        }
    } else if (irq_armed(i)) {
        fail("irq without a notification is armed", v2p(i), i->bits, 0);
    } else if (i->may != 0) {
        fail("irq without a notification may signal", v2p(i), i->may, 0);
    }
    if (i->line >= LINES) {
        fail("irq names a line there is not", v2p(i), i->line, 0);
    }
    if (line_is_cores(i->line)) {
        fail("irq is bound to the cores' line", v2p(i), i->line, 0);
    }
    /* The tick fires every due timer line, so one still armed lies ahead. */
    if (line_is_timer(i->line) && irq_armed(i) && irq_due(i, sched_ticks)) {
        fail("armed timer line is due", v2p(i), i->deadline, sched_ticks);
    }
    /* The core that armed it wakes for the nearest deadline armed there, which this one's comes no earlier than. */
    if (line_is_timer(i->line) && irq_armed(i)) {
        if (i->core >= CORES) {
            fail("armed timer line names a core there is not", v2p(i), i->core, 0);
        }
        uint32_t nearest = cores[i->core].nearest_deadline;
        if ((int32_t)(i->deadline - nearest) < 0) {
            fail("armed timer line comes due before the timer wakes for it", v2p(i), i->deadline, nearest);
        }
    }
    /*
     * The log's line is high while the reader has bytes to take,
     * and an Irq armed on a high line has been signalled, so it is not armed:
     * the same "armed and unmasked are one state" as the controller's lines,
     * with the log's head for the controller.
     */
    if (i->line == LOG_IRQ_LINE && irq_armed(i) && klog_pending()) {
        fail("armed log irq while the log has bytes untaken", v2p(i), i->bits, 0);
    }
    armed_seen += i->line != LOG_IRQ_LINE && irq_armed(i);
}

/*
 * The controller forwards a line exactly while an Irq is armed on it,
 * and at most one Irq is bound to any line, the log's and the timer lines included,
 * which the line table records exactly.
 * check_irq has vetted each object, so the bitmaps below are in range.
 * The enable bits are read back from the controller,
 * as the PMP CSRs are read back for the running process;
 * the log's line and the timer lines have no controller,
 * and check_irq reads the log's level and the timer lines' deadlines instead.
 */
static void check_lines(void)
{
    uint32_t bound[(LINES + 31) / 32] = { 0 };
    uint32_t armed[(LINES + 31) / 32] = { 0 };

    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_IRQ) {
            continue;
        }
        const struct irq *i = (const struct irq *)o;
        uint32_t word = i->line / 32;
        uint32_t bit = 1u << (i->line % 32);
        if (bound[word] & bit) {
            fail("two irqs are bound to one line", v2p(i), i->line, 0);
        }
        if (line_binding(i->line) != i) {
            fail("irq is missing from the line table", v2p(i), i->line, 0);
        }
        bound[word] |= bit;
        if (irq_armed(i)) {
            armed[word] |= bit;
        }
    }
    for (uint32_t line = 0; line < LINES; line++) {
        if (line_binding(line) != NULL && !((bound[line / 32] >> (line % 32)) & 1u)) {
            fail("line table names an irq that is gone", line, 0, 0);
        }
    }
    for (uint32_t line = 1; line < IRQ_LINES; line++) {
        bool want = (armed[line / 32] >> (line % 32)) & 1u;
        /* The cores' line is forwarded whatever is armed, and check_irq holds every Irq off it. */
        if (line_is_cores(line)) {
            continue;
        }
        if (irq_enabled(line) != want) {
            fail(want ? "armed irq's line is masked" : "controller forwards a line nothing is armed on",
                 line, 0, 0);
        }
    }
}

static void check_captable(const struct captable *table)
{
    if (table->nslots == 0 || table->nslots > CAPTABLE_MAX_SLOTS) {
        fail("table has an impossible slot count", v2p(table), table->nslots, 0);
    }
    for (uint32_t i = 0; i < table->nslots; i++) {
        const struct cap *c = &table->slots[i];
        if (c->type == CAP_NONE) {
            continue;
        }
        if (c->rights & ~RIGHT_ALL) {
            fail("capability has unknown rights bits", v2p(table), i, c->rights);
        }
        /*
         * A frame's block and a range of lines or units lie in the first word,
         * so that the second is free for the holder a process's frame or a thread's units name.
         */
        if ((c->type == CAP_FRAME || c->type == CAP_IRQ_LINE || c->type == CAP_TIME) && c->b != 0) {
            fail("a frame or a range in a table carries a second word", v2p(table), i, c->b);
        }
        switch (c->type) {
        case CAP_PRINT:
        case CAP_HALT:
        case CAP_TEST:
        case CAP_CLOCK:
            break;
        case CAP_FRAME: {
            /* The block word ends in a one for every block of at least eight bytes. */
            uint32_t base = block_base(c), size = block_size(c);
            uint8_t granted = (c->a & 1) ? granted_rights(base, size) : 0;
            if (granted == 0) {
                fail("frame outside granted memory", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("frame with rights beyond the grant", v2p(table), i, c->rights);
            }
            if (!napot_block(base, size)) {
                fail("frame is not a NAPOT block", v2p(table), i, c->a);
            }
            break;
        }
        case CAP_UNTYPED: {
            /* The block word ends in a one for every block of at least eight bytes. */
            uint32_t base = block_base(c), size = block_size(c);
            uint8_t granted = (c->a & 1) ? granted_rights(base, size) : 0;
            if (granted == 0 || !napot_block(base, size)) {
                fail("untyped outside granted memory, or not a block", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("untyped with rights beyond the grant", v2p(table), i, c->rights);
            }
            /* Every Untyped is an original, what it makes lying right after it, and carries nothing in b. */
            if (c->b != 0 || !c->orig) {
                fail("untyped with a second word, or no original", v2p(table), i, c->b);
            }
            break;
        }
        case CAP_IRQ_LINE:
            /* The root task received every line there is, with RIGHT_W; nothing widens that. */
            if (range_count(c) == 0 || range_first(c) >= LINES || range_count(c) > LINES - range_first(c)) {
                fail("line capability outside the lines there are", v2p(table), i, c->a);
            }
            if (c->rights & ~RIGHT_W) {
                fail("line capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        case CAP_TIME:
            /* The root task received every unit there is, with RIGHT_W and RIGHT_X; nothing widens that. */
            if (range_count(c) == 0 || range_first(c) >= MACHINE_UNITS || range_count(c) > MACHINE_UNITS - range_first(c)) {
                fail("time capability outside the units there are", v2p(table), i, c->a);
            }
            if (c->rights & ~(RIGHT_W | RIGHT_X)) {
                fail("time capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        case CAP_POOL:
        case CAP_CAPTABLE:
        case CAP_PROCESS:
        case CAP_THREAD:
        case CAP_NOTIFICATION:
        case CAP_IRQ: {
            /* A destroy sweeps the tables, so every object capability points at a live object. */
            struct obj_header *o = object_find(c->a);
            if (o == NULL || o->type != c->type) {
                fail("dangling object capability", v2p(table), i, c->a);
            }
            /*
             * A notification's capability may signal some bit, and no other carries any,
             * so none names a holder as a thread's process or a pool's own node does.
             */
            if ((c->b == 0) != (c->type != CAP_NOTIFICATION)) {
                fail("object capability whose bits are not its type's", v2p(table), i, c->b);
            }
            break;
        }
        default:
            fail("capability of unknown type", v2p(table), i, c->type);
        }
    }
}

/*
 * The derivation tree.
 *
 * Its nodes are the slots of every live table,
 * the table slots and region slots of every live process,
 * the process, units and watch of every live thread,
 * the notification of every live Irq and every pool's own node,
 * linked by their addresses and never checked on use,
 * so every link must land on a live node and the lists must be exactly lists:
 * each previous link a next link turned round, and every list ending.
 * The tree over a list is read off it as the kernel reads it, see node_below:
 * the nodes below an original are those right after it that it covers, and it covers none further on,
 * so a node's parent is the nearest original before it that covers it.
 * A destroy that left a link into freed memory, a delete that broke a list,
 * a revoke that stopped short, or a node put where it does not belong all show up here.
 * A node is derived from its parent: the same object with no more rights,
 * and for a notification no more bits to signal,
 * a range within the parent's with no more rights,
 * or an object built on the parent's range, a pool on a region, an Irq on a line,
 * a thread's process, a thread's watch or an Irq's notification on a capability to it,
 * a thread's units on the units it was bound through,
 * or the counter's block, or a region installed from it, below the clock.
 */

/* The node a link names, if one of a live object; only the pool the address lies in is looked at. */
static const struct cap *live_node(uint32_t link)
{
    paddr_t at = link * NODE_ALIGN;
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        if (!range_contains(pool_base(p), p->used, at)) {
            continue;
        }
        for (struct obj_header *o = &p->hdr; o != NULL; o = pool_next(p, o)) {
            uint32_t count;
            const struct cap *nodes = obj_nodes(o, &count);
            if (count == 0) {
                continue;
            }
            uint32_t off = at - v2p(nodes);
            if (off < count * sizeof(*nodes) && off % sizeof(*nodes) == 0) {
                return &nodes[off / sizeof(*nodes)];
            }
        }
        return NULL;
    }
    return NULL;
}

/* True if [base, base + size) lies within [in, in + insize). */
static bool range_within(uint32_t base, uint32_t size, uint32_t in, uint32_t insize)
{
    return base >= in && base - in <= insize - size && size <= insize;
}

/* The memory a node stands for, if it stands for memory: a frame's or an Untyped's block, or a pool's own node's pool. */
static bool node_range(const struct cap *n, uint32_t *base, uint32_t *size)
{
    switch (n->type) {
    case CAP_FRAME:
    case CAP_UNTYPED:
        *base = block_base(n);
        *size = block_size(n);
        return true;
    case CAP_POOL:
        *base = n->a;
        *size = ((const struct pool *)p2v(n->a))->size;
        return node_pool(n) != NULL;
    default:
        return false;
    }
}

/* True if a range of lines or units lies within another's: its first does, even for none, as a thread's units may. */
static bool range_in(const struct cap *c, const struct cap *p)
{
    uint32_t off = range_first(c) - range_first(p);
    return off < range_count(p) && range_count(c) <= range_count(p) - off;
}

/* A node naming an object built in a pool: a capability to one, or an object's hold on one. */
static bool is_object_type(uint8_t type)
{
    return type == CAP_CAPTABLE || type == CAP_PROCESS || type == CAP_THREAD ||
           type == CAP_NOTIFICATION || type == CAP_IRQ;
}

/* The bits a notification's node may signal: its own, or the Irq's that holds it. */
static uint32_t node_bits(const struct cap *n)
{
    return irq_held(n) ? signalled_irq(n)->may : n->b;
}

static bool derived_from(const struct cap *c, const struct cap *p)
{
    bool narrower = !(c->rights & ~p->rights);
    uint32_t base, size, pbase, psize;
    /*
     * What an Untyped made lies within it, and so does what came up to it
     * from something it made that went; a pool's own node carries rights of its own.
     */
    if (p->type == CAP_UNTYPED) {
        bool pool = node_pool(c) != NULL;
        if (c->type != CAP_UNTYPED && c->type != CAP_FRAME && !pool) {
            return false;
        }
        node_range(c, &base, &size);
        node_range(p, &pbase, &psize);
        return (narrower || pool) && range_within(base, size, pbase, psize);
    }
    /*
     * A node an object holds is of the type of the capability it hangs below:
     * a region of a frame's, a thread's units of a Time capability's, a thread's process of a Process capability's,
     * and a thread's watch and an Irq's notification of a Notification capability's.
     */
    if (c->type == p->type) {
        if (c->type == CAP_FRAME) {
            return narrower && range_within(block_base(c), block_size(c), block_base(p), block_size(p));
        }
        if (c->type == CAP_IRQ_LINE || c->type == CAP_TIME) {
            return narrower && range_in(c, p);
        }
        /* A notification's capability carries the bits it may signal, which narrow as rights do. */
        return narrower && (c->type == CAP_PRINT || c->type == CAP_HALT || c->type == CAP_TEST || c->type == CAP_CLOCK ||
                            c->a == p->a) &&
               (c->type != CAP_NOTIFICATION || !(node_bits(c) & ~p->b));
    }
    /*
     * A clock that reads gives out one frame, the counter's block, read only,
     * and adopts what was carved or installed from it when a delete or a revoke takes it.
     */
    if (c->type == CAP_FRAME && p->type == CAP_CLOCK) {
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        return (p->rights & RIGHT_R) && !(c->rights & ~g->rights) &&
               range_within(block_base(c), block_size(c), g->base, g->size);
    }
    /* Below a pool's node, and below its capability, lies every capability to the pool's objects; objects carry rights of their own. */
    if (p->type == CAP_POOL) {
        return is_object_type(c->type) && cap_object(c)->pool == p->a;
    }
    return false;
}

/* One node's links: each lands on a live, filled node that links back. */
static void check_links(const struct cap *n)
{
    if (n->type == CAP_NONE) {
        if (n->next != 0 || n->prev != 0 || n->orig) {
            fail("empty slot keeps links", v2p(n), n->next, n->prev);
        }
        return;
    }
    if (n->next != 0) {
        const struct cap *s = live_node(n->next);
        if (s == NULL || s->type == CAP_NONE || s->prev != node_link(n)) {
            fail("next link to a slot that is not a live, filled one linking back", v2p(n), n->next, 0);
        }
    }
    if (n->prev != 0) {
        const struct cap *s = live_node(n->prev);
        if (s == NULL || s->type == CAP_NONE || s->next != node_link(n)) {
            fail("previous link to a slot that is not a live, filled one linking back", v2p(n), n->prev, 0);
        }
    }
}

/* Every node of the forest in turn, filled or not: a table's slots, a process's, a pool's own. */
struct node_iter {
    struct obj_header *o;
    const struct cap *nodes;
    uint32_t count, i;
};

/* The walk at the first node of o, or at the end for NULL. */
static void node_enter(struct node_iter *it, struct obj_header *o)
{
    it->o = o;
    it->nodes = NULL;
    it->count = 0;
    it->i = 0;
    if (o != NULL) {
        it->nodes = obj_nodes(o, &it->count);
    }
}

static const struct cap *node_next(struct node_iter *it)
{
    while (it->o != NULL) {
        if (it->i < it->count) {
            return &it->nodes[it->i++];
        }
        node_enter(it, object_next(it->o));
    }
    return NULL;
}

/* The nodes there are, and one more; a walk along a list that long has gone round a cycle. */
static uint32_t tree_bound;

/* The node after n in its list, or NULL at its end; check_links has vetted the links. */
static const struct cap *list_next(const struct cap *n)
{
    return n->next != 0 ? live_node(n->next) : NULL;
}

static const struct cap *list_prev(const struct cap *n)
{
    return n->prev != 0 ? live_node(n->prev) : NULL;
}

/* The node n lies right below: the nearest original before it that covers it, or NULL for a root. */
static const struct cap *node_parent(const struct cap *n)
{
    uint32_t steps = 0;
    for (const struct cap *p = list_prev(n); p != NULL; p = list_prev(p)) {
        if (++steps > tree_bound) {
            fail("a list does not begin", v2p(n), 0, 0);
        }
        if (node_below(p, n)) {
            return p;
        }
    }
    return NULL;
}

/* True if n lies below ancestor: after it in its list and covered by it, so within what lies right after it. */
static bool lies_below(const struct cap *n, const struct cap *ancestor)
{
    if (!node_below(ancestor, n)) {
        return false;
    }
    uint32_t steps = 0;
    for (const struct cap *p = list_prev(n); p != NULL; p = list_prev(p)) {
        if (++steps > tree_bound) {
            fail("a list does not begin", v2p(n), 0, 0);
        }
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

static void check_tree(void)
{
    tree_bound = 1;
    uint32_t filled = 0;
    struct node_iter it;
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        tree_bound++;
        filled += n->type != CAP_NONE;
    }
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        check_links(n);
    }
    /* Lists: from each first node the links lead to an end, and every filled node lies on one. */
    uint32_t listed = 0;
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        if (n->type == CAP_NONE || n->prev != 0) {
            continue;
        }
        for (const struct cap *x = n; x != NULL; x = list_next(x)) {
            if (++listed > filled) {
                fail("a list does not end", v2p(n), 0, 0);
            }
        }
    }
    if (listed != filled) {
        fail("a filled node lies on no list", listed, filled, 0);
    }
    /*
     * Below an original lies what comes right after it and it covers, and it covers nothing further on:
     * a node it covers past the first it does not would stay when a revoke below it takes the rest,
     * and would come to lie below it when a delete takes what lies between.
     * So a delete hands what lay below the node it takes to the node's parent and to nothing else,
     * the nearest original before it that covers it.
     */
    node_enter(&it, object_first());
    for (const struct cap *p = node_next(&it); p != NULL; p = node_next(&it)) {
        if (p->type == CAP_NONE || !p->orig) {
            continue;
        }
        bool within = true;
        for (const struct cap *x = list_next(p); x != NULL; x = list_next(x)) {
            if (node_below(p, x)) {
                if (!within) {
                    fail("a node an original covers lies past what lies below it", v2p(p), v2p(x), 0);
                }
            } else {
                within = false;
            }
        }
    }
    /*
     * Every node is derived from its parent, and a capability to a pool or an object is never a root,
     * as a pool's own node may be once the Untyped above it went.
     */
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        if (n->type == CAP_NONE) {
            continue;
        }
        const struct cap *parent = node_parent(n);
        if (parent == NULL) {
            if ((n->type == CAP_POOL && node_pool(n) == NULL) || is_object_type(n->type)) {
                fail("capability to an object is a root", v2p(n), n->type, n->a);
            }
        } else if (!derived_from(n, parent)) {
            fail("slot is not derived from its parent", v2p(n), v2p(parent), 0);
        }
    }
}

/*
 * Memory that may become kernel memory, an Untyped or a pool,
 * overlaps another node standing for memory only where one lies below the other.
 * So a frame, which never lies above or below a pool, overlaps none,
 * and neither does anything two siblings below one Untyped stand for:
 * an Untyped makes one thing of the whole of its memory, or two halves, and nothing while they are there.
 */
static void check_nesting(void)
{
    struct node_iter it, jt;
    node_enter(&it, object_first());
    for (const struct cap *x = node_next(&it); x != NULL; x = node_next(&it)) {
        uint32_t xbase, xsize;
        /* An Untyped, or a pool's own node, for which alone of the Pool capabilities node_range answers. */
        if ((x->type != CAP_UNTYPED && x->type != CAP_POOL) || !node_range(x, &xbase, &xsize)) {
            continue;
        }
        node_enter(&jt, object_first());
        for (const struct cap *y = node_next(&jt); y != NULL; y = node_next(&jt)) {
            uint32_t ybase, ysize;
            if (y == x || !node_range(y, &ybase, &ysize) || !ranges_overlap(xbase, xsize, ybase, ysize)) {
                continue;
            }
            if (!lies_below(x, y) && !lies_below(y, x)) {
                fail("memory that may hold kernel objects overlaps a node not above or below it",
                     v2p(x), v2p(y), ybase);
            }
        }
    }
}

void selfcheck_run(void)
{
    if (pmp_grain < 4 || (pmp_grain & (pmp_grain - 1)) != 0) {
        fail("PMP grain is not a power of two of at least four bytes", pmp_grain, 0, 0);
    }

    check_pools();

    /* check_pools() ran first, so the flat walk rests on checked headers. */
    owed_threads = queued_threads = 0;
    units_seen = 0;
    lends_seen = lends_named = 0;
    armed_seen = 0;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        switch (o->type) {
        case CAP_PROCESS:
            check_process((struct process *)o);
            break;
        case CAP_THREAD:
            check_thread((struct thread *)o);
            break;
        case CAP_CAPTABLE:
            check_captable((struct captable *)o);
            break;
        case CAP_NOTIFICATION:
            check_waiters((struct notification *)o);
            break;
        case CAP_IRQ:
            check_irq((struct irq *)o);
            break;
        default:
            break;
        }
    }
    check_scheduler();
    check_wake();
    /* Every queued thread is one its queue is for, so equal counts leave none out. */
    if (owed_threads != queued_threads) {
        fail("a waiting or ready thread is on no queue", owed_threads, queued_threads, 0);
    }
    /* Every thread on a ring names the thread whose ring it is, so equal counts leave no lender off its borrower's. */
    if (lends_seen != lends_named) {
        fail("a lender is missing from its borrower's ring", lends_seen, lends_named, 0);
    }
    /* The stall in wfi reads the count instead of walking for a source. */
    if (armed_seen != armed_sources) {
        fail("the count of armed sources is wrong", armed_seen, armed_sources, 0);
    }
    check_lines();
    /* Every node has been vetted as a capability by now; the tree check reads only links. */
    check_tree();
    check_nesting();

    const struct core *core = core_self();
    const struct thread *current = core->current;
    if (core->turn != NULL) {
        /* It may have lost its units during its turn, which it finishes. */
        if (current->state != THREAD_READY) {
            fail("the running thread is not runnable", v2p(current), current->state, 0);
        }
        if (core->pmp_stale) {
            fail("the core's regions wait to be loaded again", v2p(current), 0, 0);
        }
        /* The CSRs, hardwired entries included, must grant exactly what the current process's slots do. */
        uint32_t addr[PMP_CSR_ENTRIES];
        uint8_t cfg[PMP_CSR_ENTRIES];
        for (unsigned i = 0; i < pmp_entry_end; i++) {
            pmp_get(i, &addr[i], &cfg[i]);
        }
        check_pmp_image(thread_process(current), addr, cfg, pmp_entry_end, true);
    }
}
