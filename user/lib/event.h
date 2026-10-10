#ifndef RVUOS_LIB_EVENT_H
#define RVUOS_LIB_EVENT_H

/*
 * An event in memory that processes, or threads, share and trust each other with:
 * one thread waits for it, until a deadline at most, and any other sets it.
 * A word in that memory, a notification only the waiter waits on, and a timer line bound to that notification.
 * A set is one atomic operation, and signals only when the waiter may be waiting;
 * sets before a wait are kept as one, which the wait takes.
 * One thread waits at a time, which the callers see to: with several on one notification,
 * the timer's bit could wake another, which is also why lib/lock.h's take has no deadline.
 *
 * A setter needs RIGHT_W on the notification, the waiter RIGHT_R and the timer line, which nothing else arms.
 * A timer line set again takes back no signal it already made, so a wait a set ended may leave its bit for the next;
 * the waiter arms its two timer bits in turn, the word's EVENT_PHASE saying which is next, and passes over the other.
 */

#include <stdint.h>

/* The notification's bits: the one a set signals, and the timer's two. */
#define EVENT_BIT_SET    0x1u
#define EVENT_BIT_TIMER0 0x2u
#define EVENT_BIT_TIMER1 0x4u

/* The word's bits: set, the waiter may be waiting, and with EVENT_PHASE the next wait arms EVENT_BIT_TIMER1. */
#define EVENT_SET    0x1u
#define EVENT_MARKED 0x2u
#define EVENT_PHASE  0x4u

struct event {
    uint32_t word;  /* the address of the word the waiter and every setter share */
    uint32_t note;  /* the slot of the notification the waiter waits on */
    uint32_t timer; /* the waiter's: the slot of a timer line bound to that notification */
};

/* Clears the word, before anyone sets the event or waits for it. */
void event_init(const struct event *e);
/* Sets the event, and wakes the waiter if it may be waiting. */
void event_set(const struct event *e);
/*
 * Waits until the event is set, but no longer than us microseconds, which end at the kernel's first tick after them:
 * 1 if it was set, which takes the set, or 0. A wait whose call fails returns at once, as one the deadline ended.
 */
int event_wait(const struct event *e, uint32_t us);

#endif
