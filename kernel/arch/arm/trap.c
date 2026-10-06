/*
 * Trap dispatch on ARMv7-M and ARMv8-M, and the thread's half of its frame that the core keeps on its stack.
 * See DESIGN.md, "Architectures".
 */

#include "armv7m.h"
#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "sched.h"
#include "scs.h"
#include "timer.h"
#include "trap.h"

/* Each core's running thread's frame, which start.S saves into; set as each trap returns. */
struct trap_frame *trap_current[CORES];

/* The root thread's frame, for boot_entry in start.S; see trap_start. */
struct trap_frame *trap_boot_frame;

/* Where a trap of the kernel's own came from on each core, lr, sp and psp, as start.S notes them for kernel_trap_panic. */
uint32_t kernel_trap_state[CORES][3];

#if CORES > 1
/* The board's register that reads each core's own number, as RISC-V's mhartid does. */
uint32_t core_id(void)
{
    return *(volatile const uint32_t *)CORE_ID_ADDR;
}
#endif

/*
 * Where frame_give points psp when the thread's stack cannot take the hardware frame:
 * the kernel's own stack, which no region covers,
 * so the core's unstacking faults there and the thread stops with its frame as it was.
 */
extern char __kernel_stack_bottom[];
#define STACK_GUARD ((uint32_t)__kernel_stack_bottom)

/* The hardware frame: r0 to r3, r12, lr, pc and xpsr, in eight words. */
#define HW_FRAME 32u

/* The fault status a stacking, which leaves the frame unwritten, or an unstacking reports. */
#define CFSR_STACKING (CFSR_MSTKERR | CFSR_STKERR)
#define CFSR_UNSTACKING (CFSR_MUNSTKERR | CFSR_UNSTKERR)

/*
 * The core as the kernel wants it, before anything else runs; start.S calls this before kmain.
 * ARMv8-M has no DISDEFWBUF, and its boot ROM may leave state of its own, RP2350's does:
 * every coprocessor is shut to everyone, so no thread reaches one, nor the floating-point unit,
 * and the attribution unit is off with nothing Non-secure, so all memory is Secure, as the kernel and every thread are.
 */
void arch_init(void)
{
    extern char trap_vectors[];
    SCS_REG(VTOR) = (uint32_t)trap_vectors;
    SCS_REG(CCR) |= CCR_STKALIGN;
#if ARMV8M
    SCS_REG(CPACR) = 0;
    SCS_REG(NSACR) = 0;
    SCS_REG(SAU_CTRL) = 0;
    __asm__ volatile("dsb\n\tisb" : : : "memory");
#else
    SCS_REG(ACTLR) |= ACTLR_DISDEFWBUF;
#endif
    SCS_REG(SCR) |= SCR_SEVONPEND;
    SCS_REG(DEMCR) |= DEMCR_MON_EN;
    /* Every exception at the same priority, zero, as irq_init puts every line: none preempts another. */
    SCS_REG(SHPR1) = 0;
    SCS_REG(SHPR2) = 0;
    SCS_REG(SHPR3) = 0;
    SCS_REG(SHCSR) |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA | SHCSR_USGFAULTENA;
#if ARMV8M
    SCS_REG(SHCSR) |= SHCSR_SECUREFAULTENA;
#endif
}

/*
 * The thread's registers from the hardware frame at psp, and the fault status of the trap.
 * The kernel reads the frame only where the core just wrote it with the thread's own rights:
 * not after a stacking fault, which left it unwritten, and not at the guard,
 * where the core wrote nothing and the thread's frame is still the one frame_give could not place.
 * After a stacking fault the thread loses r0 to r3, r12, lr, pc and xpsr,
 * which keep what its last trap left; see DESIGN.md, "Architectures".
 */
static void frame_take(struct trap_frame *f)
{
    /* The address before the status is cleared: MMFAR and BFAR may be one register, which the clear leaves unknown. */
    uint32_t cfsr = SCS_REG(CFSR);
    f->cfsr = cfsr;
    f->addr = (cfsr & CFSR_MMARVALID) ? SCS_REG(MMFAR) : (cfsr & CFSR_BFARVALID) ? SCS_REG(BFAR) : 0;
    SCS_REG(CFSR) = cfsr;
    f->sfsr = 0;
#if ARMV8M
    /* A SecureFault's status lies apart from the others', and its address with it. */
    if (f->exception == EXC_SECUREFAULT) {
        uint32_t sfsr = SCS_REG(SFSR);
        f->sfsr = sfsr;
        if (sfsr & SFSR_SFARVALID) {
            f->addr = SCS_REG(SFAR);
        }
        SCS_REG(SFSR) = sfsr;
    }
#endif

    if (cfsr & CFSR_STACKING) {
        /*
         * The fault and the exception whose stacking it broke are one event, the thread's;
         * whichever of the two the core entered on, the other is still pending
         * and would be taken for whichever thread runs next, a call among them.
         */
        SCS_REG(SHCSR) &= ~(SHCSR_SVCALLPENDED | SHCSR_MEMFAULTPENDED | SHCSR_BUSFAULTPENDED);
        f->regs[REG_SP] = f->psp + HW_FRAME;
        return;
    }
    if ((cfsr & CFSR_UNSTACKING) || f->psp == STACK_GUARD) {
        return;
    }
    const volatile uint32_t *hw = (const volatile uint32_t *)f->psp;
    for (unsigned i = 0; i < 4; i++) {
        LOOP_BOUND(4);
        f->regs[i] = hw[i];
    }
    f->regs[REG_R12] = hw[4];
    f->regs[REG_LR] = hw[5];
    f->pc = hw[6];
    uint32_t xpsr = hw[7];
    f->regs[REG_SP] = f->psp + HW_FRAME + ((xpsr & XPSR_ALIGNED) ? 4u : 0u);
    f->xpsr = xpsr & ~XPSR_ALIGNED;
    /* A call leaves pc past its svc; the rest of the kernel wants it at the call, as RISC-V has mepc. */
    if (f->exception == EXC_SVCALL) {
        f->pc -= CALL_SIZE;
    }
}

