#ifndef RVUOS_UART_H
#define RVUOS_UART_H

/*
 * The console UART of the QEMU virt board: a 16550 at UART_BASE, raising UART_IRQ.
 * The kernel's console polls its transmitter and touches nothing else,
 * and the same registers are granted to the root task as BOOT_CAP_UART;
 * see DESIGN.md, "Boot".
 */
#define UART_BASE 0x10000000u
#define UART_SIZE 0x100u
#define UART_IRQ  10

void uart_putc(char c);

#endif
