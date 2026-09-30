/*
 * The counter under the tick on mps2-an385: the FPGA's COUNTER, which counts from reset at COUNTER_HZ.
 * It is 32 bits wide, so the kernel counts its wraps, which it sees as long as it reads it
 * once a wrap, every 171 seconds: SysTick fires at least every 0.67 seconds while it waits for a compare,
 * and every trap reads the counter; see kernel/arch/arm/systick.c.
 */

#include <stdint.h>

#include "layout.h"
#include "timer.h"

#define FPGAIO_PRESCALE (FPGAIO_BASE + 0x1cu)
#define REG(addr) (*(volatile uint32_t *)(addr))

_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

/* The high word, and the low word as last read, so a smaller one means it wrapped. */
static uint32_t counter_high, counter_low;

uint64_t counter_read(void)
{
    uint32_t low = REG(COUNTER_ADDR);
    if (low < counter_low) {
        counter_high++;
    }
    counter_low = low;
    return ((uint64_t)counter_high << 32) | low;
}

void timer_init(void)
{
    /* A prescaler of zero counts every cycle of the system clock. */
    REG(FPGAIO_PRESCALE) = 0;
    timer_start(COUNTER_HZ / TIMER_HZ);
}
