/*
 * Escape attempt on ARM: read the System Control Space from unprivileged code.
 *
 * The System Control Space holds the core's own registers, the MPU's and the fault status among them,
 * and the architecture keeps unprivileged code out of it whatever the MPU's regions say,
 * so a load from it must fault, a precise BusFault, PRECISERR,
 * naming the load's own address in pc and the register's in BFAR.
 * The register is the configurable fault status, CFSR, which ARMv7-M and ARMv8-M both have.
 * It is ARM's counterpart of escape-csrr, and fails on a thread the kernel left privileged.
 * Were the load allowed the program would say what it read, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "escape.h"
#include "rvuos.h"

#define CFSR_ADDR 0xe000ed28u

/* The load below; see escape.h. */
extern const char load_at[];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    puts("escape: reading the fault status ");
    rv_put_hex(BOOT_CAP_DEBUG, CFSR_ADDR);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)load_at);
    puts(", expecting a fault\n");

    uint32_t v;
    ESCAPE_LOAD("load_at", v, CFSR_ADDR);

    /* Not reached when the core keeps unprivileged code out of the System Control Space. */
    puts("escape: breached, read the fault status ");
    rv_put_hex(BOOT_CAP_DEBUG, v);
    puts("\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
