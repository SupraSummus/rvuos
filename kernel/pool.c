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

struct pool *pool_create(paddr_t base, uint32_t size)
{
    struct pool *pool = p2v(base);
    pool->hdr.type = CAP_POOL;
    pool->hdr.generation = 1;
    pool->hdr.pool = base;
    pool->base = base;
    pool->size = size;
    pool->used = align_up(sizeof(*pool), OBJ_ALIGN);
    pool->next = pool_list ? v2p(pool_list) : 0;
    pool_list = pool;
    return pool;
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
    obj->generation = 1;
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

struct obj_header *pool_find(paddr_t p)
{
    for (struct pool *pool = pool_list; pool != NULL; pool = pool_next_pool(pool)) {
        if (p == pool->base) {
            return &pool->hdr;
        }
        if (p < pool->base || p >= pool->base + pool->used) {
            continue;
        }
        for (struct obj_header *o = pool_first(pool); o != NULL; o = pool_next(pool, o)) {
            if (v2p(o) == p) {
                return o;
            }
        }
    }
    return NULL;
}

bool pool_overlaps(uint32_t base, uint32_t size)
{
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        if (ranges_overlap(base, size, p->base, p->size)) {
            return true;
        }
    }
    return false;
}

bool installed_overlaps(uint32_t base, uint32_t size)
{
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        for (struct obj_header *o = pool_first(p); o != NULL; o = pool_next(p, o)) {
            if (o->type != CAP_PROCESS) {
                continue;
            }
            struct process *proc = (struct process *)o;
            for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
                const struct region_slot *s = &proc->slots[i];
                if (s->rights && ranges_overlap(base, size, s->base, s->size)) {
                    return true;
                }
            }
        }
    }
    return false;
}
