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
    struct thread *t = core_self()->current;
    if (frame != &t->frame) {
        kpanic("trap frame is not the current thread's");
    }

    bool traced = timer_trap_enter((csr_read(mip) & MIP_MTIP) != 0);

    uint32_t cause = frame->mcause;
    if (cause & MCAUSE_INTERRUPT) {
        /* mepc points at the interrupted instruction, which resumes as it was. */
        switch (cause & ~MCAUSE_INTERRUPT) {
        case IRQ_M_TIMER:
            timer_trap_tick();
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

    timer_trap_leave(traced);
    return &core_self()->current->frame;
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

/*
 * mret drops to the mode in MPP with MIE set from MPIE, which is set:
 * machine interrupts are taken in user mode whatever MIE holds,
 * but for RP2350's Hazard3, which takes none there while it is clear, erratum RP2350-E7.
 * A trap from user mode saves MIE into MPIE, so every later mret sets it again.
 * So the tick runs from the first instruction.
 */
void trap_start(struct trap_frame *frame)
{
    csr_clear(mstatus, MSTATUS_MPP_MASK);
    csr_set(mstatus, MSTATUS_MPIE);
    trap_return(frame);
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
