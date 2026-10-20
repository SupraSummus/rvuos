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
#include "route.h"
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
        /* The pool's own node names the pool and hears of nothing; the tree check reads its links. */
        if (p->node.type != CAP_POOL || p->node.a != base || cap_held(&p->node) || p->node.rights != RIGHT_ALL) {
            fail("pool's own node is malformed", base, p->node.type, p->node.a);
        }
        if (p->used < sizeof(*p) || p->used > p->size) {
            fail("pool used mark outside the pool", base, p->used, p->size);
        }
        if ((base | p->size) & (OBJ_ALIGN - 1)) {
            fail("pool is not aligned", base, p->size, 0);
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
                o->type != CAP_IRQ && o->type != CAP_ROUTE) {
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
        /* A region slot holds an installed region and nothing else; the tree check reads its links. */
        if (s->type != CAP_FRAME || s->rights == 0 || (s->rights & ~RIGHT_ALL)) {
            fail("region slot holds something other than an installed region", v2p(proc), i, s->type);
        }
        /* The process hears of its clearing, by its name in the slot. */
        if (!cap_held(s) || cap_holder(s) != v2p(proc)) {
            fail("installed region does not name its process", v2p(proc), i, s->b);
        }
        uint32_t base = block_base(s), size = block_size(s);
        uint8_t granted = granted_rights(base, size);
        if (!napot_block(base, size) || granted == 0) {
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
            struct obj_header *t = object_find(cap_addr(tc));
            if (tc->type != CAP_CAPTABLE || t == NULL || t->type != CAP_CAPTABLE || cap_held(tc)) {
                fail("process's table slot names no live table", v2p(proc), i, tc->a);
            }
            if (cap_child(tc) != 0) {
                fail("something is derived from a process's table slot", v2p(proc), i, cap_child(tc));
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

static void check_release(const struct place *p, const struct place *a, const char *what);

/*
 * What a place's thread may run on, of its own account and those its lenders lend it:
 * whether one has units, whether one holds a tick, and the earliest a request on one of those would be due.
 * With check, each with units and without a tick is held to reach one no earlier than the release.
 */
struct may_run {
    const struct place *p;
    bool check, units, time;
    uint32_t due;
};

/* The count a place's virtual time stands at, the count its account would be full at, or the count once it is. */
static uint32_t vtime_of(const struct place *q)
{
    uint32_t full = (uint32_t)((uint64_t)q->stamp * timer_tick_counts()) + (account_cap(q) - q->balance) / place_units(q);
    uint32_t now = count_of(q) * timer_tick_counts();
    return (int32_t)(now - full) < 0 ? full : now;
}

static void may_run_on(struct may_run *m, const struct place *q)
{
    if (q == NULL || place_units(q) == 0) {
        return;
    }
    m->units = true;
    if (account_now(q) >= tick_parts()) {
        uint32_t due = vtime_of(q) + place_span(q);
        if (!m->time || (int32_t)(due - m->due) < 0) {
            m->due = due;
        }
        m->time = true;
    } else if (m->check) {
        check_release(m->p, q, q == m->p ? "a place's account reaches a tick before the release"
                                         : "a lender's account reaches a tick before the release");
    }
}

/*
 * The lender of a borrower's ring at at, NULL for none or for something that does not lend to it;
 * check_lenders holds the rings themselves.
 */
static const struct thread *lender_at(const struct thread *h, paddr_t at)
{
    const struct obj_header *o = at != 0 ? object_find(at) : NULL;
    return o != NULL && o->type == CAP_THREAD && ((const struct thread *)o)->lend_to == v2p(h) ? (const struct thread *)o : NULL;
}

/*
 * The one after a lender on its borrower's ring, 0 once the ring closes, or where the next is not linked back to it,
 * so that the walk ends on a ring check_lenders has yet to hold, as its own does.
 */
static paddr_t lender_after(const struct thread *h, const struct thread *l)
{
    const struct thread *next = l->lend_next != h->lenders ? lender_at(h, l->lend_next) : NULL;
    return next != NULL && next->lend_prev == v2p(l) ? v2p(next) : 0;
}

/*
 * The accounts a place's thread may run on: its own, and those of the threads that lend to it, or lend to one that does,
 * LEND_DEPTH lends on, each thread on the way holding the time of the place's core; by the rings, as the kernel looks at the units.
 */
static struct may_run may_run(const struct place *p, bool check)
{
    struct may_run m = { p, check, false, false, 0 };
    const struct cap *b = place_time(p);
    if (b->type != CAP_TIME) {
        return m;
    }
    uint32_t core = unit_core(range_first(b));
    may_run_on(&m, p);
    _Static_assert(LEND_DEPTH == 3, "three rings deep");
    const struct thread *h0 = place_thread(p), *h1, *h2, *h3;
    for (paddr_t a1 = h0->lenders; (h1 = lender_at(h0, a1)) != NULL; a1 = lender_after(h0, h1)) {
        const struct place *p1 = thread_place_on(h1, core);
        if (p1 == NULL) {
            continue;
        }
        may_run_on(&m, p1);
        for (paddr_t a2 = h1->lenders; (h2 = lender_at(h1, a2)) != NULL; a2 = lender_after(h1, h2)) {
            const struct place *p2 = thread_place_on(h2, core);
            if (p2 == NULL) {
                continue;
            }
            may_run_on(&m, p2);
            for (paddr_t a3 = h2->lenders; (h3 = lender_at(h2, a3)) != NULL; a3 = lender_after(h2, h3)) {
                may_run_on(&m, thread_place_on(h3, core));
            }
        }
    }
    return m;
}

/*
 * The queue a place belongs on: none unless its thread is ready, the place is bound, and its thread's turn is nowhere;
 * the run queue with a tick in its account or a lender's, else the spare queue with spare time,
 * else the spent queue with units of its own or a lender's, and none without.
 */
static uint8_t queue_owed(const struct place *p)
{
    const struct thread *t = place_thread(p);
    if (t->state != THREAD_READY || is_turn(t) || place_time(p)->type != CAP_TIME) {
        return QUEUE_NONE;
    }
    struct may_run m = may_run(p, false);
    if (m.time) {
        return QUEUE_RUN;
    }
    if (place_spare(p)) {
        return QUEUE_SPARE;
    }
    return m.units ? QUEUE_SPENT : QUEUE_NONE;
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

/* An account that gives p time once it holds a tick, its own or a lender's, reaches one no earlier than the release. */
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
 * bound through a capability with the right to bind, whose rights they carry, units of one core, which it alone earns,
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
         first >= MACHINE_UNITS || count > MACHINE_UNITS - first || !cap_held(b) || cap_holder(b) != v2p(t))) {
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
    /*
     * One that waits without time has it again at the release, by its own account or a lender's, which waits on no queue,
     * and one with lenders on the run queue is looked at again there too, since an account that comes to hold a tick may make its request due sooner.
     */
    if (owed == QUEUE_SPARE || owed == QUEUE_SPENT || (owed == QUEUE_RUN && t->lenders != 0)) {
        may_run(p, true);
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
        struct obj_header *p = object_find(cap_addr(h));
        if (h->type != CAP_PROCESS || p == NULL || p->type != CAP_PROCESS || !cap_held(h) || cap_holder(h) != v2p(t)) {
            fail("thread's process slot names no live process", v2p(t), h->type, h->a);
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
        struct obj_header *n = object_find(cap_addr(w));
        if (w->type != CAP_NOTIFICATION || n == NULL || n->type != CAP_NOTIFICATION || (w->rights & RIGHT_W) == 0 ||
            cap_held(w)) {
            fail("thread's watch names no live notification", v2p(t), w->type, w->a);
        }
        if (cap_child(w) != 0) {
            fail("something is derived from a thread's watch", v2p(t), cap_child(w), 0);
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
        /*
         * A request on its own time is due a span at most ahead of the virtual time it went on from,
         * which the account as it was settled gives: the stamp, and how far it was from full.
         */
        if (queue == QUEUE_RUN && p->balance >= tick_parts()) {
            uint32_t u = place_units(p);
            uint32_t vt = (uint32_t)((uint64_t)p->stamp * timer_tick_counts()) + (account_cap(p) - p->balance) / u;
            if ((int32_t)(p->deadline - vt) > (int32_t)place_span(p)) {
                fail("a request is due further ahead than its span", v2p(p), p->deadline, vt);
            }
        }
        /* Whatever it runs on, its request is due no later than one on the account it may run on that is due first. */
        struct may_run m = queue == QUEUE_RUN ? may_run(p, false) : (struct may_run){ 0 };
        if (m.time && (int32_t)(p->deadline - m.due) > 0) {
            fail("a request waits due later than a lender's would be", v2p(p), p->deadline, m.due);
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
         * It pays from the wait in which it lent the turn's thread its time, or lent it to one that lends it on,
         * LEND_DEPTH lends at most, and once it or one on the way is woken until the turn ends.
         */
        const struct thread *by = payer;
        for (uint32_t d = 0; d < LEND_DEPTH && by->state == THREAD_WAITING && by != c->turn; d++) {
            LOOP_BOUND(LEND_DEPTH);
            if (by->lend_to == 0) {
                fail("a turn's payer that waits does not lend to the turn's thread", i, v2p(payer), 0);
            }
            by = p2v(by->lend_to);
        }
        if (by->state == THREAD_WAITING) {
            fail("a turn's payer that waits does not lend to the turn's thread", i, v2p(payer), by->lend_to);
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
    bool told = !c->recall && c->run_queue != 0 && (c == core_self() || (c->in_user && !c->interrupt_owed));
    if (told && sched_spare_turn(c)) {
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
    /* A turn with time whose request is due after one on the run queue ends as a turn on spare time does: earliest deadline first. */
    if (told && !sched_spare_turn(c) && own != NULL) {
        paddr_t at = c->run_queue;
        do {
            LOOP_BOUND(TIME_UNITS);
            const struct place *p = place_at(at);
            if ((int32_t)(p->deadline - own->deadline) < 0) {
                fail("a thread waits with a deadline before the turn's request", i, v2p(p), own->deadline);
            }
            at = p->queue_next;
        } while (at != c->run_queue);
    }
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
    /* Each tick decides afresh who pays for a turn on lent time, or one others lend to. */
    if (core->payer != NULL || core->turn->lenders != 0) {
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
        struct obj_header *n = object_find(cap_addr(s));
        if (s->type != CAP_NOTIFICATION || n == NULL || n->type != CAP_NOTIFICATION || (s->rights & RIGHT_W) == 0) {
            fail("irq's notification slot names no live notification", v2p(i), s->type, s->a);
        }
        /* The Irq hears of its clearing, by its name in the slot. */
        if (!cap_held(s) || cap_holder(s) != v2p(i)) {
            fail("irq's notification does not name its irq", v2p(i), s->b, 0);
        }
        if (i->bits & ~notify_bits(s)) {
            fail("irq is armed with bits its notification capability may not signal", v2p(i), i->bits, notify_bits(s));
        }
    } else if (irq_armed(i)) {
        fail("irq without a notification is armed", v2p(i), i->bits, 0);
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

/*
 * A Route is bound to a channel the kernel lends, and holds each end as an Irq holds its notification:
 * empty, or a leaf below a frame, connected through a capability with the right the end needs,
 * the right to read for the event and to write for the task, and an end the board's router finds that frame to hold.
 * It is armed only with both ends, and the router connects its channel exactly while it is armed,
 * from the one end's word to the other's, read back from the router itself.
 */
static void check_route(const struct route *r)
{
    if (!route_channel(r->channel)) {
        fail("route names a channel the kernel does not lend", v2p(r), r->channel, 0);
    }
    if (channel_binding(r->channel) != r) {
        fail("route is missing from the channel table", v2p(r), r->channel, 0);
    }
    for (uint32_t end = ROUTE_EVENT; end <= ROUTE_TASK; end++) {
        const struct cap *e = &r->ends[end];
        if (e->type == CAP_NONE) {
            if (r->names[end] != 0) {
                fail("route keeps the name of an end it does not hold", v2p(r), end, r->names[end]);
            }
            continue;
        }
        uint8_t need = end == ROUTE_EVENT ? RIGHT_R : RIGHT_W;
        uint32_t word;
        if (e->type != CAP_FRAME || (e->rights & need) == 0) {
            fail("route's end names no frame it may connect", v2p(r), end, e->type);
        }
        /* The Route hears of its clearing, by its name in the end. */
        if (!cap_held(e) || cap_holder(e) != v2p(r)) {
            fail("route's end does not name its route", v2p(r), end, e->b);
        }
        if (!router_end(end, block_base(e), block_size(e), r->names[end], &word)) {
            fail("route's end is none the router finds in its frame", v2p(r), end, r->names[end]);
        }
    }
    if (r->armed > 1 ||
        (r->armed && (r->ends[ROUTE_EVENT].type == CAP_NONE || r->ends[ROUTE_TASK].type == CAP_NONE))) {
        fail("route is armed without both its ends", v2p(r), r->armed, 0);
    }
    uint32_t event, task;
    bool connected = router_connected(r->channel, &event, &task);
    if (connected != (r->armed != 0)) {
        fail(r->armed ? "armed route's channel is not connected" : "router connects a disarmed route's channel",
             v2p(r), r->channel, 0);
    }
    if (connected && (event != route_word(r, ROUTE_EVENT) || task != route_word(r, ROUTE_TASK))) {
        fail("router connects a route's channel to other ends than its own", v2p(r), event, task);
    }
}

/*
 * The channel table names only live Routes of their own channel, which with check_route's holding every Route to it
 * makes it exactly the bound Routes, one to a channel; and the router connects no channel nothing is bound to.
 */
static void check_channels(void)
{
#if ROUTE_CHANNELS > 0
    for (uint32_t c = ROUTE_FIRST; c < ROUTE_FIRST + ROUTE_CHANNELS; c++) {
        const struct route *r = channel_binding(c);
        const struct obj_header *o = r != NULL ? object_find(v2p(r)) : NULL;
        uint32_t event, task;
        if (r != NULL && (o != &r->hdr || o->type != CAP_ROUTE || r->channel != c)) {
            fail("channel table names a route that is gone", c, v2p(r), 0);
        }
        if (r == NULL && router_connected(c, &event, &task)) {
            fail("router connects a channel nothing is bound to", c, event, task);
        }
    }
#endif
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
        switch (c->type) {
        case CAP_PRINT:
        case CAP_HALT:
        case CAP_TEST:
        case CAP_CLOCK:
            break;
        case CAP_FRAME: {
            uint8_t granted = granted_rights(block_base(c), block_size(c));
            if (granted == 0) {
                fail("frame outside granted memory", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("frame with rights beyond the grant", v2p(table), i, c->rights);
            }
            if (!napot_block(block_base(c), block_size(c))) {
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
        case CAP_ROUTE_LINE:
            /* The root task received every channel the kernel lends, with RIGHT_W; nothing widens that. */
            if (range_count(c) == 0 || !route_channel(range_first(c)) ||
                range_count(c) > ROUTE_FIRST + ROUTE_CHANNELS - range_first(c)) {
                fail("route line capability outside the channels lent", v2p(table), i, c->a);
            }
            if (c->rights & ~RIGHT_W) {
                fail("route line capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        case CAP_TIME:
            /* The root task received every unit there is, with RIGHT_W and RIGHT_X; nothing widens that. */
            if (range_count(c) == 0 || range_first(c) >= MACHINE_UNITS ||
                range_count(c) > MACHINE_UNITS - range_first(c)) {
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
        case CAP_IRQ:
        case CAP_ROUTE: {
            /* A destroy sweeps the tables, so every object capability points at a live object. */
            struct obj_header *o = object_find(cap_addr(c));
            if (o == NULL || o->type != c->type) {
                fail("dangling object capability", v2p(table), i, c->a);
            }
            break;
        }
        default:
            fail("capability of unknown type", v2p(table), i, c->type);
        }
        /* Only an object holds a node that names a holder; one in a table is a capability, and its holder hears nothing. */
        if (cap_held(c)) {
            fail("a capability in a table names a holder", v2p(table), i, c->b);
        }
    }
}

/*
 * The derivation tree.
 *
 * Its nodes are the slots of every live table,
 * the table slots and region slots of every live process,
 * the process, units and watch of every live thread,
 * the notification of every live Irq and the ends of every live Route,
 * linked by physical address and never checked on use,
 * so every link must land on a live node and the shape must be exactly a forest:
 * the children of a node form one ring that closes through the node,
 * each predecessor link is a next link turned round,
 * and from every node the links up reach a root.
 * A destroy that left a link into freed memory, a delete that broke a ring,
 * or a revoke that stopped short all show up here.
 * A node is derived from its parent: the same object with no more rights,
 * and for a notification no more bits to signal,
 * a range within the parent's with no more rights,
 * or an object built on the parent's range, a pool on a region, an Irq or a Route on the pool it was allocated through,
 * a Route's end on a frame,
 * a thread's process, a thread's watch or an Irq's notification on a capability to it,
 * a thread's units on the units it was bound through,
 * or the counter's block, or a region installed from it or a Route's end on it, below the clock.
 */

/* The node a link names, if one of a live object; only the pool the address lies in is looked at. */
static const struct cap *live_node(uint32_t link)
{
    paddr_t at = link & ~LINK_UP;
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

/* The memory a node stands for, if it stands for memory. */
static bool node_range(const struct cap *n, uint32_t *base, uint32_t *size)
{
    switch (n->type) {
    case CAP_FRAME:
    case CAP_UNTYPED:
        *base = block_base(n);
        *size = block_size(n);
        return true;
    case CAP_POOL:
        /* A pool's own node stands for the pool's memory; a capability to the pool does not. */
        if (node_pool(n) == NULL) {
            return false;
        }
        *base = cap_addr(n);
        *size = node_pool(n)->size;
        return true;
    default:
        return false;
    }
}

/*
 * A node naming an object built in a pool:
 * a capability to one, a thread's hold on its process or on its watch, or an Irq's on its notification.
 */
static bool is_object_type(uint8_t type)
{
    return type == CAP_CAPTABLE || type == CAP_PROCESS || type == CAP_THREAD || type == CAP_NOTIFICATION ||
           type == CAP_IRQ || type == CAP_ROUTE;
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
     * A capability to a pool hangs below the pool's own node, which carries rights of its own.
     */
    if (c->type == p->type) {
        if (c->type == CAP_FRAME) {
            return narrower && range_within(block_base(c), block_size(c), block_base(p), block_size(p));
        }
        /* A thread's units lie among those it was bound through, and the first of them does even for none. */
        if (c->type == CAP_TIME && cap_held(c)) {
            uint32_t off = range_first(c) - range_first(p);
            return narrower && off < range_count(p) && range_count(c) <= range_count(p) - off;
        }
        if (c->type == CAP_IRQ_LINE || c->type == CAP_TIME || c->type == CAP_ROUTE_LINE) {
            return narrower && range_within(range_first(c), range_count(c), range_first(p), range_count(p));
        }
        /* A notification's capability carries the bits it may signal, which narrow as rights do. */
        return (narrower || node_pool(p) != NULL) &&
               (c->type == CAP_PRINT || c->type == CAP_HALT || c->type == CAP_TEST || c->type == CAP_CLOCK ||
                cap_addr(c) == cap_addr(p)) &&
               (c->type != CAP_NOTIFICATION || !(notify_bits(c) & ~notify_bits(p)));
    }
    /*
     * A clock that reads gives out one frame, the counter's block, read only,
     * and adopts what was carved, installed or connected from it when a delete or a revoke takes it.
     */
    if (c->type == CAP_FRAME && p->type == CAP_CLOCK) {
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        return (p->rights & RIGHT_R) && !(c->rights & ~g->rights) &&
               range_within(block_base(c), block_size(c), g->base, g->size);
    }
    /* Below a pool's node, and below its capability, lies every capability to the pool's objects, which carry rights of their own. */
    if (p->type == CAP_POOL) {
        return is_object_type(c->type) && cap_object(c)->pool == cap_addr(p);
    }
    return false;
}

/*
 * A filled node's words carry nothing its type has no use for:
 * the bits below an object's address only on a Notification capability, which keeps part of its code there,
 * past B_LINK only B_HELD, B_UP, and a notification's code;
 * and a notification's code names a block of bits, as the one code of six bits that names none does not.
 */
static void check_words(const struct cap *n)
{
    uint32_t spare = B_HELD | B_UP | (n->type == CAP_NOTIFICATION ? 0x3u << 28 : 0);
    if ((n->b & ~(B_LINK | spare)) != 0 ||
        (cap_has_object(n->type) && n->type != CAP_NOTIFICATION && (n->a & (NODE_ALIGN - 1)) != 0) ||
        (!cap_has_object(n->type) && n->type != CAP_FRAME && n->type != CAP_UNTYPED && n->type != CAP_IRQ_LINE &&
         n->type != CAP_TIME && n->type != CAP_ROUTE_LINE && n->a != 0)) {
        fail("a node carries bits its type has no use for", v2p(n), n->a, n->b);
    }
    if (n->type == CAP_NOTIFICATION && notify_code(n) == 63) {
        fail("a notification's capability names no block of bits", v2p(n), n->a, n->b);
    }
}

/* One node's links; bound is more than the nodes there are, so a walk that long is a cycle. */
static void check_node(const struct cap *n, uint32_t bound)
{
    if (n->type == CAP_NONE) {
        if (n->b != 0 || cap_next(n) != 0 || cap_prev(n) != 0) {
            fail("empty slot keeps links", v2p(n), cap_child(n), cap_next(n));
        }
        return;
    }
    if (cap_next(n) == 0) {
        fail("filled slot has malformed links", v2p(n), cap_child(n), cap_next(n));
    }
    check_words(n);
    if (cap_child(n) != 0) {
        const struct cap *c = live_node(cap_child(n));
        if (c == NULL || c->type == CAP_NONE) {
            fail("child link to a slot that is not a live, filled one", v2p(n), cap_child(n), 0);
        }
    }

    /* Along the ring to the parent, every sibling live and filled. */
    uint32_t link = cap_next(n);
    uint32_t steps = 0;
    while (!(link & LINK_UP)) {
        const struct cap *s = live_node(link);
        if (s == NULL || s->type == CAP_NONE) {
            fail("link to a slot that is not a live, filled one", v2p(n), link, 0);
        }
        if (++steps > bound) {
            fail("sibling ring does not close", v2p(n), link, 0);
        }
        link = cap_next(s);
    }
    if (link == LINK_UP) {
        /* A root: roots have no ring, so nothing may lead here through siblings. */
        if (steps != 0) {
            fail("sibling ring closes on no parent", v2p(n), 0, 0);
        }
        /* Only a delete of an Untyped as a root marks it going, in the prev a root has no use for. */
        if (cap_prev(n) != 0 && !(n->type == CAP_UNTYPED && cap_prev(n) == NODE_ALIGN)) {
            fail("root has a predecessor", v2p(n), cap_prev(n), 0);
        }
        /* Every capability to a pool or its objects lies below the pool's own node, which may be a root itself. */
        if ((n->type == CAP_POOL && node_pool(n) == NULL) || is_object_type(n->type)) {
            fail("capability to an object is a root", v2p(n), n->type, n->a);
        }
        return;
    }
    const struct cap *parent = live_node(link);
    if (parent == NULL || parent->type == CAP_NONE) {
        fail("link up to a slot that is not a live, filled one", v2p(n), link, 0);
    }
    if (!derived_from(n, parent)) {
        fail("slot is not derived from its parent", v2p(n), v2p(parent), 0);
    }
    /* The parent's ring is this one: it comes round to the node. */
    steps = 0;
    link = cap_child(parent);
    for (const struct cap *s = live_node(link); s != n; s = live_node(link)) {
        if ((link & LINK_UP) || s == NULL || ++steps > bound) {
            fail("parent's ring does not reach the slot", v2p(n), v2p(parent), link);
        }
        link = cap_next(s);
    }
    /* The predecessor is the sibling whose next is this node; the first child's is the last. */
    const struct cap *pred = live_node(cap_prev(n));
    if (pred == NULL || pred->type == CAP_NONE) {
        fail("predecessor link to a slot that is not a live, filled one", v2p(n), cap_prev(n), 0);
    }
    bool first = cap_child(parent) == v2p(n);
    if (first ? cap_next(pred) != (v2p(parent) | LINK_UP) : cap_next(pred) != v2p(n)) {
        fail("predecessor's next is not the slot", v2p(n), cap_prev(n), cap_next(pred));
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

/* The nodes there are, and one more; a walk up that long has gone round a cycle. */
static uint32_t tree_bound;

/* The node a node was derived from, or NULL at a root; check_node has vetted the rings. */
static const struct cap *node_parent(const struct cap *n)
{
    uint32_t link = cap_next(n);
    while (!(link & LINK_UP)) {
        link = cap_next(live_node(link));
    }
    return link == LINK_UP ? NULL : live_node(link);
}

static bool lies_below(const struct cap *n, const struct cap *ancestor)
{
    uint32_t steps = 0;
    for (const struct cap *p = node_parent(n); p != NULL; p = node_parent(p)) {
        if (++steps > tree_bound) {
            fail("the links up do not reach a root", v2p(n), 0, 0);
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
    struct node_iter it;
    node_enter(&it, object_first());
    while (node_next(&it) != NULL) {
        tree_bound++;
    }
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        check_node(n, tree_bound);
    }
    /* A forest: from every node the links up reach a root. */
    node_enter(&it, object_first());
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        if (n->type != CAP_NONE) {
            lies_below(n, NULL);
        }
    }
}

/*
 * True if an Untyped's delete as a root has begun.
 * It makes roots of its children one by one, which lie in its memory until it goes;
 * it makes nothing meanwhile, so nothing it could make meets them.
 */
static bool going(const struct cap *u)
{
    return untyped_going(u);
}

/*
 * Memory that may become kernel memory, an Untyped or a pool,
 * overlaps another node standing for memory only where one lies below the other,
 * or where one of them is an Untyped whose delete as a root has begun,
 * which makes roots of its children one by one.
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
        if ((x->type != CAP_UNTYPED && node_pool(x) == NULL) || !node_range(x, &xbase, &xsize)) {
            continue;
        }
        node_enter(&jt, object_first());
        for (const struct cap *y = node_next(&jt); y != NULL; y = node_next(&jt)) {
            uint32_t ybase, ysize;
            if (y == x || !node_range(y, &ybase, &ysize) || !ranges_overlap(xbase, xsize, ybase, ysize)) {
                continue;
            }
            if (!lies_below(x, y) && !lies_below(y, x) && !going(x) && !going(y)) {
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
        case CAP_ROUTE:
            check_route((struct route *)o);
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
    check_channels();
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
