/* What the rest of the kernel does to a thread's registers on ARMv7-M: start it and report it. */

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
