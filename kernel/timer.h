#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

/*
 * The machine timer, which provides the scheduling tick.
 * The kernel handles it directly; see DESIGN.md, "Scheduling".
 */

/* Ticks per second. */
#define TIMER_HZ 1000

/* Program the first tick and enable the machine timer interrupt. */
void timer_init(void);

/* Acknowledge a tick by programming the next one. */
void timer_ack(void);

#endif
