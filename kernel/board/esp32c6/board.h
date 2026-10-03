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
 * The kernel owns [RAM_BASE, USER_CODE_BASE), but for the block below the root task's code.
 * The root task's memory follows, one block holding its code, data and input regions
 * and the boot pool its kernel objects live in,
 * and the upper half of RAM is handed to the root task as free RAM.
 * Each is a block aligned to its own size; what lies in none of them is left unused,
 * such as the blocks that keep the log off the code and the data off the input.
 */
#define RAM_BASE       U32(0x40800000)
#define RAM_SIZE       U32(0x00080000)
#define ROOT_RAM_BASE  U32(0x40820000)
#define ROOT_RAM_SIZE  U32(0x00020000)
#define USER_CODE_BASE U32(0x40820000)
#define USER_CODE_SIZE U32(0x00010000)
#define USER_DATA_BASE U32(0x40830000)
#define USER_DATA_SIZE U32(0x00008000)
/* Nothing loads replay input here yet; the region keeps the root task's grants the same. */
#define INPUT_BASE     U32(0x40839000)
#define INPUT_SIZE     U32(0x00001000)
#define BOOT_POOL_BASE U32(0x4083A000)
#define FREE_RAM_BASE  U32(0x40840000)
#define FREE_RAM_SIZE  U32(0x00040000)

/*
 * A misaligned store checks its second word for reading unless a store follows it,
 * so it writes into a read-only region that begins where a writable one ends.
 * The install keeps such regions apart; see DESIGN.md, "Boards".
 */
#define PMP_SPLIT_STORE_AS_READ 1

/*
 * A process keeps the performance counter's three CSRs, mpcer, mpcmr and mpccr, across other processes' turns,
 * which user mode writes as 0x800 to 0x802; see board_user_csrs_switch in board.c.
 */
#define BOARD_PROCESS_CSRS 3

/* Every pmpcfg field lies as the privileged specification lays it out; see RP2350's board.h. */
#define PMP_CFG_RX_TRANSPOSED 0

/* The probe finds the grain, so the board adds nothing to it; see RP2350's board.h. */
#define PMP_GRAIN_MIN U32(4)

/*
 * The console: the USB Serial/JTAG controller,
 * which the host sees as a CDC-ACM serial port on the chip's own USB connector.
 * It is the root task's, granted as BOOT_CAP_UART with its line;
 * the kernel writes it only from khalt, see halt.c.
 */
#define UART_BASE U32(0x6000F000)
#define UART_SIZE U32(0x100)

/*
 * The machine timer, the mtime and mtimecmp of Espressif's CLINT at 0x20001800; see clint.h.
 * timer.c starts it with a control word beside them.
 */
#define CLINT_MTIME    U32(0x20001808)
#define CLINT_MTIMECMP U32(0x20001810)

/*
 * The counter BOOT_CAP_CLOCK names: UTIME, the CLINT's read-only copy of mtime for user mode,
 * at the CPU clock timer_init measures; ESP32-C6 TRM, "Timer Counter and Interrupt".
 * The core has no time CSR. Untried on the chip; see TODO.md.
 */
#define COUNTER_ADDR U32(0x20001c08)

/*
 * The window onto flash: FLASH_WINDOW_SIZE bytes of it from FLASH_WINDOW_FLASH,
 * which board.c maps through the cache at FLASH_WINDOW_BASE,
 * for a program too large for SRAM, such as the Wi-Fi system's driver, to run from.
 * The flash must be attached when the kernel starts, as tools/esp32c6-run.py leaves it.
 * The window lies in the 1.9 MiB the boards' usual partition table gives to a file system, unused here.
 */
#define FLASH_WINDOW_BASE  U32(0x42000000)
#define FLASH_WINDOW_SIZE  U32(0x00100000)
#define FLASH_WINDOW_FLASH U32(0x00210000)

/*
 * The devices the boot grants as frames from BOOT_CAP_DEVICES up, in this order;
 * user/board/esp32c6/devices.h names their slots.
 * They are what a PHY driver drives, as tools/phymap.py finds in ESP-IDF's PHY library and the ROM:
 * the SAR ADC with the temperature sensor, which board.c takes out of reset,
 * the modem's front end, Bluetooth and Wi-Fi basebands, clock controls and analog I2C master,
 * and the eFuse, read only so that no program burns a fuse.
 * No bus master is listed: the Wi-Fi MAC from 0x600A4000 and the 802.15.4 MAC at 0x600A3000 lie outside,
 * as do the modem's pages no code names, where the Bluetooth controller may be,
 * but for the last KiB of the front end's frame.
 * PCR, PMU and the LP domain stay the kernel's: they reach the whole chip's clocks, power and resets.
 * The window onto flash comes last, read and execute.
 */
#define BOOT_DEVICES 9
#define DEVICE_RANGE_LIST                                       \
    { U32(0x6000E000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600A0000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600A2000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600A7000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600A8000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600A9800), U32(0x00000400), RIGHT_R | RIGHT_W },    \
    { U32(0x600AF000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x600B0800), U32(0x00000400), RIGHT_R },             \
    { FLASH_WINDOW_BASE, FLASH_WINDOW_SIZE, RIGHT_R | RIGHT_X },

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * A line is an interrupt matrix source, numbered as in Espressif's soc/interrupts.h, plus one;
 * the chip has 77 sources.
 * Line 0 is the kernel's log, see klog.h, so source 0, the Wi-Fi MAC's, is line 1.
 */
#define IRQ_LINES 78

/*
 * The CPU interrupt every source is routed to while it is unmasked,
 * and so the mcause, and the mie and mip bit, of the core's interrupt from the matrix.
 * Espressif reserves 1 for Wi-Fi and 3, 4 and 7 for the CLINT, and 6 is never raised.
 */
#define IRQ_EXT_CAUSE 2

#endif
