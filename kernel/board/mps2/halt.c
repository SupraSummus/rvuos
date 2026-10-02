/*
 * How the MPS2 boards halt under QEMU, and what they do with the kernel's log then.
 *
 * The kernel has no console: kputc appends to the log
 * and a logger in user mode carries it out; see DESIGN.md, "The kernel log".
 * The boards are a simulator, which exits on a halt and keeps no RAM
 * for a post-mortem reader to find the log in,
 * so their halt is that reader: it writes what no logger has taken to the UART,
 * headed by a line that tells it apart from what a logger sent before.
 */

#include "kernel.h"
#include "klog.h"

/* The CMSDK UART's registers, as words. */
#define UART_DATA    0
#define UART_STATE   1
#define UART_CTRL    2
#define UART_BAUDDIV 4
#define UART_STATE_TXFULL 0x1u
#define UART_CTRL_TXEN    0x1u

/*
 * Semihosting: QEMU exits with the code SYS_EXIT_EXTENDED gives it, when run with semihosting on.
 * Codes: 1 kernel panic, 3 invariant violated,
 * 5 no runnable thread, 6 a thread the host build cannot follow,
 * anything else is what the root task asked for; a user fault stops its thread and not the machine.
 * Only privileged code reaches it; a bkpt 0xab from user mode is a breakpoint like any other.
 */
#define SYS_EXIT_EXTENDED 0x20u
#define ADP_STOPPED_APPLICATION_EXIT 0x20026u

static volatile uint32_t *const uart = (volatile uint32_t *)UART_BASE;

static void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    while (uart[UART_STATE] & UART_STATE_TXFULL) {
    }
    uart[UART_DATA] = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

void khalt(int code)
{
    if (uart[UART_BAUDDIV] < 16) {
        uart[UART_BAUDDIV] = 16;
    }
    uart[UART_CTRL] |= UART_CTRL_TXEN;
    uart_puts("\nrvuos: halting, the log follows\n");
    klog_dump(uart_putc);

    uint32_t block[2] = { ADP_STOPPED_APPLICATION_EXIT, (uint32_t)code };
    register uint32_t op __asm__("r0") = SYS_EXIT_EXTENDED;
    register uint32_t arg __asm__("r1") = (uint32_t)block;
    __asm__ volatile("bkpt 0xab" : : "r"(op), "r"(arg) : "memory");
    /* Not under QEMU: park the core. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
