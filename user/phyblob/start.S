/*
 * The harness's entry, and the requests that are one instruction; see phytrace.h.
 * The tracer starts the harness at _start with sp at __stack_top.
 */

#include "phytrace.h"

    .section .text.header, "ax"
    .word   PHYBLOB_MAGIC
    .word   _start
    .word   __stack_top

    .globl _start
_start:
    la      t0, __bss_start
    la      t1, __bss_end
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    call    phyblob_main
    li      t0, PHYBLOB_DONE
    ebreak
3:
    j       3b

/*
 * int phy_printf(const char *format, ...), which libphy.a calls for its messages.
 * Its variadic arguments came in a1 to a7 and on the stack, where the tracer reads them.
 */
    .section .text.phy_printf, "ax"
    .globl phy_printf
phy_printf:
    li      t0, PHYBLOB_PRINTF
    ebreak
    li      a0, 0
    ret
