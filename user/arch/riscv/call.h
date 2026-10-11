#ifndef RVUOS_USER_CALL_H
#define RVUOS_USER_CALL_H

#include <stdint.h>

/*
 * How a program on RISC-V calls the kernel, for rvuos.h:
 * the registers a call travels in, see rvuos/abi.h, and the instruction that makes it,
 * which rv_call_at knows again where a stopped thread stands at its call.
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

/* a0, the first argument of a function, as OP_THREAD_READ_REG and OP_THREAD_WRITE_REG number it: x10. */
#define RV_REG_A0 10

/* a7, the operation of a call, as they number it: x17. */
#define RV_REG_A7 17

/*
 * Whether the instruction at pc, as OP_THREAD_READ_REG reads a thread's, is the call, ecall, 0x00000073.
 * Read a halfword at a time, since with compressed instructions it may lie on two bytes.
 */
static inline int rv_call_at(uint32_t pc)
{
    const volatile uint16_t *at = (const volatile uint16_t *)pc;
    return at[0] == 0x0073u && at[1] == 0x0000u;
}

/* A breakpoint, which faults: the thread stops there and its watch hears it. */
#define rv_breakpoint() __asm__ volatile("ebreak")

#endif
