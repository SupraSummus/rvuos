/*
 * Machine timer of the ESP32-C6: the CLINT's mtime and mtimecmp, at 0x20001800.
 *
 * The CLINT here is Espressif's: a control word next to the counter
 * starts it and lets its compare raise the machine timer interrupt,
 * which the ROM leaves off, and then it behaves as a standard one,
 * a level while mtime has reached mtimecmp, on mcause 7.
 * It counts at the CPU clock, which is the ROM's choice and not the kernel's,
 * so the kernel measures it at boot against the system timer,
 * which runs at 16 MHz off the crystal whatever the CPU clock is.
 */

#include <stdint.h>

#include "csr.h"
#include "timer.h"
#include "work.h"

#define CLINT_BASE     0x20001800u
#define CLINT_TIMECTL  (CLINT_BASE + 0x04u)
#define CLINT_MTIME    (CLINT_BASE + 0x08u)
#define CLINT_MTIMECMP (CLINT_BASE + 0x10u)
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

/* mtime counts per tick, measured by timer_init. */
static uint32_t tick_cycles;

/* Both are 64 bits wide and this is RV32, so each is two words. */
static uint64_t mtime_read(void)
{
    uint32_t hi, lo;
    do {
        LOOP_WAIT("the high half to hold still across the read");
        hi = REG(CLINT_MTIME + 4);
        lo = REG(CLINT_MTIME);
    } while (REG(CLINT_MTIME + 4) != hi);
    return ((uint64_t)hi << 32) | lo;
}

static void mtimecmp_write(uint64_t v)
{
    /* Push the compare value out of reach while the halves are apart. */
    REG(CLINT_MTIMECMP) = 0xffffffffu;
    REG(CLINT_MTIMECMP + 4) = (uint32_t)(v >> 32);
    REG(CLINT_MTIMECMP) = (uint32_t)v;
}

static uint64_t systimer_read(void)
{
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UPDATE;
    while ((REG(SYSTIMER_UNIT0_OP) & SYSTIMER_VALID) == 0) {
    }
    return ((uint64_t)REG(SYSTIMER_UNIT0_HI) << 32) | REG(SYSTIMER_UNIT0_LO);
}

/*
 * The next tick is a period from now rather than from the last tick,
 * so a long system call costs the slice after it one tick, not a burst of them.
 */
void timer_ack(void)
{
    mtimecmp_write(mtime_read() + tick_cycles);
}

/* One tick of the system timer, counted in mtime: the tick's length, whatever the CPU clock. */
void timer_init(void)
{
    mtimecmp_write(~0ull);
    REG(CLINT_TIMECTL) = TIMECTL_COUNTER_EN | TIMECTL_TIMERINT_EN;

    uint64_t start = systimer_read();
    uint64_t mtime_start = mtime_read();
    while (systimer_read() - start < SYSTIMER_HZ / TIMER_HZ) {
    }
    tick_cycles = (uint32_t)(mtime_read() - mtime_start);

    timer_ack();
    csr_set(mie, MIE_MTIE);
}
