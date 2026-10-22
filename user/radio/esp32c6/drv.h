#ifndef RVUOS_WIFI_DRV_H
#define RVUOS_WIFI_DRV_H

/*
 * A program of the ESP32-C6's radio, a child of the root task, and its page, which is all the two agree on;
 * the Wi-Fi station of user/wifi/esp32c6/ is one, and the programs of prog/ the others, see README.md.
 * A program is called the driver in the root task's log and in this file, as the station was the only one.
 *
 * A program is an image of its own, too large for the SRAM:
 * its code and constants run from the window onto flash, BOOT_CAP_FLASH, at FLASH_WINDOW_BASE,
 * which esptool writes at FLASH_WINDOW_FLASH, and its data lies at a fixed block of RAM, DRV_RAM_BASE,
 * whose top holds the ROM's own data, from DRV_ROM_RAM_BASE, which the ROM's Wi-Fi and PHY functions keep there:
 * the program places nothing in it, and the block is installed whole.
 * The station's block is the free RAM's upper half, 128 KiB, its heap what hostap and Mbed TLS want;
 * another program's is the upper half's upper half, 64 KiB, so that the lower half, a bank of its own,
 * may be the dump engine's, see dump.h, and the root task makes the children's memory of what lies between.
 * The linker script, flash.ld.S, reads this file for the layout, and the build names the smaller block for a program.
 *
 * The image starts with a header, which says where its entry is, what of the machine the program needs,
 * and where its block of RAM lies, which the root task reads before it takes any memory.
 * The root task builds the program as a child that runs this image, lib/child.h's child_code,
 * with its page and its first thread's stack in the child's data, DRV_DATA_SIZE bytes.
 * Its threads are the adapter's, which it builds of its own, child_give_own.
 * Of its eight regions the image takes the code region, its data, its block of RAM, the ROM, the modem and the SAR ADC,
 * and the program's needs the rest: the random number generator, or the link to the network process,
 * or the dump engine's bank and HP_SYSTEM, which take the two.
 */

#define DRV_ROM_RAM_BASE 0x4087c000 /* the ROM's data, to the block's end, which is the free RAM's */
#define DRV_ROM_RAM_END  0x40880000
#define DUMP_BANK_BASE   0x40840000 /* the free RAM's lower half, a bank of SRAM of its own */
#define DUMP_BANK_SIZE   0x00020000
#define DRV_IMAGE_MAX    0x00100000 /* the window onto flash, which the image's code and constants lie in */
#ifndef DRV_RAM_BASE
#define DRV_RAM_BASE     0x40860000
#define DRV_RAM_SIZE     0x00020000
#endif
#define DRV_HEAP_END     DRV_ROM_RAM_BASE

#define DRV_MAGIC 0x49464957u /* "WIFI" */

/*
 * What a program needs of the machine beyond its image's own, in its header's needs.
 * DRV_NEED_RNG: the random number generator, read only, which drv_random reads, as the MAC's backoffs do.
 * DRV_NEED_NET: the link to the network process, which the root task builds with its clients if a network is named.
 * DRV_NEED_I154: the 802.15.4 MAC's interrupt line, if the page names a channel of 802.15.4's.
 * DRV_NEED_DUMP: the dump engine's bank, mapped into the program, and HP_SYSTEM, which gives the bank to the engine;
 * the root task takes the bank before anything else, so it needs the smaller block of RAM.
 */
#define DRV_NEED_RNG  0x1u
#define DRV_NEED_NET  0x2u
#define DRV_NEED_I154 0x4u
#define DRV_NEED_DUMP 0x8u

#ifndef __ASSEMBLER__
#include <stdint.h>

#include "tracer/trace.h"
#include "wifi/wifi.h"

#define DRV_DATA_SIZE 0x4000u
#define DRV_CONF_SIZE 512u /* the most of the configuration the root task copies into the page */

