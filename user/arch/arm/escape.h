#ifndef RVUOS_USER_ESCAPE_H
#define RVUOS_USER_ESCAPE_H

#include <stdint.h>

/*
 * The instructions the escape suite makes its attempts with on ARMv7-M and ARMv8-M, for user/escape-*.c.
 * Each access is labelled in the statement that makes it, so the label cannot drift from it,
 * and the scenario prints the label's address for tests/escape.sh to find in the fault.
 * A label in Thumb code is not a function, so its address has no Thumb bit, as the pc the kernel reports has none.
 * Each architecture has an escape.h of its own in user/arch/<arch>/.
 */

/* A word loaded from addr into v, at label. */
#define ESCAPE_LOAD(label, v, addr) \
    __asm__ volatile(".globl " label "\n" label ":\n\tldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory")

/* The word v stored at addr, at label, followed by no store, as on RISC-V. */
#define ESCAPE_STORE(label, v, addr) \
    __asm__ volatile(".globl " label "\n" label ":\n\tstr %0, [%1]\n\tnop" : : "r"(v), "r"(addr) : "memory")

/* A word that returns to its caller when run: `bx lr` in each half. */
#define ESCAPE_RETURN_WORD 0x47704770u

/* The code at addr, as a function to call: Thumb, as a branch takes bit 0. */
#define ESCAPE_CODE(addr) ((void (*)(void))(uintptr_t)((addr) | 1u))

#endif
