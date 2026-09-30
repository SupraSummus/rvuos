#ifndef RVUOS_USER_CALL_H
#define RVUOS_USER_CALL_H

/*
 * How a program on ARMv7-M or ARMv8-M calls the kernel, for rvuos.h:
 * the registers a call travels in, r0 to r6 as a0 to a6 and r12 as a7, see rvuos/abi.h,
 * and the instruction that makes it.
 * The operation goes in r12 rather than r7, which Thumb code keeps as its frame pointer,
 * so a program may be built with one or without.
 * Each architecture has a call.h of its own in user/arch/<arch>/.
 */

#define RV_A0 "r0"
#define RV_A1 "r1"
#define RV_A2 "r2"
#define RV_A3 "r3"
#define RV_A4 "r4"
#define RV_A5 "r5"
#define RV_A6 "r6"
#define RV_A7 "r12"
#define RV_CALL "svc #0"

/* a0, the first argument of a function, as OP_THREAD_READ_REG and OP_THREAD_WRITE_REG number it: r0. */
#define RV_REG_A0 0

/* A breakpoint, which faults: the thread stops there and its watch hears it. */
#define rv_breakpoint() __asm__ volatile("bkpt #0")

#endif
