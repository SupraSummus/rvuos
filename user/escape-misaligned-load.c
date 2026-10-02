/*
 * Escape attempt: a misaligned load across the end of the data region.
 *
 * The word at the data region's last two bytes reaches two bytes past it,
 * into memory the root task holds no region over.
 * A core may split a misaligned load and check each part on its own,
 * but the part past the region must fault, mcause=5, naming the load's own address in mepc;
 * mtval is the board's, and a core that does not split it raises a misaligned exception, mcause=4.
 * On ARM the MPU faults it, a MemManage, DACCVIOL, naming it in pc, and the address MMFAR holds is the core's.
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

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    uint32_t base, size;
    if (rv_frame_info(BOOT_CAP_DATA, &base, &size) != KERR_OK) {
        puts("escape: data info: FAILED\n");
        rv_halt(BOOT_CAP_DEBUG, 3);
    }
    uint32_t addr = base + size - 2;

    puts("escape: reading across the end of the data region from ");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)load_at);
    puts(", expecting a fault\n");

    uint32_t v;
    ESCAPE_LOAD("load_at", v, addr);

    /* Not reached when PMP stops the part past the region. */
    puts("escape: breached, read past the data region ");
    rv_put_hex(BOOT_CAP_DEBUG, v);
    puts("\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
