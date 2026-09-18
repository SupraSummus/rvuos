/*
 * Kernel entry after start.S.
 */

#include "csr.h"
#include "kernel.h"
#include "object.h"
#include "pmp.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"

extern char __boot_pool_start[];
extern char __boot_pool_end[];

void kmain(void)
{
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

    paddr_t free_base = USER_DATA_BASE + USER_DATA_SIZE;
    struct thread *root = boot_create_root(
        v2p(__boot_pool_start), (uint32_t)(__boot_pool_end - __boot_pool_start),
        free_base, INPUT_BASE - free_base);
    current = root;
    process_activate(thread_process(root));

    kputs("rvuos: entering user mode\n");
    /*
     * mret drops to the mode in MPP with MIE clear,
     * which gates nothing in user mode: machine interrupts
     * are always taken there, so the tick runs from the first instruction.
     */
    timer_init();
    csr_clear(mstatus, MSTATUS_MPP_MASK | MSTATUS_MPIE);
    trap_return(&root->frame);
}
