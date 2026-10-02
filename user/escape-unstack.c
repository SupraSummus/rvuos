/*
 * Escape attempt on ARM: return from a call onto a stack the thread's process no longer holds.
 *
 * The kernel writes the half of a thread's frame the core unstacks only where the thread's process may read and write;
 * where it may not, psp points at the kernel's own stack, which no region covers; see DESIGN.md, "Architectures".
 * This program takes its data region, where its stack lies, out of its own process in a call:
 * the core's unstacking must fault, with the thread's rights, a MemManage, MUNSTKERR, which names no address,
 * and the thread stops where the call returns to, which the kernel reports as its pc.
 * Were the unstacking allowed, or the kernel to read a frame from where the core unstacked,
 * the fault would be another; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

/* Where the call below returns to, labelled in the statement that makes it, so the label cannot drift from it. */
extern const char return_at[];

/* Region slots: the root task boots with code in 0 and data in 1. */
#define DATA_SLOT 1

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    puts("escape: taking the stack's region out of the process, to return at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)return_at);
    puts(", expecting a fault\n");

    /* OP_PROCESS_UNINSTALL of the data region; nothing goes on after it. */
    register uint32_t r_a0 __asm__(RV_A0) = BOOT_CAP_PROCESS;
    register uint32_t r_a1 __asm__(RV_A1) = DATA_SLOT;
    register uint32_t r_a2 __asm__(RV_A2) = 0;
    register uint32_t r_a3 __asm__(RV_A3) = 0;
    register uint32_t r_a7 __asm__(RV_A7) = OP_PROCESS_UNINSTALL;
    __asm__ volatile(RV_CALL "\n.globl return_at\nreturn_at:\n\tudf #0"
                     : "+r"(r_a0), "+r"(r_a1), "+r"(r_a2), "+r"(r_a3)
                     : "r"(r_a7)
                     : "memory", RV_A4, RV_A5, RV_A6);
    return 0;
}
