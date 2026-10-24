#ifndef RVUOS_USER_DEVICES_H
#define RVUOS_USER_DEVICES_H

/*
 * The devices of the MPS2 boards the boot grants as frames from BOOT_CAP_DEVICES up,
 * as DEVICE_RANGE_LIST in each board's board.h lists them: the FPGA's first 32 bytes, read only.
 */
#define BOOT_DEVICES 1
#define BOOT_RAM_BLOCKS 0 /* no further blocks of RAM, from BOOT_CAP_RAM_BLOCKS up */
#define BOOT_CAP_COUNTER (BOOT_CAP_DEVICES + 0) /* the FPGA's first 32 bytes */

/* Where BOOT_CAP_COUNTER holds the counter OP_CLOCK_READ reads; see OP_CLOCK_READ. */
#define COUNTER_OFFSET 0x18u

#endif
