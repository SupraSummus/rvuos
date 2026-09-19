#ifndef RVUOS_UART_H
#define RVUOS_UART_H

/*
 * The UART of the QEMU virt board: a 16550 at UART_BASE, raising UART_IRQ.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE 0x10000000u
#define UART_SIZE 0x100u
#define UART_IRQ  10

#endif