/*
 * Whether [at, at + HW_FRAME) lies in RAM, within one region the thread's process may read and write.
 * RAM alone: the kernel's store is privileged, and a device may let the kernel do what it keeps from the thread.
 */
static bool stack_takes(const struct thread *t, uint32_t at)
{
    const struct process *proc = thread_process(t);
    if (proc == NULL || !ram_contains(at, HW_FRAME)) {
        return false;
    }
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        LOOP_BOUND(PROCESS_REGION_SLOTS);
        const struct cap *s = &proc->slots[i];
        if (s->type != CAP_NONE && (s->rights & (RIGHT_R | RIGHT_W)) == (RIGHT_R | RIGHT_W) &&
            at - s->a < s->b && s->b - (at - s->a) >= HW_FRAME) {
            return true;
        }
    }
    return false;
}

/*
 * Write the hardware frame of the thread about to run below its sp, and return where it lies,
 * which start.S puts in psp for the core to unstack from with the thread's rights.
 * The kernel writes only where the thread's own regions let it write,
 * so a sp that points anywhere else costs the thread a fault and nothing more:
 * psp goes to the guard instead, the unstacking faults, and trap_handler stops the thread.
 * An sp off eight bytes sets xpsr bit 9, as the core does, so the unstacking gives it back.
 * The MPU is on for the thread from here, whichever way, see mpu_thread.
 */
uint32_t frame_give(struct trap_frame *f)
{
    const struct thread *current = core_self()->current;
    if (f != &current->frame) {
        kpanic("trap frame is not the current thread's");
    }
    uint32_t sp = f->regs[REG_SP] & ~3u;
    uint32_t at = (sp - HW_FRAME) & ~7u;
    if (stack_takes(current, at)) {
        volatile uint32_t *hw = (volatile uint32_t *)at;
        for (unsigned i = 0; i < 4; i++) {
            LOOP_BOUND(4);
            hw[i] = f->regs[i];
        }
        hw[4] = f->regs[REG_R12];
        hw[5] = f->regs[REG_LR];
        hw[6] = f->pc;
        hw[7] = (f->xpsr & ~XPSR_ALIGNED) | (at + HW_FRAME != sp ? XPSR_ALIGNED : 0u);
    } else {
        at = STACK_GUARD;
    }
    f->psp = at;
    mpu_thread();
    /* The thread's stack is written, so the core leaves the kernel here, on every way out. */
    core_leave();
    return at;
}

/*
 * A trap from a thread, with r4 to r11 in its frame and the rest on its stack.
 * The frame is whole before the trap tells the other cores it runs the kernel, see core_enter,
 * since a core that takes the thread waits for that, and then may free its stack or read its frame.
 * The interrupt the trap entered on is taken first, whatever became of the thread;
 * another core may have stopped or destroyed it while the trap waited for the kernel's lock,
 * and then nothing else is, see core_trapped.
 * Returns the frame to resume, or NULL for nobody's turn, which start.S idles for.
 */
struct trap_frame *trap_handler(struct trap_frame *frame)
{
    mpu_kernel();
    frame_take(frame);
    core_enter();
    irq_sync();
    struct core *core = core_self();
    bool lost = (frame->cfsr & CFSR_STACKING) != 0;

    /* SysTick's own pending bit is dropped as it is taken, so the counter says whether the tick is due. */
    bool traced = timer_trap_enter(counter_due());

    uint32_t exception = frame->exception;
    bool interrupt = exception >= EXC_IRQ0 || exception == EXC_SYSTICK;
    if (exception == EXC_SYSTICK) {
        counter_tick();
        timer_trap_tick();
    } else if (interrupt && line_is_cores(exception - EXC_IRQ0)) {
        /* Another core asked this one to trap: to stop its thread, load its regions or set its timer again. */
        ipi_clear();
    } else if (interrupt) {
        /*
         * The line the exception entered on is active now, not pending, so irq_claim is told of it.
         * Delivered under tracing as well: a line masked without its Irq disarmed would break an invariant.
         * The replay driver holds no device, so none arrives; see DESIGN.md, "Verification".
         */
        nvic_entered = exception - EXC_IRQ0;
        sched_claim_interrupts();
    }

