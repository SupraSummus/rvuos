/*
 * What QEMU virt needs before the kernel starts:
 * the counters shut to user mode, so rdtime traps as on the ESP32-C6, which has none;
 * the clock reaches a process as a region instead.
 * A board with watchdogs or clocks to set replaces this file, as it does timer.c.
 */

#include "csr.h"
#include "kernel.h"

void board_init(void)
{
    csr_write(mcounteren, 0);
}
