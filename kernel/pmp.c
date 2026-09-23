/*
 * Physical Memory Protection register access.
 *
 * PMP CSRs are addressed by name in the instruction encoding,
 * so indexed access goes through a switch.
 * The tables below are the only place that lists them.
 */

#include "csr.h"
#include "kernel.h"
#include "pmp.h"

_Static_assert(PMP_MAX_ENTRIES >= 1 && PMP_MAX_ENTRIES <= 16,
               "pmp.c addresses at most sixteen entries");

static uint32_t pmpaddr_read(unsigned idx)
{
    switch (idx) {
#define CASE(n) case n: return csr_read(pmpaddr##n);
        CASE(0)  CASE(1)  CASE(2)  CASE(3)
        CASE(4)  CASE(5)  CASE(6)  CASE(7)
        CASE(8)  CASE(9)  CASE(10) CASE(11)
        CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    default:
        kpanic("pmpaddr index out of range");
    }
}

static void pmpaddr_write(unsigned idx, uint32_t v)
{
    switch (idx) {
#define CASE(n) case n: csr_write(pmpaddr##n, v); return;
        CASE(0)  CASE(1)  CASE(2)  CASE(3)
        CASE(4)  CASE(5)  CASE(6)  CASE(7)
        CASE(8)  CASE(9)  CASE(10) CASE(11)
        CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    default:
        kpanic("pmpaddr index out of range");
    }
}

/* On RV32 each pmpcfg register holds four entry bytes. */
static uint32_t pmpcfg_read(unsigned reg)
{
    switch (reg) {
    case 0: return csr_read(pmpcfg0);
    case 1: return csr_read(pmpcfg1);
    case 2: return csr_read(pmpcfg2);
    case 3: return csr_read(pmpcfg3);
    default: kpanic("pmpcfg index out of range");
    }
}

static void pmpcfg_write(unsigned reg, uint32_t v)
{
    switch (reg) {
    case 0: csr_write(pmpcfg0, v); return;
    case 1: csr_write(pmpcfg1, v); return;
    case 2: csr_write(pmpcfg2, v); return;
    case 3: csr_write(pmpcfg3, v); return;
    default: kpanic("pmpcfg index out of range");
    }
}

static void pmpcfg_set_byte(unsigned idx, uint8_t cfg)
{
    unsigned reg = idx / 4;
    unsigned shift = (idx % 4) * 8;
    uint32_t v = pmpcfg_read(reg);
    v &= ~(0xffu << shift);
    v |= (uint32_t)cfg << shift;
    pmpcfg_write(reg, v);
}

static uint8_t pmpcfg_get_byte(unsigned idx)
{
    return (uint8_t)(pmpcfg_read(idx / 4) >> ((idx % 4) * 8));
}

unsigned pmp_entry_count;
uint32_t pmp_grain;

void pmp_init(void)
{
    for (unsigned reg = 0; reg < (PMP_MAX_ENTRIES + 3) / 4; reg++) {
        pmpcfg_write(reg, 0);
    }

    /*
     * The grain, by the specification's recipe: with the mode OFF,
     * write all ones to pmpaddr0 and read back;
     * the lowest set bit is G and the grain is 2^(G+2) bytes.
     */
    pmpaddr_write(0, 0xffffffffu);
    uint32_t back = pmpaddr_read(0);
    pmpaddr_write(0, 0);
    if (back == 0) {
        kpanic("no PMP");
    }
    pmp_grain = 4;
    while ((back & 1u) == 0) {
        back >>= 1;
        pmp_grain <<= 1;
    }

    /*
     * The image uses entries from zero up, so the budget ends at the first
     * entry whose address or mode ignores a write, as RP2350's hardwired ones do.
     * NAPOT is the only mode the image uses, so it is the mode probed.
     */
    unsigned count = 0;
    for (unsigned i = 0; i < PMP_MAX_ENTRIES; i++) {
        pmpaddr_write(i, 0xffffffffu);
        uint32_t ones = pmpaddr_read(i);
        pmpaddr_write(i, 0);
        uint32_t zero = pmpaddr_read(i);
        if (ones == zero) {
            break;
        }
        pmpcfg_set_byte(i, PMP_A_NAPOT);
        uint8_t cfg = pmpcfg_get_byte(i);
        pmpcfg_set_byte(i, 0);
        if ((cfg & 0x18) != PMP_A_NAPOT) {
            break;
        }
        count++;
    }
    pmp_entry_count = count;
}

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    /* Address first, then rights; harmless today since PMP is off in machine mode. */
    pmpaddr_write(idx, addr);
    pmpcfg_set_byte(idx, cfg);
}

void pmp_clear(unsigned idx)
{
    pmpcfg_set_byte(idx, 0);
    pmpaddr_write(idx, 0);
}

void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg)
{
    *addr = pmpaddr_read(idx);
    *cfg = pmpcfg_get_byte(idx);
}
