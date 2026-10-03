#include "lib/lock.h"

#include "rvuos.h"

/* What the word holds: free, held, or held with a taker that may be waiting. */
#define LOCK_FREE   0u
#define LOCK_HELD   1u
#define LOCK_MARKED 2u

static uint32_t *word(const struct lock *l)
{
    return (uint32_t *)(uintptr_t)l->word;
}

void lock_init(const struct lock *l)
{
    __atomic_store_n(word(l), LOCK_FREE, __ATOMIC_RELEASE);
}

int lock_try(const struct lock *l)
{
    uint32_t free = LOCK_FREE;
    return __atomic_compare_exchange_n(word(l), &free, LOCK_HELD, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

uint32_t lock_take(const struct lock *l)
{
    if (lock_try(l)) {
        return KERR_OK;
    }
    /* Marked before each wait, so that the give that ends the hold signals; and taken marked, as another may wait. */
    while (__atomic_exchange_n(word(l), LOCK_MARKED, __ATOMIC_ACQUIRE) != LOCK_FREE) {
        uint32_t bits, status = rv_wait(l->note, &bits);
        if (status != KERR_OK) {
            return status;
        }
    }
    return KERR_OK;
}

void lock_give(const struct lock *l)
{
    if (__atomic_exchange_n(word(l), LOCK_FREE, __ATOMIC_RELEASE) == LOCK_MARKED) {
        rv_signal(l->note, NOTIFY_ALL_BITS);
    }
}
