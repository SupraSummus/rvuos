#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of the ESP32-C6 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists:
 * the SAR ADC and the blocks of the modem a PHY driver drives, read and write,
 * and the eFuse's registers, read only.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 8
#define BOOT_CAP_SARADC       (BOOT_CAP_DEVICES + 0) /* the SAR ADC and the temperature sensor, 4 KiB */
#define BOOT_CAP_FE           (BOOT_CAP_DEVICES + 1) /* the RF front end, 4 KiB */
#define BOOT_CAP_BT_BB        (BOOT_CAP_DEVICES + 2) /* the Bluetooth baseband, 4 KiB */
#define BOOT_CAP_WIFI_BB0     (BOOT_CAP_DEVICES + 3) /* the Wi-Fi baseband's first page, 4 KiB */
#define BOOT_CAP_WIFI_BB1     (BOOT_CAP_DEVICES + 4) /* and its second, 4 KiB */
#define BOOT_CAP_MODEM_SYSCON (BOOT_CAP_DEVICES + 5) /* the modem's clocks and resets, 1 KiB */
#define BOOT_CAP_MODEM_LPCON  (BOOT_CAP_DEVICES + 6) /* the modem's LP clocks, and the analog I2C master, 4 KiB */
#define BOOT_CAP_EFUSE        (BOOT_CAP_DEVICES + 7) /* the eFuse's registers, read only, 1 KiB */

#define SARADC_BASE       0x6000E000u
#define FE_BASE           0x600A0000u
#define BT_BB_BASE        0x600A2000u
#define WIFI_BB0_BASE     0x600A7000u
#define WIFI_BB1_BASE     0x600A8000u
#define MODEM_SYSCON_BASE 0x600A9800u
#define MODEM_LPCON_BASE  0x600AF000u
#define I2C_ANA_MST_BASE  0x600AF800u /* in MODEM_LPCON's frame */
#define EFUSE_BASE        0x600B0800u

#endif
