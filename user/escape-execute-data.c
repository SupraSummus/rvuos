/*
 * Escape attempt: execute an instruction from the data region.
 *
 * The root task boots with its code region read-execute and its data region read-write, never execute;
 * see kernel/boot.c.
 * This program jumps into a word in the data region:
 * PMP must fault the instruction fetch before a byte of it runs,
 * an instruction access fault, mcause=1, naming the word's address in mepc and mtval.
 * Were the fetch allowed the word would return and the program would say so, which fails the run;
 * see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

/* A word in .bss, which lies in the read-write data region. */
static volatile uint32_t code_word;

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void put_hex(uint32_t v)
{
    puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        rv_putc(BOOT_CAP_DEBUG, "0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

int main(void);
int main(void)
{
    /*
     * `jalr x0, 0(ra)`, a return:
     * were the fetch allowed the call would return here and the escape would have worked.
     */
    code_word = 0x00008067u;

    uint32_t at = (uint32_t)(uintptr_t)&code_word;
    puts("escape: executing from the data region at ");
    put_hex(at);
    puts(", expecting a fault\n");

    void (*enter)(void) = (void (*)(void))(uintptr_t)at;
    enter();

    /* Not reached when PMP marks the data region no-execute. */
    puts("escape: executed from data\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
