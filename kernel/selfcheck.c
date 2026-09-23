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
    kputs(msg);
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
        if (g->size && ranges_overlap(g->base, g->size, boot_pool->base, boot_pool->size)) {
            fail("granted memory overlaps the boot pool", g->base, g->size, 0);
        }
    }

    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        if (p->hdr.type != CAP_POOL || obj_pool(&p->hdr) != p || v2p(p) != p->base) {
            fail("pool descriptor is malformed", v2p(p), 0, 0);
        }
        if (p->used < sizeof(*p) || p->used > p->size) {
            fail("pool used mark outside the pool", p->base, p->used, p->size);
        }
        if ((p->base | p->size) & (OBJ_ALIGN - 1)) {
            fail("pool is not aligned", p->base, p->size, 0);
        }
        if (p != boot_pool && granted_rights(p->base, p->size) != RIGHT_ALL) {
            fail("pool lies outside memory granted with full rights", p->base, p->size, 0);
        }
        for (struct pool *q = pool_next_pool(p); q != NULL; q = pool_next_pool(q)) {
            if (ranges_overlap(p->base, p->size, q->base, q->size)) {
                fail("pools overlap", p->base, q->base, 0);
            }
        }

        /*
         * Pools form a tree rooted at the boot pool.
         * A parent is named by address and not checked on use,
         * so it must be live, and older: later on the list.
         * Then parents lead to the last pool, which has to be the boot pool,
         * and a destroy that spared a pool below it shows up here.
         */
        if ((p == boot_pool) != (p->parent == 0)) {
            fail("only the boot pool has no parent", p->base, p->parent, 0);
        }
        if (p->parent != 0) {
            struct pool *q = pool_next_pool(p);
            while (q != NULL && v2p(q) != p->parent) {
                q = pool_next_pool(q);
            }
            if (q == NULL) {
                fail("pool's parent is not a live, older pool", p->base, p->parent, 0);
            }
        }

        /*
         * Objects tile the pool from the descriptor to the used mark.
         * This is the one walk that does not use object_first/object_next:
         * those cross pools by an object's own header,
         * which is what this loop is here to check.
         */
        paddr_t at = p->base + aligned_size(&p->hdr);
        for (struct obj_header *o = pool_first(p); o != NULL; o = pool_next(p, o)) {
            if (v2p(o) != at) {
                fail("object walk skipped", p->base, at, v2p(o));
            }
            if (obj_pool(o) != p) {
                fail("object claims another pool", at, o->pool, p->base);
            }
            if (o->type != CAP_CAPTABLE && o->type != CAP_PROCESS &&
                o->type != CAP_THREAD && o->type != CAP_NOTIFICATION &&
                o->type != CAP_TIMER && o->type != CAP_IRQ) {
                fail("object has an unexpected type", at, o->type, 0);
            }
            at += aligned_size(o);
            if (at > p->base + p->used) {
                fail("object runs past the used mark", v2p(o), at, p->base + p->used);
            }
        }
        if (at != p->base + p->used) {
            fail("objects do not end at the used mark", p->base, at, p->base + p->used);
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
            if (ranges_overlap(s->a, s->b, p->base, p->size)) {
                fail("process maps a pool: isolation broken", v2p(proc), s->a, p->base);
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

    struct obj_header *t = object_find(proc->ctable);
    if (t == NULL || t->type != CAP_CAPTABLE) {
        fail("process has no live capability table", v2p(proc), proc->ctable, 0);
    }
    if (t->pool != proc->hdr.pool) {
        fail("process and its table live in different pools", v2p(proc), 0, 0);
    }
}

/* Waiting threads, and threads on some notification's ring; the two counts must agree. */
static uint32_t waiting_threads, queued_threads;

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
    switch (t->state) {
    case THREAD_STOPPED:
    case THREAD_READY:
        if (t->waiting_on != 0 || t->wait_next != 0 || t->wait_prev != 0) {
            fail("runnable thread waits on something", v2p(t), t->waiting_on, t->state);
        }
        break;
    case THREAD_WAITING: {
        struct obj_header *n = object_find(t->waiting_on);
        if (n == NULL || n->type != CAP_NOTIFICATION) {
            fail("thread waits on something that is not a notification",
                 v2p(t), t->waiting_on, 0);
        }
        waiting_threads++;
        break;
    }
    default:
        fail("thread is in an unknown state", v2p(t), t->state, 0);
    }
}

