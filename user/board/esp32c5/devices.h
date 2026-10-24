#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of the ESP32-C5 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists:
 * the eFuse's registers, read only, the ROM, read and execute,
 * the IO MUX and the GPIO matrix, read and write, the pins' functions, routes and levels,
 * SPI2, read and write, the general-purpose SPI controller, whose clocks the kernel sets: 80 MHz for its master clock,
 * and the first 128 bytes of LPPERI, read only, which hold the random number generator's registers.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 6
#define BOOT_CAP_EFUSE  (BOOT_CAP_DEVICES + 0) /* the eFuse's registers, read only, 1 KiB */
#define BOOT_CAP_ROM    (BOOT_CAP_DEVICES + 1) /* the ROM, read and execute, 512 KiB */
#define BOOT_CAP_IO_MUX (BOOT_CAP_DEVICES + 2) /* the IO MUX, the pins' functions and pads, 4 KiB */
#define BOOT_CAP_GPIO   (BOOT_CAP_DEVICES + 3) /* the GPIO matrix, the pins' routes and levels, 4 KiB */
#define BOOT_CAP_SPI2   (BOOT_CAP_DEVICES + 4) /* SPI2, the general-purpose SPI controller, 4 KiB */
#define BOOT_CAP_RNG    (BOOT_CAP_DEVICES + 5) /* LPPERI's first words, the random number generator's, read only, 128 bytes */

/* The further block of RAM the boot grants as an Untyped from BOOT_CAP_RAM_BLOCKS up, beside the free RAM. */
#define BOOT_RAM_BLOCKS 1
#define BOOT_CAP_MORE_RAM (BOOT_CAP_RAM_BLOCKS + 0) /* 128 KiB from 0x40820000, right below the free RAM */

/*
 * BOOT_CAP_DMA's channels: the GDMA's transmit channels, each reading memory, its source, into a device, its sink,
 * which a Dma names by the request the GDMA takes for it within the device's frame; see the kernel's dma.c.
 */
#define DMA_REQUEST_SPI2 1u

#define EFUSE_BASE  0x600B4800u
#define IO_MUX_BASE 0x60090000u
#define GPIO_BASE   0x60091000u
#define SPI2_BASE   0x60081000u
#define LPPERI_BASE 0x600B2800u

/*
 * In BOOT_CAP_RNG's frame: RNG_DATA_SYNC, a new random word at every read, which ESP-IDF's esp_random reads on this chip,
 * and RNG_CFG, the sources the kernel set, with a count of the ring oscillator's samples in its top byte.
 */
#define RNG_DATA_SYNC (LPPERI_BASE + 0x28u)
#define RNG_CFG       (LPPERI_BASE + 0x24u)

#endif
