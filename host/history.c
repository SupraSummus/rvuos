/*
 * The properties "Authority only flows", "Memory crosses zeroed",
 * "The caller keeps what it runs on", "A tick charges" and "A move keeps a node's place" of DESIGN.md,
 * which relate the state before a call to the state after it.
 * host_syscall runs history_begin before each call and history_end after it.
 *
 * The prologue runs untraced, with no self-check before these checks, so a node may hold anything:
 * an address it holds is looked up among the objects and pools walked, never followed.
 * Only a traced call follows links, since the self-check vetted the tree after the call before
 * and, in syscall_dispatch, after this one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "timer.h"

/* What a capability grants, as the covering rule compares it: a range, an object, or the debug capability. */
struct grant {
    uint8_t type;
    uint8_t rights;
    uint32_t a, b;
};

/* A filled node before the call. */
struct node_rec {
    uint32_t at;
    struct grant g;
};

/* An object, and the pool it lies in. */
struct obj_rec {
    uint32_t at, size, pool;
};

/* Growing arrays, kept from call to call. */
#define ARRAY(T) struct { T *v; size_t n, cap; }

typedef ARRAY(struct obj_rec) obj_array;
typedef ARRAY(uint32_t) addr_array;

static ARRAY(struct grant) held;
static ARRAY(struct node_rec) nodes;
static obj_array objects, objects_after;

/* The thread that makes the call, its process and table, and whether a destroy had begun on one's pool. */
static const struct thread *caller;
static const struct process *caller_process;
static const struct captable *caller_table;
static bool caller_dying;

/*
 * A traced OP_CAP_MOVE that names a filled source and a destination slot:
 * the two slots, what the source held, the node it hung below, 0 for none,
 * and the nodes that hung below it, in their order.
 */
static struct cap *move_from, *move_to;
static struct cap move_was;
static uint32_t move_parent;
static addr_array move_children, children_after;

/* The count before the call, and the thread whose turn it was, NULL for none, with its units and its account. */
static uint32_t ticks_before;
static const struct thread *charged;
static struct cap charged_units;
static uint32_t charged_balance;

/* A thread's account at the count, as its stamp and its units make it, held to the cap. */
static uint32_t account_at_count(const struct thread *t)
{
    uint32_t cap = thread_units(t) * ACCOUNT_TICKS;
    uint64_t balance = t->balance + (uint64_t)(sched_ticks - t->stamp) * thread_units(t);
    return balance > cap ? cap : (uint32_t)balance;
}

static bool dying_under(const struct thread *t, const struct process *proc, const struct captable *table)
{
    return obj_pool(&t->hdr)->dying || (proc != NULL && obj_pool(&proc->hdr)->dying) ||
           (table != NULL && obj_pool(&table->hdr)->dying);
}

#define PUSH(arr, x)                                                           \
    do {                                                                       \
        if ((arr).n == (arr).cap) {                                            \
            (arr).cap = (arr).cap ? 2 * (arr).cap : 64;                        \
            (arr).v = realloc((arr).v, (arr).cap * sizeof((arr).v[0]));        \
            if ((arr).v == NULL) {                                             \
                abort();                                                       \
            }                                                                  \
        }                                                                      \
        (arr).v[(arr).n++] = (x);                                              \
    } while (0)

__attribute__((noreturn)) static void violated(const char *what)
{
    fprintf(stderr, "invariant violated: %s\n", what);
    host_violated();
}

/*
 * An installed region grants what the frame it came from did, a thread's units those units,
 * a thread's process what a capability to it does, and an Untyped its block whatever it made.
 */
static struct grant grant_of(const struct cap *c)
{
    if (c->type == CAP_UNTYPED) {
        return (struct grant){ CAP_UNTYPED, c->rights, untyped_base(c), untyped_size(c) };
    }
    if (c->type == CAP_BOUND) {
        return (struct grant){ CAP_TIME, c->rights, c->a, c->b };
    }
    if (c->type == CAP_HOSTED) {
        return (struct grant){ CAP_PROCESS, c->rights, c->a, 0 };
    }
    return (struct grant){ c->type == CAP_INSTALLED ? CAP_FRAME : c->type, c->rights, c->a, c->b };
}

static bool same_grant(struct grant x, struct grant y)
{
    return x.type == y.type && x.rights == y.rights && x.a == y.a && x.b == y.b;
}

