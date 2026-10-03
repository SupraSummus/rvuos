/*
 * User program entry.
 * The kernel has already set sp to the top of the data region.
 * Initialised data is copied from behind the code, where the image holds it, and .bss zeroed;
 * see user/user.ld.S.
 */

#include "rvuos/abi.h"

    .syntax unified
    .thumb

    .section .text.start, "ax"
    .globl _start
    .type _start, %function
_start:
    ldr     r0, =__data_start
    ldr     r1, =__data_end
    ldr     r2, =__data_load
1:
    cmp     r0, r1
    bhs     2f
    ldr     r3, [r2], #4
    str     r3, [r0], #4
    b       1b
2:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
3:
    cmp     r0, r1
    bhs     4f
    str     r2, [r0], #4
    b       3b
4:
    bl      main
    /* main must not return; halt through the boot debug capability. */
    mov     r12, #OP_DEBUG_HALT
    movs    r0, #BOOT_CAP_DEBUG
    movs    r1, #1
    svc     #0
5:
    b       5b

    .ltorg