    struct thread *t = core_trapped();
    if (t != NULL && frame != &t->frame) {
        kpanic("trap frame is not the current thread's");
    }
    if (t != NULL && !interrupt) {
        switch (exception) {
        case EXC_SVCALL:
            /* A call whose stacking faulted left no registers to make it with; see below. */
            if (!lost) {
                syscall_dispatch(t);
            }
            break;
        case EXC_HARDFAULT:
        case EXC_MEMMANAGE:
        case EXC_BUSFAULT:
        case EXC_USAGEFAULT:
#if ARMV8M
        case EXC_SECUREFAULT:
#endif
        case EXC_DEBUGMON:
            /* The thread's alone: it stops, its watch hears, and the machine goes on. */
            fault_dispatch(t);
            lost = false;
            break;
        default:
            kputs("unhandled trap\n");
            report_frame(frame);
            kpanic("unhandled exception");
        }
    }
    /* A trap that lost the thread's frame is the thread's fault, whatever it entered on. */
    if (t != NULL && lost) {
        fault_dispatch(t);
    }

    timer_trap_leave(traced);
    return core->turn != NULL ? &core->current->frame : NULL;
}

/*
 * Every exception has the priority the kernel runs at, so none is taken while it runs, and wfi would not wake:
 * wfe does, since SEVONPEND makes a line becoming pending an event, taken or not.
 * A core may also treat wfe as a no-op, which the loop tolerates.
 * Where SysTick becoming pending is no event, as in QEMU's model of the MPS2 boards, wfe would sleep through the tick,
 * so the loop polls on yield instead, a no-op on silicon, on which QEMU gives another core its turn.
 * The timer is set for the tick asked for and stays there however the stall ends,
 * until the trap's end sets it for the thread that runs.
 */
bool intr_wait(uint32_t wake, uint32_t *ticks)
{
    timer_set(wake);
    core_stall_begin();
    bool device;
    while (!(device = nvic_pending()) && !counter_due() && !nvic_ipi_pending()) {
        LOOP_WAIT("an interrupt to be pending");
        /*
         * A reload of SysTick ran out short of a compare further than one reload reaches: set it for what is left.
         * Left pending, it would stay so through its next reload, which then makes no event, and wfe would not wake.
         */
        if (SCS_REG(ICSR) & ICSR_PENDSTSET) {
            counter_tick();
        }
#if SYSTICK_WAKES_WFE
        __asm__ volatile("wfe");
#else
        __asm__ volatile("yield");
#endif
    }
    core_stall_end();
    irq_sync();
#if CORES > 1
    if (nvic_ipi_pending()) {
        ipi_clear();
    }
#endif
    *ticks = timer_count();
    return device;
}

bool intr_pending(void)
{
    return nvic_pending() || counter_due() || core_lock_waited();
}

/*
 * PendSV is the way out of thread mode, where kmain runs with interrupts held off since reset:
 * pended, then let in, it enters boot_entry in start.S, which drops thread mode's privilege
 * and returns into the root thread, whose trap from then on leaves the main stack empty.
 * So the tick runs from the root task's first instruction.
 */
void trap_start(struct trap_frame *frame)
{
    trap_boot_frame = frame;
    SCS_REG(ICSR) = ICSR_PENDSVSET;
    __asm__ volatile("dsb\n\tisb\n\tcpsie i" : : : "memory");
    kpanic("PendSV did not leave for user mode");
}

#if CORES > 1
/*
 * A core other than the first, from boot_entry in start.S, once the board started it at the end of the boot:
 * it sets up its own MPU as the first did, before it takes a ticket of the lock through it, see arch_ticket_take,
 * then what the board has of its own, and its own NVIC and SysTick,
 * then idles until it has a thread, and leaves for it as a trap does.
 * Its line for the other cores' interrupts stays enabled, for them to wake it and make it trap.
 */
void core_start(void)
{
    mpu_core_init();
    core_enter();
    board_core_init();
    irq_core_init();
    timer_core_start();
    ipi_clear();
    ipi_enable();
    core_idle();
}
#endif

void kernel_trap_panic(void)
{
    uint32_t ipsr;
    const uint32_t *state = kernel_trap_state[core_index()];
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    kputs("trap from the kernel\n");
    kputs("  exception=");
    kput_hex(ipsr & 0x1ffu);
    kputs(" lr=");
    kput_hex(state[0]);
    kputs(" cfsr=");
    kput_hex(SCS_REG(CFSR));
    kputs(" hfsr=");
    kput_hex(SCS_REG(HFSR));
    kputs(" mmfar=");
    kput_hex(SCS_REG(MMFAR));
    kputs(" bfar=");
    kput_hex(SCS_REG(BFAR));
    /* A trap from handler mode or from kmain left its frame on the main stack, and its pc in it. */
    if ((state[0] & 0x4u) == 0) {
        kputs(" pc=");
        kput_hex(((const uint32_t *)state[1])[6]);
    }
    kputc('\n');
    kpanic("kernel fault");
}