struct drv_header {
    uint32_t magic;
    void (*entry)(struct child_page *page);
    uint32_t needs;          /* DRV_NEED_ bits */
    uint32_t ram_base;       /* the program's block of RAM, the linker script's */
    uint32_t ram_size;
    const char *name;        /* in the window onto flash, which the root task reads */
};

/* The radio's entry, radio.c's, which each program's header names. */
void drv_main(struct child_page *page);

/* The block of RAM the linker script placed the image's data in, see flash.ld.S. */
extern char drv_ram_start[], drv_ram_size[];

/* A program's header, at the start of its image: its name and needs. */
#define DRV_PROGRAM(program_name, program_needs)                                                                   \
    __attribute__((section(".header"), used)) const struct drv_header drv_image = {                               \
        DRV_MAGIC, drv_main, (program_needs), (uint32_t)(uintptr_t)drv_ram_start,                                  \
        (uint32_t)(uintptr_t)drv_ram_size, (program_name)                                                         \
    }

/*
 * The modem's interrupt lines, which the root task carves for the driver:
 * the Wi-Fi MAC's, its NMI's, the Wi-Fi power's and the baseband's, sources 0 to 3 of Espressif's soc/interrupts.h,
 * whose lines are the source plus one; see kernel/board/esp32c6/board.h.
 */
#define DRV_LINE_FIRST 1u
#define DRV_LINES      4u
/* The IEEE 802.15.4 MAC's line, its ZB_MAC source's, which the root task carves for the driver alone; see i154.h. */
#define DRV_LINE_I154 13u

/*
 * The units of each of the adapter's threads, of the driver's own, which the root task gives it as their sum.
 * The interrupt's thread and the station's are on every frame's way, and a frame's work takes each of them
 * more than one unit's request at the default span, about a switch on the C6, and less than four units':
 * with fewer, each frame waits a span for every request past the first while another thread wants the core;
 * see MANUAL.md, section 5.11.
 * The watcher and the timers' thread are on no frame's way and run on spare time.
 */
#define DRV_UNITS_WATCHER 1u
#define DRV_UNITS_ISR     4u
#define DRV_UNITS_TIMERS  1u
#define DRV_UNITS_STATION 4u

/*
 * What the driver tells besides its steps, from the configuration's debug=, see root.c:
 * a line for each frame to and from the link, and each frame the station reads,
 * and hostap's supplicant at its debug level.
 * rekey asks the access point for a new pairwise key after the join; the request is itself an EAPOL frame under
 * the key, so it is how that sending is exercised on a board, though this access point answers no rekey.
 */
#define DRV_DEBUG_FRAMES 0x1u
#define DRV_DEBUG_WPA    0x2u
#define DRV_DEBUG_REKEY  0x4u
#define DRV_DEBUG_PS     0x8u /* a line for each of the power save's moves, and a beacon's TIM now and then */

/*
 * The driver's page.
 * The driver reports the states of wifi.h's enum drv_state, DRV_UP once the libraries have brought the MAC up.
 * With no network named it scans, passively, reports DRV_SCANNED, and turns the radio off.
 * With one, its station, a thread of its own, sta.c's, scans for it, authenticates, associates,
 * and runs the supplicant's handshakes, WPA3's personal by SAE where the access point offers it,
 * unless the page says no_sae, and WPA2's otherwise; a network with no passphrase it refuses, see TODO.md.
 * It installs the keys it derives, reports DRV_JOINED, and serves the link, its data frames under the MAC's CCMP,
 * until the root task asks it to leave, when it leaves, turns the radio off and reports DRV_LEFT.
 * A step of the first join that fails is CHILD_FAILED,
 * with the step's name in the page and the error, or IEEE 802.11's status or reason, in its detail;
 * once joined, a link the access point ends, or that the station loses, is joined again, the state staying DRV_JOINED,
 * and with ps the station sleeps between beacons while its link is idle; see sta.c.
 * With a channel to listen on and no network named, it hears the channel without joining and reports DRV_LISTENING,
 * then tells each second what it heard, and the beacons heard and missed of the access point bssid names or else the first;
 * it leaves as when joined.
 * Listening, with a network to probe for, it asks for it once a second, a few times, as a station that scans asks,
 * and counts the answers to its own address; a run that heard none fails.
 * With a channel of IEEE 802.15.4's named, i154, it brings that radio up instead of Wi-Fi's, see i154.h,
 * reports DRV_LISTENING once it exchanges frames on the channel, and leaves as when listening.
 */
