#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of RP2350 the boot grants as frames from BOOT_CAP_DEVICES up,
 * in the order of DEVICE_RANGE_LIST in the kernel's board.h, as many as it lists.
 * Each frame is 16 KiB, read and write: a device's registers and their atomic aliases.
 * A frame maps at its own address, so a program names a register by the device's base.
 */
#define BOOT_DEVICES 5
#define BOOT_CAP_IO_BANK0   (BOOT_CAP_DEVICES + 0) /* the pins' functions, overrides and interrupts */
#define BOOT_CAP_PADS_BANK0 (BOOT_CAP_DEVICES + 1) /* the pins' pads */
#define BOOT_CAP_PIO0       (BOOT_CAP_DEVICES + 2)
#define BOOT_CAP_PIO1       (BOOT_CAP_DEVICES + 3)
#define BOOT_CAP_PIO2       (BOOT_CAP_DEVICES + 4)

#define IO_BANK0_BASE   0x40028000u
#define PADS_BANK0_BASE 0x40038000u
#define PIO0_BASE       0x50200000u
#define PIO1_BASE       0x50300000u
#define PIO2_BASE       0x50400000u

#endif
