/*
 * Kernel object pools.
 * See DESIGN.md, "Kernel pools and revocation".
 */

#include "kernel.h"
#include "object.h"

struct pool *pool_list;

static inline uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

struct pool *pool_create(paddr_t base, uint32_t size, struct cap *parent)
{
    struct pool *pool = p2v(base);
    CALL_BOUND(sizeof(struct pool));
    *pool = (struct pool){ 0 };
    pool->hdr.type = CAP_POOL;
    pool->hdr.pool = base;
    pool->node = (struct cap){ .type = CAP_RETYPED, .rights = RIGHT_ALL, .a = base };
    cap_attach(parent, &pool->node);
    pool->size = size;
    pool->used = align_up(sizeof(*pool), OBJ_ALIGN);
    pool->last = base;
    /* The list is the self-check's and the host's, and costs a step to join and one to leave. */
    pool->next = pool_list ? v2p(pool_list) : 0;
    if (pool_list != NULL) {
        pool_list->prev = base;
    }
    pool_list = pool;
    return pool;
}

static void pool_unlink(struct pool *pool)
{
    if (pool->prev != 0) {
        ((struct pool *)p2v(pool->prev))->next = pool->next;
    } else {
        pool_list = pool_next_pool(pool);
    }
    if (pool->next != 0) {
        ((struct pool *)p2v(pool->next))->prev = pool->prev;
    }
}

/*
 * The next node an object holds, from its slot *at on, or NULL when it holds no more:
 * a table's slots, and a process's table slot, 0, and its region slots.
 * *at moves to the slot found, so the scan does not look at a slot twice.
 */
static struct cap *held_node(struct obj_header *o, uint16_t *at)
{
    uint32_t count;
    struct cap *slots = obj_nodes(o, &count);
    for (; *at < count; (*at)++) {
        LOOP_BOUND(CAPTABLE_MAX_SLOTS);
        if (slots[*at].type != CAP_NONE) {
            return &slots[*at];
        }
    }
    return NULL;
}

_Static_assert(CAPTABLE_MAX_SLOTS <= UINT16_MAX && 1 + PROCESS_REGION_SLOTS <= CAPTABLE_MAX_SLOTS,
               "the sweep's slot fits its field");

bool pool_destroy(struct pool *pool, struct cap *keep, bool preempt)
{
    /* Nothing is allocated from a pool that is going, so the steps below only ever take. */
    pool->dying = 1;

    /*
     * Every capability to the pool and to its objects lies below the pool's node,
     * so revoking below it leaves none, keep aside, which the caller names the pool through.
     * Its progress is in the tree, and a new capability made meanwhile goes the same way.
     */
    if (keep != NULL) {
        while (cap_revoke_step(keep)) {
            LOOP_PAID(pool_destroy, node, "a node below the capability the destroy was made through");
            if (cap_stop_here(preempt)) {
                return false;
            }
        }
    }
    while (cap_revoke_step_except(&pool->node, keep)) {
        LOOP_PAID(pool_destroy, node, "a capability to the pool or to one of its objects");
        if (cap_stop_here(preempt)) {
            return false;
        }
    }

    /*
     * The objects go newest first, and the used mark follows,
     * so a stop leaves a pool with fewer objects and the next step is at its newest.
     * A thread is newer than its process and an Irq than its notification,
     * so no object is left pointing at one already gone.
     * The nodes an object holds are cleared as a delete clears them:
     * what was derived from them goes to their parents,
     * so no ring leads into memory that is about to be the Untyped's again,
     * and no clearing here destroys another pool.
     */
    while (pool->last != pool_base(pool)) {
        LOOP_PAID(pool_destroy, object, "an object of the pool, newest first");
        struct obj_header *o = p2v(pool->last);
        struct cap *n;
        while ((n = held_node(o, &pool->sweep)) != NULL) {
            LOOP_PAID(pool_destroy, node, "a node the object holds");
            if (!cap_delete(n, preempt) || cap_stop_here(preempt)) {
                return false;
            }
        }
        if (!sched_forget(o, preempt)) {
            return false;
        }
        /* The padding too, so that everything the pool ever handed out goes back zeroed. */
        uint32_t size = align_up((uint32_t)obj_size(o), OBJ_ALIGN);
        pool->last = v2p(o) - o->back * OBJ_ALIGN;
        pool->used = v2p(o) - pool_base(pool);
        pool->sweep = 0;
        CALL_BOUND(OBJ_MAX_SIZE + OBJ_ALIGN);
        memset(o, 0, size);
        if (pool->last != pool_base(pool) && cap_stop_here(preempt)) {
            return false;
        }
    }

    /* Nothing below may stop: the pool is empty, and its memory goes back to the Untyped. */
    if (keep != NULL) {
        cap_delete(keep, false);
    }
    cap_delete(&pool->node, false);
    pool_unlink(pool);
    CALL_BOUND(sizeof(struct pool));
    memset(pool, 0, sizeof(*pool));
    return true;
}

bool pool_fits(const struct pool *pool, size_t size)
{
    return align_up((uint32_t)size, OBJ_ALIGN) <= pool->size - pool->used;
}

void *pool_alloc(struct pool *pool, uint8_t type, size_t size)
{
    if (size > OBJ_MAX_SIZE || !pool_fits(pool, size)) {
        return NULL;
    }
    struct obj_header *obj = p2v(pool_base(pool) + pool->used);
    uint32_t aligned = align_up((uint32_t)size, OBJ_ALIGN);
    pool->used += aligned;
    /* The memory holds what it held before the pool took it; the padding is zeroed with the rest. */
    CALL_BOUND(OBJ_MAX_SIZE + OBJ_ALIGN);
    memset(obj, 0, aligned);
    obj->type = type;
    obj->pool = pool_base(pool);
    obj->back = (uint16_t)((v2p(obj) - pool->last) / OBJ_ALIGN);
    pool->last = v2p(obj);
    return obj;
}

_Static_assert(((OBJ_MAX_SIZE + OBJ_ALIGN - 1) / OBJ_ALIGN) <= UINT16_MAX, "an object's back field fits");

size_t obj_size(const struct obj_header *obj)
{
    switch (obj->type) {
    case CAP_POOL:
        return sizeof(struct pool);
    case CAP_CAPTABLE: {
        const struct captable *t = (const struct captable *)obj;
        return sizeof(*t) + t->nslots * sizeof(struct cap);
    }
    case CAP_PROCESS:
        return sizeof(struct process);
    case CAP_THREAD:
        return sizeof(struct thread);
    case CAP_NOTIFICATION:
        return sizeof(struct notification);
    case CAP_IRQ:
        return sizeof(struct irq);
    default:
        kpanic("object of unknown type in pool");
    }
}

struct obj_header *pool_first(struct pool *pool)
{
    return pool_next(pool, &pool->hdr);
}

struct obj_header *pool_next(struct pool *pool, struct obj_header *obj)
{
    paddr_t next = v2p(obj) + align_up((uint32_t)obj_size(obj), OBJ_ALIGN);
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
