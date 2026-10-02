/*
 * Trap dispatch.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"
#include "timer.h"
#include "trap.h"

uint32_t core_id(void)
{
    return csr_read(mhartid);
}

/*
 * A trap from user mode, with the thread's registers in its frame.
 * The interrupt it entered on is taken first, whatever became of the thread;
 * another core may have stopped or destroyed it while the trap waited for the kernel's lock,
 * and then nothing else is, see core_trapped.
 * Returns the frame to resume, or NULL for nobody's turn, which start.S idles for.
 */
struct trap_frame *trap_handler(struct trap_frame *frame)
{
    core_enter();
    struct core *core = core_self();
    bool traced = timer_trap_enter((csr_read(mip) & MIP_MTIP) != 0);

    uint32_t cause = csr_read(mcause);
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
#if CORES > 1
        case IRQ_M_SOFT:
            /* Another core asked this one to trap: to stop its thread, load its regions or set its timer again. */
            ipi_clear();
            break;
#endif
        default:
            kputs("unexpected interrupt\n");
            if (core->current != NULL && frame == &core->current->frame) {
                report_frame(frame);
            }
            kpanic("only the timer, the software and the external interrupt are enabled");
        }
    }

    struct thread *t = core_trapped();
    if (t != NULL && frame != &t->frame) {
        kpanic("trap frame is not the current thread's");
    }
    if (t != NULL && !(cause & MCAUSE_INTERRUPT)) {
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
    if (core->turn == NULL) {
        return NULL;
    }
    struct trap_frame *next = &core->current->frame;
    core_leave();
    return next;
}

/*
 * A core with no thread to run, from start.S as a trap ends with nobody's turn, or as the core starts:
 * it idles with the kernel's lock, given up only while it waits, until a thread has its turn,
 * then leaves for it as a trap does.
 */
void core_idle(void)
{
    sched_idle();
    timer_trap_leave(debug_trace);
    struct trap_frame *next = &core_self()->current->frame;
    core_leave();
    trap_return(next);
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
    /* Another core wakes this one with its software interrupt, which only a core of several enables. */
    const uint32_t mip_wake = MIP_MTIP | mip_ext | (CORES > 1 ? MIP_MSIP : 0);
    timer_set(wake);
    core_stall_begin();
    uint32_t ip;
    while (((ip = csr_read(mip)) & mip_wake) == 0) {
        LOOP_WAIT("an interrupt to be pending");
        __asm__ volatile("wfi");
    }
    core_stall_end();
#if CORES > 1
    if (ip & MIP_MSIP) {
        ipi_clear();
    }
#endif
    *ticks = timer_count();
    return (ip & mip_ext) != 0;
}

/* Another core waiting for the lock stops a walk as an interrupt does; see core_lock_waited. */
bool intr_pending(void)
{
    return (csr_read(mip) & (MIP_MTIP | (1u << IRQ_EXT_CAUSE))) != 0 || core_lock_waited();
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
    core_leave();
    trap_return(frame);
}

#if CORES > 1
/*
 * A hart other than the first, from start.S, once the first booted the kernel and raised the hart's software interrupt:
 * it takes the interrupt back, starts its timer and fences off what the first fenced off,
 * then idles until it has a thread, and leaves for it as trap_start does.
 * Its software interrupt stays enabled, for the other cores to wake it and make it trap.
 */
void core_start(void)
{
    core_enter();
    ipi_clear();
    timer_core_start();
    process_fence_core();
    ipi_enable();
    csr_clear(mstatus, MSTATUS_MPP_MASK);
    csr_set(mstatus, MSTATUS_MPIE);
    core_idle();
}
#endif

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
