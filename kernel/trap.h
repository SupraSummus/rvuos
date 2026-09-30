#ifndef RVUOS_TRAP_H
#define RVUOS_TRAP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Traps, as the rest of the kernel sees them on every architecture.
 * The frame's layout and the registers a call travels in are the architecture's,
 * in its arch.h, and so is trap.c, which dispatches a trap.
 */
#include "arch.h"

/*
 * Called from start.S with the frame the trap was saved into,
 * which is the current thread's.
 * Returns the frame to resume,
 * which may belong to another thread once there is a scheduler.
 */
struct trap_frame *trap_handler(struct trap_frame *frame);

/*
 * Restore a frame and return to user mode into it.
 * Never returns.
 */
__attribute__((noreturn)) void trap_return(struct trap_frame *frame);

/* Leave the kernel for user mode the first time, into frame, with interrupts taken from then on. */
__attribute__((noreturn)) void trap_start(struct trap_frame *frame);

/*
 * Where a stopped thread starts: pc and sp, as OP_THREAD_CONFIGURE takes them and the boot gives the root task;
 * the other registers stay as they were.
 * In the architecture's frame.c, which the host build has too.
 */
void frame_start(struct trap_frame *frame, uint32_t pc, uint32_t sp);

/* What the frame says of the trap that made it, as a line of the log; in frame.c too. */
void report_frame(const struct trap_frame *frame);

/*
 * What OP_THREAD_FAULT returns of the frame a fault left, into out[0] to out[3]:
 * the cause, the program counter, the address and the status, as rvuos/abi.h says them; in frame.c too.
 */
void frame_fault(const struct trap_frame *frame, uint32_t *out);

/*
 * A register of a stopped thread's frame, numbered as OP_THREAD_READ_REG numbers it, THREAD_REG_PC among them:
 * read into *value, or written with value as OP_THREAD_WRITE_REG takes it.
 * Each is false, and leaves *value and the frame alone, for a number that names no register; in frame.c too.
 */
bool frame_read(const struct trap_frame *frame, uint32_t reg, uint32_t *value);
bool frame_write(struct trap_frame *frame, uint32_t reg, uint32_t value);

/* Called from start.S when the kernel itself traps. */
__attribute__((noreturn)) void kernel_trap_panic(void);

/*
 * Stall until the wake-th tick, one being the next, or a device interrupt is pending,
 * with the ticks before it deferred rather than taken; see timer_defer.
 * The tick is acknowledged here and *ticks receives the ticks that passed, zero if none;
 * device interrupts stay in the controller for sched_claim_interrupts to claim,
 * and the result says whether one is there.
 * This is what the kernel does when no thread can run
 * but an armed Irq, on a timer line or a device's, will make one runnable; see DESIGN.md, "Scheduling".
 * The host build has no clock and no devices and must never get here.
 */
bool intr_wait(uint32_t wake, uint32_t *ticks);

/*
 * Whether the tick or a device interrupt is pending, which the kernel runs with interrupts held off to leave.
 * A preemptible walk asks between two steps; see DESIGN.md, "Bounded work".
 * The host build always answers yes.
 */
bool intr_pending(void);

#endif
