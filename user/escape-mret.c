/*
 * Escape attempt: return from a machine-mode trap while in user mode.
 *
 * mret is a machine-mode instruction, so running it in user mode is an illegal instruction, mcause=2,
 * naming its own address in mepc.
 * Were it to run, or be ignored, the run fails; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

/* The mret below, labelled in the statement that makes it, so the label cannot drift from it. */
extern const char mret_at[];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    puts("escape: running mret in user mode at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)mret_at);
    puts(", expecting a fault\n");

    __asm__ volatile(".globl mret_at\nmret_at:\n\tmret" : : : "memory");

    /* Not reached when user mode may not run a machine-mode instruction. */
    puts("escape: breached, ran mret\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
