#ifndef RVUOS_WIFI_H
#define RVUOS_WIFI_H

/*
 * The Wi-Fi system on a Pico 2 W: what its processes agree on.
 *
 * The root task builds the system and watches it.
 * The driver of the CYW43439, the radio, is a process of its own;
 * it reaches the chip through PIO0, which clocks the chip's gSPI bus, and through the control registers of four pins,
 * and loads the chip's firmware from frames the root task lends it.
 * The network process runs the IP stack, and trades Ethernet frames with the driver through two rings
 * in a frame the two share, the link.
 * Every process runs code from the one image, so a process other than the root task touches no global:
 * what it keeps lies in its own data frame.
 *
 * A capability's slot in a table is a convention between the one who fills the table and the one who uses it;
 * these are the root task's with its children.
 */

#include <stdint.h>

#include "lib.h"

/*
 * What every child starts with: the first slots of its table, the first regions,
 * the first bits of the notification it waits on, and a page it shares with the root task, whose address is its a0.
 */
enum {
    CHILD_NULL,
    CHILD_INBOX, /* the notification it waits on, RIGHT_R */
    CHILD_ROOT,  /* the root task's notification, RIGHT_W */
    CHILD_TIMER, /* an Irq on a timer line of its own, which signals CHILD_INBOX */
    CHILD_SELF,  /* its own process, RIGHT_W */
    CHILD_FIRST, /* the child's own slots begin here */
};

enum {
    CHILD_REGION_CODE,
    CHILD_REGION_DATA,
    CHILD_REGION_PAGE,
    CHILD_REGION_FIRST,
};

#define CHILD_BIT_TIMER 0x1u
#define CHILD_BIT_ROOT  0x2u /* the root task wrote something into the page */

/* A child's state in its page; each child adds states of its own from CHILD_RUNNING up. */
enum {
    CHILD_STARTING,
    CHILD_FAILED,
    CHILD_RUNNING,
};

/*
 * What every child's page starts with.
 * The root task writes the first part before the child starts; the child writes the rest and signals CHILD_ROOT.
 * The child holds no Debug capability, which would let it halt the machine and make frames over devices,
 * so its text goes into the log, which the root task copies to the console.
 */
struct child_page {
    uint32_t data_base, data_size;
    volatile uint32_t state;
    volatile uint32_t step;   /* where it got to, for a failure */
    volatile uint32_t detail; /* a value that step read, for a failure */
    struct plog log;
};

/* Bits on the root task's inbox: the log, the run's end, and two for each child, its page and its fault. */
#define ROOT_BIT_LOG        0x1u
#define ROOT_BIT_TIMER      0x2u
#define ROOT_BIT_PAGE(i)    (0x4u << (2u * (i)))
#define ROOT_BIT_FAULT(i)   (0x8u << (2u * (i)))
enum { CHILD_DRIVER, CHILD_NET, CHILDREN };

/*
 * The link between the driver and the network process: a frame of two rings of Ethernet frames,
 * from the chip in the first half and to it in the second; see ring.h.
 */
#define LINK_SIZE 0x4000u
#define LINK_SLOT 1536u
#define LINK_RX(base) ((struct ring *)(base))
#define LINK_TX(base) ((struct ring *)((base) + LINK_SIZE / 2u))

/* The driver: its slots past CHILD_FIRST, its regions past CHILD_REGION_FIRST, its bits. */
enum {
    DRV_NET = CHILD_FIRST, /* the network process's notification, RIGHT_W */
    DRV_FIRMWARE,          /* the first of the frames that hold the firmware blob, read only, in order */
    DRV_SLOTS = DRV_FIRMWARE + 8,
};

enum {
    DRV_REGION_PIO = CHILD_REGION_FIRST,
    DRV_REGION_PINS,
    DRV_REGION_WINDOW, /* where the driver installs one frame of the firmware at a time */
    DRV_REGION_LINK = DRV_REGION_WINDOW, /* where the root task installs the link once the firmware's frames are gone */
};

#define DRV_BIT_TX      0x4u /* the network process put a frame into an empty ring to the chip */
#define DRV_BIT_RXSPACE 0x8u /* the network process took a frame from a full ring from the chip */

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

enum drv_state {
    DRV_UP = CHILD_RUNNING, /* the chip runs its firmware, set up; the blob's frames may go */
    DRV_SCANNED,   /* a scan is done, its networks in the page */
    DRV_JOINED,    /* joined the network the page names */
    DRV_AP,        /* an access point runs, as the page names it */
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
#define DRV_NETS 16u

struct drv_page {
    struct child_page c;
    /* Before the start: where the devices lie, the firmware blob's frames, the size of each in order, and the mode. */
    uint32_t pio_base, pins_base;
    uint32_t blob_base;
    uint32_t blob_frames;
    uint32_t blob_frame_size[DRV_SLOTS - DRV_FIRMWARE];
    uint32_t mode, channel;
    char ssid[33];
    char pass[65];
    /* Later, with CHILD_BIT_ROOT: where the link lies, once the root task installed it; 0 until then. */
    volatile uint32_t link_base;

    /* From the driver. */
    uint8_t mac[6];
    uint32_t net_count;
    struct drv_net nets[DRV_NETS];
    volatile uint32_t rx_frames, tx_frames, rx_dropped;
};

/* The network process. */
enum {
    NET_DRIVER = CHILD_FIRST, /* the driver's notification, RIGHT_W */
    NET_SLOTS,
};

enum {
    NET_REGION_LINK = CHILD_REGION_FIRST,
};

#define NET_BIT_RX      0x4u /* the driver put a frame into an empty ring from the chip */
#define NET_BIT_TXSPACE 0x8u /* the driver took a frame from a full ring to the chip */

enum net_state {
    NET_WAITING = CHILD_RUNNING, /* for its address */
    NET_BOUND,                   /* DHCP gave it an address */
};

struct net_page {
    struct child_page c;
    uint32_t link_base;
    uint8_t mac[6];
    /* From the network process. */
    volatile uint32_t ip, mask, gateway;
    volatile uint32_t rx_frames, tx_frames, pings, datagrams;
};

/* The UDP ports the network process answers on: an echo, and a line about the system. */
#define NET_PORT_ECHO   7u
#define NET_PORT_STATUS 7777u

#endif
