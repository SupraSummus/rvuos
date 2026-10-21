/*
 * The properties "Authority only flows", "A call is as good as its capability", "Memory crosses zeroed",
 * "The caller keeps what it runs on", "A dying pool only gives back", "A step charges",
 * "A move keeps a node's place", "A carve takes what it names",
 * "A thread lends by its own wait", "A thread stops by its holder, where it stood",
 * "A description is the capability's" and "A pool's room is what an allocation may take" of DESIGN.md,
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
#include "layout.h"
#include "ops.h"

/* What a capability grants, as the covering rule compares it: a range, an object, or a type that names nothing. */
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

/*
 * A thread before the call: the thread it lent its time to, 0 for none, its state, and where it stood,
 * its pc and its registers; the frame's cause and address are left out, which a fault writes.
 */
#define THREAD_REGS (sizeof(((struct trap_frame *)0)->regs) / sizeof(uint32_t))
struct thread_rec {
    uint32_t at, lend_to;
    uint8_t state;
    uint32_t pc;
    uint32_t regs[THREAD_REGS];
    uint64_t vt[CORES];         /* each place's virtual time */
    uint32_t deadline[CORES];   /* each place's request's deadline */
    uint8_t span[CORES];        /* each place's span, the request's mark among its bits */
    struct grant time[CORES];   /* the units each place held */
};

/* Each unit's virtual time before the call: its place's while a place earns it, else what it was left at. */
static uint64_t unit_time_before[MACHINE_UNITS];

/* Growing arrays, kept from call to call. */
#define ARRAY(T) struct { T *v; size_t n, cap; }

typedef ARRAY(struct obj_rec) obj_array;
typedef ARRAY(uint32_t) addr_array;
typedef ARRAY(uint8_t) byte_array;

static ARRAY(struct grant) held;
static ARRAY(struct node_rec) nodes;
static ARRAY(struct thread_rec) threads;
static obj_array objects, objects_after;

/*
 * The thread that makes the call, its process and the tables the process mounts,
 * and on which of their pools a destroy had begun, as dying_under says.
 */
static const struct thread *caller;
static const struct process *caller_process;
static const struct captable *caller_tables[PROCESS_TABLES];
static uint32_t caller_dying;

/*
 * The call, or the fault, and what it invokes, if its slot held a capability, and where the caller stood,
 * which a call stopped for an interrupt leaves as it was.
 * For OP_NOTIFY_SIGNAL, the notification, its bits, and the threads waiting on it that a signal may wake first:
 * the oldest, and the one that pays for the turn.
 */
static bool call_made, invoked;
static int invoked_status; /* what the slot's lookup returned */
static struct cap invoked_cap;
static uint32_t call_op, call_pc;
static uint32_t call_args[4]; /* a1 to a4 */
static uint8_t caller_spans[CORES]; /* the span of each of the caller's places, as the call began */

/*
 * A traced carve that names a filled source: the source as it was, and what the call names of it,
 * the offset and the length, or for a notification's bits the mask, and the slot the carve goes to.
 */
static bool carving;
static struct cap carve_from;
static uint32_t carve_off, carve_len, carve_dst;
static const struct notification *signalled;
static uint32_t signalled_bits;
static const struct thread *signalled_waiter, *signalled_payer;
static const struct thread *bind_target; /* the thread an OP_TIME_BIND names, which a rebind to the same units charges */

/*
 * A traced OP_CAP_MOVE that names a filled source and a destination slot:
 * the two slots, what the source held, the node it hung below, 0 for none,
 * and the nodes that hung below it, in their order.
 */
static struct cap *move_from, *move_to;
static struct cap move_was;
static uint32_t move_parent;
static addr_array move_children, children_after;

/*
 * A traced describing call, OP_CAP_INFO or OP_POOL_INFO on a pool or on a slot that names nothing:
 * whether it takes its arguments, what it should return in a1 to a4,
 * and what of the kernel's state it must leave, see record_state.
 */
static bool describing, describe_takes;
static uint32_t describe_want[4];
static byte_array state, state_after;

/*
 * An allocation from a pool, OP_POOL_ALLOC's, OP_IRQ_BIND's or OP_ROUTE_BIND's, of a type whose size its arguments say:
 * the pool, its bytes in use and whether it was dying as the attempt began, and the bytes the type takes.
 */
static const struct pool *room_pool;
static uint32_t room_used, room_size, room_need;
static bool room_dying;

/*
 * The clock before the call, whether it was traced, and the thread the turn was charged to, NULL for none,
 * the turn's own or its payer, with its units, its virtual time, and what that comes to once charged up to a count;
 * see charged_up_to.
 */
static uint64_t clock_before;
static bool traced_before;
static const struct thread *charged, *charged_turn;
static const struct place *charged_place, *charged_own_place;
static const struct place *paying[CORES]; /* the place paying for each core's turn, which a bind charges on any core */
static struct cap charged_units, charged_own_units;
static uint64_t charged_vt, charged_from;
static uint32_t charged_room, charged_span;
static bool charged_open, charged_own;

/* The place of a thread that holds a core's time, as thread_place_on finds it, outside the frames fuzz-work follows. */
static const struct place *place_on(const struct thread *t, uint32_t core)
{
    for (uint32_t i = 0; i < t->places; i++) {
        if (t->time[i].type == CAP_TIME && unit_core(range_first(&t->time[i])) == core) {
            return thread_place(t, i);
        }
    }
    return NULL;
}

/*
 * The virtual time of the place the turn was charged to, charged up to the count now as the call began:
 * TIME_UNITS / units counts for each count since the turn was last charged, rounded up,
 * up to the end of the request open on it, and past that, on the turn's own place, up to a span past the count.
 */
