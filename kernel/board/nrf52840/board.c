/*
 * What nRF52840 needs before the kernel starts: its crystal.
 *
 * The chip resets onto its internal 64 MHz oscillator, which may be off by a percent and a half;
 * the 32 MHz crystal every board carries for the radio makes the clock and the tick exact.
 * The counter under them is started in timer.c.
 * Register addresses are those of the nRF52840 Product Specification.
 */

#include <stdint.h>

#include "kernel.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

#define CLOCK_TASKS_HFCLKSTART    0x40000000u
#define CLOCK_EVENTS_HFCLKSTARTED 0x40000100u

/* How long the crystal gets to start, in polls, some 10 ms; it takes a fraction of one, and a board without one runs on. */
#define XTAL_POLLS 200000u

void board_init(void)
{
    REG(CLOCK_TASKS_HFCLKSTART) = 1;
    for (uint32_t i = 0; i < XTAL_POLLS && REG(CLOCK_EVENTS_HFCLKSTARTED) == 0; i++) {
    }
}

/*
 * User mode on ARMv7-M writes no state of the core's beyond the registers a trap saves:
 * the floating-point unit stays shut, as reset leaves it, so a floating-point instruction faults its thread.
 */
void board_user_csrs_switch(struct process *from, struct process *to)
{
    (void)from;
    (void)to;
}

/* The chip's watchdog runs until a reset once started, and the runner resets the chip before every image anyway. */
void board_watchdog(uint32_t us)
{
    (void)us;
}
