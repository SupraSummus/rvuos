#ifndef RVUOS_LIB_EDF_H
#define RVUOS_LIB_EDF_H

/*
 * A scheduler in userspace, SCHED=user's: the order of requests earliest deadline first,
 * chosen by a thread of its own with OP_TIME_SCHEDULE from the accounts the kernel says have a ready thread.
 * An account's virtual time is the count at which it would be full again if its thread did not run, which the kernel tells;
 * a request is due its span past the virtual time it starts at, a tick's work at its units,
 * and it is over once the virtual time reaches that, its units' part of the span run.
 * A request starts as its account's thread is made ready, and the next as one is over;
 * the turn goes to the request due first, the oldest of those tied,
 * and while another waits it ends where its request is over, or at the next tick, whichever comes first.
 */

#include "lib/child.h"

#if SCHED_USER
/* The scheduler child's page: the units of its core, RIGHT_W, and the clock, whose description tells the tick. */
struct edf_page {
    struct child_page c;
    uint32_t time;
    uint32_t clock;
};

/* A child's entry: it chooses the turns of its core until it is taken down. */
void edf_main(struct child_page *page);
#endif

#endif