struct drv {
    struct child_page c;
    /* From the root task, before the start. */
    struct child_own own; /* what the driver builds its threads from */
    uint32_t clock;       /* the clock, to read */
    uint32_t lines;       /* the modem's interrupt lines, DRV_LINES of them from DRV_LINE_FIRST */
    char conf[DRV_CONF_SIZE]; /* the configuration as the input region holds it, its first bytes, for the program */
    uint32_t conf_len;
    /*
     * The trace: the driver maps no device, so every access the libraries' bring-up makes faults to its watcher,
     * which the root task carries out and logs; see user/tracer/trace.h and root.c.
     */
    uint32_t trace;      /* 1: trace the bring-up, and stop, instead of joining, scanning or listening */
    uint32_t own_thread; /* the slot of the driver's first thread, for the watcher to serve its faults */
    uint32_t trace_note; /* the slot of the notification the root task answers a trace request on */
    struct trace_share share; /* the request, and the counters the watcher and the root task keep */
    uint8_t mac[6];       /* the factory MAC address, which the root task read from eFuse */
    char ssid[33];        /* the network to join, or empty to scan */
    char pass[65];        /* its passphrase, 8 to 63 characters, or its PSK in 64 hexadecimal digits; empty if open */
    uint8_t bssid[6];     /* the access point of the network to join, if bssid_set, else the one heard best */
    uint8_t bssid_set;
    uint32_t debug;       /* DRV_DEBUG_ bits */
    uint8_t no_pmf;       /* join without protecting management frames, which the station otherwise offers */
    uint8_t no_sae;       /* join without WPA3's SAE, with WPA2's passphrase where the network takes both */
    uint8_t ht;           /* associate for HT too, which the access point sends at MCS rates under */
    uint8_t no_qos;       /* associate without QoS, and so send no A-MSDU */
    uint8_t no_amsdu;     /* send the link's frames one to a transmission, though associated for QoS */
    uint8_t ps;           /* sleep between beacons once joined, the radio off while the link is idle; see sta.c */
    uint8_t blast;        /* seconds the station sends a datagram to its port 9 back to its sender, as fast as the MAC
                             lets it: a test of the sending alone, once a run; see sta.c */
    uint8_t listen;       /* the channel to hear, 1 to 13, or 0 */
    char probe[33];       /* the network to probe for while listening, or empty */
    uint8_t i154;         /* the channel of IEEE 802.15.4 to exchange frames on, 11 to 26, instead of Wi-Fi, or 0 */
    uint32_t i154_line;   /* the 802.15.4 MAC's line, DRV_LINE_I154, given with i154 */
    uint8_t i154_stream;  /* frames a second the driver sends the nRF52840 besides its pings, with i154, or 0 */
#ifdef I154_LISTEN
    uint8_t i154_listen;  /* the channel of IEEE 802.15.4 to listen on, promiscuously, 11 to 26, or 0; see i154.h */
#endif
    struct bulk_end link; /* which the driver lends the network process, connected before the start */
    /* From the root task, while it runs. */
    volatile uint32_t leave; /* the run is over: leave the network, and report DRV_LEFT */

    /* From the driver. */
    char failed[32]; /* the step that failed */
    uint32_t channel; /* the channel it listens on, which the root task tells once it reports DRV_LISTENING */
    volatile uint32_t net_count;
    struct drv_net nets[DRV_NETS]; /* what a scan heard */
    struct drv_net joined;         /* the access point joined */
    volatile int authmode;         /* the joined network's, a WIFI_AUTH_ of esp.h */
    volatile uint32_t rx_frames, rx_dropped, tx_frames, tx_dropped;
};
#endif

#endif
