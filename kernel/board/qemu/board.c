/*
 * What QEMU virt needs before the kernel starts: nothing.
 * A board with watchdogs or clocks to set replaces this file, as it does timer.c.
 */

#include "kernel.h"

void board_init(void)
{
}