/* Whether a capability the caller held grants all that g does; an Untyped grants the frames it makes. */
static bool covered(struct grant g)
{
    bool range = g.type == CAP_FRAME || g.type == CAP_UNTYPED || g.type == CAP_IRQ_LINE || g.type == CAP_TIME;
    for (size_t i = 0; i < held.n; i++) {
        const struct grant *h = &held.v[i];
        bool type = h->type == g.type || (g.type == CAP_FRAME && h->type == CAP_UNTYPED);
        if (type && (g.rights & ~h->rights) == 0 &&
            (range ? g.a - h->a < h->b && g.b <= h->b - (g.a - h->a) : g.a == h->a)) {
            return true;
        }
    }
    return false;
}

/* All records start with the address they are sorted by. */
static int cmp_at(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

static bool is_object(const obj_array *a, uint32_t at)
{
    return bsearch(&at, a->v, a->n, sizeof(a->v[0]), cmp_at) != NULL;
}

/* Every object in the pools, sorted. */
static void record_objects(obj_array *a)
{
    a->n = 0;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        PUSH(*a, ((struct obj_rec){ v2p(o), (uint32_t)obj_size(o), o->pool }));
    }
    qsort(a->v, a->n, sizeof(a->v[0]), cmp_at);
}

/* One memcmp rather than a comparison per byte, which the fuzzer's instrumentation would slow. */
static bool zero(uint32_t base, uint32_t size)
{
    const uint8_t *p = p2v(base);
    return size == 0 || (p[0] == 0 && memcmp(p, p + 1, size - 1) == 0);
}

static void each_node(const obj_array *a, void (*fn)(const struct cap *))
{
    for (size_t i = 0; i < a->n; i++) {
        uint32_t count;
        const struct cap *held_nodes = obj_nodes(p2v(a->v[i].at), &count);
        for (uint32_t j = 0; j < count; j++) {
            fn(&held_nodes[j]);
        }
    }
}

static void record_node(const struct cap *c)
{
    if (c->type != CAP_NONE) {
        PUSH(nodes, ((struct node_rec){ v2p(c), grant_of(c) }));
    }
}

/* The node a filled node hangs below, 0 for a root; the last sibling links up to it. */
static uint32_t parent_of(const struct cap *c)
{
    while (!(c->next & LINK_UP)) {
        c = p2v(c->next);
    }
    return c->next & ~LINK_UP;
}

/* The nodes below a node, first to last. */
static void children_of(const struct cap *c, addr_array *out)
{
    out->n = 0;
    for (uint32_t at = c->child; at != 0;) {
        const struct cap *n = p2v(at);
        PUSH(*out, at);
        at = (n->next & LINK_UP) ? 0 : n->next;
    }
}

/* Note a move's source and destination, as the kernel will resolve them. */
static void record_move(const struct captable *table)
{
    move_from = move_to = NULL;
    if (!debug_trace || caller == NULL || table == NULL || caller->frame.regs[REG_A7] != OP_CAP_MOVE) {
        return;
    }
    const struct trap_frame *f = &caller->frame;
    struct cap tc;
    if (cap_lookup((struct captable *)table, f->regs[REG_A0], &tc) != KERR_OK || tc.type != CAP_CAPTABLE) {
        return;
    }
    struct captable *dst = (struct captable *)cap_object(&tc);
    uint32_t to = f->regs[REG_A1], from = f->regs[REG_A2];
    if (to >= dst->nslots || from >= table->nslots || table->slots[from].type == CAP_NONE) {
        return;
    }
    move_from = (struct cap *)&table->slots[from];
    move_to = &dst->slots[to];
    move_was = *move_from;
    move_parent = parent_of(move_from);
    children_of(move_from, &move_children);
}

/* A move that succeeded left its source empty and put the same node where the source stood. */
static void check_move(void)
{
    if (move_from == NULL || caller->frame.regs[REG_A0] != KERR_OK) {
        return;
    }
    if (move_from->type != CAP_NONE) {
        violated("a move left its source filled");
    }
    if (move_to->type != move_was.type || move_to->rights != move_was.rights ||
        move_to->a != move_was.a || move_to->b != move_was.b) {
        violated("a move changed the capability it moved");
    }
    if (parent_of(move_to) != move_parent) {
        violated("a move did not keep the parent of the node it moved");
    }
    children_of(move_to, &children_after);
    if (children_after.n != move_children.n ||
        (move_children.n != 0 &&
         memcmp(children_after.v, move_children.v, move_children.n * sizeof(move_children.v[0])) != 0)) {
        violated("a move did not keep what was derived from the node it moved");
    }
}

