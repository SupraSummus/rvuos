#ifndef RVUOS_USER_CONSOLE_H
#define RVUOS_USER_CONSOLE_H

/*
 * The console of QEMU virt: a 16550 UART behind BOOT_CAP_UART, on line 10,
 * and a line nothing drives, the RTC's.
 * The registers come from BOOT_CAP_UART; the line numbers are the board's.
 * Each board's console.h offers the same functions, for user/init.c and user/fuzzdrv.c.
 */

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_IRQ 10
#define SPARE_IRQ   11

#define UART_THR 0 /* transmit holding register */
#define UART_IER 1 /* interrupt enable register */
#define UART_IIR 2 /* interrupt identification register */
#define UART_LSR 5 /* line status register */
#define UART_IER_THRE 0x2u /* interrupt when the transmit holding register empties */
#define UART_IIR_ID   0xfu
#define UART_IIR_THRE 0x2u
#define UART_LSR_THRE 0x20u

static volatile uint8_t *console_regs;

/* Where the registers are mapped. */
static inline void console_init(uint32_t base)
{
    console_regs = (volatile uint8_t *)base;
}

/* One byte out, waiting for the transmitter by polling. */
static inline void console_put_polled(char c)
{
    while ((console_regs[UART_LSR] & UART_LSR_THRE) == 0) {
    }
    console_regs[UART_THR] = (uint8_t)c;
}

/* Hand what was written to the wire; the 16550 keeps nothing back. */
static inline void console_flush(void)
{
}

/* From here the transmitter raises the line whenever it is empty. */
static inline void console_start(void)
{
    console_regs[UART_IER] = UART_IER_THRE;
}

/*
 * One byte out on the transmitter's interrupt.
 * `wait` arms the console's Irq and returns when it has signalled.
 * The transmitter raises the line whenever it is empty, once started,
 * so each byte waits for that interrupt:
 * wait, read the identification register, which lowers the line
 * and must say it was the transmitter, and write the byte.
 * False if the interrupt was for something else.
 */
static inline bool console_put(char c, void (*wait)(void))
{
    wait();
    if ((console_regs[UART_IIR] & UART_IIR_ID) != UART_IIR_THRE) {
        return false;
    }
    console_regs[UART_THR] = (uint8_t)c;
    return true;
}

#endif
