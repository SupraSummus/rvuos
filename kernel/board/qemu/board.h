#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * QEMU virt, RV32: where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * QEMU enters at RAM_BASE in machine mode.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 */
#define RAM_BASE       U32(0x80000000)
#define RAM_SIZE       U32(0x00800000)
#define ROOT_RAM_BASE  U32(0x80100000)
#define ROOT_RAM_SIZE  U32(0x00040000)
#define USER_CODE_BASE U32(0x80100000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x80110000)
#define USER_DATA_SIZE U32(0x00010000)
/* Test input placed here by the loader; tests/differential.py knows the address. */
#define INPUT_BASE     U32(0x80120000)
#define INPUT_SIZE     U32(0x00010000)
#define BOOT_POOL_BASE U32(0x80130000)
#define FREE_RAM_BASE  U32(0x80400000)
#define FREE_RAM_SIZE  U32(0x00400000)

/* A misaligned store is checked for writing throughout; see the ESP32-C6's board.h. */
#define PMP_SPLIT_STORE_AS_READ 0

/* Every pmpcfg field lies as the privileged specification lays it out; see RP2350's board.h. */
#define PMP_CFG_RX_TRANSPOSED 0

/* The probe finds the grain, so the board adds nothing to it; see RP2350's board.h. */
#define PMP_GRAIN_MIN U32(4)

/*
 * The UART: a 16550 at UART_BASE, raising PLIC source 10.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x10000000)
#define UART_SIZE U32(0x100)

/*
 * The machine timer, a SiFive CLINT's mtime and hart 0's mtimecmp, with every other hart's after it,
 * and hart 0's software interrupt, the one another hart raises, likewise; see clint.h.
 */
#define CLINT_MTIME    U32(0x0200bff8)
#define CLINT_MTIMECMP U32(0x02004000)
#define CLINT_MSIP     U32(0x02000000)

/* The counter BOOT_CAP_CLOCK names: the CLINT's mtime. */
#define COUNTER_ADDR CLINT_MTIME
#define COUNTER_HZ   U32(10000000)

/*
 * The devices the boot grants as frames from BOOT_CAP_DEVICES up: none.
 * QEMU's devices are the kernel's or the console, and the host build, which shares this board, has no hardware.
 */
#define BOOT_DEVICES 0
#define DEVICE_RANGE_LIST

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * The PLIC has 96 sources, and its source 0 does not exist:
 * line 0 is the kernel's log, see klog.h.
 */
#define IRQ_LINES 96

/* The mcause, and the mie and mip bit, of the core's interrupt from the controller. */
#define IRQ_EXT_CAUSE 11

#endif
