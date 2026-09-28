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

/*
 * The ticks of its units a thread's account holds at most, a tenth of a second's:
 * an account gains a part of a tick for each unit every tick and holds that many ticks of them.
 * See DESIGN.md, "Scheduling".
 */
#define ACCOUNT_TICKS (TIMER_HZ / 10)

/* Program the first tick and enable the machine timer interrupt. */
void timer_init(void);

/*
 * Count the ticks that passed since the last counted one:
 * at least one when the timer interrupt is pending, and zero if the next is still ahead.
 * The interrupt stays where timer_set put it.
 */
uint32_t timer_count(void);

/*
 * Set the timer interrupt for the ticks-th tick after the last counted one, one being the next,
 * or as far as timer_deferred allows; timer_count counts the ticks between when it comes.
 * The kernel sets it for the first tick that could change what runs,
 * and counts the others whenever it next traps; see DESIGN.md, "Scheduling".
 */
void timer_set(uint32_t ticks);

/*
 * The tick after *next, the compare value the counter has reached:
 * the first a whole number of periods on that lies ahead of now,
 * so the tick count keeps pace with the counter however late the interrupt is taken,
 * and a tick held off costs one late interrupt, not a burst of them.
 * Returns the periods passed, zero if *next still lies ahead, as when a trap comes between two ticks.
 * A tick held off for 2^32 counts, as a debugger's stop may, starts the grid again from now.
 */
static inline uint32_t timer_next(uint64_t *next, uint64_t now, uint32_t period)
{
    if (now < *next) {
        return 0;
    }
    uint64_t late = now - *next;
    if ((late >> 32) != 0) {
        *next = now + period;
        return 1;
    }
    uint32_t passed = (uint32_t)late / period + 1;
    *next += (uint64_t)passed * period;
    return passed;
}

/*
 * The compare value for the ticks-th tick after the one before next, one being next itself.
 * It lies at most 2^31 counts past next, so that timer_next counts the interrupt taken there
 * without starting the grid again; a kernel that wants longer wakes there and sets it again.
 */
static inline uint64_t timer_deferred(uint64_t next, uint32_t ticks, uint32_t period)
{
    uint64_t skip = ticks - 1;
    if (skip * period > 0x80000000u) {
        skip = 0x80000000u / period;
    }
    return next + skip * period;
}

/* The rate of the counter the tick is made of, COUNTER_ADDR, in Hz; see OP_CLOCK_INFO. */
uint32_t timer_counter_hz(void);

#endif
