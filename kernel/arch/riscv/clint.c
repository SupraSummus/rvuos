/* The counter and compare under the tick, on the CLINT of clint.h. */

#include <stdint.h>

#include "clint.h"
#include "csr.h"
#include "layout.h"
#include "timer.h"
#include "work.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

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
    REG(CLINT_MTIMECMP) = 0xffffffffu;
    REG(CLINT_MTIMECMP + 4) = (uint32_t)(v >> 32);
    REG(CLINT_MTIMECMP) = (uint32_t)v;
}

void counter_compare_enable(void)
{
    csr_set(mie, MIE_MTIE);
}

void clint_hold(void)
{
    counter_compare(~0ull);
}
