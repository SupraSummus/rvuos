/*
 * User program entry.
 * The kernel has already set sp to the top of the data region.
 */

#include "rvuos/abi.h"

    .syntax unified
    .thumb

    .section .text.start, "ax"
    .globl _start
    .type _start, %function
_start:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
1:
    cmp     r0, r1
    bhs     2f
    str     r2, [r0], #4
    b       1b
2:
    bl      main
    /* main must not return; halt through the boot debug capability. */
    mov     r12, #OP_DEBUG_HALT
    movs    r0, #BOOT_CAP_DEBUG
    movs    r1, #1
    svc     #0
3:
    b       3b

    .ltorg