static uint64_t charged_up_to(uint64_t now)
{
    uint32_t units = range_count(&charged_units);
    if (units == 0 || now <= charged_from) {
        return charged_vt;
    }
    uint64_t bound = charged_own ? now + charged_span : charged_vt;
    if (charged_open && charged_vt + charged_room > bound) {
        bound = charged_vt + charged_room;
    }
    if (charged_vt >= bound) {
        return charged_vt;
    }
    uint64_t cost = ((now - charged_from) * TIME_UNITS + units - 1) / units;
    return cost < bound - charged_vt ? charged_vt + cost : bound;
}

/*
 * The pools a thread runs on and names capabilities in, a bit for each a destroy has begun on:
 * the thread's, its process's, and each table's, so that one already dying hides none of the others.
 */
static uint32_t dying_under(const struct thread *t, const struct process *proc,
                            const struct captable *const tables[PROCESS_TABLES])
{
    uint32_t dying = (obj_pool(&t->hdr)->dying != 0) | (uint32_t)(proc != NULL && obj_pool(&proc->hdr)->dying) << 1;
    for (unsigned i = 0; i < PROCESS_TABLES; i++) {
        dying |= (uint32_t)(tables[i] != NULL && obj_pool(&tables[i]->hdr)->dying) << (2 + i);
    }
    return dying;
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
 * A frame grants its block and an Untyped its block whatever it made, a range of lines or units that range,
 * a capability to an object that object, and a Notification capability the bits it may signal too.
 * A node an object holds grants what a capability of its type does:
 * an installed region what the frame it came from did, a thread's units those units,
 * a thread's process what a capability to it does,
 * a thread's watch and an Irq's notification what one to the notification does, with the bits they may signal,
 * and a Route's end what the frame it was connected through did.
 * What the second word holds besides, the first child or the holder, is the tree's and grants nothing.
 */
static struct grant grant_of(const struct cap *c)
{
    switch (c->type) {
    case CAP_FRAME:
    case CAP_UNTYPED:
        return (struct grant){ c->type, c->rights, block_base(c), block_size(c) };
    case CAP_IRQ_LINE:
    case CAP_TIME:
    case CAP_ROUTE_LINE:
        return (struct grant){ c->type, c->rights, range_first(c), range_count(c) };
    case CAP_NOTIFICATION:
        return (struct grant){ c->type, c->rights, cap_addr(c), notify_bits(c) };
    default:
        return (struct grant){ c->type, c->rights, cap_has_object(c->type) ? cap_addr(c) : c->a, 0 };
    }
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
    bool range = g.type == CAP_FRAME || g.type == CAP_UNTYPED || g.type == CAP_IRQ_LINE || g.type == CAP_TIME ||
                 g.type == CAP_ROUTE_LINE;
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

/* Every node every object holds, with the object. */
static void each_node(const obj_array *a, void (*fn)(const struct obj_header *, const struct cap *))
{
    for (size_t i = 0; i < a->n; i++) {
        uint32_t count;
        const struct cap *held_nodes = obj_nodes(p2v(a->v[i].at), &count);
        for (uint32_t j = 0; j < count; j++) {
            fn(p2v(a->v[i].at), &held_nodes[j]);
        }
    }
}

static void record_node(const struct obj_header *o, const struct cap *c)
{
    (void)o;
    if (c->type != CAP_NONE) {
        PUSH(nodes, ((struct node_rec){ v2p(c), grant_of(c) }));
    }
}

/* The node a filled node hangs below, 0 for a root; the last sibling links up to it. */
static uint32_t parent_of(const struct cap *c)
{
    while (!(cap_next(c) & LINK_UP)) {
        c = p2v(cap_next(c));
    }
    return cap_next(c) & ~LINK_UP;
}

/* The nodes below a node, first to last. */
static void children_of(const struct cap *c, addr_array *out)
{
    out->n = 0;
    for (uint32_t at = cap_child(c); at != 0;) {
        const struct cap *n = p2v(at);
        PUSH(*out, at);
        at = (cap_next(n) & LINK_UP) ? 0 : cap_next(n);
    }
}

/*
 * The rights an operation needs of the capability it is invoked on, as rvuos/abi.h gives them;
 * one invoked on a capability of another type fails before its rights matter, and a describing one needs none.
 * RIGHTS_IN_HANDLER names an operation whose handler decides by what the call asks,
 * and RIGHTS_UNKNOWN a code that is no operation, which history_begin holds no code below OP_COUNT to but a retired one,
 * so a call of one that went through is a violation.
 */
#define RIGHTS_IN_HANDLER 0
#define RIGHTS_UNKNOWN 0xff
_Static_assert(OP_COUNT == 47 && OP_ROUTE_SET == OP_COUNT - 1, "a new operation needs its rights in rights_needed");
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
    case OP_PROCESS_MOUNT:
    case OP_PROCESS_UNMOUNT:
    case OP_THREAD_CONFIGURE:
    case OP_THREAD_RESUME:
    case OP_THREAD_STOP:
    case OP_THREAD_WATCH:
    case OP_THREAD_FAULT:
    case OP_THREAD_READ_REG:
    case OP_THREAD_WRITE_REG:
    case OP_NOTIFY_SIGNAL:
    case OP_IRQ_BIND:
    case OP_IRQ_SET:
    case OP_ROUTE_BIND:
    case OP_ROUTE_END:
    case OP_ROUTE_SET:
    case OP_TIME_BIND:
    case OP_PRINT:
    case OP_HALT:
    case OP_TEST_TRACE:
    case OP_TEST_TICK:
    case OP_TEST_IRQ:
    case OP_TEST_PREEMPT:
    case OP_CLOCK_WATCHDOG:
        return RIGHT_W;
    case OP_NOTIFY_WAIT:
    case OP_NOTIFY_LEND:
    case OP_CLOCK_READ:
    case OP_CLOCK_FRAME:
        return RIGHT_R;
    case OP_FRAME_CARVE:
    case OP_UNTYPED_RETYPE:
    case OP_UNTYPED_SPLIT:
    case OP_IRQ_CARVE:
    case OP_TIME_CARVE:
    case OP_ROUTE_CARVE:
    case OP_NOTIFY_CARVE:
        return RIGHTS_IN_HANDLER;
    /* A description grants nothing, so it needs no right. */
    case OP_CAP_INFO:
    case OP_POOL_INFO:
        return 0;
    default:
        return RIGHTS_UNKNOWN;
    }
}

