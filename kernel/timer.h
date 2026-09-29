#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

#include <stdint.h>

#include "rvuos/abi.h"

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
 * an account gains a tick's counts of parts for each unit every tick and holds that many ticks of them.
 * See DESIGN.md, "Scheduling".
 */
#define ACCOUNT_TICKS (TIMER_HZ / 10)

/*
 * The most counts a tick may take, so that an account fits a word:
 * it holds ACCOUNT_TICKS ticks of every unit there is, and a tick's gain on top before the cap cuts it back,
 * in parts of a count, TIME_UNITS of them to a count; see COUNT_PARTS.
 */
#define TICK_COUNTS_MAX (0xffffffffu / (TIME_UNITS * (ACCOUNT_TICKS + 1)))

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
 * but at most 2^31 counts past the next; timer_count counts the ticks between when it comes.
 * The kernel sets it for the first tick that could change what runs,
 * and counts the others whenever it next traps; see DESIGN.md, "Scheduling".
 */
void timer_set(uint32_t ticks);

/* The rate of the counter the tick is made of, COUNTER_ADDR, in Hz; see OP_CLOCK_INFO. */
uint32_t timer_counter_hz(void);

/* The counter's counts per tick, at most TICK_COUNTS_MAX. */
uint32_t timer_tick_counts(void);

/*
 * How far the counter is past the last counted tick, in counts, at most a tick's:
 * a tick that passed and is not counted yet counts as the end of the one before.
 * The scheduler charges a turn by it; see DESIGN.md, "Scheduling".
 */
uint32_t timer_offset(void);

#endif
