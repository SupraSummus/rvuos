#ifndef RVUOS_USER_RADIO_H
#define RVUOS_USER_RADIO_H

/*
 * What the radio's programs of user/radio/esp32c6/ need to know of the ESP32-C5,
 * beside the devices of devices.h; drv.h and the root task read it, and so does the programs' linker script.
 */
#define RADIO_XTAL_MHZ 48 /* the crystal's */
#define RADIO_CHIP "an ESP32-C5"

/*
 * The SRAM a program's block of RAM is laid out in, see drv.h:
 * the ROM's data, which its Wi-Fi and PHY functions keep at the top of the free RAM, from 0x4085e5a0 up,
 * as the ROM's ELF places it, with the boot's stack and buffers below it, which are free once the ROM has handed over,
 * and the station's block, the whole of the free RAM, which is 128 KiB here.
 * No program has the dump engine's bank yet, which would take the free RAM's lower half, so none needs DRV_NEED_DUMP.
 */
#define DRV_ROM_RAM_BASE 0x4085e000
#define DRV_ROM_RAM_END  0x40860000
#define DUMP_BANK_BASE   0x40840000
#define DUMP_BANK_SIZE   0x00010000
#define DRV_RAM_LOWEST   0x40840000
#define DRV_STATION_RAM_BASE 0x40840000
#define DRV_STATION_RAM_SIZE 0x00020000

/*
 * The ROM's ets_delay_us counts in mcycle, which user mode cannot read, whatever mcounteren says,
 * so a program has a delay of its own over the kernel's clock, user/radio/esp32c5/delay.c,
 * which takes the place of the ROM's for every caller but the ROM's own functions.
 */

/*
 * The registers outside the program's frames that Espressif's PHY library reads and writes, which the kernel sets
 * at boot instead, see its board.c; the adapter, osi.c, feigns a program's accesses to them from a shadow of each,
 * with the shadow's first value. PMU_RF_PWC and IMM_HP_CK_POWER, PCR's SARADC_CONF, TSENS_CLK_CONF and the word
 * beside its SAR clock dividers, and LP_AON's SAR_CCT.
 */
#define RADIO_FEIGNED                                                                                            \
    { 0x600B0158u, 0 }, { 0x600B00CCu, 0 }, { 0x60096088u, 0 }, { 0x60096090u, 0 }, { 0x6009614Cu, 0 },          \
        { 0x600B1054u, 0 },

/*
 * The eFuse's words the PHY library reads, the factory MAC address and the PA's calibration among them:
 * no program maps the eFuse, so the root task copies them into the driver's page, and the adapter answers from there.
 */
#define RADIO_EFUSE_COPY  0x600B4844u
#define RADIO_EFUSE_WORDS 8u

/* The Wi-Fi MAC's layout, the ESP32-C5's, see the radio's macregs.h. */
#define RADIO_MAC_C5 1

/* The IEEE 802.15.4 MAC's line, its ZB_MAC source's plus one; see the kernel's board.h. */
#define DRV_LINE_I154 13u

/* The Waveshare board wires the chip's antenna pin straight to its antenna, with no switch to set. */

#endif
