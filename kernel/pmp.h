#ifndef RVUOS_PMP_H
#define RVUOS_PMP_H

#include <stdbool.h>
#include <stdint.h>

/* pmpcfg byte fields. */
#define PMP_R (1u << 0)
#define PMP_W (1u << 1)
#define PMP_X (1u << 2)
#define PMP_A_OFF   (0u << 3)
#define PMP_A_TOR   (1u << 3)
#define PMP_A_NA4   (2u << 3)
#define PMP_A_NAPOT (3u << 3)
#define PMP_L (1u << 7)

/*
 * How many entries the kernel will use.
 * Cores with more are used up to this limit;
 * cores with fewer report their count from pmp_init.
 * Override with -DPMP_MAX_ENTRIES=8 to exercise a small budget.
 */
#ifndef PMP_MAX_ENTRIES
#define PMP_MAX_ENTRIES 16
#endif

/* Entries the image may use: the leading run whose address and mode take a write. */
extern unsigned pmp_entry_count;

/*
 * The grain in bytes, 2^(G+2) in the privileged specification.
 * Hardware ignores address bits below it, so every region lies on it.
 */
extern uint32_t pmp_grain;

/*
 * The smallest region one NAPOT entry describes:
 * eight bytes, or the grain where that is coarser.
 * Four-byte regions would need NA4, which rvuos does not use.
 */
static inline uint32_t region_min_size(void)
{
    return pmp_grain < 8 ? 8 : pmp_grain;
}

/*
 * True if a range is a block one NAPOT entry describes:
 * a power of two of at least region_min_size(), aligned to its own size.
 * Every region capability is one; see DESIGN.md, "Physical Memory Protection".
 */
static inline bool napot_block(uint32_t base, uint32_t size)
{
    return size >= region_min_size() && (size & (size - 1)) == 0 && (base & (size - 1)) == 0;
}

/* The pmpaddr value of a NAPOT entry for a block: the base, and the size as trailing ones. */
static inline uint32_t pmp_napot_addr(uint32_t base, uint32_t size)
{
    return (base >> 2) | ((size >> 3) - 1);
}

/*
 * The block a NAPOT pmpaddr value describes, 64 bits wide:
 * an all-ones value is the whole 34-bit physical address space of RV32.
 */
static inline void pmp_napot_range(uint32_t addr, uint64_t *base, uint64_t *size)
{
    uint64_t ones = ((uint64_t)addr ^ ((uint64_t)addr + 1)) >> 1;
    *base = ((uint64_t)addr & ~ones) << 2;
    *size = (ones + 1) << 3;
}

/*
 * Probe the PMP unit into the two variables above.
 * Every PMP CSR field is WARL and may be read-only,
 * so the only way to learn what the core has is to write and read back.
 */
void pmp_init(void);

/*
 * Program one entry.
 * addr is the pmpaddr value, as pmp_napot_addr encodes it.
 * cfg is the pmpcfg byte for the entry.
 */
void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg);

/* Disable one entry. */
void pmp_clear(unsigned idx);

/* Read an entry back from the CSRs, the pmpaddr value as it reads. */
void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg);

#endif
