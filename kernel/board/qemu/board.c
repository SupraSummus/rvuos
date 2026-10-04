/*
 * What QEMU virt needs before the kernel starts:
 * the counters shut to user mode, so rdtime traps as on the ESP32-C6, which has none;
 * the clock reaches a process through its capability instead.
 * A board with watchdogs or clocks to set replaces this file, as it does timer.c.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"

void board_init(void)
{
    csr_write(mcounteren, 0);
}

/* The counters are the only user-mode CSRs rvuos knows of on QEMU, and they stay shut. */
void board_user_csrs_switch(struct process *from, struct process *to)
{
    (void)from;
    (void)to;
}

/* The machine has no watchdog of its own. */
void board_watchdog(uint32_t us)
{
    (void)us;
}

#if CORES > 1
/* The counters of each hart are its own, and shut on every one. */
void board_core_init(void)
{
    csr_write(mcounteren, 0);
}

/* Every hart enters at the reset vector with the first, and the others wait in start.S for their software interrupt. */
void board_cores_start(void)
{
    for (uint32_t c = 1; c < CORES; c++) {
        LOOP_BOUND(CORES);
        ipi_send(c);
    }
}
#endif
