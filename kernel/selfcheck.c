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
 * and, once OP_DEBUG_TRACE has been invoked, on the target as well.
 * It is freestanding so that the same file serves both.
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "pmp.h"
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

/* Shell sort; the arrays are small and no libc is assumed. */
static void sort_u64(uint64_t *v, size_t n)
{
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            uint64_t x = v[i];
            size_t j = i;
            while (j >= gap && v[j - gap] > x) {
                v[j] = v[j - gap];
                j -= gap;
            }
            v[j] = x;
        }
    }
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
        /* The pool's own node; the tree check reads its links. */
        if (p->node.type != CAP_RETYPED || p->node.a != base || p->node.b != 0 || p->node.index != 0) {
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
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE && at >= s->a && at < (uint64_t)s->a + s->b) {
            return rights_to_pmp(s->rights);
        }
    }
    return 0;
}

/*
 * The PMP image and the region slots describe the same function
 * from address to rights.
 * Both are piecewise constant, so comparing at every breakpoint
 * and between consecutive breakpoints is exact.
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

    uint64_t points[2 * PROCESS_REGION_SLOTS + 2 * PMP_MAX_ENTRIES + 2];
    size_t n = 0;
    points[n++] = 0;
    points[n++] = 0x100000000ull;
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        if (proc->slots[i].type != CAP_NONE) {
            points[n++] = proc->slots[i].a;
            points[n++] = (uint64_t)proc->slots[i].a + proc->slots[i].b;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        uint64_t base, size;
        pmp_napot_range(addr[i], &base, &size);
        points[n++] = base;
        points[n++] = base + size;
    }
    sort_u64(points, n);

    for (size_t i = 0; i + 1 < n; i++) {
        uint64_t a = points[i], b = points[i + 1];
        if (a == b) {
            continue;
        }
        uint64_t probes[2] = { a, a + (b - a) / 2 };
        for (int k = 0; k < 2; k++) {
            uint8_t want = slot_rights(proc, probes[k]);
            uint8_t got = image_rights(addr, cfg, count, probes[k]);
            if (want != got) {
                fail(csrs ? "PMP CSRs grant rights the slots do not"
                          : "PMP image grants rights the slots do not",
                     (uint32_t)probes[k], got, want);
            }
        }
    }
}

static void check_process(const struct process *proc)
{
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        if (s->type == CAP_NONE) {
            continue;
        }
        /* A region slot holds an installed region and nothing else; the tree check reads its links. */
        if (s->type != CAP_INSTALLED || s->rights == 0 || (s->rights & ~RIGHT_ALL)) {
            fail("region slot holds something other than an installed region", v2p(proc), i, s->type);
        }
        if (s->index != i) {
            fail("installed region does not know its slot", v2p(proc), i, s->index);
        }
        uint8_t granted = granted_rights(s->a, s->b);
        if (s->b == 0 || granted == 0) {
            fail("process maps memory outside what was granted", v2p(proc), s->a, s->b);
        }
        if (s->rights & ~granted) {
            fail("process maps memory with rights beyond the grant", v2p(proc), s->a, s->rights);
        }
        for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
            if (ranges_overlap(s->a, s->b, pool_base(p), p->size)) {
                fail("process maps a pool: isolation broken", v2p(proc), s->a, pool_base(p));
            }
        }
        for (unsigned j = i + 1; j < PROCESS_REGION_SLOTS; j++) {
            const struct cap *t = &proc->slots[j];
            if (t->type != CAP_NONE && ranges_overlap(s->a, s->b, t->a, t->b)) {
                fail("process has overlapping region slots", v2p(proc), i, j);
            }
#if PMP_SPLIT_STORE_AS_READ
            /* See PMP_SPLIT_STORE_AS_READ in board.h. */
            if (t->type != CAP_NONE) {
                const struct cap *lower = s->a < t->a ? s : t, *upper = s->a < t->a ? t : s;
                if ((uint64_t)lower->a + lower->b == upper->a && (lower->rights & RIGHT_W) &&
                    (upper->rights & RIGHT_R) && !(upper->rights & RIGHT_W)) {
                    fail("process may write right below a region it may only read", v2p(proc), i, j);
                }
            }
#endif
        }
    }

    if (proc->pmp.count > pmp_entry_count) {
        fail("PMP image has more entries than the hardware", v2p(proc), proc->pmp.count, 0);
    }
    check_pmp_image(proc, proc->pmp.addr, proc->pmp.cfg, proc->pmp.count, false);

    /* The sweep clears the table slot with the table, in whatever pool; the tree check reads its links. */
    const struct cap *tc = &proc->table;
    if (tc->type != CAP_NONE) {
        struct obj_header *t = object_find(tc->a);
        if (tc->type != CAP_CAPTABLE || t == NULL || t->type != CAP_CAPTABLE) {
            fail("process's table slot names no live table", v2p(proc), tc->type, tc->a);
        }
    }
}

