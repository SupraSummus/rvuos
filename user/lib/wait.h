#ifndef RVUOS_LIB_WAIT_H
#define RVUOS_LIB_WAIT_H

/*
 * Waits on one notification that lose nothing to a nested wait.
 *
 * A wait takes every bit, and a line fires once until it is armed again,
 * so a wait nested in a loop could take a bit the loop needs, and its source would fall silent.
 * A waiter keeps the bits a wait took but did not ask for, for the wait that asks,
 * and arms its periodic timer lines again as soon as any wait takes their bits;
 * a device's line stays its driver's to arm, once it has serviced the device.
 *
 * Every wait on the notification goes through the waiter, from one thread,
 * and nothing but a periodic line signals its bits.
 * A call that fails returns the kernel's status, or KERR_INVALID_ARG or KERR_LIMIT for what the waiter refuses.
 */

#include <stdint.h>

#define WAITER_PERIODS 4u /* the most periodic lines a waiter arms */

struct waiter_period {
    uint32_t timer;  /* the slot of the timer line's Irq, bound to the waiter's notification */
    uint32_t bits;   /* the bits it signals */
    uint32_t us;     /* its period */
    uint32_t passed; /* the periods that passed since waiter_passed last asked: each the line fired, and each it skipped */
};

struct waiter {
    uint32_t note;  /* the slot of the notification */
    uint32_t kept;  /* bits a wait took that its caller did not ask for */
    uint32_t count; /* of periods */
    struct waiter_period periods[WAITER_PERIODS];
};

/* A waiter on the notification in slot note, which keeps nothing yet and arms no line. */
void waiter_init(struct waiter *w, uint32_t note);
/*
 * Arms the timer line in slot timer, bound to the waiter's notification, to signal bits every us microseconds,
 * counted from its last deadline, and makes it the waiter's to arm again;
 * setting a line it arms already gives it the new bits and period, and us = 0 masks the line and forgets it.
 */
uint32_t waiter_period(struct waiter *w, uint32_t timer, uint32_t bits, uint32_t us);
/*
 * Blocks until some bit of want is set, or was kept, and returns those in bits, keeping the rest for later waits;
 * span is the request the wake begins, as rv_wait_within's.
 */
uint32_t waiter_wait(struct waiter *w, uint32_t want, uint32_t span, uint32_t *bits);
/* The periods of the line that signals bits that passed since the last call; one bit taken may stand for several. */
uint32_t waiter_passed(struct waiter *w, uint32_t bits);

#endif
