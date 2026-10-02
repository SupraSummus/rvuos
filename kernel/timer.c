/*
 * The tick of timer.h, on the counter and compare of the architecture or the board;
 * see DESIGN.md, "Timer".
 */

#include <stdint.h>

#include "kernel.h"
#include "layout.h"
#include "object.h"
#include "timer.h"
#include "work.h"

/* The counter's counts per tick, the period timer_start takes. */
static uint32_t tick_counts;

/* The compare value of the tick after the last counted one. */
static uint64_t next_tick;

/* What each core's compare holds, so that setting it for the tick it holds writes nothing. */
static uint64_t compare[CORES];

void timer_start(uint32_t period)
{
    /* The ESP32-C6 measures its tick, and a counter this fast would overflow the accounts. */
    if (period > TICK_COUNTS_MAX) {
        kpanic("the counter is too fast for the accounts to count a tick of it");
    }
    tick_counts = period;
    next_tick = counter_read() + period;
    compare[0] = next_tick;
    counter_compare(compare[0]);
    counter_compare_enable();
}

/* Another core's compare, which waits until the core first sets it, on the grid the first core's started. */
void timer_core_start(void)
{
    compare[core_index()] = ~0ull;
    counter_compare(~0ull);
    counter_compare_enable();
}

/*
 * The tick after *next, the compare value the counter has reached:
 * the first a whole number of periods on that lies ahead of now,
 * so the tick count keeps pace with the counter however late the interrupt is taken,
 * and a tick held off costs one late interrupt, not a burst of them.
 * Returns the periods passed, zero if *next still lies ahead, as when a trap comes between two ticks.
 * A tick held off for 2^32 counts, as a debugger's stop may, starts the grid again from now.
 */
static uint32_t timer_next(uint64_t *next, uint64_t now, uint32_t period)
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
 * without starting the grid again; a longer wait wakes there and sets it again.
 */
static uint64_t timer_deferred(uint64_t next, uint32_t ticks, uint32_t period)
{
    uint64_t skip = ticks - 1;
    if (skip * period > 0x80000000u) {
        skip = 0x80000000u / period;
    }
    return next + skip * period;
}

uint32_t timer_count(void)
{
    return timer_next(&next_tick, counter_read(), tick_counts);
}

void timer_set(uint32_t ticks)
{
    uint64_t at = timer_deferred(next_tick, ticks, tick_counts);
    uint64_t *held = &compare[core_index()];
    if (at != *held) {
        *held = at;
        counter_compare(at);
    }
}

uint32_t timer_counter_hz(void)
{
    return tick_counts * TIMER_HZ;
}

uint32_t timer_tick_counts(void)
{
    return tick_counts;
}

/* next_tick lies a period past the last counted tick, which the counter has reached. */
uint32_t timer_offset(void)
{
    uint64_t past = counter_read() - (next_tick - tick_counts);
    return past < tick_counts ? (uint32_t)past : tick_counts;
}

/*
 * The timer interrupts only at a tick that could change what runs,
 * so the ticks it let pass are counted first, before anything reads the count or an account,
 * and with them those other cores counted since this one last did.
 * While it is set for the next tick, a tick has passed only if its interrupt is pending,
 * so the counter is read only then or while it is set further:
 * every core's compare lies on the one grid, so no other core counted a tick this one's did not reach.
 * Under tracing time moves only by record; see DESIGN.md, "Verification".
 */
bool timer_trap_enter(bool pending)
{
    bool traced = debug_trace;
    const struct core *core = core_self();
    sched_count(!traced && (pending || core->wake_tick - core->ticks > 1) ? timer_count() : 0);
    return traced;
}

/* Counted as the trap began, and the turn ends as it leaves; under tracing it is only moved on. */
void timer_trap_tick(void)
{
    if (debug_trace) {
        timer_count();
        timer_set(1);
    }
}

/*
 * A trap whose count reached the tick the timer was set for ends the turn,
 * and the timer is set for the first tick that could change what runs next,
 * unless nothing that could has changed.
 * The trap that turns tracing on leaves it on the next tick, as the traced interrupt does.
 */
void timer_trap_leave(bool traced)
{
    const struct core *core = core_self();
    if (core->turn_due || core->wake_stale) {
        uint32_t wake = sched_wake();
        if (!debug_trace && wake != 0) {
            timer_set(wake);
        }
    }
    if (debug_trace && !traced) {
        timer_set(1);
    }
}
