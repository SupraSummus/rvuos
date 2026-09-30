/*
 * Handler-mode entry and trap glue for ARMv7-M and ARMv8-M.
 *
 * Every exception the kernel takes runs at one priority, so none preempts another
 * and the kernel runs as RISC-V's machine mode does, with interrupts held off;
 * only a fault of the kernel's own nests, as a HardFault, and that is a panic.
 * A trap from a thread enters with lr holding EXC_RETURN_THREAD_PSP
 * and the core's hardware frame, r0 to r3, r12, lr, pc and xpsr, on the thread's stack.
 * trap_entry saves r4 to r11 into the running thread's frame, which trap_current names,
 * with psp and the exception number, and trap.c takes the rest from the stack.
 * trap_return has trap.c write the hardware frame of the thread to resume
 * and returns into it through the core's own unstacking.
 * See DESIGN.md, "Architectures".
 */

#include "arch.h"
#include "layout.h"

/* EXC_RETURN values: back to thread mode on the process stack, a thread's trap, or on the main one, kmain's. */
#define EXC_RETURN_THREAD_PSP_VALUE 0xfffffffd
#define EXC_RETURN_THREAD_MSP_VALUE 0xfffffff9

    .syntax unified
    .thumb

    /*
     * The vector table, where VTOR's reset value finds it: the kernel's stack, the reset,
     * and every exception and line but NMI and PendSV to trap_entry,
     * which tells a thread's trap from the kernel's by lr.
     * PendSV is the kernel's way into user mode the first time, see trap_start.
     */
    .section .text.start, "ax"
    .globl trap_vectors
    .type trap_vectors, %object
    .balign 256
trap_vectors:
    .word   __kernel_stack_top
    .word   _start
    .word   kernel_trap          /* NMI */
    .word   trap_entry           /* HardFault */
    .word   trap_entry           /* MemManage */
    .word   trap_entry           /* BusFault */
    .word   trap_entry           /* UsageFault */
#if ARMV8M
    .word   trap_entry           /* SecureFault */
#else
    .word   0
#endif
    .word   0, 0, 0
    .word   trap_entry           /* SVCall */
    .word   trap_entry           /* DebugMonitor */
    .word   0
    .word   boot_entry           /* PendSV */
    .word   trap_entry           /* SysTick */
    .rept   IRQ_LINES
    .word   trap_entry
    .endr
    .size   trap_vectors, . - trap_vectors

    .text

    .globl _start
    .type _start, %function
_start:
    /* Nothing preempts the kernel while it starts, the first tick included; trap_start lets them in. */
    cpsid   i
    ldr     r0, =__kernel_stack_top
    mov     sp, r0
#if ARMV8M
    /*
     * The stack limits, which a boot ROM may leave set, RP2350's does:
     * a push below the kernel's stack faults, as a kernel fault that halts, and a thread's sp has no limit.
     */
    ldr     r0, =__kernel_stack_bottom
    msr     msplim, r0
    movs    r0, #0
    msr     psplim, r0
#endif

    /* Clear .bss so C code sees zeroed statics. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
1:
    cmp     r0, r1
    bhs     2f
    str     r2, [r0], #4
    b       1b
2:
    bl      arch_init
    bl      kmain
    /* kmain must not return; spin if it does. */
3:
    wfi
    b       3b

    .globl trap_entry
    .type trap_entry, %function
trap_entry:
    /* Only a thread's trap returns to the process stack in thread mode; anything else is the kernel's. */
    ldr     r0, =EXC_RETURN_THREAD_PSP_VALUE
    cmp     lr, r0
    bne     kernel_trap

    ldr     r0, =trap_current
    ldr     r0, [r0]
    add     r1, r0, #FRAME_R4
    stmia   r1, {r4-r11}
    mrs     r1, psp
    str     r1, [r0, #FRAME_PSP]
    mrs     r1, ipsr
    str     r1, [r0, #FRAME_EXCEPTION]

    ldr     r1, =__kernel_stack_top
    mov     sp, r1
    bl      trap_handler
    /* Fall through into trap_return with the frame to resume in r0. */

    .globl trap_return
    .type trap_return, %function
trap_return:
    ldr     r1, =trap_current
    str     r0, [r1]
    mov     r4, r0
    /* The hardware frame below the thread's sp, and the psp that finds it; see frame_give. */
    bl      frame_give
    msr     psp, r0
    add     r1, r4, #FRAME_R4
    ldmia   r1, {r4-r11}

    /* Every trap starts on an empty kernel stack; nothing below needs this one. */
    ldr     r0, =__kernel_stack_top
    msr     msp, r0
    /*
     * Break any exclusive reservation before the thread runs, as the architecture asks of a switch:
     * a thread preempted between its ldrex and its strex would otherwise store over another's store.
     */
    clrex
    ldr     lr, =EXC_RETURN_THREAD_PSP_VALUE
    bx      lr

    /*
     * The first way out of the kernel: kmain pends PendSV and lets interrupts in, see trap_start,
     * and PendSV comes here from thread mode on the main stack.
     * Thread mode goes unprivileged from here on, and the root thread's frame is returned into.
     * Nothing else pends PendSV, and user mode cannot.
     */
    .globl boot_entry
    .type boot_entry, %function
boot_entry:
    ldr     r0, =EXC_RETURN_THREAD_MSP_VALUE
    cmp     lr, r0
    bne     kernel_trap
    movs    r0, #1                /* CONTROL.nPRIV */
    msr     control, r0
    isb
    ldr     r0, =trap_boot_frame
    ldr     r0, [r0]
    b       trap_return

    .type kernel_trap, %function
kernel_trap:
    /* A trap while the kernel runs is a kernel bug; note where it came from, report and halt. */
    ldr     r0, =kernel_trap_state
    str     lr, [r0]
    mov     r1, sp
    str     r1, [r0, #4]
    mrs     r1, psp
    str     r1, [r0, #8]
    ldr     r0, =__kernel_stack_top
    mov     sp, r0
    bl      kernel_trap_panic

    .ltorg
