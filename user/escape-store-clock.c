/*
 * Escape attempt: a store to a device, the clock's counter, through no frame.
 *
 * OP_CLOCK_INFO tells the root task where the counter lies, but it installs no frame for it,
 * so the store must fault, a store access fault, mcause=7,
 * naming the store's own address in mepc and the counter's in mtval,
 * and on ARM a MemManage, DACCVIOL, naming them in pc and MMFAR.
 * The counter is the CLINT's mtime on QEMU and TIMER0's on RP2350,
 * which Hazard3's hardwired PMP entries open to user mode unless the kernel fences them off,
 * and the FPGA's on the MPS2 boards.
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
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void expect(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        puts("escape: ");
        puts(what);
        puts(": FAILED\n");
        rv_halt(BOOT_CAP_DEBUG, 3);
    }
}

int main(void);
int main(void)
{
    uint32_t hz, addr;
    expect("clock info", rv_clock_info(BOOT_CAP_CLOCK, &hz, &addr));
    puts("escape: storing to the clock's counter, with no frame for it, to ");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" at ");
    rv_put_hex(BOOT_CAP_DEBUG, (uint32_t)(uintptr_t)store_at);
    puts(", expecting a fault\n");

    ESCAPE_STORE("store_at", 0u, addr);

    /* Not reached when PMP keeps user mode off every device it holds no frame for. */
    puts("escape: breached, stored to the clock's counter\n");
    rv_halt(BOOT_CAP_DEBUG, 2);
    return 0;
}
