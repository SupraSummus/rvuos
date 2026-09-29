/*
 * Trap dispatch.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"
#include "timer.h"
#include "trap.h"

struct trap_frame *trap_handler(struct trap_frame *frame)
{
    struct thread *t = current;
    if (frame != &t->frame) {
        kpanic("trap frame is not the current thread's");
    }

    /*
     * The timer interrupts only at a tick that could change what runs,
     * so the ticks it let pass are counted first, before anything reads the count or an account.
     * While it is set for the next tick, a tick has passed only if its interrupt is pending,
     * so the counter is read only then or while it is set further.
     * Under tracing time moves only by record; see DESIGN.md, "Verification".
     */
    bool traced = debug_trace;
    if (!traced && ((csr_read(mip) & MIP_MTIP) != 0 || wake_tick - sched_ticks > 1)) {
        sched_count(timer_count());
    }

    uint32_t cause = frame->mcause;
    if (cause & MCAUSE_INTERRUPT) {
        /* mepc points at the interrupted instruction, which resumes as it was. */
        switch (cause & ~MCAUSE_INTERRUPT) {
        case IRQ_M_TIMER:
            /* Counted above, and the turn ends below; under tracing it is only moved on. */
            if (debug_trace) {
                timer_count();
                timer_set(1);
            }
            break;
        case IRQ_EXT_CAUSE:
            /*
             * Delivered under tracing as well: a line left claimed would storm
             * and one masked without its Irq disarmed would break an invariant.
             * The replay driver holds no device, so none arrives; see DESIGN.md, "Verification".
             */
            sched_claim_interrupts();
            break;
        default:
            kputs("unexpected interrupt\n");
            report_frame(frame);
            kpanic("only the timer and the external interrupt are enabled");
        }
    } else {
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
            /* The thread's alone: it stops, its watch hears, and the machine goes on. */
            fault_dispatch(t);
            break;
        default:
            kputs("unhandled trap\n");
            report_frame(frame);
            kpanic("unhandled trap cause");
        }
    }

    /*
     * A trap whose count reached the tick the timer was set for ends the turn,
     * and the timer is set for the first tick that could change what runs next,
     * unless nothing that could has changed.
     * The trap that turns tracing on leaves it on the next tick, as the traced interrupt does.
     */
    if (turn_due || wake_stale) {
        uint32_t wake = sched_wake();
        if (!debug_trace && wake != 0) {
            timer_set(wake);
        }
    }
    if (debug_trace && !traced) {
        timer_set(1);
    }
    return &current->frame;
}

/*
 * wfi resumes when an enabled interrupt is pending,
 * whether or not machine mode would take it,
 * and the kernel runs with MIE clear, so the interrupt is polled rather than taken.
 * A core may also treat wfi as a no-op, which the loop tolerates.
 * The timer is set for the tick asked for and stays there however the stall ends,
 * until the trap's end sets it for the thread that runs.
 */
bool intr_wait(uint32_t wake, uint32_t *ticks)
{
    const uint32_t mip_ext = 1u << IRQ_EXT_CAUSE;
    timer_set(wake);
    uint32_t ip;
    while (((ip = csr_read(mip)) & (MIP_MTIP | mip_ext)) == 0) {
        LOOP_WAIT("an interrupt to be pending");
        __asm__ volatile("wfi");
    }
    *ticks = timer_count();
    return (ip & mip_ext) != 0;
}

bool intr_pending(void)
{
    return (csr_read(mip) & (MIP_MTIP | (1u << IRQ_EXT_CAUSE))) != 0;
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
