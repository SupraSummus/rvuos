/*
 * Escape attempt: read a machine-mode CSR from user mode.
 *
 * mstatus is a machine-mode CSR, so reading it in user mode is an illegal instruction, mcause=2,
 * whatever the register would have held.
 * Were the read allowed the program would say so, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    puts("escape: reading mstatus from user mode, expecting a fault\n");

    uint32_t v;
    __asm__ volatile("csrr %0, mstatus" : "=r"(v));

    /* Not reached when user mode may not touch a machine CSR. */
    puts("escape: read a machine csr\n");
    (void)v;
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
