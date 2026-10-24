/*
 * Escape attempt: a store to the console's registers, through no frame.
 *
 * The root task holds the console's frame, BOOT_CAP_UART, but installs no region for it,
 * so the store must fault, a store access fault, mcause=7,
 * naming the store's own address in mepc and the console's in mtval,
 * and on ARM a MemManage, DACCVIOL, naming them in pc and MMFAR.
 * On RP2350 the console is the USB controller, which the board opens to user mode,
 * and which Hazard3's hardwired PMP entries open too unless the kernel fences them off.
 * Were the store allowed the program would say so, which fails the run; see tests/escape.sh.
 *
 * One escape per program: the fault stops the only thread, and the machine with nothing left to run;
 * see DESIGN.md, "Verification".
 */

#include <stdint.h>

#include "escape.h"
#include "rvuos.h"

/* The store below; see escape.h. */
extern const char store_at[];

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_PRINT, s);
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        puts("escape: ");
        puts(what);
        puts(": FAILED\n");
        rv_halt(BOOT_CAP_HALT, 3);
    }
}

int main(void);
int main(void)
{
    uint32_t addr, size;
    expect("frame info", rv_frame_info(BOOT_CAP_UART, &addr, &size));
    puts("escape: storing to the console, with no region for it, to ");
    rv_put_hex(BOOT_CAP_PRINT, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_PRINT, (uint32_t)(uintptr_t)store_at);
    puts(", expecting a fault\n");

    ESCAPE_STORE("store_at", 0u, addr);

    /* Not reached when PMP keeps user mode off every device it holds no frame for. */
    puts("escape: breached, stored to the console\n");
    rv_halt(BOOT_CAP_HALT, 2);
    return 0;
}
