#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * nRF52840: a Cortex-M4 with an eight-region MPU, 1 MiB of flash and 256 KiB of RAM.
 * Where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * Nothing is written to flash: tools/nrf52840-run.py loads the image into RAM over SWD and starts the core at _start,
 * which sets its own stack, and arch_init the vector table.
 * The console takes the first 8 KiB, see UART_BASE, and the kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 * The chip has half RP2350's RAM, so the root task's code, data and free RAM are each half its.
 */
#define RAM_BASE       U32(0x20002000)
#define RAM_SIZE       U32(0x0003e000)
#define ROOT_RAM_BASE  U32(0x20010000)
#define ROOT_RAM_SIZE  U32(0x00010000)
#define USER_CODE_BASE U32(0x20010000)
#define USER_CODE_SIZE U32(0x00008000)
#define USER_DATA_BASE U32(0x20018000)
#define USER_DATA_SIZE U32(0x00004000)
/* Nothing loads replay input here yet; the region keeps the root task's grants the same. */
#define INPUT_BASE     U32(0x2001c000)
#define INPUT_SIZE     U32(0x00001000)
#define BOOT_POOL_BASE U32(0x2001d000)
#define FREE_RAM_BASE  U32(0x20020000)
#define FREE_RAM_SIZE  U32(0x00020000)

/* A misaligned store is checked for writing throughout; see the ESP32-C6's board.h. */
#define PMP_SPLIT_STORE_AS_READ 0

/* The MPU's smallest region, which it has no grain to probe for; see kernel/arch/arm/mpu.c. */
#define PMP_GRAIN_MIN U32(32)

/*
 * The console: a block of RAM below the kernel that the root task writes its output into,
 * and which the runner reads over SWD as the core runs, with no pin wired; user/board/nrf52840/console.h says how.
 * The block lies below RAM_BASE, so no Untyped covers it and it never becomes a pool.
 */
#define UART_BASE U32(0x20000000)
#define UART_SIZE U32(0x2000)

/*
 * The counter BOOT_CAP_CLOCK names: TIMER0 counting microseconds, 32 bits.
 * A TIMER's count is read only by capturing it into a CC register,
 * so TIMER1 raises an event every microsecond and a PPI channel captures TIMER0's count into CC[1] at each,
 * which user mode then reads as a counter; see timer.c.
 * It wraps every 71 minutes, and the kernel counts the wraps for its tick and for OP_CLOCK_READ.
 */
#define TIMER0_BASE  U32(0x40008000)
#define COUNTER_ADDR (TIMER0_BASE + U32(0x544))
#define COUNTER_HZ   U32(1000000)

/* SysTick runs on the processor's clock, 64 MHz; see kernel/arch/arm/systick.c. */
#define SYSTICK_PER_COUNT 64

/*
 * SysTick wakes wfe only while a debugger keeps the chip in its debug interface mode:
 * with the probe let go, a program blinking in wfe slowed to a blink every two seconds.
 * So the kernel's idle wait polls the counter instead; see intr_wait.
 */
#define SYSTICK_WAKES_WFE 0

/*
 * The devices the boot grants as frames from BOOT_CAP_DEVICES up, in this order,
 * devices a driver in user mode needs, none of them a bus master:
 * GPIO, both ports' registers, P0's at 0x50000000 and P1's at 0x50000300, in one 4 KiB block, read and write;
 * user/board/nrf52840/devices.h names their slots.
 * TIMER0, TIMER1 and the PPI are the kernel's, for the counter, and lie in none.
 */
#define BOOT_DEVICES 1
#define DEVICE_RANGE_LIST                                       \
    { U32(0x50000000), U32(0x00001000), RIGHT_R | RIGHT_W },

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * A line is a peripheral's ID, as the product specification numbers them, the last SPIM3's, 47.
 * Line 0, POWER_CLOCK, is shadowed by the kernel's log, see klog.h.
 */
#define IRQ_LINES 48

#endif
