#ifndef RVUOS_WIFI_H
#define RVUOS_WIFI_H

/*
 * The Wi-Fi system on a Pico 2 W: what its processes agree on.
 * On an ESP32-C6 the network process, its clients but the logger, and the link are the same,
 * beside a driver of its own, see esp32c6/drv.h, which reports the driver's states below.
 *
 * The root task builds the system with user/lib/ and watches it; system.h is what of it the two boards' root tasks share.
 * The driver of the CYW43439, the radio, is a process of its own;
 * it reaches the chip through PIO0, which clocks the chip's gSPI bus, and through the control registers of four pins,
 * and loads the chip's firmware from frames the root task lends it.
 * The network process runs the IP stack, and trades Ethernet frames with the driver through a channel, the link,
 * see lib/chan.h.
 * Its clients are processes of their own, each with a channel to it from a hub, through which they use sockets, sock.h:
 * the echo, which answers on UDP port 7, the clock, which asks the network for the time and tells it on port 13
 * and reads the network's addresses as the network process publishes them,
 * and the logger, which carries the kernel's log to a host on port 7070 while the system runs.
 * The root task asks every child each second whether its loop still comes round, lib/child.h, and feeds the watchdog;
 * a client that faults or does not answer is taken down and built again,
 * and the network process lets go of its ports meanwhile.
 * Every process runs code from the one image, so a process other than the root task touches no global:
 * what it keeps lies on its stack or in its page, see lib/child.h.
 *
 * The library hands out each child's slots, regions and bits, and the root task writes which into the child's page,
 * so what the processes agree on is the pages' types below and nothing else.
 */

#include <stdint.h>

#include "lib/chan.h"
#include "lib/child.h"
#include "lib/seqlock.h"
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
    DRV_UP = CHILD_RUNNING, /* the chip runs its firmware, set up, the blob's frames may go; or the libraries run */
    DRV_SCANNED,            /* a scan is done, its networks in the page */
    DRV_JOINED,             /* joined the network the page names */
    DRV_AP,                 /* an access point runs, as the page names it */
    DRV_LEFT,               /* left the network, as the root task asked once the run was over */
    DRV_LISTENING,          /* hears a channel without joining, as the page names it */
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
    uint8_t bssid[6]; /* the access point to join, if bssid_set */
    uint8_t bssid_set;
    uint8_t dhcp_log; /* power save off, and a line for each DHCP message heard */
    uint8_t no_sae;   /* WPA2 alone: no SAE, joining or as an access point */
    /* Later, with CHILD_BIT_PARENT: the link, once the root task connected it, and that the run is over. */
    struct chan_end link;
    volatile uint32_t leave; /* leave the network, and report DRV_LEFT */

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

/* The bytes of its page the network process publishes the network's addresses in, through lib/seqlock.h. */
#define NET_CONFIG_SIZE 64u
_Static_assert(SEQLOCK_BYTES(sizeof(struct sock_config)) <= NET_CONFIG_SIZE, "the addresses fit their bytes");

struct net_page {
    struct child_page c;
    struct chan_end link;
    struct chan_end clients[SOCK_CLIENTS];
    uint32_t more;     /* the slot of its own inbox, carved to more_bit: it wakes itself with what it left for later */
    uint32_t more_bit;
    uint8_t mac[6];
    const char *board; /* what its status line says the system runs on */
    /* From the network process. */
    volatile uint32_t ip; /* its address, once DHCP gave it one, for the root task's line */
    volatile uint32_t rx_frames, tx_frames, pings, datagrams;
    /* The network's addresses, as net_config lays them out; the clock is given these bytes alone. */
    uint32_t config[NET_CONFIG_SIZE / 4u] __attribute__((aligned(NET_CONFIG_SIZE)));
};

/* The seqlock of the network's addresses in the network process's page. */
static inline struct seqlock net_config(const struct net_page *p)
{
    return seqlock_shape((uint32_t)(uintptr_t)p->config, sizeof(struct sock_config));
}

/* The UDP port the network process answers on itself, with a line about the system. */
#define NET_PORT_STATUS 7777u

/* What every client's page starts with: its channel to the network process. */
struct client_page {
    struct child_page c;
    struct chan_end net;
};

/*
 * The echo: datagrams to port 7 sent back as they came;
 * one that reads "fault" makes it store where it may not, and one that reads "hang" makes it spin.
 */
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
    struct seqlock config;     /* the network's addresses, read only */
    volatile uint32_t server;  /* the NTP server's address, network order */
    volatile uint32_t seconds; /* the time it learned, since 1970 */
};

/*
 * The logger: the kernel's log to a host, on UDP port 7070, for as long as the host keeps asking.
 * A host asks with four bytes, the offset in the log of the first byte it lacks, big-endian,
 * which may come by broadcast;
 * each datagram it is sent starts with the offset of its first byte, the same way, and the bytes follow.
 * The host says how far it has them with the same four bytes, as each datagram comes and every second besides,
 * and a byte leaves the log only once the host has it, so the halt writes out every byte no host had.
 * Offsets count the kernel's bytes from the boot, so a gap in them is bytes the log lost before any host had them.
 */
#define LOGGER_PORT 7070u
enum logger_state {
    LOGGER_SERVING = CHILD_RUNNING + 1, /* holds its port */
};
struct logger_page {
    struct client_page c;
    uint32_t log_base;         /* the kernel's log, struct rvuos_log, installed in a region of the logger's */
    volatile uint32_t carried; /* from the logger: the bytes hosts have said they have */
};

void driver_main(struct child_page *page);
void net_main(struct child_page *page);
void echo_main(struct child_page *page);
void clock_main(struct child_page *page);
void logger_main(struct child_page *page);

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
