#include "lib/event.h"

#include "rvuos.h"

static uint32_t *word(const struct event *e)
{
    return (uint32_t *)(uintptr_t)e->word;
}

void event_init(const struct event *e)
{
    __atomic_store_n(word(e), 0, __ATOMIC_RELEASE);
}

void event_set(const struct event *e)
{
    /* A waiter marks the word before it waits, so a set that finds the mark and no set before it signals. */
    if ((__atomic_fetch_or(word(e), EVENT_SET, __ATOMIC_RELEASE) & (EVENT_SET | EVENT_MARKED)) == EVENT_MARKED) {
        rv_signal(e->note, EVENT_BIT_SET);
    }
}

int event_wait(const struct event *e, uint32_t us)
{
    uint32_t was = __atomic_fetch_or(word(e), EVENT_MARKED, __ATOMIC_ACQUIRE);
    if (!(was & EVENT_SET)) {
        uint32_t timer = was & EVENT_PHASE ? EVENT_BIT_TIMER1 : EVENT_BIT_TIMER0;
        if (rv_timer_set(e->timer, timer, us) == KERR_OK) {
            /* A set's bit with the word clear, or the other timer's bit, was left by a wait before. */
            uint32_t bits = 0;
            while (!(bits & timer)) {
                if (rv_wait(e->note, &bits) != KERR_OK) {
                    bits = 0;
                    break;
                }
                if (!(bits & timer) && (__atomic_load_n(word(e), __ATOMIC_ACQUIRE) & EVENT_SET)) {
                    break;
                }
            }
            if (!(bits & timer)) {
                /* The line may still signal this bit, so the next wait arms the other. */
                __atomic_fetch_xor(word(e), EVENT_PHASE, __ATOMIC_RELAXED);
            }
        }
    }
    return (__atomic_fetch_and(word(e), ~(EVENT_SET | EVENT_MARKED), __ATOMIC_ACQUIRE) & EVENT_SET) != 0;
}
