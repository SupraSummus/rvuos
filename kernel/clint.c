/* timer.h on the CLINT of clint.h. */

#include <stdint.h>

#include "clint.h"
#include "csr.h"
#include "kernel.h"
#include "layout.h"
#include "timer.h"
#include "work.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* mtime's counts per tick, the period clint_start takes. */
static uint32_t tick_counts;

/* The compare value of the tick after the last counted one. */
static uint64_t next_tick;

/* What mtimecmp holds, so that setting it for the tick it holds writes nothing. */
static uint64_t compare;

/* Both are 64 bits wide and this is RV32, so each is two words. */
uint64_t clint_mtime(void)
{
    uint32_t hi, lo;
    do {
        LOOP_WAIT("the high half to hold still across the read");
        hi = REG(CLINT_MTIME + 4);
        lo = REG(CLINT_MTIME);
    } while (REG(CLINT_MTIME + 4) != hi);
    return ((uint64_t)hi << 32) | lo;
}

static void mtimecmp_write(uint64_t v)
{
    /* Push the compare value out of reach while the halves are apart. */
    REG(CLINT_MTIMECMP) = 0xffffffffu;
    REG(CLINT_MTIMECMP + 4) = (uint32_t)(v >> 32);
    REG(CLINT_MTIMECMP) = (uint32_t)v;
}

void clint_hold(void)
{
    mtimecmp_write(~0ull);
}

void clint_start(uint32_t period)
{
    /* The ESP32-C6 measures its tick, and a counter this fast would overflow the accounts. */
    if (period > TICK_COUNTS_MAX) {
        kpanic("the counter is too fast for the accounts to count a tick of it");
    }
    tick_counts = period;
    next_tick = clint_mtime() + period;
    compare = next_tick;
    mtimecmp_write(compare);
    csr_set(mie, MIE_MTIE);
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
    return timer_next(&next_tick, clint_mtime(), tick_counts);
}

void timer_set(uint32_t ticks)
{
    uint64_t at = timer_deferred(next_tick, ticks, tick_counts);
    if (at != compare) {
        compare = at;
        mtimecmp_write(at);
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
    uint64_t past = clint_mtime() - (next_tick - tick_counts);
    return past < tick_counts ? (uint32_t)past : tick_counts;
}
