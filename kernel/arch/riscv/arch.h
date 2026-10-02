#ifndef RVUOS_ARCH_H
#define RVUOS_ARCH_H

#include <stdint.h>

/*
 * What the rest of the kernel knows of RISC-V: a thread's registers as a trap leaves them,
 * and which of them carry a call.
 * Each architecture has an arch.h of its own in kernel/arch/<arch>/; see DESIGN.md, "Architectures".
 */

/*
 * Register state saved on every trap.
 *
 * The frame lives inside the thread object.
 * While user mode runs, mscratch points at the current thread's frame
 * so the trap entry can save registers without a free register.
 * The layout is shared with start.S,
 * which fills it on entry and drains it on return.
 * regs[0] holds x0, is never written back, and is where trap_return's sc.w points.
 * regs[2] is the trapped stack pointer, and pc the trapped mepc.
 */
struct trap_frame {
    uint32_t regs[32];
    uint32_t pc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t pad;
};

#define TRAP_FRAME_SIZE 144

_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
               "trap frame layout must match start.S");

/* Register numbers as indices into trap_frame.regs: a0 to a7 carry a call, see rvuos/abi.h. */
enum {
    REG_SP = 2,
    REG_A0 = 10,
    REG_A1 = 11,
    REG_A2 = 12,
    REG_A3 = 13,
    REG_A4 = 14,
    REG_A5 = 15,
    REG_A6 = 16,
    REG_A7 = 17,
};

/* The call instruction, ecall, is four bytes: a call resumes past it and a preempted one at it. */
#define CALL_SIZE 4

/* The program counter a thread starts at to run the code at addr. */
#define ENTRY_PC(addr) (addr)

/* PMP grants execute without read, so a region may be execute only. */
#define EXECUTE_NEEDS_READ 0

/* What the boot banner calls the kernel's mode and the entries of the protection unit. */
#define ARCH_KERNEL_MODE "machine mode"
#define ARCH_REGIONS "pmp entries"

/* The next ticket of the kernel's lock, see core.c: an atomic add, one amoadd. */
static inline uint32_t arch_ticket_take(uint32_t *next)
{
    return __atomic_fetch_add(next, 1, __ATOMIC_RELAXED);
}

/* A hart that waits on another in core.c spins, and one that stores what it waits for wakes it by the store alone. */
static inline void arch_core_wait(void)
{
}

static inline void arch_core_wake(void)
{
}

#endif
