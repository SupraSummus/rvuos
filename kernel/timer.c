/*
 * Machine timer of the QEMU virt board: the CLINT's mtime and mtimecmp.
 * A board with its timer elsewhere replaces this file, as it does uart.c.
 */

#include <stdint.h>

#include "csr.h"
#include "timer.h"

#define CLINT_BASE     0x02000000u
#define CLINT_MTIMECMP (CLINT_BASE + 0x4000u) /* hart 0 */
#define CLINT_MTIME    (CLINT_BASE + 0xbff8u)

/* QEMU virt runs mtime at 10 MHz. */
#define TIMEBASE_HZ 10000000u
#define TICK_CYCLES (TIMEBASE_HZ / TIMER_HZ)

#define REG(addr) (*(volatile uint32_t *)(addr))

/* Both are 64 bits wide and this is RV32, so each is two words. */
static uint64_t mtime_read(void)
{
    uint32_t hi, lo;
    do {
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

/*
 * The next tick is a period from now rather than from the last tick,
 * so a long system call costs the slice after it one tick, not a burst of them.
 */
void timer_ack(void)
{
    mtimecmp_write(mtime_read() + TICK_CYCLES);
}

void timer_init(void)
{
    timer_ack();
    csr_set(mie, MIE_MTIE);
}

/*
 * wfi resumes when an enabled interrupt is pending,
 * whether or not machine mode would take it,
 * and the kernel runs with MIE clear, so the tick is polled rather than taken.
 * A core may also treat wfi as a no-op, which the loop tolerates.
 */
void timer_wait(void)
{
    while (!(csr_read(mip) & MIP_MTIP)) {
        __asm__ volatile("wfi");
    }
    timer_ack();
}
