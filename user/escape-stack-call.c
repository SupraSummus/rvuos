/*
 * Escape attempt on ARM: a call whose frame the core would stack into the kernel.
 *
 * As it takes an exception the core saves half of a thread's registers on the thread's own stack,
 * with the thread's rights; see DESIGN.md, "Architectures".
 * This program points its sp just past the kernel's first bytes, which no region of its reaches, and makes a call:
 * the core's stacking must fault, a MemManage, MSTKERR, which names no address,
 * and the kernel must drop the call and stop the thread with the registers its last trap left,
 * whose pc is the return from the call just before, since a thread alone takes no tick between.
 * Were the stacking allowed, the kernel could not give the frame back there, see frame_give,
 * and the fault would be the unstacking's;
 * were the kernel to read a frame the core never wrote, the pc would be another; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "rvuos.h"

/* Where the kernel starts; see user/user.ld.S. */
extern const char __kernel_start[];

/* Where the call that ends the line below returns, labelled in the statement that makes it, so the label cannot drift. */
extern const char last_at[];

/* The hardware frame: r0 to r3, r12, lr, pc and xpsr. */
#define HW_FRAME 32u

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    uint32_t sp = (uint32_t)(uintptr_t)__kernel_start + HW_FRAME;
    puts("escape: calling with sp ");
    rv_put_hex(BOOT_CAP_DEBUG, sp);
    puts(" in the kernel, after the call that returns at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)last_at);
    puts(", expecting a fault");

    /* The line's end, OP_DEBUG_PUTC, and the same call again from the kernel's memory; nothing goes on after it. */
    register uint32_t r_a0 __asm__(RV_A0) = BOOT_CAP_DEBUG;
    register uint32_t r_a1 __asm__(RV_A1) = '\n';
    register uint32_t r_a7 __asm__(RV_A7) = OP_DEBUG_PUTC;
    __asm__ volatile(RV_CALL "\n.globl last_at\nlast_at:\n\tmov sp, %3\n\t" RV_CALL "\n\tudf #0"
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a7), "r"(sp)
                     : "memory");
    return 0;
}
