#ifndef RVUOS_LIB_LOCK_H
#define RVUOS_LIB_LOCK_H

/*
 * A lock over memory that processes, or threads, share and trust each other with:
 * a word in that memory, and a notification every holder has with RIGHT_R and RIGHT_W and nothing else uses.
 * Taking a free lock and giving it back is one atomic operation each, with no system call;
 * a taker that finds it held marks the word and waits, and a give that finds it marked signals.
 * The bits are sticky, so a give between the mark and the wait is not lost.
 * No taker spins first: on one core the holder cannot run while the taker does.
 * A give does not hand the lock to the taker it wakes, so a holder that takes it again at once keeps it:
 * the lock is not fair.
 *
 * Each holder keeps the word's address and the notification's slot in memory of its own,
 * so none can make another write past the word; but any can keep the lock, or break it by writing the word,
 * and the kernel, which knows nothing of it, does not give back a lock whose holder faulted.
 */

#include <stdint.h>

struct lock {
    uint32_t word; /* the address of the word every holder shares */
    uint32_t note; /* the slot of the notification its takers wait on */
};

/* Empties the word, before anyone takes the lock. */
void lock_init(const struct lock *l);
/* Takes the lock if it is free: 1, or 0 if it is held. */
int lock_try(const struct lock *l);
/* Takes the lock, waiting while it is held: KERR_OK, or the status of a wait that failed, without the lock. */
uint32_t lock_take(const struct lock *l);
/* Gives the lock back, and wakes a taker if one may be waiting. */
void lock_give(const struct lock *l);

#endif