/* Every code below OP_COUNT but a retired one is an operation, so each says what it needs; checked once, at the first call. */
static void check_rights_table(void)
{
    static bool checked;
    for (uint32_t op = 1; !checked && op < OP_COUNT; op++) {
        if (rights_needed(op) == RIGHTS_UNKNOWN && !op_retired(op)) {
            fprintf(stderr, "host/history.c: operation %u has no rights in rights_needed\n", (unsigned)op);
            abort();
        }
    }
    checked = true;
}

/*
 * The bytes an object of a type takes of a pool, by its structure,
 * slots its count for a table, its region slots for a process and its places for a thread;
 * 0 for no such object.
 */
static uint32_t type_bytes(uint32_t type, uint32_t slots)
{
    size_t size;
    switch (type) {
    case CAP_CAPTABLE:
        size = slots != 0 && slots <= CAPTABLE_MAX_SLOTS ? sizeof(struct captable) + slots * sizeof(struct cap) : 0;
        break;
    case CAP_PROCESS:
        /* A node and an entry's pmpaddr for each region slot, and its pmpcfg byte, the bytes rounded to a word. */
        size = slots != 0 && slots <= pmp_entry_count ? sizeof(struct process) +
                                                            slots * (sizeof(struct cap) + sizeof(uint32_t)) +
                                                            (slots + 3) / 4 * 4 : 0;
        break;
    case CAP_THREAD:
        size = slots != 0 && slots <= CORES ? sizeof(struct thread) +
                                                  slots * (sizeof(struct cap) + sizeof(struct place)) : 0;
        break;
    case CAP_NOTIFICATION:
        size = sizeof(struct notification);
        break;
    case CAP_IRQ:
        size = sizeof(struct irq);
        break;
    case CAP_ROUTE:
        size = sizeof(struct route);
        break;
    case CAP_POOL:
        size = sizeof(struct pool);
        break;
    default:
        size = 0;
        break;
    }
    return (uint32_t)(size + 15) & ~15u;
}

static void push_bytes(byte_array *out, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        PUSH(*out, ((const uint8_t *)p)[i]);
    }
}

/*
 * What a describing call leaves as it was, every object in order:
 * the nodes it holds, a pool's descriptor and a table's header.
 * What the tail of any call and its trace line may move is left out:
 * a thread's state, queues, virtual times and frame, a notification's bits and waiters,
 * and an Irq's bits, which the log's line may signal as the line is written.
 */
static void record_state(const obj_array *a, byte_array *out)
{
    out->n = 0;
    for (size_t i = 0; i < a->n; i++) {
        struct obj_header *o = p2v(a->v[i].at);
        uint32_t count;
        const struct cap *held_nodes = obj_nodes(o, &count);
        push_bytes(out, held_nodes, count * sizeof(struct cap));
        if (o->type == CAP_POOL) {
            push_bytes(out, o, sizeof(struct pool));
        } else if (o->type == CAP_CAPTABLE) {
            push_bytes(out, o, sizeof(struct captable));
        }
    }
}

/*
 * What a describing call returns in a1 to a4, as abi.h says, from the capability and what it names before the call,
 * which only a traced call follows.
 */
static void describe_expect(void)
{
    const struct cap *c = &invoked_cap;
    uint32_t *w = describe_want;
    if (call_op == OP_POOL_INFO) {
        const struct pool *p = (const struct pool *)cap_object(c);
        w[0] = type_bytes(call_args[0], call_args[2]);
        w[1] = p->dying ? 0 : p->size - p->used;
        w[2] = call_args[2];
        w[3] = call_args[3];
        return;
    }
    w[0] = c->type | (uint32_t)c->rights << 8;
    w[1] = w[2] = w[3] = 0;
    switch (c->type) {
    case CAP_FRAME:
        w[1] = block_base(c);
        w[2] = block_size(c);
        w[3] = pmp_grain < 8 ? 8 : pmp_grain;
        break;
    case CAP_UNTYPED:
        /* A frame, a region installed from one or a Route's end on one, a half or a derived Untyped, or a pool's own node. */
        w[1] = block_base(c);
        w[2] = block_size(c);
        w[3] = cap_child(c) != 0 ? ((const struct cap *)p2v(cap_child(c)))->type : CAP_NONE;
        break;
    case CAP_POOL: {
        const struct pool *p = (const struct pool *)cap_object(c);
        w[1] = p->size;
        w[2] = p->dying ? 0 : p->size - p->used;
        w[3] = p->dying != 0;
        break;
    }
    case CAP_CAPTABLE:
        w[1] = ((const struct captable *)cap_object(c))->nslots;
        break;
    case CAP_PROCESS:
        w[1] = ((const struct process *)cap_object(c))->regions;
        w[2] = pmp_entry_count;
        break;
    case CAP_THREAD: {
        const struct thread *t = (const struct thread *)cap_object(c);
        w[1] = t->places;
        for (uint32_t i = 0; i < t->places; i++) {
            if (t->time[i].type == CAP_TIME) {
                w[2] |= 1u << (range_first(&t->time[i]) / TIME_UNITS);
            }
        }
        break;
    }
    case CAP_NOTIFICATION:
        w[1] = notify_bits(c);
        break;
    case CAP_IRQ_LINE:
        w[1] = range_first(c);
        w[2] = range_count(c);
        w[3] = range_first(c) >= IRQ_LINES;
        break;
    case CAP_IRQ: {
        const struct irq *irq = (const struct irq *)cap_object(c);
        w[1] = irq->line;
        w[2] = irq->ntfn.type == CAP_NOTIFICATION ? notify_bits(&irq->ntfn) : 0;
        w[3] = irq->line >= IRQ_LINES;
        break;
    }
    case CAP_TIME:
        w[1] = range_first(c);
        w[2] = range_count(c);
        break;
    case CAP_ROUTE_LINE:
        w[1] = range_first(c);
        w[2] = range_count(c);
        break;
    case CAP_ROUTE: {
        const struct route *r = (const struct route *)cap_object(c);
        w[1] = r->channel;
        w[2] = (r->ends[ROUTE_EVENT].type == CAP_FRAME ? 1u << ROUTE_EVENT : 0) |
               (r->ends[ROUTE_TASK].type == CAP_FRAME ? 1u << ROUTE_TASK : 0);
        w[3] = r->armed != 0;
        break;
    }
    case CAP_CLOCK:
        /* The rate, and the longest power of two of counts within a millisecond, the default span. */
        w[1] = COUNTER_HZ;
        w[2] = 1u << (31 - __builtin_clz(COUNTER_HZ / 1000u));
        break;
    default:
        break;
    }
}

