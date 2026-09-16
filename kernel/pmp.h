#ifndef RVUOS_PMP_H
#define RVUOS_PMP_H

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

/* Probe the implemented entries. Returns how many are usable. */
unsigned pmp_init(void);

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
