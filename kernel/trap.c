/*
 * Trap dispatch.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"
#include "trap.h"

static void report_frame(const struct trap_frame *frame)
{
    kputs("  mcause=");
    kput_hex(frame->mcause);
    kputs(" mepc=");
    kput_hex(frame->mepc);
    kputs(" mtval=");
    kput_hex(frame->mtval);
    kputc('\n');
}

static __attribute__((noreturn)) void handle_user_fault(struct trap_frame *frame)
{
    /*
     * Every fault stops the machine, even when another thread could run.
     * A faulting thread has nobody to report to yet:
     * that would be a notification the kernel signals,
     * or a state its creator can read, and neither exists.
     * Stopping and printing is the honest placeholder; see TODO.md.
     */
    kputs("user fault\n");
    report_frame(frame);
    khalt(4);
}

struct trap_frame *trap_handler(struct trap_frame *frame)
{
    struct thread *t = current;
    if (frame != &t->frame) {
        kpanic("trap frame is not the current thread's");
    }

    uint32_t cause = frame->mcause;
    if (cause & MCAUSE_INTERRUPT) {
        kputs("unexpected interrupt\n");
        report_frame(frame);
        kpanic("interrupts are not enabled yet");
    }

    switch (cause) {
    case CAUSE_ECALL_U:
        syscall_dispatch(t);
        break;
    case CAUSE_INSN_ACCESS:
    case CAUSE_LOAD_ACCESS:
    case CAUSE_STORE_ACCESS:
    case CAUSE_ILLEGAL_INSN:
    case CAUSE_INSN_MISALIGNED:
    case CAUSE_LOAD_MISALIGNED:
    case CAUSE_STORE_MISALIGNED:
    case CAUSE_BREAKPOINT:
        handle_user_fault(frame);
    default:
        kputs("unhandled trap\n");
        report_frame(frame);
        kpanic("unhandled trap cause");
    }

    return &current->frame;
}

void kernel_trap_panic(void)
{
    kputs("trap from machine mode\n");
    kputs("  mcause=");
    kput_hex(csr_read(mcause));
    kputs(" mepc=");
    kput_hex(csr_read(mepc));
    kputs(" mtval=");
    kput_hex(csr_read(mtval));
    kputc('\n');
    kpanic("kernel fault");
}
