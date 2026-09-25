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

struct pool *pool_create(paddr_t base, uint32_t size, uint8_t rights, struct pool *parent)
{
    struct pool *pool = p2v(base);
    pool->hdr.type = CAP_POOL;
    pool->hdr.pool = base;
    pool->base = base;
    pool->size = size;
    pool->used = align_up(sizeof(*pool), OBJ_ALIGN);
    pool->rights = rights;
    pool->next = pool_list ? v2p(pool_list) : 0;
    pool->parent = parent != NULL ? v2p(parent) : 0;
    pool_list = pool;
    return pool;
}

bool pool_under(const struct pool *pool, const struct pool *ancestor)
{
    for (const struct pool *p = pool; p != NULL; p = pool_parent(p)) {
        LOOP_WALK(pool_under);
        if (p == ancestor) {
            return true;
        }
    }
    return false;
}

void pool_destroy(struct pool *pool)
{
    /*
     * The capability sweep first, over every pool that is going,
     * while all of them still read as they were:
     * a slot in one of them can be linked to a slot in another,
     * and the sweep follows those links.
     */
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        LOOP_WALK(pool_destroy);
        if (pool_under(p, pool)) {
            cap_revoke_range(p->base, p->size);
        }
    }
    /*
     * The list has the newest pool first and a child is newer than its parent,
     * so one walk takes every pool below this one before the pool itself,
     * and a parent read on the way up is never one already zeroed.
     * Keeping the last pool that stays unlinks each one that goes without a second walk.
     */
    for (struct pool *p = pool_list, *next, *kept = NULL; p != NULL; p = next) {
        LOOP_WALK(pool_destroy);
        next = pool_next_pool(p);
        if (!pool_under(p, pool)) {
            kept = p;
            continue;
        }
        /* Nothing below may fail: the pool is going. */
        sched_forget_pool(p);
        if (kept != NULL) {
            kept->next = p->next;
        } else {
            pool_list = next;
        }
        /* The memory is about to be user memory again, holding other processes' tables. */
        CALL_WALK(memset);
        memset(p2v(p->base), 0, p->size);
    }
}

void *pool_alloc(struct pool *pool, uint8_t type, size_t size)
{
    uint32_t len = align_up((uint32_t)size, OBJ_ALIGN);
    if (len > pool->size - pool->used) {
        return NULL;
    }
    struct obj_header *obj = p2v(pool->base + pool->used);
    pool->used += len;
    /* Pool memory is zeroed on creation and never reused before destroy. */
    obj->type = type;
    obj->pool = pool->base;
    return obj;
}

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
    case CAP_TIMER:
        return sizeof(struct timer);
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
    if (next >= pool->base + pool->used) {
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

bool pool_overlaps(uint32_t base, uint32_t size)
{
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        LOOP_WALK(pool_overlaps);
        if (ranges_overlap(base, size, p->base, p->size)) {
            return true;
        }
    }
    return false;
}

bool installed_overlaps(uint32_t base, uint32_t size)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        LOOP_WALK(installed_overlaps);
        if (o->type != CAP_PROCESS) {
            continue;
        }
        struct process *proc = (struct process *)o;
        for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
            LOOP_BOUND(PROCESS_REGION_SLOTS);
            const struct cap *s = &proc->slots[i];
            if (s->type != CAP_NONE && ranges_overlap(base, size, s->a, s->b)) {
                return true;
            }
        }
    }
    return false;
}

