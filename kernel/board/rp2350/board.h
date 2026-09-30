#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * RP2350, on its Hazard3 cores or with ARCH=arm on its Cortex-M33 ones:
 * where RAM and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 * What only one kind of core reads is said so; the rest is the chip's, and the same for both.
 *
 * The 512 KiB of striped SRAM, SRAM0 to SRAM7, answer at one address for code and data.
 * The bootrom's BOOTSEL mode takes the image's segments into it over USB,
 * then reboots into it, finding it by the block at RAM_BASE; see image.S and DESIGN.md, "Boards".
 * The kernel owns [RAM_BASE, USER_CODE_BASE).
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused.
 */
#define RAM_BASE       U32(0x20000000)
#define RAM_SIZE       U32(0x00080000)
#define ROOT_RAM_BASE  U32(0x20020000)
#define ROOT_RAM_SIZE  U32(0x00020000)
#define USER_CODE_BASE U32(0x20020000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x20030000)
#define USER_DATA_SIZE U32(0x00008000)
/* Nothing loads replay input here yet; the region keeps the root task's grants the same. */
#define INPUT_BASE     U32(0x20038000)
#define INPUT_SIZE     U32(0x00001000)
#define BOOT_POOL_BASE U32(0x20039000)
#define FREE_RAM_BASE  U32(0x20040000)
#define FREE_RAM_SIZE  U32(0x00040000)

/*
 * Hazard3 does not split a misaligned access: it raises a misaligned exception instead.
 * The Cortex-M33 takes the flag off as mps2-an385's core does; nothing has checked a misaligned store on it yet.
 */
#define PMP_SPLIT_STORE_AS_READ 0

/*
 * Hazard3's: the R and X bits of every pmpcfg field are transposed, erratum RP2350-E6;
 * pmp.c swaps them on the way to the CSRs and back.
 */
#define PMP_CFG_RX_TRANSPOSED 1

/*
 * Hazard3's PMP decodes no block smaller than 32 bytes, but its address registers do not show it,
 * so the probe finds a grain of four; pmp.c takes the grain as no finer than this.
 * user/escape-past-region.c reads past an 8-byte frame without it.
 * The Cortex-M33's MPU has no region smaller either; see kernel/arch/arm/mpu.c.
 */
#define PMP_GRAIN_MIN U32(32)

/*
 * The console: a block of RAM, SRAM8 and SRAM9, that the root task writes its output into
 * and the kernel sends to the host over USB when it halts; see halt.c.
 * The chip has no USB serial port of its own, as the ESP32-C6 has,
 * and a driver for its USB controller would be the root task's to write.
 * The block lies above RAM_SIZE, so no Untyped covers it and it never becomes a pool.
 * user/board/rp2350/console.h says how the bytes lie in it.
 */
#define UART_BASE U32(0x20080000)
#define UART_SIZE U32(0x2000)

/*
 * Hazard3's machine timer: the RISC-V platform timer in SIO, mtime and core 0's mtimecmp,
 * counting the ticks of the tick generator riscv/timer.c starts; see clint.h.
 * It lies only in the Secure bank of SIO, which user mode does not reach.
 */
#define CLINT_MTIME    U32(0xd00001b0)
#define CLINT_MTIMECMP U32(0xd00001b8)

/*
 * The counter BOOT_CAP_CLOCK names: TIMER0's TIMERAWL, which counts the same microseconds as mtime,
 * and which user mode reaches once board.c opens TIMER0 to it.
 * TIMER0 keeps the high half below the low one, at TIMERAWH,
 * so the word above TIMERAWL, which rv_counter_read takes for the high half, is DBGPAUSE;
 * see TODO.md.
 * On the Cortex-M33 it is the kernel's counter too, see arm/timer.c.
 */
#define COUNTER_ADDR U32(0x400b0028)
#define COUNTER_HZ   U32(1000000)

/*
 * The Cortex-M33's: SysTick runs on the processor's clock, clk_sys, 150 MHz from the crystal the counter's
 * microseconds come from too, so 150 of its counts make one of the counter's; see kernel/arch/arm/systick.c.
 */
#define SYSTICK_PER_COUNT 150

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * A line is a system IRQ, numbered as in the datasheet's table of them; the chip has 52,
 * each the same line of Hazard3's controller and of the Cortex-M33's NVIC.
 * Line 0 is the kernel's log, see klog.h, so TIMER0_IRQ_0 is out of reach.
 */
#define IRQ_LINES 52

/* Hazard3's: the mcause, and the mie and mip bit, of the core's interrupt from its controller. */
#define IRQ_EXT_CAUSE 11

#endif
