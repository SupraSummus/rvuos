/*
 * Machine timer of the QEMU virt board: the CLINT's mtime and mtimecmp.
 * A board with its timer elsewhere replaces this file, as it does irq.c.
 */

#include <stdint.h>

#include "csr.h"
#include "layout.h"
#include "timer.h"
#include "work.h"

#define CLINT_BASE     0x02000000u
#define CLINT_MTIMECMP (CLINT_BASE + 0x4000u) /* hart 0 */
#define CLINT_MTIME    (CLINT_BASE + 0xbff8u)
_Static_assert(CLINT_MTIME == COUNTER_ADDR, "the clock names the counter the tick is made of");

#define TICK_CYCLES (COUNTER_HZ / TIMER_HZ)

#define REG(addr) (*(volatile uint32_t *)(addr))

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

/* The compare value last programmed. */
static uint64_t next_tick;

uint32_t timer_ack(void)
{
    uint32_t passed = timer_next(&next_tick, mtime_read(), TICK_CYCLES);
    mtimecmp_write(next_tick);
    return passed;
}

uint32_t timer_counter_hz(void)
{
    return COUNTER_HZ;
}

void timer_init(void)
{
    next_tick = mtime_read() + TICK_CYCLES;
    mtimecmp_write(next_tick);
    csr_set(mie, MIE_MTIE);
}
