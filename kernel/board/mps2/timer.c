/*
 * The counter under the tick on the MPS2 boards: the FPGA's COUNTER, which counts from reset at COUNTER_HZ.
 * It is 32 bits wide, so the kernel counts its wraps, which it sees as long as some core reads it
 * once every 2^31 counts: SysTick fires at least once a reload, 2^24 counts here, whatever its compare,
 * and every trap reads the counter; see kernel/arch/arm/systick.c.
 * A core reads it with the lock or, as it waits, without, so what the wraps leave is one word,
 * the count's bits from 31 up as the latest read saw them, which a read only moves forward:
 * a read lies less than a wrap past what that word says, so the word and the low 32 bits make the whole count.
 */

#include <stdint.h>

#include "layout.h"
#include "timer.h"

#define FPGAIO_PRESCALE (FPGAIO_BASE + 0x1cu)
#define REG(addr) (*(volatile uint32_t *)(addr))

_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

/* The count's bits from 31 up, at least as far as any read has seen them. */
static uint32_t counter_upper;

uint64_t counter_read(void)
{
    uint32_t upper = __atomic_load_n(&counter_upper, __ATOMIC_ACQUIRE);
    uint32_t low = REG(COUNTER_ADDR);
    uint64_t from = (uint64_t)upper << 31;
    uint64_t now = from + (uint32_t)(low - (uint32_t)from);
    uint32_t seen = (uint32_t)(now >> 31);
    /*
     * Another core that moved the word first moved it at least as far, so a lost exchange changes nothing,
     * and one that fails for no reason, as an exclusive store may, leaves the word for the next read to move.
     */
    if (seen != upper) {
        __atomic_compare_exchange_n(&counter_upper, &upper, seen, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    return now;
}

void timer_init(void)
{
    /* A prescaler of zero counts every cycle of the system clock. */
    REG(FPGAIO_PRESCALE) = 0;
    timer_start(COUNTER_HZ / TIMER_HZ);
}
