#ifndef RVUOS_LIB_SEQLOCK_H
#define RVUOS_LIB_SEQLOCK_H

/*
 * A snapshot one writer publishes in memory it shares and any number of readers copy out whole,
 * neither side making a system call or waiting for the other; see DESIGN.md, "Communication and synchronisation".
 * The memory holds a count, then two copies:
 * a write fills the copy the count does not name, then moves the count on, which names it,
 * and a reader copies the one the count names and reads the count again.
 * So a reader gets the last whole snapshot however far a writer got, or stopped, halfway,
 * a writer that starts again after one stopped fills the same copy anew,
 * and a reader finds the count moved only when a write was published meanwhile, which sends it round again.
 *
 * Readers write nothing, so they may hold the memory read only.
 * A writer can keep its readers going round, or hand them what it likes,
 * so a reader that does not trust it bounds its tries and checks what it read.
 * One writer at a time, and no pointer in the snapshot to what a write may free, since a reader follows it after its check.
 * The count is 32 bits: versions wrap, and a reader stopped in the middle of its copy for 2^32 writes takes a torn one.
 */

#include <stdint.h>

struct seqlock {
    uint32_t base; /* the count, then the two copies */
    uint32_t size; /* bytes a copy holds, a multiple of four */
};

/* The bytes a seqlock of a snapshot of size bytes takes in shared memory. */
#define SEQLOCK_BYTES(size) (4u + 2u * (size))

/* A seqlock at base, of a snapshot of size bytes, a multiple of four. */
struct seqlock seqlock_shape(uint32_t base, uint32_t size);
/* Zeroes the count and both copies, before anyone reads: version 0. */
void seqlock_init(const struct seqlock *s);
/* Publishes s->size bytes from data, word aligned. */
void seqlock_write(const struct seqlock *s, const void *data);
/*
 * Copies the snapshot into out, word aligned: 1, with *version the writes published before it,
 * or 0 if one was published meanwhile, which leaves out torn.
 */
int seqlock_read(const struct seqlock *s, void *out, uint32_t *version);

#endif
