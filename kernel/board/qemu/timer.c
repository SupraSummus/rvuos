/* Machine timer of the QEMU virt board: a SiFive CLINT, which counts from reset at COUNTER_HZ. */

#include "layout.h"
#include "timer.h"


void timer_init(void)
{
    timer_start(COUNTER_HZ);
}
