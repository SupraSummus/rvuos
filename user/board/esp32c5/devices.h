#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of the ESP32-C5 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists:
 * the eFuse's registers, read only, the ROM, read and execute,
 * the IO MUX and the GPIO matrix, read and write, the pins' functions, routes and levels,
 * and SPI2, read and write, the general-purpose SPI controller, whose clocks the kernel sets: 80 MHz for its master clock.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 5
#define BOOT_CAP_EFUSE  (BOOT_CAP_DEVICES + 0) /* the eFuse's registers, read only, 1 KiB */
#define BOOT_CAP_ROM    (BOOT_CAP_DEVICES + 1) /* the ROM, read and execute, 512 KiB */
#define BOOT_CAP_IO_MUX (BOOT_CAP_DEVICES + 2) /* the IO MUX, the pins' functions and pads, 4 KiB */
#define BOOT_CAP_GPIO   (BOOT_CAP_DEVICES + 3) /* the GPIO matrix, the pins' routes and levels, 4 KiB */
#define BOOT_CAP_SPI2   (BOOT_CAP_DEVICES + 4) /* SPI2, the general-purpose SPI controller, 4 KiB */

/* The further block of RAM the boot grants as an Untyped from BOOT_CAP_RAM_BLOCKS up, beside the free RAM. */
#define BOOT_RAM_BLOCKS 1
#define BOOT_CAP_MORE_RAM (BOOT_CAP_RAM_BLOCKS + 0) /* 128 KiB from 0x40820000, right below the free RAM */

#define EFUSE_BASE  0x600B4800u
#define IO_MUX_BASE 0x60090000u
#define GPIO_BASE   0x60091000u
#define SPI2_BASE   0x60081000u

#endif
