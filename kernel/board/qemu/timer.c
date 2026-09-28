/* Machine timer of the QEMU virt board: a SiFive CLINT, which counts from reset at COUNTER_HZ. */

#include "clint.h"
#include "layout.h"
#include "timer.h"

/* A tick is a whole number of counts, so timer_counter_hz returns COUNTER_HZ. */
_Static_assert(COUNTER_HZ % TIMER_HZ == 0, "the tick divides the counter's second");

void timer_init(void)
{
    clint_start(COUNTER_HZ / TIMER_HZ);
}
