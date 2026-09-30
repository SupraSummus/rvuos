#ifndef RVUOS_ARCH_H
#define RVUOS_ARCH_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * What the rest of the kernel knows of ARMv7-M: a thread's registers as a trap leaves them,
 * and which of them carry a call.
 * Each architecture has an arch.h of its own in kernel/arch/<arch>/; see DESIGN.md, "Architectures".
 */

/*
 * Register state saved on every trap.
 *
 * The frame lives inside the thread object, as on RISC-V,
 * but the core saves half of it itself, on the thread's own stack:
 * r0 to r3, r12, lr, pc and xpsr, the hardware frame, at the process stack pointer.
 * start.S saves r4 to r11 into regs and notes psp and the exception,
 * and trap.c copies the hardware frame in when the core managed to write it;
 * on the way out trap.c writes it back below the thread's sp and start.S restores the rest.
 * See DESIGN.md, "Architectures".
 * The layout is shared with start.S.
 */
#define TRAP_FRAME_SIZE 88
#define FRAME_R4        16
#define FRAME_EXCEPTION 68
#define FRAME_PSP       80

#ifndef __ASSEMBLER__
struct trap_frame {
    uint32_t regs[15];  /* r0 to r14: regs[13] is the thread's sp and regs[14] its lr */
    uint32_t pc;        /* at the call instruction for a call, as on RISC-V; see frame_take */
    uint32_t xpsr;      /* its T bit, and the IT state an interrupt may find the thread in */
    uint32_t exception; /* the exception number the trap entered on, IPSR */
    uint32_t cfsr;      /* the configurable fault status the trap left, and cleared */
    uint32_t addr;      /* the address MMFAR or BFAR names when valid, else zero */
    uint32_t psp;       /* where the hardware frame lies: on entry what the core used, on exit what trap.c chose */
    uint32_t pad;
};

_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE, "trap frame layout must match start.S");
_Static_assert(__builtin_offsetof(struct trap_frame, regs[4]) == FRAME_R4, "trap frame layout must match start.S");
_Static_assert(__builtin_offsetof(struct trap_frame, exception) == FRAME_EXCEPTION,
               "trap frame layout must match start.S");
_Static_assert(__builtin_offsetof(struct trap_frame, psp) == FRAME_PSP, "trap frame layout must match start.S");

/*
 * Register numbers as indices into trap_frame.regs: r0 to r6 and r12 carry a call, see rvuos/abi.h.
 * The operation goes in r12, not r7, which Thumb code keeps as its frame pointer.
 */
enum {
    REG_A0 = 0,
    REG_A1 = 1,
    REG_A2 = 2,
    REG_A3 = 3,
    REG_A4 = 4,
    REG_A5 = 5,
    REG_A6 = 6,
    REG_R12 = 12,
    REG_A7 = REG_R12,
    REG_SP = 13,
    REG_LR = 14,
};

/* The call instruction, svc, is two bytes: a call resumes past it and a preempted one at it. */
#define CALL_SIZE 2

/* The program counter a thread starts at to run the code at addr: Thumb, as a function pointer to it is. */
#define ENTRY_PC(addr) ((addr) | 1u)

/*
 * No region grants execute without read: the MPU fetches only what it lets the thread read.
 * RISC-V's PMP can, so the install refuses the rights there alone; see OP_PROCESS_INSTALL.
 */
#define EXECUTE_NEEDS_READ 1

/* What the boot banner calls the kernel's mode and the entries of the protection unit. */
#define ARCH_KERNEL_MODE "handler mode"
#define ARCH_REGIONS "mpu regions"
#endif

#endif
