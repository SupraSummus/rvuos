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

/* True if a range's boundaries lie on the grain. */
static inline bool grain_aligned(uint32_t base, uint32_t size)
{
    return ((base | size) & (pmp_grain - 1)) == 0;
}

/*
 * Probe the PMP unit into the two variables above.
 * Every PMP CSR field is WARL and may be read-only,
 * so the only way to learn what the core has is to write and read back.
 */
void pmp_init(void);

/*
 * Program one entry.
 * addr is a byte address; it is shifted for the CSR here.
 * cfg is the pmpcfg byte for the entry.
 */
void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg);

/* Disable one entry. */
void pmp_clear(unsigned idx);

/* Read an entry back from the CSRs. addr is returned as a byte address. */
void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg);

#endif
