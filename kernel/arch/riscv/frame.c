/*
 * What the rest of the kernel does to a thread's registers on RISC-V: start it, report it, read and write it.
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

void frame_fault(const struct trap_frame *frame, uint32_t *out)
{
    out[0] = frame->mcause;
    out[1] = frame->pc;
    out[2] = frame->mtval;
    out[3] = 0;
}

/* x0 to x31, then the pc; regs[0] is where trap_return's sc.w points, so x0 reads zero and is never written. */
bool frame_read(const struct trap_frame *frame, uint32_t reg, uint32_t *value)
{
    if (reg > THREAD_REG_PC) {
        return false;
    }
    *value = reg == THREAD_REG_PC ? frame->pc : reg == 0 ? 0 : frame->regs[reg];
    return true;
}

bool frame_write(struct trap_frame *frame, uint32_t reg, uint32_t value)
{
    if (reg > THREAD_REG_PC) {
        return false;
    }
    if (reg == THREAD_REG_PC) {
        frame->pc = value;
    } else if (reg != 0) {
        frame->regs[reg] = value;
    }
    return true;
}
