/* The counter and compare under the tick, on the CLINT of clint.h. */

#include <stdint.h>

#include "clint.h"
#include "csr.h"
#include "kernel.h"
#include "layout.h"
#include "object.h"
#include "timer.h"
#include "work.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/*
 * Each hart's mtimecmp is two words, one after the other's, from hart 0's at CLINT_MTIMECMP,
 * unless the board says each hart sees its own there, CLINT_MTIMECMP_LOCAL, as RP2350's SIO has it.
 */
#ifdef CLINT_MTIMECMP_LOCAL
#define MTIMECMP CLINT_MTIMECMP
#else
#define MTIMECMP (CLINT_MTIMECMP + 8 * core_index())
#endif

/* Both are 64 bits wide and this is RV32, so each is two words. */
uint64_t counter_read(void)
{
    uint32_t hi, lo;
    do {
        LOOP_WAIT("the high half to hold still across the read");
        hi = REG(CLINT_MTIME + 4);
        lo = REG(CLINT_MTIME);
    } while (REG(CLINT_MTIME + 4) != hi);
    return ((uint64_t)hi << 32) | lo;
}

void counter_compare(uint64_t v)
{
    /* Push the compare value out of reach while the halves are apart. */
    REG(MTIMECMP) = 0xffffffffu;
    REG(MTIMECMP + 4) = (uint32_t)(v >> 32);
    REG(MTIMECMP) = (uint32_t)v;
}

void counter_compare_enable(void)
{
    csr_set(mie, MIE_MTIE);
}

void clint_hold(void)
{
    counter_compare(~0ull);
}

/*
 * A hart's software interrupt is the low bit of its word at CLINT_MSIP, one word a hart,
 * where the board's CLINT has them; RP2350's lie in SIO, and its board.c raises them.
 */
#if CORES > 1 && defined(CLINT_MSIP)
void ipi_enable(void)
{
    csr_set(mie, MIE_MSIE);
}

void ipi_send(uint32_t core)
{
    REG(CLINT_MSIP + 4 * core) = 1;
}

void ipi_clear(void)
{
    REG(CLINT_MSIP + 4 * core_index()) = 0;
}
#endif
