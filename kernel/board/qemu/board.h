#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * QEMU virt, RV32: where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * QEMU enters at RAM_BASE in machine mode.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's code, data and input regions follow,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 */
#define RAM_BASE       U32(0x80000000)
#define RAM_SIZE       U32(0x00800000)
#define USER_CODE_BASE U32(0x80100000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x80200000)
#define USER_DATA_SIZE U32(0x00010000)
/* Test input placed here by the loader; tests/differential.py knows the address. */
#define INPUT_BASE     U32(0x80210000)
#define INPUT_SIZE     U32(0x00010000)
#define FREE_RAM_BASE  U32(0x80400000)
#define FREE_RAM_SIZE  U32(0x00400000)

/*
 * The UART: a 16550 at UART_BASE, raising PLIC source 10.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x10000000)
#define UART_SIZE U32(0x100)

/* The counter BOOT_CAP_CLOCK names: the CLINT's mtime; mtimecmp is 0x7ff8 bytes below. */
#define COUNTER_ADDR U32(0x0200bff8)
#define COUNTER_HZ   U32(10000000)

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * The PLIC has 96 sources, and its source 0 does not exist:
 * line 0 is the kernel's log, see klog.h.
 */
#define IRQ_LINES 96

/* The mcause, and the mie and mip bit, of the core's interrupt from the controller. */
#define IRQ_EXT_CAUSE 11

#endif
