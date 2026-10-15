/*
 * How RP2350 halts, and what it does with the kernel's log then.
 *
 * The chip has no serial port of its own on USB, so the root task's console is a port it makes of the USB controller,
 * see user/board/rp2350/console.h, and the halt makes one of its own to bring the log to the host;
 * see DESIGN.md, "The kernel log".
 * It resets the controller, which takes the root task's port away, makes a USB serial port of it, see cdc.c,
 * and writes to it what no logger took from the log, under the line QEMU's halt writes,
 * then the code a simulator would have exited with, for tools/rp2350-run.py to exit with.
 * Once the host lets the port go, the chip reboots into BOOTSEL for the next image,
 * which wipes RAM, the log with it; see bootsel.h.
 */

#include <stdint.h>

#include "bootsel.h"
#include "cdc.h"
#include "kernel.h"
#include "klog.h"
#include "layout.h"
#ifdef __riscv
#include "csr.h"
#endif

static void put_hex(uint32_t v)
{
    cdc_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        cdc_putc("0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

void khalt(int code)
{
    /*
     * Nothing may interrupt the halt: on Hazard3 every interrupt off, and machine mode leaves MIE clear.
     * The Cortex-M33 already holds every exception off while the kernel runs.
     */
#ifdef __riscv
    csr_write(mie, 0);
#endif
    /* The host has the watchdog's whole count to take the log; one that never comes is not waited for. */
    bootsel_arm(WATCHDOG_LONGEST);

    cdc_start();
    cdc_puts("\nrvuos: halting, the log follows\n");
    klog_dump(cdc_putc);
    cdc_puts("rvuos: halted with code ");
    put_hex((uint32_t)code);
    cdc_putc('\n');
    cdc_flush();
    cdc_wait_closed();
    bootsel_now();
}
