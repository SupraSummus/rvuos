/*
 * Escape attempt: a load just past the end of the smallest region there is.
 *
 * The PMP decodes no block smaller than its grain,
 * so a kernel that took the grain for finer than it is would make frames that open more than they hold.
 * This program carves a frame of the smallest region's size, see OP_FRAME_INFO, out of the free RAM,
 * installs it read and write, and loads the word right after it, where the root task holds no region:
 * PMP must fault the load, mcause=5, naming its own address in mepc and the word's in mtval;
 * on ARM the MPU must, with a MemManage, DACCVIOL, naming them in pc and MMFAR.
 * Were the load allowed the program would say what it read, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "escape.h"
#include "rvuos.h"

/* The load below; see escape.h. */
extern const char load_at[];

enum {
    SLOT_FREE_FRAME = BOOT_CAP_COUNT,
    SLOT_SMALLEST,
};

/* Region slots: the root task boots with code in 0 and data in 1. */
#define SMALLEST_SLOT 2

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        puts("escape: ");
        puts(what);
        puts(": FAILED\n");
        rv_halt(BOOT_CAP_DEBUG, 3);
    }
}

int main(void);
int main(void)
{
    uint32_t base, min_size;
    expect("make the free ram a frame", rv_retype(BOOT_CAP_FREE_RAM, CAP_FRAME, SLOT_FREE_FRAME, &base));
    expect("smallest region", rv_frame_min_size(SLOT_FREE_FRAME, &min_size));
    expect("carve the smallest frame",
           rv_invoke(OP_FRAME_CARVE, SLOT_FREE_FRAME, 0, min_size, SLOT_SMALLEST));
    expect("map it",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, SMALLEST_SLOT, SLOT_SMALLEST, RIGHT_R | RIGHT_W));
    /* The frame's own first word loads, so that the fault below is the end's and not the install's. */
    (void)*(volatile uint32_t *)(uintptr_t)base;

    uint32_t addr = base + min_size;
    puts("escape: reading past the smallest region, ");
    rv_put_hex(BOOT_CAP_DEBUG, min_size);
    puts(" bytes, from ");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)load_at);
    puts(", expecting a fault\n");

    uint32_t v;
    ESCAPE_LOAD("load_at", v, addr);

    /* Not reached when PMP ends the region where the kernel says it ends. */
    puts("escape: breached, read past the smallest region ");
    rv_put_hex(BOOT_CAP_DEBUG, v);
    puts("\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
