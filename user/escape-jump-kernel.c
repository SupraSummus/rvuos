/*
 * Escape attempt: jump into the kernel.
 *
 * No region of the root task's reaches the kernel, see user/user.ld.S,
 * so a call to where the kernel starts must fault on the fetch, an instruction access fault, mcause=1,
 * naming the address in mepc and mtval, and on ARM a MemManage, IACCVIOL, naming it in pc.
 * Were the fetch allowed, the kernel's first instructions would run in user mode
 * and the fault would be another; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "escape.h"
#include "rvuos.h"

/* Where the kernel starts; see user/user.ld.S. */
extern const char __kernel_start[];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    uint32_t at = (uint32_t)(uintptr_t)__kernel_start;
    puts("escape: jumping into the kernel at ");
    rv_put_hex(BOOT_CAP_DEBUG, at);
    puts(", expecting a fault\n");

    ESCAPE_CODE(at)();

    /* Not reached when PMP keeps user mode out of the kernel. */
    puts("escape: breached, returned from the kernel\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
