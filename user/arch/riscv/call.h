#ifndef RVUOS_USER_CALL_H
#define RVUOS_USER_CALL_H

/*
 * How a program on RISC-V calls the kernel, for rvuos.h:
 * the registers a call travels in, see rvuos/abi.h, and the instruction that makes it.
 * Each architecture has a call.h of its own in user/arch/<arch>/.
 */

#define RV_A0 "a0"
#define RV_A1 "a1"
#define RV_A2 "a2"
#define RV_A3 "a3"
#define RV_A4 "a4"
#define RV_A5 "a5"
#define RV_A6 "a6"
#define RV_A7 "a7"
#define RV_CALL "ecall"

/* A breakpoint, which faults: the thread stops there and its watch hears it. */
#define rv_breakpoint() __asm__ volatile("ebreak")

#endif
