/*
 * Kernel entry after start.S.
 */

#include "csr.h"
#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "pmp.h"
#include "timer.h"
#include "trap.h"

void kmain(void)
{
    board_init();
    /* Nothing prints before the log exists to take it. */
    klog_init();
    kputs("rvuos: machine mode up\n");

    pmp_init();
    kputs("rvuos: pmp entries ");
    kput_hex(pmp_entry_count);
    kputs(" grain ");
    kput_hex(pmp_grain);
    kputc('\n');
    if (pmp_entry_count < 4) {
        kpanic("need at least four PMP entries");
    }

    /*
     * An account counts in the counter's counts, which the ESP32-C6 measures here,
     * so the timer starts before the root thread's account is filled.
     * Machine mode runs with MIE clear, so its first tick waits for the mret.
     */
    timer_init();
    struct thread *root = boot_create_root();
    sched_start(root);
    process_activate(thread_process(root));
    board_user_csrs_reset();

    kputs("rvuos: entering user mode\n");
    /*
     * mret drops to the mode in MPP with MIE set from MPIE, which is set:
     * machine interrupts are taken in user mode whatever MIE holds,
     * but for RP2350's Hazard3, which takes none there while it is clear, erratum RP2350-E7.
     * A trap from user mode saves MIE into MPIE, so every later mret sets it again.
     * So the tick runs from the first instruction.
     * Every device line starts masked and stays so until an Irq is armed on it.
     */
    irq_init();
    csr_clear(mstatus, MSTATUS_MPP_MASK);
    csr_set(mstatus, MSTATUS_MPIE);
    trap_return(&root->frame);
}
