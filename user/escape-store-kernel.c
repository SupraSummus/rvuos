/*
 * Escape attempt: a store into the kernel.
 *
 * No region of the root task's reaches the kernel, see user/user.ld.S,
 * so a store to the kernel's first word must fault, a store access fault, mcause=7,
 * naming the store's own address in mepc and the word's in mtval,
 * and on ARM a MemManage, DACCVIOL, naming them in pc and MMFAR.
 * Were the store allowed the program would say so, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "escape.h"
#include "rvuos.h"

/* Where the kernel starts; see user/user.ld.S. */
extern const char __kernel_start[];

/* The store below; see escape.h. */
extern const char store_at[];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

int main(void);
int main(void)
{
    uint32_t addr = (uint32_t)(uintptr_t)__kernel_start;
    puts("escape: storing into the kernel to ");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)store_at);
    puts(", expecting a fault\n");

    ESCAPE_STORE("store_at", 0u, addr);

    /* Not reached when PMP keeps user mode out of the kernel. */
    puts("escape: breached, stored into the kernel\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
