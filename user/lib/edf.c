#include "lib/edf.h"

#include "lib/libc.h"

#if SCHED_USER

#define ACCOUNTS TIME_UNITS

struct account {
    uint32_t vtime;    /* its virtual time, as the kernel last told it */
    uint32_t deadline; /* when its request is due */
    uint32_t since;    /* when it joined, for the oldest of those tied */
    uint32_t units;    /* 0 until the kernel told it */
};

static int before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* Whether a request due at deadline goes on at virtual time vt: it lies at most a span ahead. */
static int open(uint32_t deadline, uint32_t vt, uint32_t span)
{
    return deadline - vt - 1u < span;
}

void edf_main(struct child_page *page)
{
    struct edf_page *p = (struct edf_page *)page;
    struct account acc[ACCOUNTS];
    memset(acc, 0, sizeof(acc));
    uint32_t w[3];
    if (rv_cap_info_of(p->clock, CAP_CLOCK, w) != KERR_OK) {
        rv_breakpoint();
    }
    uint32_t tick = w[1];
    struct sched_choice c;
    uint32_t chosen = SCHED_NONE, limit = 0, joins = 0;
    uint64_t was = 0; /* the accounts the last choice found ready */
    for (;;) {
        uint32_t status = rv_time_schedule(p->time, chosen, limit, &c);
        if (status != KERR_OK && status != KERR_STATE) {
            rv_breakpoint();
        }
        /* The account that ran has moved on, and its request may be over. */
        if (status == KERR_OK && chosen != SCHED_NONE) {
            struct account *a = &acc[chosen];
            uint32_t span = TIME_UNITS * tick / a->units;
            a->vtime = c.vtime;
            if (!open(a->deadline, a->vtime, span)) {
                a->deadline = a->vtime + span;
            }
        }
        uint64_t ready = (uint64_t)c.ready[1] << 32 | c.ready[0];
        /* A thread made ready starts a request, from the count if its account is full. */
        for (uint64_t joined = ready & ~was; joined != 0; joined &= joined - 1) {
            struct account *a = &acc[__builtin_ctzll(joined)];
            if (a->units == 0) {
                uint32_t now;
                if (rv_time_due(p->time, (uint32_t)__builtin_ctzll(joined), &a->vtime, &a->units, &now) != KERR_OK ||
                    a->units == 0) {
                    rv_breakpoint();
                }
            }
            uint32_t vt = before(a->vtime, c.now) ? c.now : a->vtime;
            a->deadline = vt + TIME_UNITS * tick / a->units;
            a->since = joins++;
        }
        was = ready;
        chosen = SCHED_NONE;
        for (uint64_t left = ready; left != 0; left &= left - 1) {
            uint32_t i = (uint32_t)__builtin_ctzll(left);
            const struct account *a = &acc[i], *b = &acc[chosen != SCHED_NONE ? chosen : i];
            if (chosen == SCHED_NONE || before(a->deadline, b->deadline) ||
                (a->deadline == b->deadline && before(a->since, b->since))) {
                chosen = i;
            }
        }
        /* While another waits, its request ends where its virtual time reaches the deadline, units / 64 of a count a count. */
        limit = 0;
        if (chosen != SCHED_NONE && (ready & (ready - 1)) != 0) {
            const struct account *a = &acc[chosen];
            uint32_t left = before(a->vtime, a->deadline) ? a->deadline - a->vtime : 0;
            limit = (left * a->units + TIME_UNITS - 1u) / TIME_UNITS;
            limit = limit != 0 ? limit : 1u;
        }
    }
}

#endif
