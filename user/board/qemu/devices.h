#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of QEMU virt the boot grants as frames from BOOT_CAP_DEVICES up,
 * as DEVICE_RANGE_LIST in the kernel's board.h lists them: the CLINT's mtime and the words below it, read only.
 */
#define BOOT_DEVICES 1
#define BOOT_RAM_BLOCKS 0 /* no further blocks of RAM, from BOOT_CAP_RAM_BLOCKS up */
#define BOOT_CAP_COUNTER (BOOT_CAP_DEVICES + 0) /* the CLINT's 32 bytes that end with mtime */

/* Where BOOT_CAP_COUNTER holds the counter OP_CLOCK_READ reads; see OP_CLOCK_READ. */
#define COUNTER_OFFSET 0x18u

#endif
