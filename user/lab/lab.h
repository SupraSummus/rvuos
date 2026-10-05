#ifndef RVUOS_LAB_H
#define RVUOS_LAB_H

/*
 * The laboratory, see NOTES.md: a made-up system of servers and clients,
 * whose root task, root.c, runs one scenario after another from the roles of roles.c
 * and says how long each client's asks waited.
 * The roles ask and answer with what the library offers, a hub of lib/chan.h on each server and the lock of lib/lock.h,
 * so a way of modelling IPC is tried by changing those few calls.
 */

#include "lib/chan.h"
#include "lib/child.h"
#include "lib/lock.h"

/* An ask, which costs the server cost iterations of lab_work, and its answer; the tag is the gate's. */
struct ask {
    uint32_t seq, cost, tag;
};

struct answer {
    uint32_t seq, tag, result;
};

#define LAB_ENDS      4u     /* the most clients a server has */
#define LAB_CHAN_SIZE 0x400u /* a client's channel of the hub */
#define LAB_SLOT      32u    /* a packet's room in a ring */
#define LAB_SAMPLES   256u   /* the latencies a client keeps, the first in the window */

/* What the root task says of the run, in each page. */
enum { LAB_WARM, LAB_MEASURE, LAB_OVER };

/* How long each ask a client made in the window took to be answered, in counts of the clock. */
struct lab_stats {
    volatile uint32_t n; /* sample[i] is written before n passes i */
    volatile uint32_t worst;
    uint32_t sample[LAB_SAMPLES];
};

/* Every role's page; each uses what its role needs of it. */
struct lab_page {
    struct child_page c;
    uint32_t clock; /* the slot of the clock, RIGHT_R */
    volatile uint32_t phase;
    uint32_t cost;      /* iterations of work for each ask, or for each hold of the lock */
    uint32_t period_us; /* how often a client asks or takes the lock; 0 for as often as it can */
    uint32_t jitter;    /* the most a periodic client works before each ask, drawn afresh each period */
    uint32_t gap;       /* a locker's work between holds */
    uint32_t local;     /* a client that does the work itself, as a library would, and asks nobody */
    struct chan_end up;             /* to its server, for a client and the gate */
    struct chan_end ends[LAB_ENDS]; /* from its clients, for a server */
    struct lock lock;               /* a locker's */
    struct lab_stats st;
};

/* Work of iterations that the compiler cannot leave out, about six instructions each. */
uint32_t lab_work(uint32_t iterations, uint32_t seed);
/* A number drawn from seed, the same for the same seed, so that a run under QEMU repeats. */
uint32_t lab_draw(uint32_t seed);

void store_main(struct child_page *page);
void gate_main(struct child_page *page);
void ui_main(struct child_page *page);
void bulk_main(struct child_page *page);
void hog_main(struct child_page *page);
void locker_main(struct child_page *page);

#endif
