/*
 * The counter under the tick on RP2350's Cortex-M33: TIMER0's, counting the microseconds board.c starts.
 *
 * It is the counter user mode reads through BOOT_CAP_CLOCK too, see COUNTER_ADDR in board.h,
 * so the clock's rate is the tick's; the compare is SysTick, see kernel/arch/arm/systick.c.
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
_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

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
    timer_start(COUNTER_HZ / TIMER_HZ);
}