/* Threads that belong on a queue, and threads found on one; the two counts must agree. */
static uint32_t owed_threads, queued_threads;

/* The units the threads the walk met are bound to. */
static uint32_t units_seen;

/* The threads found on their borrowers' rings of lenders, and the threads found lending; the two counts must agree. */
static uint32_t lends_seen, lends_named;

/* Whether it is the thread's turn, or it pays for the turn, either of which it has only on the core it names. */
static bool is_turn(const struct thread *t)
{
    return t->core < CORES && (cores[t->core].turn == t || cores[t->core].payer == t);
}

/*
 * The count a thread's account is counted to: the count its core charged its turn to while it has one there or pays for one,
 * which lags the machine's while that core has not trapped since another counted, and the machine's otherwise.
 */
static uint32_t count_of(const struct thread *t)
{
    return is_turn(t) ? cores[t->core].ticks : sched_ticks;
}

/* A thread's account at the count, its stamp's balance and its tick's gain every tick since, held to the cap. */
static uint32_t account_now(const struct thread *t)
{
    uint64_t balance = t->balance + (uint64_t)(count_of(t) - t->stamp) * tick_gain(t);
    uint32_t cap = account_cap(t);
    return balance > cap ? cap : (uint32_t)balance;
}

/*
 * The thread a borrower runs on the time of while it has none:
 * the one that has lent it time longest, if that one names the borrower's core.
 * check_lenders has vetted the ring.
 */
static const struct thread *lender_of(const struct thread *t)
{
    if (t->lenders == 0) {
        return NULL;
    }
    const struct thread *l = p2v(t->lenders);
    return l->core == t->core ? l : NULL;
}

/*
 * The queue a thread belongs on: none unless it is ready, bound, and it is not its turn;
 * the run queue with a tick in its account or its lender's, else the spare queue with spare time,
 * else the spent queue with units of its own or its lender's, and none without.
 */