/*
 * Note a traced describing call, as the kernel will resolve it:
 * whether it takes its arguments, what it should return, and the state it must leave.
 * OP_POOL_INFO on another type than a pool fails as any operation of another type does, which check_call holds.
 */
static void record_describe(void)
{
    describing = debug_trace && (call_op == OP_CAP_INFO ||
                                 (call_op == OP_POOL_INFO && (!invoked || invoked_cap.type == CAP_POOL)));
    if (!describing) {
        return;
    }
    if (call_op == OP_CAP_INFO) {
        describe_takes = (call_args[0] | call_args[1] | call_args[2]) == 0;
    } else {
        describe_takes = type_bytes(call_args[0], call_args[2]) != 0 && call_args[1] == 0 &&
                         (call_args[0] == CAP_CAPTABLE || call_args[0] == CAP_PROCESS || call_args[0] == CAP_THREAD ||
                          call_args[2] == 0);
    }
    if (invoked) {
        describe_expect();
    }
    record_state(&objects, &state);
}

/* Note an allocation from a pool, as the kernel will resolve the pool, from the objects walked. */
static void record_room(void)
{
    room_pool = NULL;
    struct cap pc = invoked_cap;
    if (call_op == OP_POOL_ALLOC && invoked_cap.type == CAP_POOL) {
        /* A process's region slots and a thread's places are their a4, a table's slots its a3. */
        bool a4 = call_args[0] == CAP_PROCESS || call_args[0] == CAP_THREAD;
        room_need = call_args[0] == CAP_IRQ || call_args[0] == CAP_ROUTE || call_args[0] == CAP_POOL ? 0
                    : type_bytes(call_args[0], a4 ? call_args[3] : call_args[2]);
    } else if (call_op == OP_IRQ_BIND && invoked_cap.type == CAP_IRQ_LINE &&
               slot_lookup(caller, call_args[0], &pc) == KERR_OK && pc.type == CAP_POOL) {
        room_need = type_bytes(CAP_IRQ, 0);
    } else if (call_op == OP_ROUTE_BIND && invoked_cap.type == CAP_ROUTE_LINE &&
               slot_lookup(caller, call_args[0], &pc) == KERR_OK && pc.type == CAP_POOL) {
        room_need = type_bytes(CAP_ROUTE, 0);
    } else {
        return;
    }
    if (room_need == 0 || !is_object(&objects, cap_addr(&pc))) {
        return;
    }
    room_pool = (const struct pool *)cap_object(&pc);
    room_used = room_pool->used;
    room_size = room_pool->size;
    room_dying = room_pool->dying != 0;
}

/* Note what a call invokes, as the kernel will resolve it; a fault invokes nothing, whatever a7 holds. */
static void record_call(bool call)
{
    call_made = call;
    invoked = describing = false;
    room_pool = NULL;
    signalled = NULL;
    bind_target = NULL;
    if (!call || caller == NULL) {
        return;
    }
    const struct trap_frame *f = &caller->frame;
    call_op = f->regs[REG_A7];
    call_pc = f->pc;
    for (unsigned i = 0; i < 4; i++) {
        call_args[i] = f->regs[REG_A1 + i];
    }
    invoked_status = slot_lookup(caller, f->regs[REG_A0], &invoked_cap);
    invoked = invoked_status == KERR_OK;
    record_describe();
    if (invoked) {
        record_room();
    }
    struct cap target;
    if (invoked && call_op == OP_TIME_BIND && slot_lookup(caller, call_args[0], &target) == KERR_OK &&
        target.type == CAP_THREAD) {
        bind_target = (const struct thread *)cap_object(&target);
    }
    if (!invoked || call_op != OP_NOTIFY_SIGNAL || invoked_cap.type != CAP_NOTIFICATION) {
        return;
    }
    signalled = (const struct notification *)cap_object(&invoked_cap);
    signalled_bits = signalled->bits;
    signalled_waiter = signalled->waiters != 0 ? p2v(signalled->waiters) : NULL;
    signalled_payer = core_self()->payer;
    if (signalled_payer != NULL && signalled_payer->waiting_on != v2p(&signalled->hdr)) {
        signalled_payer = NULL;
    }
}

/*
 * Of a1 to a3, those a call of the operation leaves unread: the ARG_NONE of host/ops.h,
 * and those three operations leave by their own values, as rvuos/abi.h says:
 * an allocation of a notification its a3, a watch with no bits its a1,
 * and a set with no bits, or of an Irq that is no timer's, its a2 and a3.
 */
static unsigned unread_args(const struct op_info *info)
{
    unsigned unread = 0;
    for (unsigned i = 0; i < 3; i++) {
        if (info->arg[i] == ARG_NONE) {
            unread |= 1u << i;
        }
    }
    if (call_op == OP_POOL_ALLOC && call_args[0] == CAP_NOTIFICATION) {
        unread |= 4;
    } else if (call_op == OP_THREAD_WATCH && call_args[1] == 0) {
        unread |= 1;
    } else if (call_op == OP_IRQ_SET &&
               (call_args[0] == 0 || ((const struct irq *)cap_object(&invoked_cap))->line < IRQ_LINES)) {
        unread |= 6;
    }
    return unread;
}

