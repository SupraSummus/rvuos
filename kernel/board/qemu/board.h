#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * QEMU virt, RV32: where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * QEMU enters at RAM_BASE in machine mode.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's code and data regions follow,
 * and everything after them up to the input is handed to the root task as free RAM.
 */
#define RAM_BASE       U32(0x80000000)
#define RAM_SIZE       U32(0x00800000)
#define USER_CODE_BASE U32(0x80100000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x80200000)
#define USER_DATA_SIZE U32(0x00010000)
#define INPUT_SIZE     U32(0x00010000)

/*
 * The UART: a 16550 at UART_BASE, raising PLIC source 10.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x10000000)
#define UART_SIZE U32(0x100)

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * The PLIC has 96 sources, and its source 0 does not exist:
 * line 0 is the kernel's log, see klog.h.
 */
#define IRQ_LINES 96

/* The mcause, and the mie and mip bit, of the core's interrupt from the controller. */
#define IRQ_EXT_CAUSE 11

#endif
