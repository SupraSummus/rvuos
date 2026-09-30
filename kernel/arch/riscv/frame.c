/*
 * What the rest of the kernel does to a thread's registers on RISC-V: start it and report it.
 * The host build has this file too, since its threads are RISC-V's.
 */

#include "kernel.h"
#include "trap.h"

void frame_start(struct trap_frame *frame, uint32_t pc, uint32_t sp)
{
    frame->pc = pc;
    frame->regs[REG_SP] = sp;
}

void report_frame(const struct trap_frame *frame)
{
    kputs("  mcause=");
    kput_hex(frame->mcause);
    kputs(" mepc=");
    kput_hex(frame->pc);
    kputs(" mtval=");
    kput_hex(frame->mtval);
    kputc('\n');
}
