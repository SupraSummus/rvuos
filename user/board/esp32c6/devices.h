#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of the ESP32-C6 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists:
 * the SAR ADC and the whole modem, read and write, whose MACs reach all RAM, see the kernel's board.h,
 * the eFuse's registers, read only,
 * the window onto flash, read and execute, which the kernel maps there from FLASH_WINDOW_FLASH in flash,
 * the ROM, read and execute, the random number generator's data register, read only,
 * and the IO MUX and the GPIO matrix, read and write, the pins' functions, routes and levels.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 8
#define BOOT_CAP_SARADC (BOOT_CAP_DEVICES + 0) /* the SAR ADC and the temperature sensor, 4 KiB */
#define BOOT_CAP_MODEM  (BOOT_CAP_DEVICES + 1) /* the modem, 64 KiB, with its MACs, which are bus masters */
#define BOOT_CAP_EFUSE  (BOOT_CAP_DEVICES + 2) /* the eFuse's registers, read only, 1 KiB */
#define BOOT_CAP_FLASH  (BOOT_CAP_DEVICES + 3) /* the window onto flash, read and execute, 1 MiB */
#define BOOT_CAP_ROM    (BOOT_CAP_DEVICES + 4) /* the ROM, read and execute, 512 KiB */
#define BOOT_CAP_RNG    (BOOT_CAP_DEVICES + 5) /* the random number generator's data, read only, 8 bytes */
#define BOOT_CAP_IO_MUX (BOOT_CAP_DEVICES + 6) /* the IO MUX, the pins' functions and pads, 4 KiB */
#define BOOT_CAP_GPIO   (BOOT_CAP_DEVICES + 7) /* the GPIO matrix, the pins' routes and levels, 4 KiB */

#define SARADC_BASE       0x6000E000u
#define MODEM_BASE        0x600A0000u
#define BT_BB_BASE        0x600A2000u
#define WIFI_BB0_BASE     0x600A7000u
#define WIFI_BB1_BASE     0x600A8000u
#define MODEM_SYSCON_BASE 0x600A9800u
#define MODEM_LPCON_BASE  0x600AF000u
#define I2C_ANA_MST_BASE  0x600AF800u /* in MODEM_LPCON's frame */
#define EFUSE_BASE        0x600B0800u
#define RNG_DATA          0x600B2808u /* LPPERI's RNG_DATA register */
#define IO_MUX_BASE       0x60090000u
#define GPIO_BASE         0x60091000u
#define FLASH_WINDOW_BASE  0x42000000u
#define FLASH_WINDOW_FLASH 0x00210000u /* where the window's first byte lies in flash */

#endif
