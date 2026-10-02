#ifndef RVUOS_USER_ESCAPE_H
#define RVUOS_USER_ESCAPE_H

#include <stdint.h>

/*
 * The instructions the escape suite makes its attempts with on RISC-V, for user/escape-*.c.
 * Each access is labelled in the statement that makes it, so the label cannot drift from it,
 * and the scenario prints the label's address for tests/escape.sh to find in the fault.
 * Each architecture has an escape.h of its own in user/arch/<arch>/.
 */

/* A word loaded from addr into v, at label. */
#define ESCAPE_LOAD(label, v, addr) \
    __asm__ volatile(".globl " label "\n" label ":\n\tlw %0, 0(%1)" : "=r"(v) : "r"(addr) : "memory")

/*
 * The word v stored at addr, at label.
 * Not followed at once by a store, which would have the ESP32-C6 check a misaligned store's second word for writing;
 * see DESIGN.md, "Boards".
 */
#define ESCAPE_STORE(label, v, addr) \
    __asm__ volatile(".globl " label "\n" label ":\n\tsw %0, 0(%1)\n\tnop" : : "r"(v), "r"(addr) : "memory")

/* A word that returns to its caller when run: `jalr x0, 0(ra)`. */
#define ESCAPE_RETURN_WORD 0x00008067u

/* The code at addr, as a function to call. */
#define ESCAPE_CODE(addr) ((void (*)(void))(uintptr_t)(addr))

#endif
