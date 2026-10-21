#ifndef RVUOS_TIMER_H
#define RVUOS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "rvuos/abi.h"

/*
 * The clock and the timer: the counter's counts since boot, and a compare on it
 * that the kernel sets for the first count that could change what runs.
 * The kernel handles it directly; see DESIGN.md, "Time" and "Scheduling".
 */

/* The board's timer.c: start the counter and the compare, through timer_start with the counter's rate. */
void timer_init(void);

/*
 * The counter counts hz a second from now on: the timer takes the rate,
 * and enables its interrupt with the compare set for no count; timer_counter_hz is hz from then on.
 * In kernel/timer.c, on the three functions below.
 */
void timer_start(uint32_t hz);

/* A core other than the first starts its compare, set for no count until it is set. */
void timer_core_start(void);

/*
 * The hardware below the timer, the architecture's or the board's:
 * the machine's counter, 64 bits counting up from boot,
 * and a compare on it, whose interrupt is pending while the counter has reached it.
 * kernel/arch/riscv/clint.c has them on a CLINT.
 */
uint64_t counter_read(void);
void counter_compare(uint64_t at);
void counter_compare_enable(void);

/* Set this core's timer to interrupt once the counter reaches at, COUNT_NEVER for never. */
void timer_set(uint64_t at);

/*
 * What every trap does with the timer, around what it came for, on either architecture:
 * enter does what is due once the counter reached what the timer was set for, pending saying whether its interrupt is,
 * and returns whether tracing was on; fired is the timer's own interrupt; leave sets the timer for what runs next.
 */
bool timer_trap_enter(bool pending);
void timer_trap_fired(void);
void timer_trap_leave(bool traced);

/* The rate of the counter, COUNTER_ADDR, in Hz; see OP_CLOCK_READ. */
uint32_t timer_counter_hz(void);

/*
 * The machine's time, in counts of the counter from boot, which OP_CLOCK_READ tells:
 * the counter's, or under tracing, where time moves only by record, sched_trace_clock,
 * which the host build, with no counter, tells as well.
 */
uint64_t timer_now(void);

/*
 * The counts of a delay in microseconds, rounded up, so that a delay of them lies no earlier than the one asked for.
 * The counter's rate in counts per microsecond is kept as a fixed-point number, rounded up, so this multiplies only.
 */
uint64_t timer_us_counts(uint32_t us);

/*
 * How many whole periods of counts a stretch of counts holds, and in *rest what is left over; period is not zero.
 * A division the kernel makes only for a timer line's period, a bit at a time, since it divides no wider than a word.
 */
uint64_t timer_periods(uint64_t stretch, uint64_t period, uint64_t *rest);

/*
 * The counts a record moves the clock by under tracing, OP_TEST_TICK's step: a millisecond's.
 * And the default span, the longest power of two of counts within a millisecond: see place_span.
 * Both the counter's rate gives, so they stay as they were set at boot; inline, since a switch reads the span.
 */
extern uint32_t timer_step;
extern uint32_t timer_span;

static inline uint32_t timer_step_counts(void)
{
    return timer_step;
}

static inline uint32_t timer_default_span(void)
{
    return timer_span;
}

#endif
