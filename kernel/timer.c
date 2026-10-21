/*
 * The clock and the timer of timer.h, on the counter and compare of the architecture or the board;
 * see DESIGN.md, "Time".
 */

#include <stdint.h>

#include "kernel.h"
#include "layout.h"
#include "object.h"
#include "sched.h"
#include "timer.h"
#include "work.h"

/* The counter's rate, and that rate in counts per microsecond, a whole part and a fraction of 2^32, rounded up. */
static uint32_t counter_hz;
static uint32_t us_whole;
static uint32_t us_frac;

uint32_t timer_step;
uint32_t timer_span;

/* What each core's compare holds, so that setting it for the count it holds writes nothing. */
static uint64_t compare[CORES];

void timer_start(uint32_t hz)
{
    counter_hz = hz;
    us_whole = hz / 1000000u;
    /* The fraction by long division, a bit at a time: the remainder stays below twice the divisor. */
    uint32_t rest = hz % 1000000u, frac = 0;
    for (uint32_t i = 0; i < 32; i++) {
        LOOP_BOUND(32);
        rest <<= 1;
        frac <<= 1;
        if (rest >= 1000000u) {
            rest -= 1000000u;
            frac |= 1;
        }
    }
    if (rest != 0 && ++frac == 0) {
        us_whole++;
    }
    us_frac = frac;
    timer_step = hz / 1000u != 0 ? hz / 1000u : 1;
    timer_span = 1u << (31 - __builtin_clz(timer_step));
    compare[0] = COUNT_NEVER;
    counter_compare(COUNT_NEVER);
    counter_compare_enable();
}

/* Another core's compare, set for no count until the core first sets it. */
void timer_core_start(void)
{
    compare[core_index()] = COUNT_NEVER;
    counter_compare(COUNT_NEVER);
    counter_compare_enable();
}

uint64_t timer_us_counts(uint32_t us)
{
    return (uint64_t)us * us_whole + (((uint64_t)us * us_frac + 0xffffffffu) >> 32);
}

/* The long division of timer_periods, carried a word of the dividend at a time, a bit a step. */
static __attribute__((noinline)) uint64_t periods_word(uint32_t word, uint64_t period, uint64_t *remainder)
{
    uint64_t quotient = 0, rest = *remainder;
    for (uint32_t i = 0; i < 32; i++) {
        LOOP_BOUND(32);
        uint64_t r = rest << 1 | ((word >> (31 - i)) & 1);
        uint32_t fits = r >= period;
        rest = fits ? r - period : r;
        quotient = quotient << 1 | fits;
    }
    *remainder = rest;
    return quotient;
}

uint64_t timer_periods(uint64_t stretch, uint64_t period, uint64_t *rest)
{
    uint64_t remainder = 0;
    uint64_t high = periods_word((uint32_t)(stretch >> 32), period, &remainder);
    uint64_t low = periods_word((uint32_t)stretch, period, &remainder);
    *rest = remainder;
    return high << 32 | low;
}

void timer_set(uint64_t at)
{
    uint64_t *held = &compare[core_index()];
    if (at != *held) {
        *held = at;
        counter_compare(at);
    }
}

uint32_t timer_counter_hz(void)
{
    return counter_hz;
}

uint64_t timer_now(void)
{
    return debug_trace ? sched_trace_clock : counter_read();
}

/*
 * The timer interrupts only at a count that could change what runs, and what is due by then is done first,
 * before the trap reads a virtual time or a deadline: an interrupt pending while the trap began is done here,
 * whatever the trap came for.
 * Under tracing time moves only by record; see DESIGN.md, "Verification".
 */
bool timer_trap_enter(bool pending)
{
    bool traced = debug_trace;
    if (!traced && pending) {
        sched_time(counter_read());
    }
    return traced;
}

/* Done as the trap began; under tracing the timer is only quieted, since only a record moves time. */
void timer_trap_fired(void)
{
    if (debug_trace) {
        timer_set(COUNT_NEVER);
    }
}

/*
 * The timer is set for the first count that could change what runs next,
 * unless nothing that could has changed.
 * The trap that turns tracing on quiets it, as the traced interrupt does.
 */
void timer_trap_leave(bool traced)
{
    uint64_t at;
    if (sched_wake(&at) && !debug_trace) {
        timer_set(at);
    }
    if (debug_trace && !traced) {
        timer_set(COUNT_NEVER);
    }
}
