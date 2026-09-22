/*
 * How QEMU virt halts, and what it does with the kernel's log then.
 *
 * The kernel has no console: kputc appends to the log
 * and a logger in user mode carries it out; see DESIGN.md, "The kernel log".
 * This board is a simulator, which exits on a halt and keeps no RAM
 * for a post-mortem reader to find the log in,
 * so its halt is that reader: it writes what no logger has taken to the UART,
 * headed by a line that tells it apart from what a logger sent before.
 */

#include "kernel.h"
#include "klog.h"

/*
 * QEMU virt exposes the SiFive test device at 0x100000.
 * Writing FINISHER_PASS exits with status 0;
 * writing (code << 16) | FINISHER_FAIL exits with that code.
 * Codes: 1 kernel panic, 3 invariant violated, 4 user fault,
 * 5 no runnable thread, 6 a thread the host build cannot follow,
 * anything else is what the root task asked for.
 */
#define SIFIVE_TEST_BASE 0x100000u
#define FINISHER_FAIL 0x3333u
#define FINISHER_PASS 0x5555u

/* The 16550's transmitter, polled; the log holds bare newlines and the terminal wants CR LF. */
#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE (1u << 5)

static void uart_putc(char c)
{
    volatile uint8_t *uart = (volatile uint8_t *)UART_BASE;
    if (c == '\n') {
        uart_putc('\r');
    }
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
    }
    uart[UART_THR] = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

void khalt(int code)
{
    uart_puts("\nrvuos: halting, the log follows\n");
    klog_dump(uart_putc);

    volatile uint32_t *finisher = (volatile uint32_t *)SIFIVE_TEST_BASE;
    if (code == 0) {
        *finisher = FINISHER_PASS;
    } else {
        *finisher = ((uint32_t)code << 16) | FINISHER_FAIL;
    }
    /* Not under QEMU: park the hart. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
