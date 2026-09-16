/*
 * Console helpers and machine halt.
 */

#include "kernel.h"
#include "uart.h"

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

void kputc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    uart_putc(c);
}

void kputs(const char *s)
{
    while (*s != '\0') {
        kputc(*s++);
    }
}

void kput_hex(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    kputs("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        kputc(digits[(v >> shift) & 0xf]);
    }
}

void khalt(int code)
{
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

void selfcheck_fail(void)
{
    khalt(3);
}

void kpanic(const char *msg)
{
    kputs("kernel panic: ");
    kputs(msg);
    kputc('\n');
    khalt(1);
}