static uint8_t queue_owed(const struct thread *t)
{
    if (t->state != THREAD_READY || is_turn(t) || t->time.type != CAP_BOUND) {
        return QUEUE_NONE;
    }
    const struct thread *l = lender_of(t);
    if (account_now(t) >= tick_parts() || (l != NULL && account_now(l) >= tick_parts())) {
        return QUEUE_RUN;
    }
    if (thread_spare(t)) {
        return QUEUE_SPARE;
    }
    return thread_units(t) != 0 || (l != NULL && thread_units(l) != 0) ? QUEUE_SPENT : QUEUE_NONE;
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

/* An account that gives t time once it holds a tick, its own or its lender's, reaches one no earlier than the release. */
static void check_release(const struct thread *t, const struct thread *a, const char *what)
{
    if (a == NULL || thread_units(a) == 0) {
        return;
    }
    uint32_t missing = tick_parts() - account_now(a);
    uint32_t at = sched_ticks + (missing + tick_gain(a) - 1) / tick_gain(a);
    uint32_t release = cores[t->core].nearest_release;
    if ((int32_t)(at - release) < 0) {
        fail(what, v2p(t), at, release);
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
        if (h->type != CAP_HOSTED || p == NULL || p->type != CAP_PROCESS || h->b != 0 || h->index != 0) {
            fail("thread's process slot names no live process", v2p(t), h->type, h->a);
        }
        if (h->child != 0) {
            fail("something is derived from a thread's process", v2p(t), h->child, 0);
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
    /*
     * A thread's units are a leaf of the tree, which the tree check reads the links of, and it alone earns them.
     * They were bound through a capability with the right to bind, whose rights they carry.
     */
    const struct cap *b = &t->time;
    if (b->type != CAP_NONE &&
        (b->type != CAP_BOUND || (b->rights & RIGHT_W) == 0 || (b->rights & ~(RIGHT_W | RIGHT_X)) != 0 ||
         b->a >= MACHINE_UNITS ||
         b->b > MACHINE_UNITS - b->a || b->index != 0)) {
        fail("thread's units are malformed", v2p(t), b->type, b->a);
    }
    /*
     * A thread runs on one core, the one its units are of, and names it as it joins a queue there;
     * one bound elsewhere during its turn finishes the turn where it had it, and names that core till it settles again.
     */
    if (b->type != CAP_NONE && b->b != 0 && unit_core(b->a) != unit_core(b->a + b->b - 1)) {
        fail("thread's units are of two cores", v2p(t), b->a, b->b);
    }
    if (t->core >= CORES) {
        fail("thread names a core there is not", v2p(t), t->core, 0);
    }
    if (b->type != CAP_NONE && t->queue != QUEUE_NONE && t->core != unit_core(b->a)) {
        fail("thread waits on another core than its units are of", v2p(t), t->core, b->a);
    }
    if (b->type != CAP_NONE && b->child != 0) {
        fail("something is derived from a thread's units", v2p(t), b->child, 0);
    }
    for (uint32_t i = 0; b->type != CAP_NONE && i < b->b; i++) {
        if (unit_thread[b->a + i] != v2p(t)) {
            fail("a unit a thread is bound to names another", v2p(t), b->a + i, unit_thread[b->a + i]);
        }
    }
    /*
     * The sweep clears a thread's watch with its notification, in whatever pool; the tree check reads the node's links.
     * It was set through a capability with the right to signal, whose rights it carries, and it signals some bit.
     */
    const struct cap *w = &t->watch;
    if (w->type != CAP_NONE) {
        struct obj_header *n = object_find(w->a);
        if (w->type != CAP_WATCHED || n == NULL || n->type != CAP_NOTIFICATION || (w->rights & RIGHT_W) == 0 ||
            w->b == 0 || w->index != 0) {
            fail("thread's watch names no live notification", v2p(t), w->type, w->a);
        }
        if (w->child != 0) {
            fail("something is derived from a thread's watch", v2p(t), w->child, 0);
        }
    }
    units_seen += thread_units(t);
    check_lenders(t);
    /* Every account holds at most its cap, and so nothing without units, and was last counted no later than the count. */
    if (t->balance > account_cap(t)) {
        fail("thread's account holds more than its cap", v2p(t), t->balance, thread_units(t));
    }
    if ((int32_t)(count_of(t) - t->stamp) < 0) {
        fail("thread's account is stamped ahead of the count", v2p(t), t->stamp, count_of(t));
    }
    uint8_t owed = queue_owed(t);
    if (t->queue != owed) {
        fail(owed == QUEUE_NONE ? "a thread names a queue it is not for"
             : t->queue == QUEUE_NONE ? "a ready thread that may run waits on no queue"
                                      : "a thread waits on another queue than its account says",
             v2p(t), t->queue, owed);
    }
    /* One that waits without time has it again at the release, by its own account or its lender's, which waits on no queue. */
    if (owed == QUEUE_SPARE || owed == QUEUE_SPENT) {
        check_release(t, t, "a thread's account reaches a tick before the release");
        check_release(t, lender_of(t), "a lender's account reaches a tick before the release");
    }
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
        if (owed != QUEUE_NONE) {
            owed_threads++;
        } else if (t->queue_next != 0 || t->queue_prev != 0) {
            fail("thread on no queue is linked into one", v2p(t), t->queue_next, t->state);
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
 * A queue is a ring of live threads, each linked back to the one before,
 * none of them running, and each in the state and waiting on what the queue is for:
 * a notification's waiters, or one of the scheduler's queues, which the thread names with its core.
 * The back links make the walk close at the oldest or fail:
 * no thread can be entered twice from two different predecessors.
 */
static void check_queue(paddr_t head, uint8_t state, paddr_t waiting_on, uint8_t queue, uint32_t core)
{
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
        /* A waiter may pay for a turn it lent its time to, while a thread on the scheduler's queues has none. */
        bool turn = queue != QUEUE_NONE ? is_turn(t) : t->core < CORES && cores[t->core].turn == t;
        if (t->state != state || t->waiting_on != waiting_on || turn || t->queue != queue) {
            fail("queue holds a thread it is not for", head, at, t->state);
        }
        if (queue != QUEUE_NONE && t->core != core) {
            fail("a core's queue holds a thread of another core", head, at, t->core);
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
    if (c->turn != NULL && c->turn->stamp != c->ticks) {
        fail("the ticks of a turn were not charged to its thread", v2p(c->turn), c->turn->stamp, c->ticks);
    }
    /* A turn on lent time is paid by another live thread of the core, which is charged as the turn's thread would be. */
    const struct thread *payer = c->payer;
    if (payer != NULL) {
        struct obj_header *o = object_find(v2p(payer));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("a core's turn is paid by something that is not a live thread", i, v2p(payer), 0);
        }
        if (c->turn == NULL || payer == c->turn || payer->core != i) {
            fail("a turn's payer is not another thread of its core", i, v2p(payer), payer->core);
        }
        if (payer->stamp != c->ticks) {
            fail("the ticks of a turn were not charged to its payer", v2p(payer), payer->stamp, c->ticks);
        }
        /*
         * It pays from the wait in which it lent the turn's thread its time, the first of the thread's lenders,
         * and once woken until the turn ends.
         */
        if (payer->state == THREAD_WAITING && (c->turn == NULL || c->turn->lenders != v2p(payer))) {
            fail("a turn's payer that waits is not the turn's first lender", i, v2p(payer), payer->lend_to);
        }
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
    check_queue(c->run_queue, THREAD_READY, 0, QUEUE_RUN, i);
    check_queue(c->spare_queue, THREAD_READY, 0, QUEUE_SPARE, i);
    check_queue(c->spent_queue, THREAD_READY, 0, QUEUE_SPENT, i);
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
        if (unit_thread[i] == 0) {
            continue;
        }
        struct obj_header *o = object_find(unit_thread[i]);
        if (o == NULL || o->type != CAP_THREAD) {
            fail("a unit names something that is not a live thread", i, unit_thread[i], 0);
        }
        named++;
    }
    if (named != units_seen) {
        fail("a unit names a thread that does not earn it", named, units_seen, 0);
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
    uint32_t balance = account_now(t);
    bool time = balance >= tick_parts();
    bool alone = thread_spare(t) && core->spare_queue == 0;
    if (core->run_queue != 0 || (!time && !alone)) {
        fail("the timer lets a tick pass that ends the turn", wake, v2p(t), balance);
    }
    /* A thread bound to another core's units during its turn here goes there at the next tick. */
    if (t->time.type == CAP_BOUND && unit_core(t->time.a) != core_index()) {
        fail("the timer lets the tick pass that moves the turn's thread to its core", wake, v2p(t), t->time.a);
    }
    /*
     * On the tick before the timer's the account still has a tick in it, unless it may go on alone on spare time:
     * the next tick it pays for the rest of this one, from turn_from, and gains its units, up to its cap,
     * and every tick after costs it a tick less the gain.
     */
    if (time && !alone) {
        uint32_t next = balance - COUNT_PARTS * (timer_tick_counts() - core->turn_from) + tick_gain(t);
        next = next < account_cap(t) ? next : account_cap(t);
        if (next < tick_parts() || (uint64_t)(wake - 2) * (tick_parts() - tick_gain(t)) > next - tick_parts()) {
            fail("the timer lets the tick pass that drains the turn's account", wake, balance, thread_units(t));
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
        if (s->type != CAP_SIGNALLED || n == NULL || n->type != CAP_NOTIFICATION || (s->rights & RIGHT_W) == 0 ||
            s->b == 0 || s->index != 0) {
            fail("irq's notification slot names no live notification", v2p(i), s->type, s->a);
        }
        if (s->child != 0) {
            fail("something is derived from an irq's notification", v2p(i), s->child, 0);
        }
        if (i->bits & ~s->b) {
            fail("irq is armed with bits its notification capability may not signal", v2p(i), i->bits, s->b);
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
        case CAP_DEBUG:
        case CAP_CLOCK:
            break;
        case CAP_FRAME: {
            uint8_t granted = granted_rights(c->a, c->b);
            if (c->b == 0 || granted == 0) {
                fail("frame outside granted memory", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("frame with rights beyond the grant", v2p(table), i, c->rights);
            }
            if (!napot_block(c->a, c->b)) {
                fail("frame is not a NAPOT block", v2p(table), i, c->a);
            }
            break;
        }
        case CAP_UNTYPED: {
            /* The block word ends in a one for every block of at least eight bytes. */
            uint32_t base = untyped_base(c), size = untyped_size(c);
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
            if (c->b == 0 || c->a >= LINES || c->b > LINES - c->a) {
                fail("line capability outside the lines there are", v2p(table), i, c->a);
            }
            if (c->rights & ~RIGHT_W) {
                fail("line capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        case CAP_TIME:
            /* The root task received every unit there is, with RIGHT_W and RIGHT_X; nothing widens that. */
            if (c->b == 0 || c->a >= MACHINE_UNITS || c->b > MACHINE_UNITS - c->a) {
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
            /* A notification's capability may signal some bit, and no other carries any. */
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
 * the table slot and region slots of every live process,
 * the process, units and watch of every live thread
 * and the notification of every live Irq,
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
 * or an object built on the parent's range, a pool on a region, an Irq on a line,
 * a thread's process, a thread's watch or an Irq's notification on a capability to it,
 * a thread's units on the units it was bound through,
 * or the counter's block, or a region installed from it, below the clock.
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
    case CAP_INSTALLED:
        *base = n->a;
        *size = n->b;
        return true;
    case CAP_UNTYPED:
        *base = untyped_base(n);
        *size = untyped_size(n);
        return true;
    case CAP_RETYPED:
        *base = n->a;
        *size = ((const struct pool *)p2v(n->a))->size;
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
    return type == CAP_CAPTABLE || type == CAP_PROCESS || type == CAP_THREAD ||
           type == CAP_NOTIFICATION || type == CAP_IRQ || type == CAP_HOSTED ||
           type == CAP_SIGNALLED || type == CAP_WATCHED;
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
        if (c->type != CAP_UNTYPED && c->type != CAP_FRAME && c->type != CAP_INSTALLED &&
            c->type != CAP_RETYPED) {
            return false;
        }
        node_range(c, &base, &size);
        node_range(p, &pbase, &psize);
        return (narrower || c->type == CAP_RETYPED) && range_within(base, size, pbase, psize);
    }
    if (c->type == p->type) {
        if (c->type == CAP_FRAME || c->type == CAP_IRQ_LINE || c->type == CAP_TIME) {
            return narrower && range_within(c->a, c->b, p->a, p->b);
        }
        /* A notification's capability carries the bits it may signal, which narrow as rights do. */
        return narrower && (c->type == CAP_DEBUG || c->type == CAP_CLOCK || c->a == p->a) &&
               (c->type != CAP_NOTIFICATION || !(c->b & ~p->b));
    }
    /*
     * A clock that reads gives out one frame, the counter's block, read only,
     * and adopts what was carved or installed from it when a delete or a revoke takes it.
     */
    if ((c->type == CAP_FRAME || c->type == CAP_INSTALLED) && p->type == CAP_CLOCK) {
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        return (p->rights & RIGHT_R) && !(c->rights & ~g->rights) && range_within(c->a, c->b, g->base, g->size);
    }
    if (c->type == CAP_INSTALLED && p->type == CAP_FRAME) {
        return narrower && range_within(c->a, c->b, p->a, p->b);
    }
    /* A thread's units lie among those it was bound through, and the first of them does even for none. */
    if (c->type == CAP_BOUND && p->type == CAP_TIME) {
        return narrower && c->a - p->a < p->b && c->b <= p->b - (c->a - p->a);
    }
    /*
     * A thread's process is the process it was made with, its watch the notification it was set with,
     * and an Irq's notification the one it was bound to, each signalling only bits that capability may.
     */
    if (c->type == CAP_HOSTED && p->type == CAP_PROCESS) {
        return narrower && c->a == p->a;
    }
    if ((c->type == CAP_WATCHED || c->type == CAP_SIGNALLED) && p->type == CAP_NOTIFICATION) {
        return narrower && c->a == p->a && !(c->b & ~p->b);
    }
    /* Below a pool's node lies every capability to the pool and to its objects. */
    if (p->type == CAP_RETYPED || p->type == CAP_POOL) {
        if (c->type == CAP_POOL) {
            return c->a == p->a;
        }
        /* Objects built in a pool carry rights of their own. */
        return is_object_type(c->type) && cap_object(c)->pool == p->a;
    }
    return false;
}

/* One node's links; bound is more than the nodes there are, so a walk that long is a cycle. */
static void check_node(const struct cap *n, uint32_t bound)
{
    if (n->type == CAP_NONE) {
        if (n->child != 0 || n->next != 0 || n->prev != 0) {
            fail("empty slot keeps links", v2p(n), n->child, n->next);
        }
        return;
    }
    if (n->next == 0 || (n->child & LINK_UP)) {
        fail("filled slot has malformed links", v2p(n), n->child, n->next);
    }
    if (n->child != 0) {
        const struct cap *c = live_node(n->child);
        if (c == NULL || c->type == CAP_NONE) {
            fail("child link to a slot that is not a live, filled one", v2p(n), n->child, 0);
        }
    }

    /* Along the ring to the parent, every sibling live and filled. */
    uint32_t link = n->next;
    uint32_t steps = 0;
    while (!(link & LINK_UP)) {
        const struct cap *s = live_node(link);
        if (s == NULL || s->type == CAP_NONE) {
            fail("link to a slot that is not a live, filled one", v2p(n), link, 0);
        }
        if (++steps > bound) {
            fail("sibling ring does not close", v2p(n), link, 0);
        }
        link = s->next;
    }
    if (link == LINK_UP) {
        /* A root: roots have no ring, so nothing may lead here through siblings. */
        if (steps != 0) {
            fail("sibling ring closes on no parent", v2p(n), 0, 0);
        }
        if (n->prev != 0) {
            fail("root has a predecessor", v2p(n), n->prev, 0);
        }
        /* Every capability to a pool or its objects lies below the pool's own node. */
        if (n->type == CAP_POOL || is_object_type(n->type)) {
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
    link = parent->child;
    for (const struct cap *s = live_node(link); s != n; s = live_node(link)) {
        if ((link & LINK_UP) || s == NULL || ++steps > bound) {
            fail("parent's ring does not reach the slot", v2p(n), v2p(parent), link);
        }
        link = s->next;
    }
    /* The predecessor is the sibling whose next is this node; the first child's is the last. */
    const struct cap *pred = live_node(n->prev);
    if (pred == NULL || pred->type == CAP_NONE || (n->prev & LINK_UP)) {
        fail("predecessor link to a slot that is not a live, filled one", v2p(n), n->prev, 0);
    }
    bool first = parent->child == v2p(n);
    if (first ? pred->next != (v2p(parent) | LINK_UP) : pred->next != v2p(n)) {
        fail("predecessor's next is not the slot", v2p(n), n->prev, pred->next);
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
    uint32_t link = n->next;
    while (!(link & LINK_UP)) {
        link = live_node(link)->next;
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
 * True if an Untyped is a root with something made of it below it.
 * A delete below a root makes roots of its children one by one,
 * which lie in its memory until it goes with the last of them;
 * it makes nothing meanwhile, so nothing it could make meets them.
 */
static bool made_root(const struct cap *u)
{
    return u->type == CAP_UNTYPED && !untyped_free(u) && u->next == LINK_UP;
}

/*
 * Memory that may become kernel memory, an Untyped or a pool,
 * overlaps another node standing for memory only where one lies below the other,
 * or where one of them is a root Untyped that made something,
 * which a delete below it leaves while it makes roots of its children one by one.
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
        if ((x->type != CAP_UNTYPED && x->type != CAP_RETYPED) || !node_range(x, &xbase, &xsize)) {
            continue;
        }
        node_enter(&jt, object_first());
        for (const struct cap *y = node_next(&jt); y != NULL; y = node_next(&jt)) {
            uint32_t ybase, ysize;
            if (y == x || !node_range(y, &ybase, &ysize) || !ranges_overlap(xbase, xsize, ybase, ysize)) {
                continue;
            }
            if (!lies_below(x, y) && !lies_below(y, x) && !made_root(x) && !made_root(y)) {
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
            check_queue(((struct notification *)o)->waiters, THREAD_WAITING, v2p(o), QUEUE_NONE, CORES);
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
        uint32_t addr[PMP_MAX_ENTRIES];
        uint8_t cfg[PMP_MAX_ENTRIES];
        for (unsigned i = 0; i < pmp_entry_end; i++) {
            pmp_get(i, &addr[i], &cfg[i]);
        }
        check_pmp_image(thread_process(current), addr, cfg, pmp_entry_end, true);
    }
}
