#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

#include <stdint.h>

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

/* Acknowledge a tick by programming the next one; returns the ticks that passed, at least one. */
uint32_t timer_ack(void);

/*
 * The tick after *next, the compare value the counter has reached:
 * the first a whole number of periods on that lies ahead of now,
 * so the tick count keeps pace with the counter however late the interrupt is taken,
 * and a tick held off costs one late interrupt, not a burst of them.
 * Returns the periods passed.
 * A tick held off for 2^32 counts, as a debugger's stop may, starts the grid again from now.
 */
static inline uint32_t timer_next(uint64_t *next, uint64_t now, uint32_t period)
{
    uint64_t late = now - *next;
    if ((late >> 32) != 0) {
        *next = now + period;
        return 1;
    }
    uint32_t passed = (uint32_t)late / period + 1;
    *next += (uint64_t)passed * period;
    return passed;
}

/* The rate of the counter the tick is made of, COUNTER_ADDR, in Hz; see OP_CLOCK_INFO. */
uint32_t timer_counter_hz(void);

#endif
