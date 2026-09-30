/*
 * The compare under the tick, on SysTick, which every Cortex-M has;
 * the counter it compares with is the board's, counter_read in its timer.c.
 *
 * SysTick is a 24-bit down-counter with a reload, not a compare,
 * so it is set for the counts left before the compare, or for as many as one reload lasts,
 * and set again each time it fires until the counter has reached the compare.
 * Its pending bit is set only where a CLINT's level would be high,
 * so a compare already reached takes the interrupt at once, and one moved ahead drops it.
 */

#include <stdint.h>

#include "armv7m.h"
#include "layout.h"
#include "scs.h"
#include "timer.h"

_Static_assert(SYSTICK_PER_COUNT >= 1 && SYST_MAX / SYSTICK_PER_COUNT >= COUNTER_HZ / TIMER_HZ,
               "one reload of SysTick lasts at least a tick of the counter");

static uint64_t compare_at = ~0ull;

/* SysTick set for the counts before the compare, at most a reload's, or pending if none are left. */
static void systick_arm(void)
{
    uint64_t now = counter_read();
    SCS_REG(ICSR) = ICSR_PENDSTCLR;
    uint32_t reload = SYST_MAX;
    if (now >= compare_at) {
        SCS_REG(ICSR) = ICSR_PENDSTSET;
    } else if (compare_at - now < SYST_MAX / SYSTICK_PER_COUNT) {
        reload = (uint32_t)(compare_at - now) * SYSTICK_PER_COUNT;
    }
    /* A reload of zero never fires, and one of a single count fires as the load is taken. */
    if (reload < 2) {
        SCS_REG(ICSR) = ICSR_PENDSTSET;
        reload = SYST_MAX;
    }
    SCS_REG(SYST_RVR) = reload - 1;
    SCS_REG(SYST_CVR) = 0;
}

void counter_compare(uint64_t at)
{
    compare_at = at;
    systick_arm();
}

void counter_compare_enable(void)
{
    SCS_REG(SYST_CSR) = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;
}

bool counter_due(void)
{
    return counter_read() >= compare_at;
}

void counter_tick(void)
{
    systick_arm();
}
