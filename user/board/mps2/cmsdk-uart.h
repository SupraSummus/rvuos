#ifndef RVUOS_USER_CMSDK_UART_H
#define RVUOS_USER_CMSDK_UART_H

/*
 * UART0 of the CMSDK, the console of both MPS2 boards behind BOOT_CAP_UART, whose registers that frame maps;
 * each board's console.h names its lines and includes this, so it offers what every board's does.
 *
 * The transmitter holds one byte and raises its interrupt as that byte leaves,
 * latched until the driver clears it, rather than for as long as it is empty,
 * so a byte waits for the interrupt only while the one before has not left.
 */

#include <stdbool.h>
#include <stdint.h>

/* Registers, as words. */
#define UART_DATA      0
#define UART_STATE     1
#define UART_CTRL      2
#define UART_INTSTATUS 3 /* written: clears what it names */
#define UART_BAUDDIV   4
#define UART_STATE_TXFULL  0x1u
#define UART_CTRL_TXEN     0x1u
#define UART_CTRL_TXINTEN  0x4u
#define UART_INT_TX        0x1u

static volatile uint32_t *console_regs;

/* Where the registers are mapped; the transmitter is enabled here, with the least divider it takes. */
static inline void console_init(uint32_t base)
{
    console_regs = (volatile uint32_t *)base;
    if (console_regs[UART_BAUDDIV] < 16) {
        console_regs[UART_BAUDDIV] = 16;
    }
    console_regs[UART_CTRL] |= UART_CTRL_TXEN;
}

/* One byte out, waiting for the transmitter by polling. */
static inline void console_put_polled(char c)
{
    while (console_regs[UART_STATE] & UART_STATE_TXFULL) {
    }
    console_regs[UART_DATA] = (uint8_t)c;
}

/* Hand what was written to the wire; the transmitter keeps nothing back. */
static inline void console_flush(void)
{
}

/* From here the transmitter raises the line each time a byte leaves. */
static inline void console_start(void)
{
    console_regs[UART_INTSTATUS] = UART_INT_TX;
    console_regs[UART_CTRL] |= UART_CTRL_TXINTEN;
}

/*
 * One byte out, waiting on the interrupt only while the transmitter is full.
 * `wait` arms the console's Irq and returns when it has signalled.
 * The interrupt is latched, so it is cleared after each wake and the transmitter looked at again:
 * one latched by an earlier byte wakes the wait at once, and the one this byte waits for comes after.
 * False if the interrupt was for something else.
 */
static inline bool console_put(char c, void (*wait)(void))
{
    while (console_regs[UART_STATE] & UART_STATE_TXFULL) {
        wait();
        if ((console_regs[UART_INTSTATUS] & UART_INT_TX) == 0) {
            return false;
        }
        console_regs[UART_INTSTATUS] = UART_INT_TX;
    }
    console_regs[UART_DATA] = (uint8_t)c;
    return true;
}

#endif
