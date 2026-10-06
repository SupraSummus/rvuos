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

/* A named lock's word: free, or its holder's name shifted up, with the low bit marked while a taker may be waiting. */
#define NAMED_MARKED 1u

static uint32_t *named_word(const struct named_lock *l)
{
    return (uint32_t *)(uintptr_t)l->word;
}

void named_lock_init(const struct named_lock *l)
{
    __atomic_store_n(named_word(l), LOCK_FREE, __ATOMIC_RELEASE);
}

int named_lock_try(const struct named_lock *l)
{
    uint32_t free = LOCK_FREE;
    return __atomic_compare_exchange_n(named_word(l), &free, l->name << 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

uint32_t named_lock_take(const struct named_lock *l)
{
    uint32_t *word = named_word(l);
    uint32_t seen = LOCK_FREE;
    if (__atomic_compare_exchange_n(word, &seen, l->name << 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return KERR_OK;
    }
    /*
     * Marked before each wait, so that the give that ends the hold signals, and taken marked, as another may wait;
     * a failed exchange leaves in seen what the word holds, and the next round looks at that.
     */
    for (;;) {
        if (seen == LOCK_FREE) {
            if (__atomic_compare_exchange_n(word, &seen, l->name << 1 | NAMED_MARKED, 0, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return KERR_OK;
            }
            continue;
        }
        if (!(seen & NAMED_MARKED) &&
            !__atomic_compare_exchange_n(word, &seen, seen | NAMED_MARKED, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            continue;
        }
        uint32_t holder = seen >> 1;
        uint32_t lend = holder <= LOCK_NAMES && holder != l->name ? l->lend[holder] : 0;
        uint32_t bits, status = lend != 0 ? rv_lend(l->note, lend, &bits) : rv_wait(l->note, &bits);
        if (status != KERR_OK) {
            return status;
        }
        seen = __atomic_load_n(word, __ATOMIC_RELAXED);
    }
}

void named_lock_give(const struct named_lock *l)
{
    if (__atomic_exchange_n(named_word(l), LOCK_FREE, __ATOMIC_RELEASE) & NAMED_MARKED) {
        rv_signal(l->note, NOTIFY_ALL_BITS);
    }
}
