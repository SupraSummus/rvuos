#ifndef RVUOS_USER_RADIO_H
#define RVUOS_USER_RADIO_H

/*
 * What the radio's programs of user/radio/esp32c6/ need to know of the ESP32-C6 and of the XIAO it sits on,
 * beside the devices of devices.h; drv.h and the root task read it, and so does the programs' linker script.
 */
#define RADIO_XTAL_MHZ 40 /* the crystal's */
#define RADIO_CHIP "an ESP32-C6"

/*
 * The SRAM a program's block of RAM is laid out in, see drv.h:
 * the ROM's data, which its Wi-Fi and PHY functions keep at the top of the free RAM,
 * the dump engine's bank, the free RAM's lower half, a bank of SRAM of its own,
 * and the station's block, the free RAM's upper half; the lowest a program's block may start is past the bank.
 */
#define DRV_ROM_RAM_BASE 0x4087c000 /* the ROM's data, to the block's end, which is the free RAM's */
#define DRV_ROM_RAM_END  0x40880000
#define DUMP_BANK_BASE   0x40840000
#define DUMP_BANK_SIZE   0x00020000
#define DRV_RAM_LOWEST   (DUMP_BANK_BASE + DUMP_BANK_SIZE)
#define DRV_STATION_RAM_BASE 0x40860000
#define DRV_STATION_RAM_SIZE 0x00020000

/*
 * The ROM's ets_delay_us counts in the user-mode performance counter, 0x802, which the program starts;
 * see the radio's radio.c.
 */
#define RADIO_ROM_DELAY_UPCCR 1

/* The IEEE 802.15.4 MAC's line, its ZB_MAC source's plus one; see the kernel's board.h. */
#define DRV_LINE_I154 13u

/*
 * The XIAO ESP32-C6's RF switch, between the chip and its two antennas, on two pins the ROM leaves floating:
 * GPIO3 low powers it, and GPIO14 picks the antenna on the board, low, or the U.FL connector, high, as Seeed's startup does.
 * Set so, a scan heard the nearest access point 12 to 22 dB louder, and two more,
 * and the station joined at -64 dBm where it joined at -85 with the pins left alone, measured on the chip.
 */
#define RF_SWITCH_POWER   3u
#define RF_SWITCH_ANTENNA 14u

#endif
