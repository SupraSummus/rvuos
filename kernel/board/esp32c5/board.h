#ifndef RVUOS_BOARD_H
#define RVUOS_BOARD_H

/*
 * ESP32-C5: where RAM, flash and the root task lie, and which devices it has.
 * Included through layout.h, which the linker scripts read as well.
 *
 * The ROM boots from flash: it loads the kernel, behind Espressif's header at FLASH_BOOT in flash, into SRAM
 * and enters it at RAM_BASE in machine mode; see DESIGN.md, "Boards".
 * The 384 KiB of HP SRAM answer at one address for code and data.
 * The kernel owns [RAM_BASE, KLOG_BASE) and its log above it.
 * The root task's memory follows, one block holding its data region and the boot pool its kernel objects live in;
 * its code and input lie in flash, see FLASH_IMAGE_BASE.
 * The top third of RAM is handed to the root task as free RAM, and the third below it as a further block.
 * Each is a block aligned to its own size.
 */
#define RAM_BASE       U32(0x40800000)
#define RAM_SIZE       U32(0x00060000)
#define KLOG_BASE      U32(0x4080F000)
#define ROOT_RAM_BASE  U32(0x40810000)
#define ROOT_RAM_SIZE  U32(0x00010000)
#define USER_DATA_BASE U32(0x40810000)
#define USER_DATA_SIZE U32(0x00008000)
#define BOOT_POOL_BASE U32(0x40818000)
#define FREE_RAM_BASE  U32(0x40840000)
#define FREE_RAM_SIZE  U32(0x00020000)
/* The further block of RAM, which the boot grants as an Untyped from BOOT_CAP_RAM_BLOCKS up; see boot.c. */
#define BOOT_RAM_BLOCKS 1
#define RAM_BLOCK_LIST { U32(0x40820000), U32(0x00020000), RIGHT_ALL },

/*
 * The flash, 4 MiB in the ESP32-C5HF4's package, whose first MiB the MMU shows page for page from FLASH_IMAGE_BASE.
 * It holds the image: at FLASH_BOOT the kernel, which the ROM loads into RAM, behind Espressif's header,
 * the ROM looking there and not at the flash's start, as ESP-IDF's bootloader lies on this chip;
 * from USER_CODE_FLASH the root task's code region, read and execute, which runs from flash,
 * and at INPUT_FLASH its input region, read only, which tools/esp32c6-run.py writes a configuration into.
 * The MMU maps 64 KiB pages, which the kernel's board_init sets before the root task runs; see board.c.
 * A program that would rather run from RAM copies itself there; the kernel runs from RAM, as on the ESP32-C6.
 */
#define FLASH_BOOT        U32(0x00002000)
#define FLASH_IMAGE_BASE  U32(0x42800000)
#define FLASH_IMAGE_SIZE  U32(0x00100000)
#define USER_CODE_FLASH   U32(0x00040000)
#define USER_CODE_BASE    (FLASH_IMAGE_BASE + USER_CODE_FLASH)
#define USER_CODE_SIZE    U32(0x00040000)
#define INPUT_FLASH       U32(0x00080000)
#define INPUT_BASE        (FLASH_IMAGE_BASE + INPUT_FLASH)
#define INPUT_SIZE        U32(0x00001000)

/* Set as on the ESP32-C6, erratum DIG-694, until a test shows whether this core checks a misaligned access's halves apart. */
#define PMP_SPLIT_STORE_AS_READ 1

/* Every pmpcfg field lies as the privileged specification lays it out; see RP2350's board.h. */
#define PMP_CFG_RX_TRANSPOSED 0

/* The probe finds the grain, 128 bytes, so the board adds nothing to it; see RP2350's board.h. */
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
 * The machine timer, the mtime and mtimecmp of the CLINT at 0x20000000, laid out as SiFive's; see clint.h.
 * timer.c starts it with a control word beside mtimecmp.
 */
#define CLINT_MTIME    U32(0x2000BFF8)
#define CLINT_MTIMECMP U32(0x20004000)

/*
 * The counter BOOT_CAP_CLOCK names: mtime, which counts the CPU's cycles at the clock timer_init measures.
 * The core has no copy of it for user mode, and refuses user mode the CLINT,
 * so a thread that maps the counter's frame faults on its load; see DESIGN.md, open decision 41.
 */
#define COUNTER_ADDR CLINT_MTIME

/* The CLIC's word for each interrupt, from interrupt 0; see clic.h. */
#define CLIC_CTRL U32(0x20801000)

/*
 * The devices the boot grants as frames from BOOT_CAP_DEVICES up, in this order;
 * user/board/esp32c5/devices.h names their slots.
 * The eFuse, read only so that no program burns a fuse, and short of the OTP debug registers above it,
 * the ROM, read and execute,
 * and the IO MUX and the GPIO matrix, the pins, for a program to set as its board wires them,
 * the flash's among them, so that whoever holds the IO MUX can cut the flash off.
 */
#define BOOT_DEVICES 4
#define DEVICE_RANGE_LIST                                       \
    { U32(0x600B4800), U32(0x00000400), RIGHT_R },             \
    { U32(0x40000000), U32(0x00080000), RIGHT_R | RIGHT_X },    \
    { U32(0x60090000), U32(0x00001000), RIGHT_R | RIGHT_W },    \
    { U32(0x60091000), U32(0x00001000), RIGHT_R | RIGHT_W },

/*
 * Interrupt line identifiers lie below IRQ_LINES; see irq.h.
 * A line is an interrupt matrix source, numbered as in Espressif's soc/interrupts.h, plus one;
 * the chip has 85 sources.
 * Line 0 is the kernel's log, see klog.h, so source 0, the Wi-Fi MAC's, is line 1.
 */
#define IRQ_LINES 86

/*
 * The CPU interrupt every source is routed to while it is unmasked, and so its mcause:
 * the CLIC numbers the matrix's 32 interrupts from 16, above the core's own,
 * and rvuos takes the matrix's interrupt 2, as on the ESP32-C6.
 */
#define IRQ_EXT_CAUSE 18

#endif
