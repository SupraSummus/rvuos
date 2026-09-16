#ifndef RVUOS_TRAP_H
#define RVUOS_TRAP_H

#include <stdint.h>

/*
 * Register state saved on every trap.
 *
 * The frame lives inside the thread object.
 * While user mode runs, mscratch points at the current thread's frame
 * so the trap entry can save registers without a free register.
 * The layout is shared with start.S,
 * which fills it on entry and drains it on return.
 * regs[0] holds x0 and is never written back.
 * regs[2] is the trapped stack pointer.
 */
struct trap_frame {
    uint32_t regs[32];
    uint32_t mepc;
    uint32_t mcause;
    uint32_t mtval;
    uint32_t pad;
};

#define TRAP_FRAME_SIZE 144

_Static_assert(sizeof(struct trap_frame) == TRAP_FRAME_SIZE,
               "trap frame layout must match start.S");

/* Register numbers as indices into trap_frame.regs. */
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

/*
 * Called from start.S with the frame the trap was saved into,
 * which is the current thread's.
 * Returns the frame to resume,
 * which may belong to another thread once there is a scheduler.
 */
struct trap_frame *trap_handler(struct trap_frame *frame);

/*
 * Restore a frame and mret into it.
 * Never returns.
 */
__attribute__((noreturn)) void trap_return(struct trap_frame *frame);

/* Called from start.S when the kernel itself traps. */
__attribute__((noreturn)) void kernel_trap_panic(void);

#endif
