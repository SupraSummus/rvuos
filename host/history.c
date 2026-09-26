/*
 * The properties "Authority only flows", "Memory crosses zeroed"
 * and "The caller keeps what it runs on" of DESIGN.md,
 * which relate the state before a call to the state after it.
 * host_syscall runs history_begin before each call and history_end after it.
 *
 * The prologue runs untraced, with no self-check before these checks, so a node may hold anything:
 * an address it holds is looked up among the objects and pools walked, never followed.
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

static ARRAY(struct grant) held;
static ARRAY(struct node_rec) nodes;
static obj_array objects, objects_after;

/* The thread that makes the call, its table, and whether a destroy had begun on either's pool. */
static const struct thread *caller;
static const struct captable *caller_table;
static bool caller_dying;

static bool dying_under(const struct thread *t, const struct captable *table)
{
    return obj_pool(&t->hdr)->dying || (table != NULL && obj_pool(&table->hdr)->dying);
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
 * An installed region grants what the frame it came from did, a thread's share its one share,
 * and an Untyped its block whatever its watermark.
 */
static struct grant grant_of(const struct cap *c)
{
    if (c->type == CAP_UNTYPED) {
        return (struct grant){ CAP_UNTYPED, c->rights, untyped_base(c), untyped_size(c) };
    }
    if (c->type == CAP_BOUND) {
        return (struct grant){ CAP_SHARE, c->rights, c->a, 1 };
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
    bool range = g.type == CAP_FRAME || g.type == CAP_UNTYPED || g.type == CAP_IRQ_LINE || g.type == CAP_SHARE;
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

void history_begin(void)
{
    held.n = nodes.n = 0;

    const struct captable *table = current != NULL ? thread_table(current) : NULL;
    caller = current;
    caller_table = table;
    caller_dying = caller != NULL && dying_under(caller, table);
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
        bound = covered((struct grant){ CAP_PROCESS, RIGHT_W, ((const struct thread *)o)->proc, 0 });
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
    /* A thread bound to a share by this call is one its caller could control. */
    if (c->type == CAP_BOUND && !covered((struct grant){ CAP_THREAD, RIGHT_W, v2p(bound_thread((struct cap *)c)), 0 })) {
        violated("a call bound a thread its caller could not control to a share");
    }
}

void history_end(void)
{
    if (caller != NULL && !caller_dying && dying_under(caller, caller_table)) {
        violated("a call began to destroy the pool its caller or its table lives in");
    }

    record_objects(&objects_after);
    each_node(&objects_after, check_node);

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
