#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * ESP32-C6: where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * The 512 KiB of HP SRAM answer at one address for code and data.
 * The ROM loads the image's segments into it and enters at RAM_BASE in machine mode;
 * see DESIGN.md, "Boards".
 * While it loads, the ROM keeps its own buffers, stack and data from 0x4086ad08 up,
 * so everything the image carries must lie below that;
 * the kernel takes the whole of RAM once it runs.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's code, data and input regions follow,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 */
#define RAM_BASE       U32(0x40800000)
#define RAM_SIZE       U32(0x00080000)
#define USER_CODE_BASE U32(0x40820000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x40830000)
#define USER_DATA_SIZE U32(0x00008000)
/* Nothing loads replay input here yet; the region keeps the root task's grants the same. */
#define INPUT_BASE     U32(0x40838000)
#define INPUT_SIZE     U32(0x00001000)
#define FREE_RAM_BASE  U32(0x40840000)
#define FREE_RAM_SIZE  U32(0x00040000)

/*
 * The console: the USB Serial/JTAG controller,
 * which the host sees as a CDC-ACM serial port on the chip's own USB connector.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x6000F000)
#define UART_SIZE U32(0x100)

/*
 * The counter BOOT_CAP_CLOCK names: UTIME, the CLINT's read-only copy of mtime for user mode,
 * at the CPU clock timer_init measures; ESP32-C6 TRM, "Timer Counter and Interrupt".
 * The core has no time CSR. Untried on the chip; see TODO.md.
 */
#define COUNTER_ADDR U32(0x20001c08)

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * A line is an interrupt matrix source, numbered as in Espressif's soc/interrupts.h;
 * the chip has 77 of them.
 * Line 0 is the kernel's log, see klog.h, so source 0, the Wi-Fi MAC's, is out of reach.
 */
#define IRQ_LINES 77

/*
 * The CPU interrupt every source is routed to while it is unmasked,
 * and so the mcause, and the mie and mip bit, of the core's interrupt from the matrix.
 * Espressif reserves 1 for Wi-Fi and 3, 4 and 7 for the CLINT, and 6 is never raised.
 */
#define IRQ_EXT_CAUSE 2

#endif
