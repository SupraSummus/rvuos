#ifndef RVUOS_LIB_SEQLOCK_H
#define RVUOS_LIB_SEQLOCK_H

/*
 * A snapshot one writer publishes in memory it shares and any number of readers copy out whole,
 * neither side making a system call or waiting for the other; see DESIGN.md, "Communication and synchronisation".
 * The memory holds a count, then two copies:
 * a write moves the count to odd, fills the first copy, moves it to even and fills the second,
 * and a reader copies the one the count names and reads the count again.
 * So a reader gets the last whole snapshot however far the writer got, or stopped, halfway,
 * and finds the count moved only when a write began meanwhile, which sends it round again.
 *
 * Readers write nothing, so they may hold the memory read only.
 * A writer can keep its readers going round, or hand them what it likes,
 * so a reader that does not trust it bounds its tries and checks what it read.
 * One writer at a time, and no pointer in the snapshot to what a write may free, since a reader follows it after its check.
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
/* Copies the snapshot into out, word aligned: 1, with *version the writes it holds, or 0 if a write began meanwhile. */
int seqlock_read(const struct seqlock *s, void *out, uint32_t *version);

#endif
