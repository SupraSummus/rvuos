#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * QEMU's mps2-an385, ARM's MPS2 board with the AN385 image: a Cortex-M3 with an eight-region MPU.
 * Where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * QEMU loads the image and resets the core, which takes its stack and entry
 * from the vector table at the bottom of SSRAM1, where the kernel lies.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of SSRAM1 is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 * The layout is QEMU virt's, moved to where this board has its RAM.
 */
#define RAM_BASE       U32(0x00000000)
#define RAM_SIZE       U32(0x00400000)
#define ROOT_RAM_BASE  U32(0x00100000)
#define ROOT_RAM_SIZE  U32(0x00040000)
#define USER_CODE_BASE U32(0x00100000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x00110000)
#define USER_DATA_SIZE U32(0x00010000)
/* Test input a loader may place here; nothing does on this board yet. */
#define INPUT_BASE     U32(0x00120000)
#define INPUT_SIZE     U32(0x00010000)
#define BOOT_POOL_BASE U32(0x00130000)
#define FREE_RAM_BASE  U32(0x00200000)
#define FREE_RAM_SIZE  U32(0x00200000)

/* A misaligned store is checked for writing throughout; see the ESP32-C6's board.h. */
#define PMP_SPLIT_STORE_AS_READ 0

/* The MPU's smallest region, which it has no grain to probe for; see kernel/arch/arm/mpu.c. */
#define PMP_GRAIN_MIN U32(32)

/*
 * The UART: UART0 of the CMSDK, at UART_BASE, raising line 1 as its transmitter drains.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x40004000)
#define UART_SIZE U32(0x1000)

/*
 * The counter BOOT_CAP_CLOCK names: the FPGA's COUNTER, 32 bits counting up at the 25 MHz system clock.
 * The word above it is the prescaler, which the kernel leaves at zero, so the high word reads zero
 * and the counter wraps every 171 seconds; the kernel counts the wraps for itself, see timer.c.
 */
#define FPGAIO_BASE  U32(0x40028000)
#define COUNTER_ADDR (FPGAIO_BASE + U32(0x18))
#define COUNTER_HZ   U32(25000000)

/* SysTick runs on the processor's clock, which is the system clock too; see kernel/arch/arm/systick.c. */
#define SYSTICK_PER_COUNT 1

/*
 * What OP_DEBUG_FRAME hands out: nothing.
 * The board's devices are the kernel's or granted at boot.
 */
#define DEBUG_RANGES 0
#define DEBUG_RANGE_LIST

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * The NVIC has 32 lines here, and line 0, UART0's receiver, is shadowed by the kernel's log, see klog.h.
 */
#define IRQ_LINES 32

#endif
