#ifndef RVUOS_WIFI_RING_H
#define RVUOS_WIFI_RING_H

/*
 * A ring of packets in memory two processes share, one writing and the other reading.
 * Slots are of one size, a length word and the bytes; the count of slots is a power of two.
 * Only the writer moves head and only the reader moves tail, so neither needs a lock,
 * and a fence orders each slot's bytes before the index that hands it over.
 *
 * The rings say when to wake the other side, and the caller signals its notification:
 * a put into an empty ring, which the reader may be waiting on,
 * and a take from a full one, which the writer may be waiting on.
 * A reader takes until the ring is empty before it waits, and a notification's bits are sticky,
 * so no wake is lost.
 */

#include <stdint.h>

struct ring {
    volatile uint32_t head; /* slots put, by the writer */
    volatile uint32_t tail; /* slots taken, by the reader */
    uint32_t slots;         /* a power of two */
    uint32_t slot_size;     /* bytes a slot holds, past its length word */
};

/* Lays a ring out in bytes of memory, before either side uses it; the slots that fit, rounded down to a power of two. */
void ring_init(struct ring *r, uint32_t bytes, uint32_t slot_size);

/* The slot to fill next, or 0 if the ring is full. */
uint8_t *ring_put_begin(struct ring *r);
/* Hands the slot over with len bytes; 1 if the ring was empty, when the reader is to be woken. */
int ring_put_end(struct ring *r, uint32_t len);

/* The next slot to read and its length, or 0 if the ring is empty. */
const uint8_t *ring_get_begin(struct ring *r, uint32_t *len);
/* Gives the slot back; 1 if the ring was full, when the writer is to be woken. */
int ring_get_end(struct ring *r);

#endif