/*
 * A call that returned failed at the first of the checks the manual orders that it fails, if any does:
 * a slot that names nothing, an operation the capability's type does not take, a right it lacks,
 * or something in what the operation does not read, a bit of a rights mask past RIGHT_ALL among it.
 * Which of the rest fails is the operation's own, and a description's arguments check_describe holds.
 */
static void check_order(void)
{
    if (!call_made || caller == NULL || caller->frame.pc == call_pc || caller->state == THREAD_WAITING ||
        call_op == OP_CAP_INFO) {
        return;
    }
    uint32_t status = caller->frame.regs[REG_A0];
    uint32_t want;
    const struct op_info *info = call_op < OP_COUNT ? op_info((uint8_t)call_op) : NULL;
    if (!invoked) {
        want = (uint32_t)invoked_status;
    } else if (info == NULL || info->cap != invoked_cap.type) {
        want = KERR_WRONG_TYPE;
    } else if ((invoked_cap.rights & rights_needed(call_op)) != rights_needed(call_op)) {
        want = KERR_NO_RIGHTS;
    } else {
        unsigned unread = unread_args(info);
        bool garbage = false;
        for (unsigned i = 0; i < 3; i++) {
            garbage |= ((unread >> i) & 1) && call_args[i] != 0;
            garbage |= info->arg[i] == ARG_RIGHTS && (call_args[i] & ~(uint32_t)RIGHT_ALL) != 0;
        }
        if (!garbage) {
            return;
        }
        want = KERR_INVALID_ARG;
    }
    if (status != want) {
        violated("a refused call did not fail at the first check it fails");
    }
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
    if (signalled_payer != NULL && signalled_payer->state != THREAD_WAITING) {
        set |= signalled_payer->frame.regs[REG_A1];
    }
    if (set & ~notify_bits(&invoked_cap)) {
        violated("a signal set bits its capability may not signal");
    }
}

/*
 * A traced describing call that returned took exactly the arguments abi.h gives it,
 * succeeded on any filled slot whatever its rights, returned what abi.h says the capability tells,
 * and changed nothing of the state record_state keeps.
 */
static void check_describe(void)
{
    if (!describing || caller->frame.pc == call_pc) {
        return;
    }
    uint32_t status = caller->frame.regs[REG_A0];
    if (!invoked) {
        if (status != KERR_INVALID_CAP) {
            violated("a describing call described a slot that names nothing");
        }
    } else if (!describe_takes) {
        if (status != KERR_INVALID_ARG) {
            violated("a describing call took an argument it does not take");
        }
    } else if (status != KERR_OK) {
        violated("a describing call failed on a filled slot");
    } else if (memcmp(&caller->frame.regs[REG_A1], describe_want, sizeof(describe_want)) != 0) {
        violated("a description differs from the capability it describes");
    }
    record_state(&objects_after, &state_after);
    if (objects_after.n != objects.n || memcmp(objects_after.v, objects.v, objects.n * sizeof(objects.v[0])) != 0 ||
        state_after.n != state.n || memcmp(state_after.v, state.v, state.n) != 0) {
        violated("a describing call changed the kernel's state");
    }
}

/*
 * An allocation that finished found no room exactly when its type took more than the room as the attempt found it,
 * in a pool not being destroyed, and one that succeeded took exactly what its type takes.
 * Every other status comes from a check before the room, and says nothing of it.
 */
static void check_room(void)
{
    if (room_pool == NULL || caller->frame.pc == call_pc) {
        return;
    }
    uint32_t status = caller->frame.regs[REG_A0];
    bool fits = !room_dying && room_need <= room_size - room_used;
    if ((status == KERR_NO_MEMORY && fits) || (status == KERR_OK && !fits)) {
        violated("an allocation's room differs from the pool's");
    }
    if (status == KERR_OK && (!is_object(&objects_after, v2p(&room_pool->hdr)) || room_pool->used != room_used + room_need)) {
        violated("an object took other bytes than its type takes");
    }
}

