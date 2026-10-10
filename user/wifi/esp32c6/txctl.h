#ifndef RVUOS_WIFI_TXCTL_H
#define RVUOS_WIFI_TXCTL_H

/*
 * The sending's choices for each try of a frame, which mac.c carries out: its rate, and the backoff before it.
 *
 * The rates are indexes into mac.c's table, fastest first, the last the slowest.
 * The rate control is AARF's (Lacage et al., 2004), over the data frames to the access point:
 * a frame's first two tries go at the rate held, each try after them a step slower.
 * `up` frames in a row whose first try was acknowledged raise the held rate a step, on probation:
 * the next first try lost brings it back and doubles `up`, to TXCTL_UP_MAX at the most;
 * two first tries lost in a row lower it a step and set `up` back to TXCTL_UP_MIN.
 *
 * The backoff is the slots the MAC waits, once the medium has been idle for the AIFS, before the try goes.
 * Each try draws it anew, evenly from 0 to the contention window,
 * which is TXCTL_CW_MIN for a frame's first try and doubles with each try after, to TXCTL_CW_MAX:
 * 802.11's best-effort bounds, which the libraries' lmacInitAc gives their best-effort queue.
 * A station that draws less, or does not double, takes the medium from the others more often than its share.
 */

#include <stdint.h>

#define TXCTL_CW_MIN 15u
#define TXCTL_CW_MAX 1023u
#define TXCTL_CW_STEPS 6u /* the doublings from the least to the most */
#define TXCTL_UP_MIN 10u
#define TXCTL_UP_MAX 50u

struct txctl {
    uint32_t rates;   /* how many rates there are */
    uint32_t at;      /* the index of the rate held */
    uint32_t ok;      /* frames in a row whose first try was acknowledged */
    uint32_t lost;    /* frames in a row whose first try was not */
    uint32_t up;      /* the frames in a row that raise the rate */
    int probing;      /* the rate was just raised */
    uint32_t prng;    /* the backoff's generator, xorshift32, never 0 */
};

/* The control started for rates rates, holding rate start, its backoffs drawn from seed. */
void txctl_start(struct txctl *c, uint32_t rates, uint32_t start, uint32_t seed);

/* The index of the rate a frame's try goes at, its first try 0. */
uint32_t txctl_rate(const struct txctl *c, uint32_t try);

/* A data frame's first try told: acknowledged or not. */
void txctl_first(struct txctl *c, int acked);

/* The contention window of a frame's try, and a backoff drawn from it, in slots. */
uint32_t txctl_window(uint32_t try);
uint32_t txctl_backoff(struct txctl *c, uint32_t try);

#endif
