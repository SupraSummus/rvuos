#ifndef RVUOS_WIFI_DRV_H
#define RVUOS_WIFI_DRV_H

/*
 * The Wi-Fi driver of the ESP32-C6, Espressif's closed libraries around an adapter of rvuos's,
 * a child of the root task, and its page, which is all the two agree on.
 *
 * The driver is an image of its own, too large for the SRAM:
 * its code and constants run from the window onto flash, BOOT_CAP_FLASH, at FLASH_WINDOW_BASE,
 * which esptool writes at FLASH_WINDOW_FLASH, and its data lies at a fixed block of RAM, DRV_RAM_BASE,
 * since the libraries' data is linked where it lies.
 * The block's top holds the ROM's own data, which the ROM's Wi-Fi and PHY functions the libraries call keep there:
 * the driver places nothing in it, and the block is installed whole.
 * The image starts with a header, which says where its entry is;
 * the linker script, flash.ld.S, reads this file for the layout.
 *
 * The root task builds the driver as a child that runs this image, lib/child.h's child_code,
 * with its page and its first thread's stack in the child's data, DRV_DATA_SIZE bytes.
 * The libraries want tasks, which the adapter runs as threads the driver builds of its own, child_give_own.
 * Its eight regions are all in use: the image in the code region, its data, its block of RAM,
 * the ROM, the modem, the SAR ADC, the random number generator, and the link to the network process.
 */

#define DRV_RAM_BASE     0x40860000
#define DRV_RAM_SIZE     0x00020000
#define DRV_ROM_RAM_BASE 0x4087c000 /* the ROM's data, to the block's end */

#define DRV_MAGIC 0x49464957u /* "WIFI" */

#ifndef __ASSEMBLER__
#include <stdint.h>

#include "wifi/wifi.h"

#define DRV_DATA_SIZE 0x4000u

struct drv_header {
    uint32_t magic;
    void (*entry)(struct child_page *page);
};

/*
 * The modem's interrupt lines, which the root task carves for the driver:
 * the Wi-Fi MAC's, its NMI's, the Wi-Fi power's and the baseband's, sources 0 to 3 of Espressif's soc/interrupts.h,
 * whose lines are the source plus one; see kernel/board/esp32c6/board.h.
 */
#define DRV_LINE_FIRST 1u
#define DRV_LINES      4u

/*
 * What the driver tells besides its steps, from the configuration's debug=, see root.c:
 * a line for each frame to and from the link, the libraries' counters of the radio once the run is over,
 * hostap's supplicant at its debug level,
 * and once joined, a line for each frame the radio hears of the access point's to the group or to the station,
 * each acknowledgement to the station, and each frame heard broken, timed by the MAC;
 * and of a probe the libraries send, what they wrote to the MAC to send it, see mac.h.
 */
#define DRV_DEBUG_FRAMES 0x1u
#define DRV_DEBUG_STATS  0x2u
#define DRV_DEBUG_WPA    0x4u
#define DRV_DEBUG_AIR    0x8u
#define DRV_DEBUG_TX     0x10u

/*
 * The driver's page.
 * With no network named the driver scans, passively, by the libraries or with own_rx by its own code,
 * and with one it joins it, WPA3's personal where the access point offers it,
 * unless the page says no_sae, and WPA2's otherwise;
 * it asks for an open one if the page names no passphrase, which the libraries refuse yet, see TODO.md.
 * it reports the states of wifi.h's enum drv_state, DRV_UP once the libraries run in station mode,
 * then DRV_SCANNED or DRV_JOINED, and once joined it moves frames between the libraries and the link,
 * until the root task asks it to leave, when it leaves, turns the radio off and reports DRV_LEFT;
 * a scan's run turns the radio off before DRV_SCANNED too.
 * A step that fails, or the access point letting the station go, is CHILD_FAILED,
 * with the step's name in the page and ESP-IDF's error, or the reason, in its detail.
 * With a channel to listen on and no network named, it hears the channel without joining and reports DRV_LISTENING,
 * then tells each second what it heard, and the beacons heard and missed of the access point bssid names or else the first,
 * received by the libraries or, with own_rx, by mac.c, so that the two can be compared; it leaves as when joined.
 * Listening, with a network to probe for, it asks for it once a second, a few times, as a station that scans asks,
 * and counts the answers to its own address; a run that heard none fails.
 */
struct drv {
    struct child_page c;
    /* From the root task, before the start. */
    struct child_own own; /* what the driver builds its threads from */
    uint32_t clock;       /* the clock, to read */
    uint32_t lines;       /* the modem's interrupt lines, DRV_LINES of them from DRV_LINE_FIRST */
    uint8_t mac[6];       /* the factory MAC address, which the root task read from eFuse */
    char ssid[33];        /* the network to join, or empty to scan */
    char pass[65];        /* its passphrase, 8 to 63 characters, or its PSK in 64 hexadecimal digits; empty if open */
    uint8_t bssid[6];     /* the access point of the network to join, if bssid_set, else the one heard best */
    uint8_t bssid_set;
    uint32_t debug;       /* DRV_DEBUG_ bits */
    uint8_t no_ax;        /* join without 802.11ax, as 802.11n at best */
    uint8_t no_pmf;       /* join without protecting management frames, which the station otherwise offers */
    uint8_t no_sae;       /* join without WPA3's SAE, with WPA2's passphrase where the network takes both */
    uint8_t modem_sleep;  /* sleep between beacons, WIFI_PS_MIN_MODEM, which loses the access point yet; see TODO.md */
    uint32_t lib_log;     /* the libraries' log level, ESP-IDF's wifi_log_level_t, or 0 for its INFO */
    uint8_t listen;       /* the channel to hear, 1 to 13, or 0 */
    uint8_t own_rx;       /* hear it, or scan, through mac.c rather than the libraries */
    char probe[33];       /* the network to probe for while listening, or empty */
    struct chan_end link; /* to the network process, connected before the start */
    /* From the root task, while it runs. */
    volatile uint32_t leave; /* the run is over: leave the network, and report DRV_LEFT */
    volatile uint32_t stats; /* how many times the libraries' counters were asked for; told once joined */

    /* From the driver. */
    char failed[32]; /* the step that failed */
    volatile uint32_t net_count;
    struct drv_net nets[DRV_NETS]; /* what a scan heard */
    struct drv_net joined;         /* the access point joined, its RSSI left zero */
    volatile int authmode;         /* the joined network's, a WIFI_AUTH_ of esp.h */
    volatile uint32_t rx_frames, rx_dropped, tx_frames, tx_dropped;
};
#endif

#endif