/*
 * A notification's waiters are a ring of live threads waiting on it,
 * each linked back to the one before.
 * The back links make the walk close at the oldest or fail:
 * no thread can be entered twice from two different predecessors.
 */
static void check_notification(const struct notification *n)
{
    if (n->waiters == 0) {
        return;
    }
    paddr_t at = n->waiters;
    do {
        struct obj_header *o = object_find(at);
        if (o == NULL || o->type != CAP_THREAD) {
            fail("notification queues something that is not a thread", v2p(n), at, 0);
        }
        const struct thread *t = (const struct thread *)o;
        if (t->state != THREAD_WAITING || t->waiting_on != v2p(n)) {
            fail("notification queues a thread that does not wait on it", v2p(n), at, t->state);
        }
        struct obj_header *next = object_find(t->wait_next);
        if (next == NULL || next->type != CAP_THREAD ||
            ((const struct thread *)next)->wait_prev != at) {
            fail("notification's queue is not linked back", v2p(n), at, t->wait_next);
        }
        queued_threads++;
        at = t->wait_next;
    } while (at != n->waiters);
}

static void check_timer(const struct timer *t)
{
    struct obj_header *n = object_find(t->ntfn);
    if (n == NULL || n->type != CAP_NOTIFICATION) {
        fail("timer has no live notification", v2p(t), t->ntfn, 0);
    }
    if (n->pool != t->hdr.pool) {
        fail("timer and its notification live in different pools", v2p(t), 0, 0);
    }
    /* The tick fires every due timer, so one still armed lies ahead. */
    if (timer_armed(t) && timer_due(t, sched_ticks)) {
        fail("armed timer is due", v2p(t), t->deadline, sched_ticks);
    }
}

