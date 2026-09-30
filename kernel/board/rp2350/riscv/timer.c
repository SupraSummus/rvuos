/*
 * Machine timer of RP2350: the RISC-V platform timer in SIO, mtime and mtimecmp.
 *
 * It counts the microsecond ticks board.c starts, and raises the machine timer interrupt
 * as a standard one does, so clint.c drives it.
 * TIMER0 counts the same ticks for the counter user mode reads, since only machine mode reaches mtime,
 * so the clock's rate is mtime's; see COUNTER_ADDR in board.h.
 */

#include <stdint.h>

#include "clint.h"
#include "layout.h"
#include "timer.h"

#define SIO_MTIME_CTRL 0xd00001a4u
#define MTIME_EN       1u /* and not FULLSPEED: count the ticks, not clk_sys */

_Static_assert(COUNTER_HZ == 1000000u, "the ticks come every microsecond");
_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

void timer_init(void)
{
    clint_hold();
    *(volatile uint32_t *)SIO_MTIME_CTRL = MTIME_EN;
    timer_start(COUNTER_HZ / TIMER_HZ);
}
