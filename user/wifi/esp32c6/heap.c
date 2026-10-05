/*
 * The heap the adapter hands Espressif's libraries, from the end of the image's data to the ROM's data.
 * First fit over blocks in address order, each with its size before it, free ones linked;
 * a block freed merges with a free neighbour on either side.
 * A pointer freed that is no block in use of the heap's is told in the log with whoever freed it, and left alone,
 * since taking it would hand out memory that is not the heap's.
 * The MAC reaches its buffers here, and these are what HP_APM is to let it reach; see drv.h.
 */

#include <stdint.h>

#include "drv.h"
#include "lib/libc.h"
#include "osi.h"

struct chunk {
    uint32_t size; /* of the whole block, header included; the low bit set while it is in use */
    struct chunk *next_free;
};

#define HEADER    8u
#define USED      1u
#define ALIGN     8u
#define MIN_BLOCK 16u

/* From the linker script, flash.ld.S. */
extern uint8_t drv_heap_start[], drv_heap_end[];

static struct {
    const struct lock *lock;
    struct chunk *free;
    uint8_t *base, *end;
    uint32_t free_bytes;
} heap;

void osi_heap_init(const struct lock *l)
{
    heap.lock = l;
    heap.base = (uint8_t *)(((uintptr_t)drv_heap_start + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));
    heap.end = (uint8_t *)((uintptr_t)drv_heap_end & ~(uintptr_t)(ALIGN - 1));
    heap.free = (struct chunk *)heap.base;
    heap.free->size = (uint32_t)(heap.end - heap.base);
    heap.free->next_free = 0;
    heap.free_bytes = heap.free->size;
}

void *osi_malloc(size_t size)
{
    uint32_t need = ((uint32_t)size + HEADER + ALIGN - 1) & ~(ALIGN - 1);
    if (need < MIN_BLOCK) {
        need = MIN_BLOCK;
    }
    lock_take(heap.lock);
    struct chunk **p = &heap.free;
    while (*p && (*p)->size < need) {
        p = &(*p)->next_free;
    }
    struct chunk *b = *p;
    if (b) {
        if (b->size - need >= MIN_BLOCK) {
            struct chunk *rest = (struct chunk *)((uint8_t *)b + need);
            rest->size = b->size - need;
            rest->next_free = b->next_free;
            b->size = need;
            *p = rest;
        } else {
            *p = b->next_free;
        }
        heap.free_bytes -= b->size;
        b->size |= USED;
    }
    lock_give(heap.lock);
    if (b == 0) {
        drv_say("heap: %u bytes asked for, %u free in all\n", (unsigned)size, (unsigned)heap.free_bytes);
    }
    return b ? (uint8_t *)b + HEADER : 0;
}

/* Whether ptr is what osi_malloc handed out and nobody freed since. */
static int in_use(const void *ptr)
{
    const uint8_t *p = ptr;
    if (p < heap.base + HEADER || p >= heap.end || ((uintptr_t)p & (ALIGN - 1)) != 0) {
        return 0;
    }
    const struct chunk *b = (const struct chunk *)(p - HEADER);
    uint32_t size = b->size & ~USED;
    return (b->size & USED) && size >= MIN_BLOCK && size <= (uint32_t)(heap.end - (const uint8_t *)b) &&
           (size & (ALIGN - 1)) == 0;
}

void osi_free(void *ptr)
{
    if (ptr == 0) {
        return;
    }
    struct chunk *b = (struct chunk *)((uint8_t *)ptr - HEADER);
    lock_take(heap.lock);
    if (!in_use(ptr)) {
        lock_give(heap.lock);
        drv_say("heap: %p freed from %p is no block in use; left alone\n", ptr, __builtin_return_address(0));
        return;
    }
    b->size &= ~USED;
    heap.free_bytes += b->size;
    /* Into the address-ordered free list, merging with the block after it and the block before it. */
    struct chunk **p = &heap.free;
    while (*p && *p < b) {
        p = &(*p)->next_free;
    }
    b->next_free = *p;
    if (b->next_free && (uint8_t *)b + b->size == (uint8_t *)b->next_free) {
        b->size += b->next_free->size;
        b->next_free = b->next_free->next_free;
    }
    *p = b;
    if (p != &heap.free) {
        struct chunk *before = (struct chunk *)((uint8_t *)p - offsetof(struct chunk, next_free));
        if ((uint8_t *)before + before->size == (uint8_t *)b) {
            before->size += b->size;
            before->next_free = b->next_free;
        }
    }
    lock_give(heap.lock);
}

void *osi_calloc(size_t n, size_t size)
{
    void *p = osi_malloc(n * size);
    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void *osi_realloc(void *ptr, size_t size)
{
    if (ptr == 0) {
        return osi_malloc(size);
    }
    if (!in_use(ptr)) {
        drv_say("heap: %p grown from %p is no block in use\n", ptr, __builtin_return_address(0));
        return 0;
    }
    struct chunk *b = (struct chunk *)((uint8_t *)ptr - HEADER);
    uint32_t have = (b->size & ~USED) - HEADER;
    if (size <= have) {
        return ptr;
    }
    void *p = osi_malloc(size);
    if (p) {
        memcpy(p, ptr, have);
        osi_free(ptr);
    }
    return p;
}

uint32_t osi_heap_free(void)
{
    return heap.free_bytes;
}