/* Note a move's source and destination, as the kernel will resolve them; a fault is no move, whatever a7 holds. */
static void record_move(bool call)
{
    move_from = move_to = NULL;
    if (!call || !debug_trace || caller == NULL || caller->frame.regs[REG_A7] != OP_CAP_MOVE) {
        return;
    }
    const struct trap_frame *f = &caller->frame;
    struct cap tc;
    if (slot_lookup(caller, f->regs[REG_A0], &tc) != KERR_OK || tc.type != CAP_CAPTABLE) {
        return;
    }
    struct captable *dst = (struct captable *)cap_object(&tc);
    uint32_t to = f->regs[REG_A1];
    struct cap *from = slot_at(caller, f->regs[REG_A2]);
    if (to >= dst->nslots || from == NULL || from->type == CAP_NONE) {
        return;
    }
    move_from = from;
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
    /* Its words, the first child's link aside, which the check of its children below reads. */
    if (move_to->type != move_was.type || move_to->rights != move_was.rights || move_to->a != move_was.a ||
        (move_to->b & ~B_LINK) != (move_was.b & ~B_LINK)) {
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

/* Every thread as it stands, sorted, compared after the call and never followed. */
static void record_threads(void)
{
    threads.n = 0;
    for (size_t i = 0; i < objects.n; i++) {
        const struct obj_header *o = p2v(objects.v[i].at);
        if (o->type != CAP_THREAD) {
            continue;
        }
        const struct thread *t = (const struct thread *)o;
        struct thread_rec r = { objects.v[i].at, t->lend_to, t->state, t->frame.pc, { 0 }, { 0 }, { 0 }, { 0 }, { { 0 } } };
        memcpy(r.regs, t->frame.regs, sizeof(r.regs));
        for (uint32_t k = 0; k < thread_places(t); k++) {
            const struct place *p = thread_place(t, k);
            r.vt[k] = p->vt;
            r.deadline[k] = p->deadline;
            r.span[k] = p->span;
            r.time[k] = grant_of(place_time(p));
        }
        PUSH(threads, r);
    }
}

/* A unit's virtual time: its place's while a place earns it, else what the place that earned it last left it at. */
static uint64_t unit_time(uint32_t u)
{
    return unit_place[u] != 0 ? ((const struct place *)p2v(unit_place[u]))->vt : unit_vt[u];
}

/* The record of the thread at at before the call, or NULL for one the call built. */
static const struct thread_rec *thread_before(uint32_t at)
{
    return threads.n != 0 ? bsearch(&at, threads.v, threads.n, sizeof(threads.v[0]), cmp_at) : NULL;
}

/*
 * A thread comes to lend its time to another only by its own wait, OP_NOTIFY_LEND,
 * through a capability to that thread with RIGHT_X, which check_call holds to the wait's rights;
 * no other call, nor a fault, makes one thread lend to another or to a third.
 */
static void check_lends(void)
{
    for (size_t i = 0; i < objects_after.n; i++) {
        const struct obj_header *o = p2v(objects_after.v[i].at);
        if (o->type != CAP_THREAD) {
            continue;
        }
        uint32_t at = objects_after.v[i].at, lend_to = ((const struct thread *)o)->lend_to;
        const struct thread_rec *was = thread_before(at);
        if (lend_to == 0 || (was != NULL && lend_to == was->lend_to)) {
            continue;
        }
        bool waited = invoked && call_op == OP_NOTIFY_LEND && invoked_cap.type == CAP_NOTIFICATION && at == v2p(caller);
        if (!waited || !covered((struct grant){ CAP_THREAD, RIGHT_X, lend_to, 0 })) {
            violated("a thread came to lend its time but by its own wait, through a capability with the right to lend");
        }
    }
}

/*
 * A thread stops by its holder, where it stood.
 * One ready or waiting before a call, or a fault, and stopped after it is the caller of a fault,
 * one whose process the call took, or the thread the call's OP_THREAD_STOP named,
 * through a capability check_call holds to the rights the stop needs.
 * The caller of a call stands past it, as every call's does that it does not stop for an interrupt.
 * A waiter stands at the call it waited in, every register as it was, since that call wrote none,
 * unless the call answered its wait first, as a destroy does, with KERR_INVALID_CAP and no bits.
 * Any other stands where it stood, every register as it was.
 */
static bool same_regs(const struct thread *t, const struct thread_rec *was, bool but_results)
{
    for (unsigned r = 0; r < THREAD_REGS; r++) {
        if (t->frame.regs[r] != was->regs[r] && !(but_results && (r == REG_A0 || r == REG_A1))) {
            return false;
        }
    }
    return true;
}

static void check_stops(void)
{
    for (size_t i = 0; i < objects_after.n; i++) {
        uint32_t at = objects_after.v[i].at;
        const struct obj_header *o = p2v(at);
        if (o->type != CAP_THREAD || ((const struct thread *)o)->state != THREAD_STOPPED) {
            continue;
        }
        const struct thread *t = (const struct thread *)o;
        const struct thread_rec *was = thread_before(at);
        if (was == NULL || was->state == THREAD_STOPPED) {
            continue;
        }
        bool self = caller != NULL && at == v2p(caller);
        bool named = call_made && invoked && call_op == OP_THREAD_STOP && invoked_cap.type == CAP_THREAD &&
                     cap_addr(&invoked_cap) == at;
        if (!(self && !call_made) && t->proc.type != CAP_NONE && !named) {
            violated("a thread stopped that no stop named, nor its own fault nor the loss of its process");
        }
        const struct trap_frame *f = &t->frame;
        if (self && call_made) {
            if (f->pc != call_pc + CALL_SIZE) {
                violated("a stop moved a thread from where it stood");
            }
        } else if (was->state == THREAD_WAITING) {
            bool at_call = f->pc == was->pc - CALL_SIZE && same_regs(t, was, false);
            bool answered = f->pc == was->pc && f->regs[REG_A0] == KERR_INVALID_CAP && f->regs[REG_A1] == 0 &&
                            same_regs(t, was, true);
            if (!at_call && !answered) {
                violated("a stop left a waiting thread elsewhere than at the call it waited in");
            }
        } else if (f->pc != was->pc || !same_regs(t, was, false)) {
            violated("a stop moved a thread from where it stood");
        }
    }
}

/*
 * Under tracing a virtual time only moves on: no traced call puts a place's back,
 * nor a unit's, which is its place's while a place earns it and what it was left at otherwise,
 * so neither a bind, nor an unbind, nor a destroy forgives what a thread ran ahead of its units.
 * A step moves a place's virtual time only by charging a turn it pays for, on any core,
 * by bringing it up to the clock as a request starts, or by a bind, which charges it first, even to the same units.
 * A request that starts on a thread's own units starts at the later of its virtual time and the clock,
 * once that lies at most a span past the clock, and is due a span past that; see sched.c, "Requests".
 */
static void check_vtimes(void)
{
    if (!traced_before || !debug_trace) {
        return;
    }
    for (uint32_t u = 0; u < MACHINE_UNITS; u++) {
        if (unit_time(u) < unit_time_before[u]) {
            violated("a call put a unit's virtual time back");
        }
    }
    uint64_t clock = sched_trace_clock;
    for (size_t i = 0; i < objects_after.n; i++) {
        const struct obj_header *o = p2v(objects_after.v[i].at);
        const struct thread_rec *was = o->type == CAP_THREAD ? thread_before(objects_after.v[i].at) : NULL;
        if (was == NULL) {
            continue;
        }
        const struct thread *t = (const struct thread *)o;
        for (uint32_t k = 0; k < thread_places(t); k++) {
            const struct place *p = thread_place(t, k);
            if (p->vt < was->vt[k]) {
                violated("a call put a place's virtual time back");
            }
            bool bound = !same_grant(grant_of(place_time(p)), was->time[k]) || t == bind_target;
            bool paid = false;
            for (uint32_t c = 0; c < CORES; c++) {
                paid |= p == paying[c];
            }
            if (p->vt != was->vt[k] && !paid && p->vt != clock && !bound) {
                violated("a step moved the virtual time of a place that paid for no turn and started no request");
            }
            bool started = (p->span & PLACE_REQUEST) != 0 &&
                           ((was->span[k] & PLACE_REQUEST) == 0 || p->deadline != was->deadline[k]);
            if (!started || place_units(p) == 0 || !request_open(p, p)) {
                continue;
            }
            uint32_t span = place_span(p);
            if (p->vt < clock || p->vt > clock + span || p->deadline != (uint32_t)p->vt + span) {
                violated("a request started elsewhere than at the later of its virtual time and the clock, within a span of it");
            }
        }
    }
}

/*
 * A wait or a lend that went through, returned or blocked, leaves its caller's requests due within the span it gave,
 * on every core whose time it holds, and one that gave none, or was refused, leaves them as they were;
 * see sched.c, "Requests".
 */
static void check_spans(void)
{
    if (!call_made || caller == NULL || (call_op != OP_NOTIFY_WAIT && call_op != OP_NOTIFY_LEND) ||
        caller->frame.pc == call_pc) {
        return;
    }
    bool through = invoked && (caller->state == THREAD_WAITING || caller->frame.regs[REG_A0] == KERR_OK);
    uint32_t span = through ? call_args[call_op == OP_NOTIFY_WAIT ? 0 : 1] : 0;
    for (uint32_t i = 0; i < thread_places(caller); i++) {
        if ((thread_place(caller, i)->span & PLACE_SPAN) != (span != 0 ? span_code(span) : caller_spans[i] & PLACE_SPAN)) {
            violated("a wait left its caller's requests due within another span than it gave");
        }
    }
}

/* Note a carve's source and what it names of it, as the kernel will resolve them. */
static void record_carve(void)
{
    carving = false;
    if (!debug_trace || !invoked) {
        return;
    }
    switch (call_op) {
    case OP_FRAME_CARVE:
    case OP_IRQ_CARVE:
    case OP_TIME_CARVE:
    case OP_ROUTE_CARVE:
        carve_off = call_args[0];
        carve_len = call_args[1];
        carve_dst = call_args[2];
        break;
    case OP_NOTIFY_CARVE:
        carve_off = call_args[0];
        carve_len = 0;
        carve_dst = call_args[1];
        break;
    default:
        return;
    }
    carving = true;
    carve_from = invoked_cap;
}

/*
 * A carve that returned took what it names of its source and nothing else, with the source's type and rights:
 * a frame the block at the offset of the length, a range the lines, units or channels there,
 * and a Notification capability the bits of the mask its source may signal.
 */
static void check_carve(void)
{
    if (!carving || caller->frame.pc == call_pc || caller->frame.regs[REG_A0] != KERR_OK) {
        return;
    }
    const struct cap *s = &carve_from;
    struct cap c;
    bool named = slot_lookup(caller, carve_dst, &c) == KERR_OK && c.type == s->type && c.rights == s->rights;
    if (named && s->type == CAP_FRAME) {
        named = block_base(&c) == block_base(s) + carve_off && block_size(&c) == carve_len;
    } else if (named && (s->type == CAP_IRQ_LINE || s->type == CAP_TIME || s->type == CAP_ROUTE_LINE)) {
        named = range_first(&c) == range_first(s) + carve_off && range_count(&c) == carve_len;
    } else if (named) {
        named = cap_addr(&c) == cap_addr(s) && notify_bits(&c) == (notify_bits(s) & carve_off);
    }
    if (!named) {
        violated("a carve made other than what it names of its source");
    }
}

void history_begin(bool call)
{
    check_rights_table();
    held.n = nodes.n = 0;

    const struct thread *current = core_self()->current;
    caller = current;
    caller_process = current != NULL ? thread_process(current) : NULL;
    /* What the caller holds is what any of its tables does. */
    for (uint32_t t = 0; t < PROCESS_TABLES; t++) {
        const struct captable *table = caller_process != NULL ? process_table(caller_process, t) : NULL;
        caller_tables[t] = table;
        for (uint32_t i = 0; table != NULL && i < table->nslots; i++) {
            const struct cap *c = &table->slots[i];
            if (c->type == CAP_NONE) {
                continue;
            }
            PUSH(held, grant_of(c));
            /* A clock that reads holds the counter's frame, which OP_CLOCK_FRAME hands out. */
            if (c->type == CAP_CLOCK && (c->rights & RIGHT_R)) {
                const struct granted_range *r = &boot_granted[GRANT_COUNTER];
                PUSH(held, ((struct grant){ CAP_FRAME, r->rights, r->base, r->size }));
            }
        }
    }
    caller_dying = caller != NULL ? dying_under(caller, caller_process, caller_tables) : 0;
    for (uint32_t i = 0; caller != NULL && i < thread_places(caller); i++) {
        caller_spans[i] = thread_place(caller, i)->span;
    }

    record_objects(&objects);
    each_node(&objects, record_node);
    qsort(nodes.v, nodes.n, sizeof(nodes.v[0]), cmp_at);
    record_threads();
    record_move(call);
    record_call(call);
    record_carve();

    clock_before = sched_trace_clock;
    traced_before = debug_trace;
    for (uint32_t u = 0; u < MACHINE_UNITS; u++) {
        unit_time_before[u] = unit_time(u);
    }
    charged = core_self()->payer != NULL ? core_self()->payer : core_self()->turn;
    for (uint32_t c = 0; c < CORES; c++) {
        const struct thread *payer = cores[c].payer != NULL ? cores[c].payer : cores[c].turn;
        paying[c] = payer != NULL ? place_on(payer, c) : NULL;
    }
    charged_place = charged != NULL ? place_on(charged, core_index()) : NULL;
    charged_units = charged_place != NULL ? *place_time(charged_place) : (struct cap){ 0 };
    const struct place *own = core_self()->turn != NULL ? place_on(core_self()->turn, core_index()) : NULL;
    charged_vt = charged_place != NULL ? charged_place->vt : 0;
    charged_from = core_self()->turn_from;
    charged_open = charged_place != NULL && own != NULL && range_count(&charged_units) != 0 && request_open(charged_place, own);
    charged_room = charged_open ? own->deadline - (uint32_t)charged_vt : 0;
    charged_own = charged_place != NULL && charged_place == own;
    charged_turn = core_self()->turn;
    charged_own_place = own;
    charged_own_units = own != NULL ? *place_time(own) : (struct cap){ 0 };
    charged_span = charged_place != NULL ? place_span(charged_place) : 0;
}

/*
 * A table mounted by this call is mounted in a process its caller could control.
 * A process the call built holds the table it was allocated with, which check_node holds to a capability the caller had.
 */
static void check_mounts(void)
{
    for (size_t i = 0; i < objects_after.n; i++) {
        uint32_t at = objects_after.v[i].at;
        const struct obj_header *o = p2v(at);
        if (o->type != CAP_PROCESS || !is_object(&objects, at)) {
            continue;
        }
        for (unsigned j = 0; j < PROCESS_TABLES; j++) {
            const struct cap *c = &((const struct process *)o)->tables[j];
            uint32_t node = v2p(c);
            const struct node_rec *was = bsearch(&node, nodes.v, nodes.n, sizeof(nodes.v[0]), cmp_at);
            if (c->type != CAP_NONE && (was == NULL || !same_grant(was->g, grant_of(c))) &&
                !covered((struct grant){ CAP_PROCESS, RIGHT_W, at, 0 })) {
                violated("a call mounted a table in a process its caller could not control");
            }
        }
    }
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
        bound = covered((struct grant){ CAP_PROCESS, RIGHT_W, cap_addr(&((const struct thread *)o)->proc), 0 });
    } else if (o->type == CAP_IRQ) {
        const struct irq *irq = (const struct irq *)o;
        bound = covered((struct grant){ CAP_NOTIFICATION, RIGHT_W, cap_addr(&irq->ntfn), notify_bits(&irq->ntfn) }) &&
                covered((struct grant){ CAP_IRQ_LINE, RIGHT_W, irq->line, 1 });
    } else if (o->type == CAP_ROUTE) {
        bound = covered((struct grant){ CAP_ROUTE_LINE, RIGHT_W, ((const struct route *)o)->channel, 1 });
    }
    if (!bound) {
        violated("a call bound an object to something its caller could not use");
    }
}

static void check_node(const struct obj_header *o, const struct cap *c)
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
    if (cap_has_object(c->type) && is_object(&objects_after, cap_addr(c)) && !is_object(&objects, cap_addr(c))) {
        check_built(p2v(cap_addr(c)));
    } else if (!covered(g)) {
        violated("a call made a capability no capability of its caller covers");
    }
    /* A thread bound to units, or watched, by this call is one its caller could control. */
    if (c->type == CAP_TIME && cap_held(c) &&
        !covered((struct grant){ CAP_THREAD, RIGHT_W, cap_holder(c), 0 })) {
        violated("a call bound a thread its caller could not control to units");
    }
    if (o->type == CAP_THREAD && c == &((const struct thread *)o)->watch &&
        !covered((struct grant){ CAP_THREAD, RIGHT_W, v2p(o), 0 })) {
        violated("a call set the watch of a thread its caller could not control");
    }
    if (o->type == CAP_ROUTE && c->type != CAP_NONE && !covered((struct grant){ CAP_ROUTE, RIGHT_W, v2p(o), 0 })) {
        violated("a call connected an end of a route its caller could not control");
    }
}

