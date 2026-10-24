#include "lib/wait.h"

#include "rvuos.h"

void waiter_init(struct waiter *w, uint32_t note)
{
    w->note = note;
    w->kept = 0;
    w->count = 0;
}

static struct waiter_period *period_of(struct waiter *w, uint32_t timer)
{
    for (uint32_t i = 0; i < w->count; i++) {
        if (w->periods[i].timer == timer) {
            return &w->periods[i];
        }
    }
    return 0;
}

uint32_t waiter_period(struct waiter *w, uint32_t timer, uint32_t bits, uint32_t us)
{
    struct waiter_period *p = period_of(w, timer);
    if (us == 0) {
        uint32_t status = rv_irq_set(timer, 0);
        if (status == KERR_OK && p != 0) {
            *p = w->periods[--w->count];
        }
        return status;
    }
    if (bits == 0) {
        return KERR_INVALID_ARG;
    }
    if (p == 0 && w->count == WAITER_PERIODS) {
        return KERR_LIMIT;
    }
    uint32_t skipped;
    uint32_t status = rv_timer_period(timer, bits, us, &skipped);
    if (status != KERR_OK) {
        return status;
    }
    if (p == 0) {
        p = &w->periods[w->count++];
        p->passed = 0;
    }
    p->timer = timer;
    p->bits = bits;
    p->us = us;
    return KERR_OK;
}

uint32_t waiter_wait(struct waiter *w, uint32_t want, uint32_t span, uint32_t *bits)
{
    if (want == 0) {
        return KERR_INVALID_ARG;
    }
    while (!(w->kept & want)) {
        uint32_t got;
        uint32_t status = rv_wait_within(w->note, span, &got);
        if (status != KERR_OK) {
            return status;
        }
        w->kept |= got;
        /* Each line that fired, armed again now, whichever wait asked for its bits. */
        for (uint32_t i = 0; i < w->count; i++) {
            struct waiter_period *p = &w->periods[i];
            if (got & p->bits) {
                uint32_t skipped;
                status = rv_timer_period(p->timer, p->bits, p->us, &skipped);
                if (status != KERR_OK) {
                    return status;
                }
                p->passed += 1u + skipped;
            }
        }
    }
    *bits = w->kept & want;
    w->kept &= ~want;
    return KERR_OK;
}

uint32_t waiter_passed(struct waiter *w, uint32_t bits)
{
    uint32_t passed = 0;
    for (uint32_t i = 0; i < w->count; i++) {
        if (w->periods[i].bits & bits) {
            passed += w->periods[i].passed;
            w->periods[i].passed = 0;
        }
    }
    return passed;
}
