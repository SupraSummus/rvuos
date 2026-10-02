/*
 * Kernel entry after start.S.
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "pmp.h"
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
     * An account counts in the counter's counts, which the ESP32-C6 measures here,
     * so the timer starts before the root thread's account is filled.
     * The kernel runs with interrupts held off, so its first tick waits for user mode.
     */
    timer_init();
    struct thread *root = boot_create_root();
    sched_start(root);
    process_activate(thread_process(root));
    board_user_csrs_reset();

    kputs("rvuos: entering user mode\n");
    /*
     * The tick runs from the root task's first instruction, see trap_start.
     * Every device line starts masked and stays so until an Irq is armed on it.
     */
    irq_init();
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
