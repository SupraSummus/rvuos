/*
 * Machine timer of the ESP32-C6: the CLINT's mtime and mtimecmp, at 0x20001800.
 *
 * The CLINT here is Espressif's: a control word next to the counter
 * starts it and lets its compare raise the machine timer interrupt,
 * which the ROM leaves off, and then it behaves as a standard one,
 * a level while mtime has reached mtimecmp, on mcause 7, and clint.c drives it.
 * It counts at the CPU clock, which is the ROM's choice and not the kernel's,
 * so the kernel measures it at boot against the system timer,
 * which runs at 16 MHz off the crystal whatever the CPU clock is.
 */

#include <stdint.h>

#include "clint.h"
#include "layout.h"
#include "timer.h"

#define CLINT_BASE     0x20001800u
#define CLINT_TIMECTL  (CLINT_BASE + 0x04u)
_Static_assert(CLINT_MTIME == CLINT_BASE + 0x08u && CLINT_MTIMECMP == CLINT_BASE + 0x10u,
               "board.h names this CLINT's registers");
/* The copy of mtime user mode reads, 0x400 bytes up among the user timer's registers. */
#define CLINT_UTIME    (CLINT_BASE + 0x408u)
_Static_assert(CLINT_UTIME == COUNTER_ADDR, "the clock names the counter the tick is made of");
#define TIMECTL_COUNTER_EN  (1u << 0)
#define TIMECTL_TIMERINT_EN (1u << 1)

/* The system timer's unit 0, which counts from reset. */
#define SYSTIMER_BASE     0x6000A000u
#define SYSTIMER_UNIT0_OP (SYSTIMER_BASE + 0x04u)
#define SYSTIMER_UNIT0_HI (SYSTIMER_BASE + 0x40u)
#define SYSTIMER_UNIT0_LO (SYSTIMER_BASE + 0x44u)
#define SYSTIMER_UPDATE   (1u << 30) /* latch the count into HI and LO */
#define SYSTIMER_VALID    (1u << 29) /* the latched count is there to read */
#define SYSTIMER_HZ       16000000u

#define REG(addr) (*(volatile uint32_t *)(addr))

static uint64_t systimer_read(void)
{
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UPDATE;
    while ((REG(SYSTIMER_UNIT0_OP) & SYSTIMER_VALID) == 0) {
    }
    return ((uint64_t)REG(SYSTIMER_UNIT0_HI) << 32) | REG(SYSTIMER_UNIT0_LO);
}

/*
 * One tick of the system timer, counted in mtime: the tick's length, whatever the CPU clock.
 * The rate timer_counter_hz returns is measured over that one tick, so it is as exact as the loop below.
 */
void timer_init(void)
{
    clint_hold();
    REG(CLINT_TIMECTL) = TIMECTL_COUNTER_EN | TIMECTL_TIMERINT_EN;

    uint64_t start = systimer_read();
    uint64_t mtime_start = clint_mtime();
    while (systimer_read() - start < SYSTIMER_HZ / TIMER_HZ) {
    }
    clint_start((uint32_t)(clint_mtime() - mtime_start));
}
