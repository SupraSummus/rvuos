/*
 * 16550-compatible UART as found on QEMU virt at 0x10000000.
 * Output only, polled.
 */

#include <stdint.h>

#include "uart.h"

#define UART_BASE 0x10000000u

#define UART_THR 0 /* transmit holding register */
#define UART_LSR 5 /* line status register */
#define UART_LSR_THRE (1u << 5)

static volatile uint8_t *const uart = (volatile uint8_t *)UART_BASE;

void uart_putc(char c)
{
    while ((uart[UART_LSR] & UART_LSR_THRE) == 0) {
    }
    uart[UART_THR] = (uint8_t)c;
}
