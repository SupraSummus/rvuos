#include "lib/ring.h"

#define SLOT_HEADER 4u

/* What the two sides share at the ring's base. */
struct counters {
    volatile uint32_t head;
    volatile uint32_t tail;
};

static struct counters *counters(const struct ring *r)
{
    return (struct counters *)(uintptr_t)r->base;
}

static uint8_t *slot(const struct ring *r, uint32_t n)
{
    uint8_t *first = (uint8_t *)(uintptr_t)r->base + sizeof(struct counters);
    return first + (n & (r->slots - 1u)) * (SLOT_HEADER + r->slot_size);
}

struct ring ring_shape(uint32_t base, uint32_t bytes, uint32_t slot_size)
{
    slot_size = (slot_size + 3u) & ~3u;
    uint32_t fit = (bytes - sizeof(struct counters)) / (SLOT_HEADER + slot_size);
    uint32_t slots = 1;
    while (slots * 2u <= fit) {
        slots *= 2u;
    }
    return (struct ring){ base, slots, slot_size };
}

void ring_init(const struct ring *r)
{
    counters(r)->head = 0;
    counters(r)->tail = 0;
}

uint8_t *ring_put_begin(const struct ring *r)
{
    struct counters *c = counters(r);
    uint32_t head = c->head;
    if (head - c->tail >= r->slots) {
        return 0;
    }
    return slot(r, head) + SLOT_HEADER;
}

int ring_put_end(const struct ring *r, uint32_t len)
{
    struct counters *c = counters(r);
    uint32_t head = c->head;
    *(uint32_t *)slot(r, head) = len;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    c->head = head + 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return c->tail == head;
}

const uint8_t *ring_get_begin(const struct ring *r, uint32_t *len)
{
    struct counters *c = counters(r);
    uint32_t tail = c->tail;
    if (tail == c->head) {
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint8_t *s = slot(r, tail);
    *len = *(const volatile uint32_t *)s;
    if (*len > r->slot_size) {
        *len = r->slot_size;
    }
    return s + SLOT_HEADER;
}

int ring_get_end(const struct ring *r)
{
    struct counters *c = counters(r);
    uint32_t tail = c->tail;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    c->tail = tail + 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return c->head - tail == r->slots;
}
