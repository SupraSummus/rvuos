/*
 * Root task for the current stage.
 * It walks the capability system once end to end on real hardware paths:
 * carve memory, make a pool, allocate from it,
 * map a region and use it, unmap it and fault on it.
 * Negative paths are covered by the fuzz corpus and tests/differential.py.
 */

#include <stdint.h>

#include "rvuos.h"

enum {
    SLOT_POOL_REGION = 10,
    SLOT_SCRATCH_REGION,
    SLOT_POOL,
    SLOT_TABLE,
};

#define SCRATCH_SLOT 2

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void expect(const char *what, uint32_t status)
{
    puts(what);
    puts(status == KERR_OK ? ": ok\n" : ": FAILED\n");
    if (status != KERR_OK) {
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

int main(void);

int main(void)
{
    puts("hello from user mode\n");

    uint32_t free_base, free_size;
    expect("free ram info", rv_region_info(BOOT_CAP_FREE_RAM, &free_base, &free_size));

    expect("carve pool region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, 0, 0x1000, SLOT_POOL_REGION));
    expect("carve scratch region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, 0x1000, 0x1000, SLOT_SCRATCH_REGION));
    expect("region to pool",
           rv_invoke(OP_REGION_TO_POOL, SLOT_POOL_REGION, SLOT_POOL, 0, 0));
    expect("allocate a table",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_TABLE, 8));
    expect("copy into the table",
           rv_invoke(OP_CAP_COPY, SLOT_TABLE, 0, BOOT_CAP_DEBUG, RIGHT_ALL));

    expect("install scratch region",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, SCRATCH_SLOT, SLOT_SCRATCH_REGION,
                     RIGHT_R | RIGHT_W));
    volatile uint32_t *scratch = (volatile uint32_t *)(free_base + 0x1000);
    *scratch = 0x12345678u;
    expect("scratch region works", *scratch == 0x12345678u ? KERR_OK : KERR_INVALID_ARG);

    expect("uninstall scratch region",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, SCRATCH_SLOT, 0, 0));

    puts("reading the removed region, expecting a fault\n");
    uint32_t word = *scratch;

    /* Not reached when PMP works. */
    puts("PMP did not stop the read\n");
    (void)word;
    rv_halt(BOOT_CAP_DEBUG, 2);
}
