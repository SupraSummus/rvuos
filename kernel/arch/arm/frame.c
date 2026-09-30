/* What the rest of the kernel does to a thread's registers on ARMv7-M: start it, report it, read and write it. */

#include "kernel.h"
#include "scs.h"
#include "trap.h"

/*
 * The pc is taken as a branch takes it: bit 0 says Thumb, the only state the core has,
 * as a function pointer carries it, and a pc without it starts the thread in a fault, as a bx to it would.
 * The rest of xpsr starts clear, out of any IT block.
 */
void frame_start(struct trap_frame *frame, uint32_t pc, uint32_t sp)
{
    frame->pc = pc & ~1u;
    frame->xpsr = (pc & 1u) ? XPSR_T : 0u;
    frame->regs[REG_SP] = sp;
}

void report_frame(const struct trap_frame *frame)
{
    kputs("  exception=");
    kput_hex(frame->exception);
    kputs(" cfsr=");
    kput_hex(frame->cfsr);
    kputs(" pc=");
    kput_hex(frame->pc);
    kputs(" addr=");
    kput_hex(frame->addr);
    kputc('\n');
}

void frame_fault(const struct trap_frame *frame, uint32_t *out)
{
    out[0] = frame->exception;
    frame_read(frame, THREAD_REG_PC, &out[1]);
    out[2] = frame->addr;
    out[3] = frame->cfsr;
}

/*
 * r0 to r14, then the pc, as frame_start takes it: bit 0 is the Thumb state, so a pc read writes back as it was.
 * After a stacking fault, which the fault status says, r0 to r3, r12, lr, pc and xpsr are what the last trap left;
 * see DESIGN.md, "Architectures".
 */
bool frame_read(const struct trap_frame *frame, uint32_t reg, uint32_t *value)
{
    if (reg == THREAD_REG_PC) {
        *value = frame->pc | ((frame->xpsr & XPSR_T) ? 1u : 0u);
        return true;
    }
    if (reg > REG_LR) {
        return false;
    }
    *value = frame->regs[reg];
    return true;
}

/* A pc written leaves any IT block, as a branch does, and keeps the flags. */
bool frame_write(struct trap_frame *frame, uint32_t reg, uint32_t value)
{
    if (reg == THREAD_REG_PC) {
        frame->pc = value & ~1u;
        frame->xpsr = (frame->xpsr & XPSR_APSR) | ((value & 1u) ? XPSR_T : 0u);
        return true;
    }
    if (reg > REG_LR) {
        return false;
    }
    frame->regs[reg] = value;
    return true;
}
