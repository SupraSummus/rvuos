/*
 * The properties "Authority only flows" and "Memory crosses zeroed" of DESIGN.md,
 * which relate the state before a call to the state after it.
 * host_syscall runs history_begin before each call and history_end after it.
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

/* A pool before the call; stays is set when the call leaves it standing. */
struct pool_rec {
    uint32_t base, size, used;
    bool stays;
};

/* Growing arrays, kept from call to call. */
#define ARRAY(T) struct { T *v; size_t n, cap; }

static ARRAY(struct grant) held;
static ARRAY(struct node_rec) nodes;
static ARRAY(struct pool_rec) pools;

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
    abort();
}

/* An installed region grants what the region capability it came from did. */
static struct grant grant_of(const struct cap *c)
{
    return (struct grant){ c->type == CAP_INSTALLED ? CAP_REGION : c->type, c->rights, c->a, c->b };
}

static bool same_grant(struct grant x, struct grant y)
{
    return x.type == y.type && x.rights == y.rights && x.a == y.a && x.b == y.b;
}

/* Whether a capability the caller held grants all that g does. */
static bool covered(struct grant g)
{
    bool range = g.type == CAP_REGION || g.type == CAP_IRQ_LINE;
    for (size_t i = 0; i < held.n; i++) {
        const struct grant *h = &held.v[i];
        if (h->type == g.type && (g.rights & ~h->rights) == 0 &&
            (range ? g.a - h->a < h->b && g.b <= h->b - (g.a - h->a) : g.a == h->a)) {
            return true;
        }
    }
    return false;
}

/* Both records start with the address they are sorted by. */
static int cmp_at(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}

static struct pool_rec *pool_before(uint32_t base)
{
    return bsearch(&base, pools.v, pools.n, sizeof(pools.v[0]), cmp_at);
}

/* One memcmp rather than a comparison per byte, which the fuzzer's instrumentation would slow. */
static bool zero(uint32_t base, uint32_t size)
{
    const uint8_t *p = p2v(base);
    return size == 0 || (p[0] == 0 && memcmp(p, p + 1, size - 1) == 0);
}

static void each_node(void (*fn)(const struct cap *))
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_CAPTABLE) {
            struct captable *t = (struct captable *)o;
            for (uint32_t i = 0; i < t->nslots; i++) {
                fn(&t->slots[i]);
            }
        } else if (o->type == CAP_PROCESS) {
            struct process *p = (struct process *)o;
            fn(&p->table);
            for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
                fn(&p->slots[i]);
            }
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
    held.n = nodes.n = pools.n = 0;

    const struct captable *table = current != NULL ? thread_table(current) : NULL;
    for (uint32_t i = 0; table != NULL && i < table->nslots; i++) {
        const struct cap *c = &table->slots[i];
        if (c->type == CAP_NONE) {
            continue;
        }
        PUSH(held, grant_of(c));
        /* The right to destroy a pool holds the memory the destroy gives back. */
        if (c->type == CAP_POOL && (c->rights & RIGHT_W)) {
            const struct pool *p = (const struct pool *)cap_object(c);
            PUSH(held, ((struct grant){ CAP_REGION, p->rights, p->base, p->size }));
        }
    }

    each_node(record_node);
    qsort(nodes.v, nodes.n, sizeof(nodes.v[0]), cmp_at);

    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        PUSH(pools, ((struct pool_rec){ p->base, p->size, p->used, false }));
    }
    qsort(pools.v, pools.n, sizeof(pools.v[0]), cmp_at);
}

/* Built by this call: in a pool that is new, or above the pool's used mark before it. */
static bool built(const struct obj_header *o)
{
    const struct pool_rec *p = pool_before(o->pool);
    return p == NULL || v2p(o) >= p->base + p->used;
}

/* An object the call built, which the caller must have been able to build. */
static void check_built(const struct obj_header *o)
{
    if (o->type == CAP_POOL) {
        const struct pool *p = (const struct pool *)o;
        if (!covered((struct grant){ CAP_REGION, p->rights, p->base, p->size })) {
            violated("a call made a pool of memory its caller held no region to");
        }
        return;
    }
    if (!covered((struct grant){ CAP_POOL, RIGHT_W, o->pool, 0 })) {
        violated("a call built an object in a pool its caller could not allocate from");
    }
    bool bound = true;
    if (o->type == CAP_THREAD) {
        bound = covered((struct grant){ CAP_PROCESS, RIGHT_W, ((const struct thread *)o)->proc, 0 });
    } else if (o->type == CAP_TIMER) {
        bound = covered((struct grant){ CAP_NOTIFICATION, RIGHT_W, ((const struct timer *)o)->ntfn, 0 });
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
    if (cap_has_object(c->type) && built(cap_object(c))) {
        check_built(cap_object(c));
    } else if (!covered(g)) {
        violated("a call made a capability no capability of its caller covers");
    }
}

void history_end(void)
{
    each_node(check_node);

    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        struct pool_rec *was = pool_before(p->base);
        if (was != NULL) {
            was->stays = true;
        } else if (!zero(p->base + p->used, p->size - p->used)) {
            violated("memory became a pool with bytes not zeroed");
        }
    }
    for (size_t i = 0; i < pools.n; i++) {
        if (!pools.v[i].stays && !zero(pools.v[i].base, pools.v[i].size)) {
            violated("memory left the kernel with bytes not zeroed");
        }
    }
}
