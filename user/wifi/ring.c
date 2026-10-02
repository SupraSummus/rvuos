#include "ring.h"

#define SLOT_HEADER 4u

static uint8_t *slot(struct ring *r, uint32_t n)
{
    uint8_t *base = (uint8_t *)(r + 1);
    return base + (n & (r->slots - 1u)) * (SLOT_HEADER + r->slot_size);
}

void ring_init(struct ring *r, uint32_t bytes, uint32_t slot_size)
{
    slot_size = (slot_size + 3u) & ~3u;
    uint32_t fit = (bytes - sizeof(*r)) / (SLOT_HEADER + slot_size);
    uint32_t slots = 1;
    while (slots * 2u <= fit) {
        slots *= 2u;
    }
    r->head = 0;
    r->tail = 0;
    r->slots = slots;
    r->slot_size = slot_size;
}

uint8_t *ring_put_begin(struct ring *r)
{
    uint32_t head = r->head;
    if (head - r->tail >= r->slots) {
        return 0;
    }
    return slot(r, head) + SLOT_HEADER;
}

int ring_put_end(struct ring *r, uint32_t len)
{
    uint32_t head = r->head;
    *(uint32_t *)slot(r, head) = len;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->head = head + 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r->tail == head;
}

const uint8_t *ring_get_begin(struct ring *r, uint32_t *len)
{
    uint32_t tail = r->tail;
    if (tail == r->head) {
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint8_t *s = slot(r, tail);
    *len = *(const uint32_t *)s;
    if (*len > r->slot_size) {
        *len = r->slot_size;
    }
    return s + SLOT_HEADER;
}

int ring_get_end(struct ring *r)
{
    uint32_t tail = r->tail;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->tail = tail + 1u;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return r->head - tail == r->slots;
}
