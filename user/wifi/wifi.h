#ifndef RVUOS_WIFI_H
#define RVUOS_WIFI_H

/*
 * The Wi-Fi system on a Pico 2 W: what its processes agree on.
 *
 * The root task builds the system with user/lib/ and watches it.
 * The driver of the CYW43439, the radio, is a process of its own;
 * it reaches the chip through PIO0, which clocks the chip's gSPI bus, and through the control registers of four pins,
 * and loads the chip's firmware from frames the root task lends it.
 * The network process runs the IP stack, and trades Ethernet frames with the driver through a channel, the link,
 * see lib/chan.h.
 * Its clients are processes of their own, each with a channel to it from a hub, through which they use sockets, sock.h:
 * the echo, which answers on UDP port 7, and the clock, which asks the network for the time and tells it on port 13.
 * A client that faults is taken down and built again, and the network process lets go of its ports meanwhile.
 * Every process runs code from the one image, so a process other than the root task touches no global:
 * what it keeps lies on its stack or in its page, see lib/child.h.
 *
 * The library hands out each child's slots, regions and bits, and the root task writes which into the child's page,
 * so what the processes agree on is the pages' types below and nothing else.
 */

#include <stdint.h>

#include "lib/chan.h"
#include "lib/child.h"
#include "sock.h"

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
#define BLOB_FRAMES 8u /* the most frames it is lent in */

/* The link: a channel of two rings of Ethernet frames, from the chip and to it. */
#define LINK_SIZE 0x4000u
#define LINK_SLOT 1536u

/* The driver. */
enum drv_state {
    DRV_UP = CHILD_RUNNING, /* the chip runs its firmware, set up; the blob's frames may go */
    DRV_SCANNED,            /* a scan is done, its networks in the page */
    DRV_JOINED,             /* joined the network the page names */
    DRV_AP,                 /* an access point runs, as the page names it */
};

/* What the root task asks of the driver, from the configuration the loader left in its input region. */
enum drv_mode {
    MODE_SCAN, /* scan, and stop */
    MODE_STA,  /* join ssid with pass, or open if pass is empty */
    MODE_AP,   /* run an access point named ssid with pass */
};

/* An access point a scan heard, by its address, as strong as it was heard at best. */
struct drv_net {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int16_t rssi;
};
#define DRV_NETS 16u

struct drv_page {
    struct child_page c;
    /*
     * Before the start: where the devices lie, the firmware blob's frames, in order, the slot and size of each,
     * the region the driver installs them in one at a time, and what to do.
     */
    uint32_t pio_base, pins_base;
    uint32_t blob_base;
    uint32_t blob_frames;
    uint32_t blob_slot[BLOB_FRAMES];
    uint32_t blob_size[BLOB_FRAMES];
    uint32_t window;
    uint32_t mode, channel;
    char ssid[33];
    char pass[65];
    /* Later, with CHILD_BIT_PARENT: the link, once the root task connected it. */
    struct chan_end link;

    /* From the driver. */
    uint8_t mac[6];
    uint32_t net_count;
    struct drv_net nets[DRV_NETS];
    volatile uint32_t rx_frames, tx_frames, rx_dropped;
};

/* The network process. */
enum net_state {
    NET_WAITING = CHILD_RUNNING, /* for its address */
    NET_BOUND,                   /* DHCP gave it an address */
};

/* The clients' channels, from a hub in the network process: two rings each, of a few messages of the most a datagram takes. */
#define CLIENT_CHAN_SIZE 0x4000u
#define CLIENT_SLOT      SOCK_MSG_MAX

struct net_page {
    struct child_page c;
    struct chan_end link;
    struct chan_end clients[SOCK_CLIENTS];
    uint32_t more;     /* the slot of its own inbox, carved to more_bit: it wakes itself with what it left for later */
    uint32_t more_bit;
    uint8_t mac[6];
    /* From the network process. */
    volatile uint32_t ip, mask, gateway;
    volatile uint32_t rx_frames, tx_frames, pings, datagrams;
};

/* The UDP port the network process answers on itself, with a line about the system. */
#define NET_PORT_STATUS 7777u

/* What every client's page starts with: its channel to the network process. */
struct client_page {
    struct child_page c;
    struct chan_end net;
};

/* The echo: datagrams to port 7 sent back as they came; one that reads "fault" makes it store where it may not. */
#define ECHO_PORT 7u
enum echo_state {
    ECHO_SERVING = CHILD_RUNNING + 1, /* holds its port */
};
struct echo_page {
    struct client_page c;
    volatile uint32_t echoed;
};

/* The clock: the time by SNTP from a server of pool.ntp.org, and the time as text to any datagram to port 13. */
#define CLOCK_PORT   13u
#define CLOCK_SERVER "pool.ntp.org"
enum clock_state {
    CLOCK_SET = CHILD_RUNNING + 1, /* knows the time */
};
struct clock_page {
    struct client_page c;
    volatile uint32_t server;  /* the NTP server's address, network order */
    volatile uint32_t seconds; /* the time it learned, since 1970 */
};

void driver_main(struct child_page *page);
void net_main(struct child_page *page);
void echo_main(struct child_page *page);
void clock_main(struct child_page *page);

/* A client's request into its channel: 0, or -1 if the ring is full or the channel not connected; see sock_ask. */
static inline int client_ask(const struct chan_end *e, uint32_t op, uint16_t port, uint32_t ip, uint16_t peer_port,
                             const void *data, uint32_t len)
{
    uint8_t *slot = chan_put_begin(e);
    if (slot == 0 || sizeof(struct sock_msg) + len > e->tx.slot_size) {
        return -1;
    }
    chan_put_end(e, sock_ask(slot, op, port, ip, peer_port, data, len));
    return 0;
}

#endif
