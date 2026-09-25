#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

/*
 * The machine timer, which provides the scheduling tick.
 * The kernel handles it directly; see DESIGN.md, "Scheduling".
 */

/* Ticks per second. */
#define TIMER_HZ 1000

/* Microseconds per tick; the delay OP_IRQ_SET takes on a timer line is rounded up to whole ticks. */
#define TIMER_US_PER_TICK (1000000u / TIMER_HZ)
_Static_assert(TIMER_US_PER_TICK * TIMER_HZ == 1000000u, "the tick divides a second");

/* Program the first tick and enable the machine timer interrupt. */
void timer_init(void);

/* Acknowledge a tick by programming the next one. */
void timer_ack(void);

#endif
