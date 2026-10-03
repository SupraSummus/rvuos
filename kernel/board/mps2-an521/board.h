#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * QEMU's mps2-an521, ARM's MPS2 board with the AN521 image:
 * an SSE-200 with two Cortex-M33s, each with an eight-region PMSAv8 MPU, its own NVIC and its own SysTick.
 * Where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * The kernel and every thread run Secure, see kernel/arch/arm/trap.c,
 * so every address here is a Secure alias, with bit 28 set.
 * QEMU loads the image and resets the first core, which takes its stack and entry
 * from the vector table at the bottom of SSRAM1, where INITSVTOR0 points at reset and the kernel lies;
 * the second core waits in the system control block's CPUWAIT.
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of SSRAM1 is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 * The layout is mps2-an385's, moved to SSRAM1's Secure alias.
 */
#define RAM_BASE       U32(0x10000000)
#define RAM_SIZE       U32(0x00400000)
#define ROOT_RAM_BASE  U32(0x10100000)
#define ROOT_RAM_SIZE  U32(0x00040000)
#define USER_CODE_BASE U32(0x10100000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x10110000)
#define USER_DATA_SIZE U32(0x00010000)
/* Test input a loader may place here; nothing does on this board yet. */
#define INPUT_BASE     U32(0x10120000)
#define INPUT_SIZE     U32(0x00010000)
#define BOOT_POOL_BASE U32(0x10130000)
#define FREE_RAM_BASE  U32(0x10200000)
#define FREE_RAM_SIZE  U32(0x00200000)

/* A misaligned store is checked for writing throughout; see the ESP32-C6's board.h. */
#define PMP_SPLIT_STORE_AS_READ 0

/* The MPU's smallest region, which it has no grain to probe for; see kernel/arch/arm/mpu.c. */
#define PMP_GRAIN_MIN U32(32)

/*
 * The UART: UART0 of the CMSDK, at UART_BASE, raising its combined line, 42, as its transmitter drains.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x50200000)
#define UART_SIZE U32(0x1000)

/*
 * The counter BOOT_CAP_CLOCK names: the FPGA's COUNTER, 32 bits counting up at the 20 MHz system clock.
 * The word above it is the prescaler, which the kernel leaves at zero, so the high word reads zero
 * and the counter wraps every 214 seconds; the kernel counts the wraps for itself, see timer.c.
 */
#define FPGAIO_BASE  U32(0x50302000)
#define COUNTER_ADDR (FPGAIO_BASE + U32(0x18))
#define COUNTER_HZ   U32(20000000)

/* SysTick runs on the processor's clock, which is the system clock too; see kernel/arch/arm/systick.c. */
#define SYSTICK_PER_COUNT 1

/*
 * The cores, for a kernel built for both, see board.c:
 * the SSE-200's CPU_IDENTITY, whose first word reads each core's own number,
 * the first message handling unit, MHU0, through which one core raises IPI_LINE on another's NVIC,
 * and the system control block, whose INITSVTOR1 and CPUWAIT start the second core.
 */
#define CORE_ID_ADDR U32(0x5001f000)
#define MHU0_BASE    U32(0x50003000)
#define SYSCTL_BASE  U32(0x50021000)
#define IPI_LINE     6

/*
 * The devices the boot grants as frames from BOOT_CAP_DEVICES up: none.
 * The board's devices are the kernel's or the console.
 */
#define BOOT_DEVICES 0
#define DEVICE_RANGE_LIST

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * The NVIC has 124 lines here, the SSE-200's 32 and the board's 92 after them;
 * the kernel takes the first 48, which hold every UART's, so its vector table stays 256 bytes.
 * Line 0, the Non-secure watchdog's reset, is shadowed by the kernel's log, see klog.h,
 * and with both cores line 6, MHU0's, is the kernel's, for the cores to interrupt each other.
 */
#define IRQ_LINES 48

#endif
