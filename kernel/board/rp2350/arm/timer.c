/*
 * The counter under the timer on RP2350's Cortex-M33: TIMER0's, counting the microseconds board.c starts.
 *
 * It counts the microseconds Hazard3's mtime counts, see COUNTER_ADDR in board.h,
 * so the clock's rate is the tick's on either kind of core; the compare is SysTick, see kernel/arch/arm/systick.c.
 * Its 64 bits are two words, the high one below the low, and no read takes both at once:
 * a high word that changed across a read of the low one says the low one wrapped,
 * and the low one read again belongs to the second high one.
 */

#include <stdint.h>

#include "layout.h"
#include "timer.h"

#define TIMER0_TIMERAWH (COUNTER_ADDR - 4u)
#define REG(addr) (*(volatile uint32_t *)(addr))

_Static_assert(COUNTER_HZ == 1000000u, "the ticks come every microsecond");

uint64_t counter_read(void)
{
    uint32_t high = REG(TIMER0_TIMERAWH);
    uint32_t low = REG(COUNTER_ADDR);
    uint32_t again = REG(TIMER0_TIMERAWH);
    if (again != high) {
        low = REG(COUNTER_ADDR);
    }
    return ((uint64_t)again << 32) | low;
}

void timer_init(void)
{
    timer_start(COUNTER_HZ);
}
