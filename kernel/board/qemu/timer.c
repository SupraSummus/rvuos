/* Machine timer of the QEMU virt board: a SiFive CLINT, which counts from reset at COUNTER_HZ. */

#include "layout.h"
#include "timer.h"

/* A tick is a whole number of counts, so timer_counter_hz returns COUNTER_HZ. */
_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");
_Static_assert(COUNTER_HZ / TIMER_HZ <= TICK_COUNTS_MAX, "an account counts a tick of the counter");

void timer_init(void)
{
    timer_start(COUNTER_HZ / TIMER_HZ);
}
