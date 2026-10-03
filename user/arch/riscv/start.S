/*
 * User program entry.
 * The kernel has already set sp to the top of the data region.
 * Initialised data is copied from behind the code, where the image holds it, and .bss zeroed;
 * see user/user.ld.S.
 */

#include "rvuos/abi.h"

    .section .text.start, "ax"
    .globl _start
_start:
    la      t0, __data_start
    la      t1, __data_end
    la      t2, __data_load
1:
    bgeu    t0, t1, 2f
    lw      t3, 0(t2)
    sw      t3, 0(t0)
    addi    t0, t0, 4
    addi    t2, t2, 4
    j       1b
2:
    la      t0, __bss_start
    la      t1, __bss_end
3:
    bgeu    t0, t1, 4f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       3b
4:
    call    main
    /* main must not return; halt through the boot debug capability. */
    li      a7, OP_DEBUG_HALT
    li      a0, BOOT_CAP_DEBUG
    li      a1, 1
    ecall
5:
    j       5b
