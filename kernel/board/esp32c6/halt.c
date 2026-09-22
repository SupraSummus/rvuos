/*
 * How the ESP32-C6 halts, and what it does with the kernel's log then.
 *
 * The kernel has no console: kputc appends to the log
 * and a logger in user mode carries it out; see DESIGN.md, "The kernel log".
 * A halt here parks the core rather than reset it,
 * so the log stays in RAM for whoever looks next,
 * and it also writes what no logger has taken to the USB Serial/JTAG console,
 * headed by the same line QEMU's halt writes,
 * and then the code, which a simulator would have exited with,
 * so that tools/esp32c6-run.py can exit with it.
 * The console is polled and a host that takes nothing is given up on,
 * since nothing is left to wait for it.
 */

#include "csr.h"
#include "kernel.h"
#include "klog.h"

#define USJ_EP1      (*(volatile uint32_t *)(UART_BASE + 0x0u))
#define USJ_EP1_CONF (*(volatile uint32_t *)(UART_BASE + 0x4u))
#define USJ_WR_DONE  0x1u
#define USJ_FREE     0x2u

/* Polls before the console is given up on. */
#define USJ_POLLS 1000000u

static bool console_dead;

static void usj_flush(void)
{
    USJ_EP1_CONF = USJ_WR_DONE;
}

/* The log holds bare newlines and the terminal wants CR LF. */
static void usj_putc(char c)
{
    if (c == '\n') {
        usj_putc('\r');
    }
    for (uint32_t n = 0; !console_dead && (USJ_EP1_CONF & USJ_FREE) == 0; n++) {
        if (n == 0) {
            usj_flush();
        }
        if (n == USJ_POLLS) {
            console_dead = true;
        }
    }
    if (!console_dead) {
        USJ_EP1 = (uint8_t)c;
    }
}

static void usj_puts(const char *s)
{
    while (*s != '\0') {
        usj_putc(*s++);
    }
}

static void usj_put_hex(uint32_t v)
{
    usj_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        usj_putc("0123456789abcdef"[(v >> shift) & 0xfu]);
    }
}

void khalt(int code)
{
    usj_puts("\nrvuos: halting, the log follows\n");
    klog_dump(usj_putc);
    usj_puts("rvuos: halted with code ");
    usj_put_hex((uint32_t)code);
    usj_putc('\n');
    usj_flush();

    /* Nothing may wake the core: every interrupt off, and wfi in case one is pending anyway. */
    csr_write(mie, 0);
    for (;;) {
        __asm__ volatile("wfi");
    }
}
