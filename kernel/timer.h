#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

/*
 * The machine timer, which provides the scheduling tick.
 * The kernel handles it directly; see DESIGN.md, "Scheduling".
 */

/* Ticks per second. */
#define TIMER_HZ 1000

/* Microseconds per tick; the unit OP_TIMER_SET takes is rounded up to whole ticks. */
#define TIMER_US_PER_TICK (1000000u / TIMER_HZ)
_Static_assert(TIMER_US_PER_TICK * TIMER_HZ == 1000000u, "the tick divides a second");

/* Program the first tick and enable the machine timer interrupt. */
void timer_init(void);

/* Acknowledge a tick by programming the next one. */
void timer_ack(void);

/*
 * Stall until the next tick is pending and acknowledge it.
 * This is what the kernel does when no thread can run
 * but a Timer will make one runnable; see DESIGN.md, "Scheduling".
 * The host build has no clock to wait for and must never get here.
 */
void timer_wait(void);

#endif
