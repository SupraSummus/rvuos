/*
 * The properties "Authority only flows", "A call is as good as its capability", "Memory crosses zeroed",
 * "The caller keeps what it runs on", "A dying pool only gives back", "A tick charges"
 * and "A move keeps a node's place" of DESIGN.md,
 * which relate the state before a call to the state after it.
 * host_trap runs history_begin before each call and history_end after it,
 * and around a fault, which must change no more than a call that moves no time.
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
 * The call: the capability it invokes, if its slot held one, and where the caller stood,
 * which a call stopped for an interrupt leaves as it was.
 * For OP_NOTIFY_SIGNAL, the notification, its bits, and the thread waiting first on it.
 */
static bool invoked;
static struct cap invoked_cap;
static uint32_t call_op, call_pc;
static const struct notification *signalled;
static uint32_t signalled_bits;
static const struct thread *signalled_waiter;

/*
 * A traced OP_CAP_MOVE that names a filled source and a destination slot:
 * the two slots, what the source held, the node it hung below, 0 for none,
 * and the nodes that hung below it, in their order.
 */
static struct cap *move_from, *move_to;
static struct cap move_was;
static uint32_t move_parent;
static addr_array move_children, children_after;

/* The count before the call, whether it was traced, and the thread whose turn it was, NULL for none, with its units and its account. */
static uint32_t ticks_before;
static bool traced_before;
static const struct thread *charged;
static struct cap charged_units;
static uint32_t charged_balance;

/* A thread's account at the count, as its stamp and its units make it, held to the cap. */
static uint32_t account_at_count(const struct thread *t)
{
    uint64_t balance = t->balance + (uint64_t)(sched_ticks - t->stamp) * tick_gain(t);
    return balance > account_cap(t) ? account_cap(t) : (uint32_t)balance;
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
 * a thread's process what a capability to it does,
 * a thread's watch and an Irq's notification that too, with the bits they may signal,
 * and an Untyped its block whatever it made.
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
    if (c->type == CAP_SIGNALLED) {
        return (struct grant){ CAP_NOTIFICATION, c->rights, c->a, c->b };
    }
    if (c->type == CAP_WATCHED) {
        return (struct grant){ CAP_NOTIFICATION, c->rights, c->a, c->b };
    }
    return (struct grant){ c->type == CAP_INSTALLED ? CAP_FRAME : c->type, c->rights, c->a, c->b };
}

static bool same_grant(struct grant x, struct grant y)
{
    return x.type == y.type && x.rights == y.rights && x.a == y.a && x.b == y.b;
}

/*
 * Whether a capability the caller held grants all that g does; an Untyped grants the frames it makes,
 * and a notification's capability the bits it may signal.
 */
