/*
 * User program entry.
 * The kernel has already set sp to the top of the data region.
 */

#include "rvuos/abi.h"

    .section .text.start, "ax"
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
    call    main
    /* main must not return; halt through the boot debug capability. */
    li      a7, OP_DEBUG_HALT
    li      a0, BOOT_CAP_DEBUG
    li      a1, 1
    ecall
3:
    j       3b
