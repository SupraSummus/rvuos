/*
 * What mps2-an385 needs before the kernel starts: nothing, since QEMU leaves no watchdog running.
 * A board with watchdogs or clocks to set replaces this file, as it does timer.c.
 */

#include "kernel.h"

void board_init(void)
{
}

/*
 * User mode on ARMv7-M writes no state of the core's beyond the registers a trap saves,
 * and this board has no floating point for it to leave behind.
 */
void board_user_csrs_reset(void)
{
}