static bool covered(struct grant g)
{
    bool range = g.type == CAP_FRAME || g.type == CAP_UNTYPED || g.type == CAP_IRQ_LINE || g.type == CAP_TIME;
    for (size_t i = 0; i < held.n; i++) {
        const struct grant *h = &held.v[i];
        bool type = h->type == g.type || (g.type == CAP_FRAME && h->type == CAP_UNTYPED);
        if (type && (g.rights & ~h->rights) == 0 &&
            (range ? g.a - h->a < h->b && g.b <= h->b - (g.a - h->a)
                   : g.a == h->a && (g.type != CAP_NOTIFICATION || (g.b & ~h->b) == 0))) {
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

/* Objects that left the kernel are poisoned, so ASan must not see this read. */
__attribute__((no_sanitize("address"))) static bool zero(uint32_t base, uint32_t size)
{
    const uint8_t *p = p2v(base);
    uint8_t any = 0;
    for (uint32_t i = 0; i < size; i++) {
        any |= p[i];
    }
    return any == 0;
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

/*
 * The rights an operation needs of the capability it is invoked on, as rvuos/abi.h gives them;
 * one invoked on a capability of another type fails before its rights matter.
 */
_Static_assert(OP_COUNT == 37, "a new operation needs its rights in rights_needed");
static uint8_t rights_needed(uint32_t op)
{
    switch (op) {
    case OP_CAP_COPY:
    case OP_CAP_DELETE:
    case OP_CAP_REVOKE:
    case OP_CAP_DERIVE:
    case OP_CAP_MOVE:
    case OP_POOL_ALLOC:
    case OP_PROCESS_INSTALL:
    case OP_PROCESS_UNINSTALL:
    case OP_THREAD_CONFIGURE:
    case OP_THREAD_RESUME:
    case OP_THREAD_WATCH:
    case OP_THREAD_FAULT:
    case OP_THREAD_READ_REG:
    case OP_THREAD_WRITE_REG:
    case OP_NOTIFY_SIGNAL:
    case OP_IRQ_BIND:
    case OP_IRQ_SET:
    case OP_TIME_BIND:
    case OP_DEBUG_WRITE:
        return RIGHT_W;
    case OP_NOTIFY_WAIT:
        return RIGHT_R;
    case OP_DEBUG_HALT:
    case OP_DEBUG_TRACE:
    case OP_DEBUG_TICK:
    case OP_DEBUG_IRQ:
    case OP_DEBUG_PREEMPT:
        return RIGHT_X;
    default:
        return 0;
    }
}

/* Note what a call invokes, as the kernel will resolve it; a fault invokes nothing, whatever a7 holds. */
static void record_call(const struct captable *table, bool call)
{
    invoked = false;
    signalled = NULL;
    if (!call || caller == NULL || table == NULL) {
        return;
    }
    const struct trap_frame *f = &caller->frame;
    call_op = f->regs[REG_A7];
    call_pc = f->pc;
    invoked = cap_lookup((struct captable *)table, f->regs[REG_A0], &invoked_cap) == KERR_OK;
    if (!invoked || call_op != OP_NOTIFY_SIGNAL || invoked_cap.type != CAP_NOTIFICATION) {
        return;
    }
    signalled = (const struct notification *)cap_object(&invoked_cap);
    signalled_bits = signalled->bits;
    signalled_waiter = signalled->waiters != 0 ? p2v(signalled->waiters) : NULL;
}

/*
 * A call that went through, returned or blocked, was made through a capability with the rights its operation needs,
 * and a signal set only bits its capability may signal, in the notification or in the thread it woke.
 */
static void check_call(void)
{
    if (!invoked || caller->frame.pc == call_pc ||
        (caller->state != THREAD_WAITING && caller->frame.regs[REG_A0] != KERR_OK)) {
        return;
    }
    uint8_t needs = rights_needed(call_op);
    if ((invoked_cap.rights & needs) != needs) {
        violated("a call went through a capability without the rights its operation needs");
    }
    if (signalled == NULL) {
        return;
    }
    uint32_t set = signalled->bits & ~signalled_bits;
    if (signalled_waiter != NULL && signalled_waiter->state != THREAD_WAITING) {
        set |= signalled_waiter->frame.regs[REG_A1];
    }
    if (set & ~invoked_cap.b) {
        violated("a signal set bits its capability may not signal");
    }
}

/* Note a move's source and destination, as the kernel will resolve them; a fault is no move, whatever a7 holds. */
static void record_move(const struct captable *table, bool call)
{
    move_from = move_to = NULL;
    if (!call || !debug_trace || caller == NULL || table == NULL || caller->frame.regs[REG_A7] != OP_CAP_MOVE) {
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

void history_begin(bool call)
{
    held.n = nodes.n = 0;

    const struct thread *current = core_self()->current;
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
    record_move(table, call);
    record_call(table, call);

    ticks_before = sched_ticks;
    traced_before = debug_trace;
    charged = core_self()->turn;
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
    if (obj_pool(o)->dying) {
        violated("a call built an object in a pool being destroyed");
    }
    bool bound = true;
    if (o->type == CAP_THREAD) {
        bound = covered((struct grant){ CAP_PROCESS, RIGHT_W, ((const struct thread *)o)->proc.a, 0 });
    } else if (o->type == CAP_IRQ) {
        const struct irq *irq = (const struct irq *)o;
        bound = covered((struct grant){ CAP_NOTIFICATION, RIGHT_W, irq->ntfn.a, irq->ntfn.b }) &&
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
    /* A thread bound to units, or watched, by this call is one its caller could control. */
    if (c->type == CAP_BOUND && !covered((struct grant){ CAP_THREAD, RIGHT_W, v2p(bound_thread((struct cap *)c)), 0 })) {
        violated("a call bound a thread its caller could not control to units");
    }
    if (c->type == CAP_WATCHED &&
        !covered((struct grant){ CAP_THREAD, RIGHT_W, v2p(watched_thread((struct cap *)c)), 0 })) {
        violated("a call set the watch of a thread its caller could not control");
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
    check_call();

    /*
     * Only OP_DEBUG_TICK moves time here, and only under tracing, where the clock is the tick count:
     * a tick costs the thread whose turn it is a whole tick while it holds one, and earns it its units, as every tick does,
     * and a call that moves no time costs it nothing.
     */
    if (charged != NULL && traced_before && sched_ticks - ticks_before <= 1 &&
        same_grant(grant_of(&charged->time), grant_of(&charged_units))) {
        uint32_t gain = tick_gain(charged);
        uint32_t want = sched_ticks == ticks_before ? charged_balance
                        : charged_balance >= tick_parts() ? charged_balance - tick_parts() + gain
                                                          : charged_balance + gain;
        if (account_at_count(charged) != want) {
            violated(sched_ticks == ticks_before ? "a traced call that moved no time charged the thread whose turn it was"
                                                 : "a tick did not cost the thread whose turn it was a tick less its units");
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
