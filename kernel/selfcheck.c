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

static void check_pools(void)
{
    /* The boot pool is kernel memory; nothing granted may reach it. */
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        const struct granted_range *g = &boot_granted[i];
        if (g->size && ranges_overlap(g->base, g->size, pool_base(boot_pool), boot_pool->size)) {
            fail("granted memory overlaps the boot pool", g->base, g->size, 0);
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
        if (p != boot_pool && granted_rights(base, p->size) != RIGHT_ALL) {
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

static void check_thread(const struct thread *t)
{
    struct obj_header *p = object_find(t->proc);
    if (p == NULL || p->type != CAP_PROCESS) {
        fail("thread has no live process", v2p(t), t->proc, 0);
    }
    if (p->pool != t->hdr.pool) {
        fail("thread and its process live in different pools", v2p(t), 0, 0);
    }
    if (t->flags & ~(uint8_t)THREAD_UNTRACED) {
        fail("thread has unknown flags", v2p(t), t->flags, 0);
    }
    /* A thread's share is a leaf of the tree, which the tree check reads the links of. */
    const struct cap *b = &t->share;
    if (b->type != CAP_NONE &&
        (b->type != CAP_BOUND || b->rights != RIGHT_W || b->a >= SHARES || b->b != 0 || b->index != 0)) {
        fail("thread's share is malformed", v2p(t), b->type, b->a);
    }
    if (b->type != CAP_NONE && b->child != 0) {
        fail("something is derived from a thread's share", v2p(t), b->child, 0);
    }
    switch (t->state) {
    case THREAD_STOPPED:
    case THREAD_READY:
        if (t->waiting_on != 0) {
            fail("runnable thread waits on something", v2p(t), t->waiting_on, t->state);
        }
        if (t->state == THREAD_READY && t != current && b->type == CAP_BOUND) {
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
 * none of them running, and each in the state and waiting on what the queue is for,
 * and on a share's ring bound to that share.
 * The back links make the walk close at the oldest or fail:
 * no thread can be entered twice from two different predecessors.
 */
static void check_queue(paddr_t head, uint8_t state, paddr_t waiting_on, const struct share *share)
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
        if (t->state != state || t->waiting_on != waiting_on || t == current ||
            (share != NULL && thread_share(t) != share)) {
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

/* True if p points at one of the shares. */
static bool is_share(const struct share *p)
{
    for (unsigned i = 0; i < SHARES; i++) {
        if (p == &shares[i]) {
            return true;
        }
    }
    return false;
}

/*
 * Every share's ring holds its ready threads but the running one, which check_queue reads,
 * and the run queue is a ring of exactly the shares with a ready thread but the one whose turn it is.
 * Something runs, so it is some share's turn.
 */
static void check_shares(void)
{
    if (current != NULL && (turn == NULL || !is_share(turn))) {
        fail("it is no share's turn", 0, 0, 0);
    }
    uint32_t owed = 0, queued = 0;
    for (unsigned i = 0; i < SHARES; i++) {
        const struct share *s = &shares[i];
        check_queue(s->threads, THREAD_READY, 0, s);
        bool on = s->next != NULL || s->prev != NULL;
        if (on != (s->threads != 0 && s != turn)) {
            fail(on ? "run queue holds a share it is not for" : "a share with a ready thread is not on the run queue",
                 i, s->threads, 0);
        }
        owed += on;
    }
    const struct share *at = run_queue;
    while (at != NULL) {
        if (!is_share(at) || !is_share(at->next) || at->next->prev != at) {
            fail("run queue is not linked back", (uint32_t)(at - shares), 0, 0);
        }
        if (++queued > SHARES) {
            fail("run queue does not close", queued, 0, 0);
        }
        at = at->next;
        if (at == run_queue) {
            break;
        }
    }
    if (owed != queued) {
        fail("run queue does not hold every share it is for", owed, queued, 0);
    }
}

/* The Irqs the walk found armed on a device's line or a timer line, to hold armed_sources to. */
static uint32_t armed_seen;

static void check_irq(const struct irq *i)
{
    struct obj_header *n = object_find(i->ntfn);
    if (n == NULL || n->type != CAP_NOTIFICATION) {
        fail("irq has no live notification", v2p(i), i->ntfn, 0);
    }
    if (n->pool != i->hdr.pool) {
        fail("irq and its notification live in different pools", v2p(i), 0, 0);
    }
    if (i->line >= LINES) {
        fail("irq names a line there is not", v2p(i), i->line, 0);
    }
    /* The tick fires every due timer line, so one still armed lies ahead. */
    if (line_is_timer(i->line) && irq_armed(i) && irq_due(i, sched_ticks)) {
        fail("armed timer line is due", v2p(i), i->deadline, sched_ticks);
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
            if (c->b > size) {
                fail("untyped's watermark lies past its end", v2p(table), i, c->b);
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
        case CAP_SHARE:
            /* The root task received every share there is, with RIGHT_W; nothing widens that. */
            if (c->b == 0 || c->a >= SHARES || c->b > SHARES - c->a) {
                fail("share capability outside the shares there are", v2p(table), i, c->a);
            }
            if (c->rights & ~RIGHT_W) {
                fail("share capability with rights beyond the grant", v2p(table), i, c->rights);
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
 * the table slot and region slots of every live process
 * and the share of every live thread,
 * linked by physical address and never checked on use,
 * so every link must land on a live node and the shape must be exactly a forest:
 * the children of a node form one ring that closes through the node,
 * each predecessor link is a next link turned round,
 * and from every node the links up reach a root.
 * A destroy that left a link into freed memory, a delete that broke a ring,
 * or a revoke that stopped short all show up here.
 * A node is derived from its parent: the same object with no more rights,
 * a range within the parent's with no more rights,
 * or an object built on the parent's range, a pool on a region, an Irq on a line,
 * a thread's share on the shares it was bound through,
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

static bool is_object_type(uint8_t type)
{
    return type == CAP_CAPTABLE || type == CAP_PROCESS || type == CAP_THREAD ||
           type == CAP_NOTIFICATION || type == CAP_IRQ;
}

static bool derived_from(const struct cap *c, const struct cap *p)
{
    bool narrower = !(c->rights & ~p->rights);
    uint32_t base, size, pbase, psize;
    /*
     * What an Untyped made lies below its watermark, and so does what came up to it
     * from something it made that went; a pool's own node carries rights of its own.
     */
    if (p->type == CAP_UNTYPED) {
        if (c->type != CAP_UNTYPED && c->type != CAP_FRAME && c->type != CAP_INSTALLED &&
            c->type != CAP_RETYPED) {
            return false;
        }
        node_range(c, &base, &size);
        node_range(p, &pbase, &psize);
        return (narrower || c->type == CAP_RETYPED) && range_within(base, size, pbase, p->b);
    }
    if (c->type == p->type) {
        if (c->type == CAP_FRAME || c->type == CAP_IRQ_LINE || c->type == CAP_SHARE) {
            return narrower && range_within(c->a, c->b, p->a, p->b);
        }
        return narrower && (c->type == CAP_DEBUG || c->type == CAP_CLOCK || c->a == p->a);
    }
    /*
     * The clock gives out one frame, the counter's block, read only,
     * and adopts what was carved or installed from it when a delete or a revoke takes it.
     */
    if ((c->type == CAP_FRAME || c->type == CAP_INSTALLED) && p->type == CAP_CLOCK) {
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        return !(c->rights & ~g->rights) && range_within(c->a, c->b, g->base, g->size);
    }
    if (c->type == CAP_INSTALLED && p->type == CAP_FRAME) {
        return narrower && range_within(c->a, c->b, p->a, p->b);
    }
    /* A thread's share is one of the shares it was bound through. */
    if (c->type == CAP_BOUND && p->type == CAP_SHARE) {
        return narrower && c->a - p->a < p->b;
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
    for (link = parent->child; live_node(link) != n; link = live_node(link)->next) {
        if ((link & LINK_UP) || live_node(link) == NULL || ++steps > bound) {
            fail("parent's ring does not reach the slot", v2p(n), v2p(parent), link);
        }
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
    uint32_t i;
};

static const struct cap *node_next(struct node_iter *it)
{
    while (it->o != NULL) {
        uint32_t count;
        const struct cap *nodes = obj_nodes(it->o, &count);
        if (it->i < count) {
            return &nodes[it->i++];
        }
        it->o = object_next(it->o);
        it->i = 0;
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
    struct node_iter it = { object_first(), 0 };
    while (node_next(&it) != NULL) {
        tree_bound++;
    }
    it = (struct node_iter){ object_first(), 0 };
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        check_node(n, tree_bound);
    }
    /* A forest: from every node the links up reach a root. */
    it = (struct node_iter){ object_first(), 0 };
    for (const struct cap *n = node_next(&it); n != NULL; n = node_next(&it)) {
        if (n->type != CAP_NONE) {
            lies_below(n, NULL);
        }
    }
}

/*
 * True if an Untyped will make nothing more of [base, base + size):
 * it lies below the watermark, which stays while the Untyped has made something.
 */
static bool behind_mark(const struct cap *u, uint32_t base, uint32_t size)
{
    return u->type == CAP_UNTYPED && u->child != 0 && range_within(base, size, untyped_base(u), u->b);
}

/*
 * Memory that may become kernel memory, an Untyped or a pool,
 * overlaps another node standing for memory only where one lies below the other,
 * or where an Untyped overlaps what lies behind its watermark,
 * which a delete below a root Untyped leaves while it makes roots of its children one by one.
 * So a frame, which never lies above or below a pool, overlaps none,
 * and neither does anything two siblings below one Untyped stand for:
 * the watermark kept them apart.
 */
static void check_nesting(void)
{
    struct node_iter it = { object_first(), 0 };
    for (const struct cap *x = node_next(&it); x != NULL; x = node_next(&it)) {
        uint32_t xbase, xsize;
        if ((x->type != CAP_UNTYPED && x->type != CAP_RETYPED) || !node_range(x, &xbase, &xsize)) {
            continue;
        }
        struct node_iter jt = { object_first(), 0 };
        for (const struct cap *y = node_next(&jt); y != NULL; y = node_next(&jt)) {
            uint32_t ybase, ysize;
            if (y == x || !node_range(y, &ybase, &ysize) || !ranges_overlap(xbase, xsize, ybase, ysize)) {
                continue;
            }
            if (!lies_below(x, y) && !lies_below(y, x) && !behind_mark(x, ybase, ysize) &&
                !behind_mark(y, xbase, xsize)) {
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
            check_queue(((struct notification *)o)->waiters, THREAD_WAITING, v2p(o), NULL);
            break;
        case CAP_IRQ:
            check_irq((struct irq *)o);
            break;
        default:
            break;
        }
    }
    check_shares();
    /* Every queued thread is one its queue is for, so equal counts leave none out. */
    if (owed_threads != queued_threads) {
        fail("a waiting or ready thread is on no queue", owed_threads, queued_threads, 0);
    }
    /* The stall in wfi reads the count instead of walking for a source. */
    if (armed_seen != armed_sources) {
        fail("the count of armed sources is wrong", armed_seen, armed_sources, 0);
    }
    check_lines();
    /* Every node has been vetted as a capability by now; the tree check reads only links. */
    check_tree();
    check_nesting();

    if (current != NULL) {
        struct obj_header *o = object_find(v2p(current));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("current thread is not a live object", v2p(current), 0, 0);
        }
        /* It may have lost its share during its turn, which it finishes. */
        if (current->state != THREAD_READY) {
            fail("the running thread is not runnable", v2p(current), current->state, 0);
        }
        /* The CSRs must carry exactly the current process's image. */
        uint32_t addr[PMP_MAX_ENTRIES];
        uint8_t cfg[PMP_MAX_ENTRIES];
        for (unsigned i = 0; i < pmp_entry_count; i++) {
            pmp_get(i, &addr[i], &cfg[i]);
        }
        check_pmp_image(thread_process(current), addr, cfg, pmp_entry_count, true);
    }
}