static void check_irq(const struct irq *i)
{
    struct obj_header *n = object_find(i->ntfn);
    if (n == NULL || n->type != CAP_NOTIFICATION) {
        fail("irq has no live notification", v2p(i), i->ntfn, 0);
    }
    if (n->pool != i->hdr.pool) {
        fail("irq and its notification live in different pools", v2p(i), 0, 0);
    }
    if (i->line >= IRQ_LINES) {
        fail("irq names a line the controller does not have", v2p(i), i->line, 0);
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
}

/*
 * The controller forwards a line exactly while an Irq is armed on it,
 * and at most one Irq is bound to any line, the log's included,
 * which the line table records exactly.
 * check_irq has vetted each object, so the bitmaps below are in range.
 * The enable bits are read back from the controller,
 * as the PMP CSRs are read back for the running process;
 * the log's line has no controller and check_irq reads its level instead.
 */
static void check_lines(void)
{
    uint32_t bound[(IRQ_LINES + 31) / 32] = { 0 };
    uint32_t armed[(IRQ_LINES + 31) / 32] = { 0 };

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
    for (uint32_t line = 0; line < IRQ_LINES; line++) {
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
            break;
        case CAP_REGION: {
            uint8_t granted = granted_rights(c->a, c->b);
            if (c->b == 0 || granted == 0) {
                fail("region capability outside granted memory", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("region capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            if (!napot_block(c->a, c->b)) {
                fail("region capability is not a NAPOT block", v2p(table), i, c->a);
            }
            break;
        }
        case CAP_IRQ_LINE:
            /* The root task received the log's line and the controller's, with RIGHT_W; nothing widens that. */
            if (c->b == 0 || c->a >= IRQ_LINES || c->b > IRQ_LINES - c->a) {
                fail("line capability outside the controller", v2p(table), i, c->a);
            }
            if (c->rights & ~RIGHT_W) {
                fail("line capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        case CAP_POOL:
        case CAP_CAPTABLE:
        case CAP_PROCESS:
        case CAP_THREAD:
        case CAP_NOTIFICATION:
        case CAP_TIMER:
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
 * Its nodes are the slots of every live table and the region slots of every live process,
 * linked by physical address and never checked on use,
 * so every link must land on a live node and the shape must be exactly a forest:
 * the children of a node form one ring that closes through the node,
 * and from every node the links up reach a root.
 * A destroy that left a link into freed memory, a delete that broke a ring,
 * or a revoke that stopped short all show up here.
 * A node is derived from its parent: the same object with no more rights,
 * a range within the parent's with no more rights,
 * or an object built on the parent's range, a pool on a region, an Irq on a line.
 */

/* The node a link names, if it is a slot of a live table or a region slot of a live process. */
static const struct cap *live_node(uint32_t link)
{
    paddr_t at = link & ~LINK_UP;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        const struct cap *slots;
        uint32_t count;
        if (o->type == CAP_CAPTABLE) {
            slots = ((const struct captable *)o)->slots;
            count = ((const struct captable *)o)->nslots;
        } else if (o->type == CAP_PROCESS) {
            slots = ((const struct process *)o)->slots;
            count = PROCESS_REGION_SLOTS;
        } else {
            continue;
        }
        uint32_t off = at - v2p(slots);
        if (off < count * sizeof(*slots) && off % sizeof(*slots) == 0) {
            return &slots[off / sizeof(*slots)];
        }
    }
    return NULL;
}

/* True if [base, base + size) lies within [in, in + insize). */
static bool range_within(uint32_t base, uint32_t size, uint32_t in, uint32_t insize)
{
    return base >= in && base - in <= insize - size && size <= insize;
}

static bool derived_from(const struct cap *c, const struct cap *p)
{
    bool narrower = !(c->rights & ~p->rights);
    if (c->type == p->type) {
        if (c->type == CAP_REGION || c->type == CAP_IRQ_LINE) {
            return narrower && range_within(c->a, c->b, p->a, p->b);
        }
        return narrower && (c->type == CAP_DEBUG || c->a == p->a);
    }
    if (c->type == CAP_INSTALLED && p->type == CAP_REGION) {
        return narrower && range_within(c->a, c->b, p->a, p->b);
    }
    /* Objects built on a range carry rights of their own. */
    if (c->type == CAP_POOL && p->type == CAP_REGION) {
        const struct pool *pool = (const struct pool *)cap_object(c);
        return range_within(pool->base, pool->size, p->a, p->b);
    }
    if (c->type == CAP_IRQ && p->type == CAP_IRQ_LINE) {
        return range_within(((const struct irq *)cap_object(c))->line, 1, p->a, p->b);
    }
    return false;
}

/* One node's links; bound is more than the nodes there are, so a walk that long is a cycle. */
static void check_node(const struct cap *n, uint32_t bound)
{
    if (n->type == CAP_NONE) {
        if (n->child != 0 || n->next != 0) {
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
}

static void check_tree(void)
{
    uint32_t bound = 1;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_CAPTABLE) {
            bound += ((const struct captable *)o)->nslots;
        } else if (o->type == CAP_PROCESS) {
            bound += PROCESS_REGION_SLOTS;
        }
    }
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_CAPTABLE) {
            const struct captable *t = (const struct captable *)o;
            for (uint32_t i = 0; i < t->nslots; i++) {
                check_node(&t->slots[i], bound);
            }
        } else if (o->type == CAP_PROCESS) {
            const struct process *p = (const struct process *)o;
            for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
                check_node(&p->slots[i], bound);
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
    waiting_threads = queued_threads = 0;
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
            check_notification((struct notification *)o);
            break;
        case CAP_TIMER:
            check_timer((struct timer *)o);
            break;
        case CAP_IRQ:
            check_irq((struct irq *)o);
            break;
        default:
            break;
        }
    }
    /* Every queued thread waits on the notification it is queued on, so equal counts leave none out. */
    if (waiting_threads != queued_threads) {
        fail("a waiting thread is on no notification's queue", waiting_threads, queued_threads, 0);
    }
    check_lines();
    /* Every node has been vetted as a capability by now; the tree check reads only links. */
    check_tree();

    if (current != NULL) {
        struct obj_header *o = object_find(v2p(current));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("current thread is not a live object", v2p(current), 0, 0);
        }
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
