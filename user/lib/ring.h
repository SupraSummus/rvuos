#ifndef RVUOS_LIB_RING_H
#define RVUOS_LIB_RING_H

/*
 * A ring of packets in memory two processes share, one writing and the other reading.
 * The shared memory holds two counters, then the slots:
 * head, the slots put, which only the writer moves, and tail, the slots taken, which only the reader moves,
 * so neither needs a lock, and a fence orders each slot's bytes before the counter that hands it over.
 * Slots are of one size, a length word and the bytes; the count of slots is a power of two.
 *
 * Where the ring lies and its shape are kept apart from it, by each side in memory of its own,
 * so that neither can make the other read or write past the ring by changing them;
 * what the other side writes into the ring can cost a packet, never memory outside it.
 * It can make the ring seem to hold as many packets as it likes, though, by moving the counters,
 * so a reader that does not trust the writer takes a bounded number at a time.
 *
 * The ring says when to wake the other side, and the caller signals its notification:
 * a put into an empty ring, which the reader may be waiting on,
 * and a take from a full one, which the writer may be waiting on.
 * A reader takes until the ring is empty before it waits, and a notification's bits are sticky,
 * so no wake is lost.
 */

#include <stdint.h>

struct ring {
    uint32_t base;      /* the counters, then the slots */
    uint32_t slots;     /* a power of two */
    uint32_t slot_size; /* bytes a slot holds past its length word, a multiple of four */
};

/*
 * A ring laid out in bytes of memory at base: the slots of slot_size that fit, rounded down to a power of two.
 * Zeroed memory is an empty ring.
 */
struct ring ring_shape(uint32_t base, uint32_t bytes, uint32_t slot_size);
/* The bytes a ring of slots slots of slot_size takes, its counters among them. */
uint32_t ring_bytes(uint32_t slots, uint32_t slot_size);

/* The slot to fill next, or 0 if the ring is full. */
uint8_t *ring_put_begin(const struct ring *r);
/* Hands the slot over with len bytes; 1 if the ring was empty, when the reader is to be woken. */
int ring_put_end(const struct ring *r, uint32_t len);

/*
 * How many packets the ring holds as the writer says, at most its slots, which no honest writer goes past:
 * what a reader that does not trust the writer takes for one wake.
 */
uint32_t ring_ready(const struct ring *r);
/* The packets the reader has taken, as it says, counted from the ring's start: what a writer knows it has seen. */
uint32_t ring_taken(const struct ring *r);
/* The next slot to read and its length, at most slot_size, or 0 if the ring is empty. */
const uint8_t *ring_get_begin(const struct ring *r, uint32_t *len);
/* Gives the slot back; 1 if the ring was full, when the writer is to be woken. */
int ring_get_end(const struct ring *r);

#endif
