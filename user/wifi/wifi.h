#ifndef RVUOS_WIFI_H
#define RVUOS_WIFI_H

/*
 * The Wi-Fi system on a Pico 2 W: what its processes agree on.
 *
 * The root task builds the system and watches it.
 * The driver of the CYW43439, the radio, is a process of its own;
 * it reaches the chip through PIO0, which clocks the chip's gSPI bus, and through the control registers of four pins,
 * and loads the chip's firmware from frames the root task lends it.
 * Every process runs code from the one image, so a process other than the root task touches no global:
 * what it keeps lies in its own data frame, whose base it is started with in a0.
 *
 * A capability's slot in a table is a convention between the one who fills the table and the one who uses it;
 * these are the driver's.
 */

#include <stdint.h>

/* The driver's table. */
enum {
    DRV_NULL,
    DRV_SELF,    /* its own process, RIGHT_W, to install the firmware's frames one at a time */
    DRV_INBOX,   /* the notification it waits on, RIGHT_R */
    DRV_ROOT,    /* the root task's notification, RIGHT_W */
    DRV_TIMER,   /* an Irq on a timer line, which signals DRV_INBOX */
    DRV_FIRMWARE, /* the first of the frames that hold the firmware blob, read only, in order */
    DRV_SLOTS = DRV_FIRMWARE + 8,
};

/* The driver's regions. */
enum {
    DRV_REGION_CODE,
    DRV_REGION_DATA,
    DRV_REGION_PIO,
    DRV_REGION_PINS,
    DRV_REGION_SHARED,
    DRV_REGION_WINDOW, /* where the driver installs one frame of the firmware at a time */
};

/* Bits on the driver's inbox. */
#define DRV_BIT_TIMER 0x1u
#define DRV_BIT_ROOT  0x2u

/* Bits on the root task's inbox. */
#define ROOT_BIT_LOG     0x1u
#define ROOT_BIT_TIMER   0x2u
#define ROOT_BIT_DRIVER  0x4u /* the driver changed its state in the shared page */
#define ROOT_BIT_FAULT   0x8u /* the driver's thread faulted */

/*
 * The firmware blob, as tools/cyw43-blob.py lays it out, which the loader places at the start of free RAM:
 * the header, then the firmware, the CLM and the NVRAM, each at a multiple of four.
 */
#define BLOB_MAGIC 0x33345943u /* "CY43" */
struct blob_header {
    uint32_t magic;
    uint32_t size;      /* of the whole blob, header included */
    uint32_t fw_offset, fw_size;
    uint32_t clm_offset, clm_size;
    uint32_t nvram_offset, nvram_size;
};

/*
 * The page the root task and the driver share.
 * The driver writes its state and what it learned; the root task writes requests.
 * Each side signals the other after writing.
 */
enum drv_state {
    DRV_STARTING,
    DRV_UP,        /* the chip runs its firmware, set up */
    DRV_SCANNED,   /* a scan is done, its networks in the page */
    DRV_JOINED,    /* joined the network the page names */
    DRV_AP,        /* an access point runs, as the page names it */
    DRV_FAILED,
};

/* What the root task asks of the driver, from the configuration the loader left in its input region. */
enum drv_mode {
    MODE_SCAN, /* scan, and stop */
    MODE_STA,  /* scan, then join ssid with pass, or open if pass is empty */
    MODE_AP,   /* run an access point named ssid with pass */
};

/* A network a scan heard, the strongest of each name. */
struct drv_net {
    char ssid[33];
    uint8_t channel;
    int16_t rssi;
};
#define DRV_NETS 24u

#define DRV_LOG_SIZE 2048u

struct drv_shared {
    /*
     * What the root task tells the driver before it starts, since a0 carries one word, this page's address:
     * where its regions lie, and the firmware blob's frames, the size of each in order.
     */
    uint32_t data_base, data_size;
    uint32_t pio_base, pins_base;
    uint32_t blob_base;
    uint32_t blob_frames;
    uint32_t blob_frame_size[DRV_SLOTS - DRV_FIRMWARE];
    uint32_t mode, channel;
    char ssid[33];
    char pass[65];

    /* What the driver tells the root task. */
    volatile uint32_t state;
    volatile uint32_t step;   /* where it got to, for a failure */
    volatile uint32_t detail; /* a value that step read, for a failure */
    uint8_t mac[6];
    uint32_t net_count;
    struct drv_net nets[DRV_NETS];

    /*
     * The driver's text, which the root task copies to the console: it holds no Debug capability,
     * which would let it halt the machine and make frames over its devices.
     * log_head counts every byte the driver wrote; byte n lies at log[n % DRV_LOG_SIZE].
     */
    volatile uint32_t log_head;
    char log[DRV_LOG_SIZE];
};

#endif
