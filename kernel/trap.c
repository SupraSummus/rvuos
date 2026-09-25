/*
 * Trap dispatch.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"
#include "timer.h"
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
        /* mepc points at the interrupted instruction, which resumes as it was. */
        switch (cause & ~MCAUSE_INTERRUPT) {
        case IRQ_M_TIMER:
            timer_ack();
            /* Under tracing time moves only by record; see DESIGN.md, "Verification". */
            if (!debug_trace) {
                sched_tick();
            }
            return &current->frame;
        case IRQ_EXT_CAUSE:
            /*
             * Delivered under tracing as well: a line left claimed would storm
             * and one masked without its Irq disarmed would break an invariant.
             * The replay driver holds no device, so none arrives; see DESIGN.md, "Verification".
             */
            sched_claim_interrupts();
            return &current->frame;
        default:
            kputs("unexpected interrupt\n");
            report_frame(frame);
            kpanic("only the timer and the external interrupt are enabled");
        }
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

/*
 * wfi resumes when an enabled interrupt is pending,
 * whether or not machine mode would take it,
 * and the kernel runs with MIE clear, so the interrupt is polled rather than taken.
 * A core may also treat wfi as a no-op, which the loop tolerates.
 */
unsigned intr_wait(void)
{
    const uint32_t mip_ext = 1u << IRQ_EXT_CAUSE;
    uint32_t ip;
    while (((ip = csr_read(mip)) & (MIP_MTIP | mip_ext)) == 0) {
        LOOP_WAIT("an interrupt to be pending");
        __asm__ volatile("wfi");
    }
    unsigned pending = 0;
    if (ip & MIP_MTIP) {
        timer_ack();
        pending |= INTR_TICK;
    }
    if (ip & mip_ext) {
        pending |= INTR_DEVICE;
    }
    return pending;
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
