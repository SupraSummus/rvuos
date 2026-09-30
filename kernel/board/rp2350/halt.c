/*
 * How RP2350 halts, and what it does with the kernel's log then.
 *
 * The chip has no serial port of its own on USB, so the logger's console is a block of RAM, see board.h,
 * and the halt is what brings the words to the host; see DESIGN.md, "The kernel log".
 * It makes a USB serial port of the chip's controller, see cdc.c, and writes to it
 * what the logger put in the console, then what no logger took from the log under the line QEMU's halt writes,
 * then the code a simulator would have exited with, for tools/rp2350-run.py to exit with.
 * Once the host lets the port go, the chip reboots into BOOTSEL for the next image,
 * which wipes RAM, the log with it; see bootsel.h.
 */

#include <stdint.h>

#include "bootsel.h"
#include "cdc.h"
#include "csr.h"
#include "kernel.h"
#include "klog.h"
#include "layout.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/*
 * The console, as user/board/rp2350/console.h writes it:
 * the count of every byte the root task wrote, then the bytes, as many as fit.
 */
#define CONSOLE_HEAD  UART_BASE
#define CONSOLE_BYTES (UART_BASE + 16u)
#define CONSOLE_ROOM  (UART_SIZE - 16u)

static void put_hex(uint32_t v)
{
    cdc_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        cdc_putc("0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

void khalt(int code)
{
    /* Nothing may interrupt the halt: every interrupt off, and machine mode leaves MIE clear. */
    csr_write(mie, 0);
    /* The host has the watchdog's whole count to take the log; one that never comes is not waited for. */
    bootsel_arm(WATCHDOG_LONGEST);

    cdc_start();
    uint32_t written = REG(CONSOLE_HEAD);
    uint32_t kept = written < CONSOLE_ROOM ? written : CONSOLE_ROOM;
    for (uint32_t i = 0; i < kept; i++) {
        cdc_put_raw(*(volatile char *)(CONSOLE_BYTES + i));
    }
    if (kept < written) {
        cdc_puts("\nrvuos: the console lost ");
        put_hex(written - kept);
        cdc_puts(" bytes\n");
    }

    cdc_puts("\nrvuos: halting, the log follows\n");
    klog_dump(cdc_putc);
    cdc_puts("rvuos: halted with code ");
    put_hex((uint32_t)code);
    cdc_putc('\n');
    cdc_flush();
    cdc_wait_closed();
    bootsel_now();
}
