/*
 * Kernel entry after start.S: the boot, and the idle a core enters as a trap ends with nobody's turn.
 */

#include "dma.h"
#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "pmp.h"
#include "route.h"
#include "sched.h"
#include "timer.h"
#include "trap.h"

void kmain(void)
{
    /* The kernel's lock, which the other cores wait for once they start, at the end of the boot. */
    core_enter();
    board_init();
    /* Nothing prints before the log exists to take it. */
    klog_init();
    kputs("rvuos: " ARCH_KERNEL_MODE " up\n");

    pmp_init();
    process_fence();
    kputs("rvuos: " ARCH_REGIONS " ");
    kput_hex(pmp_entry_count);
    kputs(" grain ");
    kput_hex(pmp_grain);
    kputc('\n');
    if (pmp_entry_count < 4) {
        kpanic("need at least four " ARCH_REGIONS);
    }

    /*
     * Spans and delays count in the counter's counts, whose rate the ESP32-C6 measures here,
     * so the timer starts before the root thread is bound.
     * The kernel runs with interrupts held off, so its first timer interrupt waits for user mode.
     */
    timer_init();
    struct thread *root = boot_create_root();
    sched_start(root);
    process_activate(thread_process(root));
    board_user_csrs_switch(NULL, thread_process(root));

    kputs("rvuos: entering user mode\n");
    /*
     * The timer runs from the root task's first instruction, see trap_start.
     * Every device line starts masked and stays so until an Irq is armed on it.
     */
    irq_init();
    /* Every channel the kernel lends wires nothing until a Route is armed on it, as every line is masked. */
    route_init();
    /* Every channel the kernel lends moves nothing until a Dma is started on it. */
    dma_init();
#if CORES > 1
    /*
     * This core takes the others' interrupts from now on, which wake it and make it trap.
     * The others start now, as the board starts them, then idle until a thread is bound to their units.
     */
    ipi_enable();
    board_cores_start();
#endif
    trap_start(&root->frame);
}

/*
 * A core with no thread to run, from start.S as a trap ends with nobody's turn, or as the core starts:
 * it idles with the kernel's lock, given up only while it waits, until a thread has its turn,
 * then leaves for it as a trap does, through trap_return.
 */
void core_idle(void)
{
    sched_idle();
    timer_trap_leave(debug_trace);
    trap_return(&core_self()->current->frame);
}