void history_begin(void)
{
    held.n = nodes.n = 0;

    const struct captable *table = current != NULL ? thread_table(current) : NULL;
    caller = current;
    caller_process = current != NULL ? thread_process(current) : NULL;
    caller_table = table;
    caller_dying = caller != NULL && dying_under(caller, caller_process, table);
    for (uint32_t i = 0; table != NULL && i < table->nslots; i++) {
        const struct cap *c = &table->slots[i];
        if (c->type == CAP_NONE) {
            continue;
        }
        PUSH(held, grant_of(c));
        /* The clock holds the counter's frame, which OP_CLOCK_FRAME hands out. */
        if (c->type == CAP_CLOCK) {
            const struct granted_range *r = &boot_granted[GRANT_COUNTER];
            PUSH(held, ((struct grant){ CAP_FRAME, r->rights, r->base, r->size }));
        }
    }

    record_objects(&objects);
    each_node(&objects, record_node);
    qsort(nodes.v, nodes.n, sizeof(nodes.v[0]), cmp_at);
    record_move(table);

    ticks_before = sched_ticks;
    charged = turn;
    charged_units = charged != NULL ? charged->time : (struct cap){ 0 };
    charged_balance = charged != NULL ? account_at_count(charged) : 0;
}

/* An object the call built, which the caller must have been able to build. */
static void check_built(const struct obj_header *o)
{
    if (o->type == CAP_POOL) {
        const struct pool *p = (const struct pool *)o;
        if (!covered((struct grant){ CAP_UNTYPED, RIGHT_R | RIGHT_W, pool_base(p), p->size })) {
            violated("a call made a pool of memory its caller held no Untyped to");
        }
        return;
    }
    if (!covered((struct grant){ CAP_POOL, RIGHT_W, o->pool, 0 })) {
        violated("a call built an object in a pool its caller could not allocate from");
    }
    bool bound = true;
    if (o->type == CAP_THREAD) {
        bound = covered((struct grant){ CAP_PROCESS, RIGHT_W, ((const struct thread *)o)->proc.a, 0 });
    } else if (o->type == CAP_IRQ) {
        const struct irq *irq = (const struct irq *)o;
        bound = covered((struct grant){ CAP_NOTIFICATION, RIGHT_W, irq->ntfn, 0 }) &&
                covered((struct grant){ CAP_IRQ_LINE, RIGHT_W, irq->line, 1 });
    }
    if (!bound) {
        violated("a call bound an object to something its caller could not use");
    }
}

static void check_node(const struct cap *c)
{
    if (c->type == CAP_NONE) {
        return;
    }
    struct grant g = grant_of(c);
    uint32_t at = v2p(c);
    const struct node_rec *was = bsearch(&at, nodes.v, nodes.n, sizeof(nodes.v[0]), cmp_at);
    if (was != NULL && same_grant(was->g, g)) {
        return;
    }
    /* Built by this call: an object now that was none before. */
    if (cap_has_object(c->type) && is_object(&objects_after, c->a) && !is_object(&objects, c->a)) {
        check_built(p2v(c->a));
    } else if (!covered(g)) {
        violated("a call made a capability no capability of its caller covers");
    }
    /* A thread bound to units by this call is one its caller could control. */
    if (c->type == CAP_BOUND && !covered((struct grant){ CAP_THREAD, RIGHT_W, v2p(bound_thread((struct cap *)c)), 0 })) {
        violated("a call bound a thread its caller could not control to units");
    }
}

void history_end(void)
{
    if (caller != NULL && !caller_dying && dying_under(caller, caller_process, caller_table)) {
        violated("a call began to destroy the pool its caller, its process or its table lives in");
    }

    record_objects(&objects_after);
    each_node(&objects_after, check_node);
    check_move();

    /*
     * A tick costs the thread whose turn it is a whole tick while it holds one,
     * and earns it its units, as every tick does; only OP_DEBUG_TICK moves time here.
     */
    if (charged != NULL && sched_ticks == ticks_before + 1 && same_grant(grant_of(&charged->time), grant_of(&charged_units))) {
        uint32_t units = thread_units(charged);
        uint32_t want = charged_balance >= TICK_PARTS ? charged_balance - (TICK_PARTS - units) : charged_balance + units;
        if (account_at_count(charged) != want) {
            violated("a tick did not cost the thread whose turn it was a tick less its units");
        }
    }

    /*
     * Every object the call took away is zeroed, whether its pool went or a destroy stopped half way;
     * the rest of a pool's memory comes back as it went in.
     */
    for (size_t i = 0; i < objects.n; i++) {
        const struct obj_rec *o = &objects.v[i];
        if (!is_object(&objects_after, o->at) && !zero(o->at, o->size)) {
            violated("memory left the kernel with an object not zeroed");
        }
    }
}