void history_end(void)
{
    /* The tables are those the call began with: one it mounted from a dying pool it did not begin to destroy. */
    if (caller != NULL && (dying_under(caller, caller_process, caller_tables) & ~caller_dying) != 0) {
        violated("a call began to destroy the pool its caller, its process or one of its tables lives in");
    }

    record_objects(&objects_after);
    each_node(&objects_after, check_node);
    check_mounts();
    check_move();
    check_carve();
    check_call();
    check_order();
    check_lends();
    check_spans();
    check_vtimes();
    check_stops();
    check_describe();
    check_room();

    /*
     * Only OP_TEST_TICK moves time here, and only under tracing, where the clock is the record's:
     * the thread the turn is charged to pays TIME_UNITS / units counts of virtual time for each count it ran,
     * up to the end of its request, and past it, on its own units, up to a span past the clock,
     * as the call charges the turn, which moves the count the turn is charged from, and not otherwise:
     * a call on one core that ends no turn charges nothing for the counts another core's step moved the clock;
     * a request it starts after brings its virtual time up to the clock, and nothing else moves it.
     * The caller keeps what it runs on, but a payer that lent it time may go with a pool the call destroyed,
     * and a turn whose thread lost its place on the core is charged up to then and no more,
     * which a step that also moved the clock, as a preempted call's does, may have done before the clock moved or after.
     */
    bool own_kept = charged_own_place != NULL && is_object(&objects_after, v2p(charged_turn)) &&
                    same_grant(grant_of(place_time(charged_own_place)), grant_of(&charged_own_units));
    if (charged_place != NULL && traced_before && sched_trace_clock - clock_before <= timer_step_counts() &&
        is_object(&objects_after, v2p(charged)) && same_grant(grant_of(place_time(charged_place)), grant_of(&charged_units)) &&
        (own_kept || sched_trace_clock == clock_before)) {
        uint64_t want = core_self()->turn_from != charged_from ? charged_up_to(sched_trace_clock) : charged_vt;
        uint64_t vt = charged_place->vt;
        if (vt != want && vt != (want > sched_trace_clock ? want : sched_trace_clock)) {
            violated(sched_trace_clock == clock_before ? "a traced call that moved no time charged the thread whose turn it was"
                                                       : "a step of the clock did not charge the thread whose turn it was its units' part");
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
