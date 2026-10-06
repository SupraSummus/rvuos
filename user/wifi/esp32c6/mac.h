#ifndef RVUOS_WIFI_MAC_H
#define RVUOS_WIFI_MAC_H

/*
 * The Wi-Fi MAC driven by the driver's own code rather than by Espressif's libraries,
 * on the way to a driver that leaves them the PHY alone; see TODO.md.
 * It receives and retunes the radio, once the libraries have brought the MAC up and set its filters:
 * mac_rx_take gives the MAC a list of descriptors of its own and takes its interrupt,
 * so that each frame goes to heard, in the interrupt's thread, and none to the libraries;
 * mac_rx_give_back stops the MAC receiving and gives the libraries their interrupt back, before they stop.
 * It sends too: mac_tx builds a frame's descriptor, programs a slot the libraries' lmac uses and arms it,
 * and finishes it, clearing the hardware txq state its completion leaves, so that it needs no interrupt of theirs;
 * see mac.c.
 */

#include <stdint.h>

/*
 * The channels the radio may be tuned to, 2.4 GHz's 1 to 13, as Europe allows:
 * mac_channel refuses any other, wherever the number came from, a beacon heard among them.
 */
#define MAC_CHANNEL_FIRST 1u
#define MAC_CHANNEL_LAST  13u

/*
 * A frame received whole: the 802.11 frame without its FCS, and what the MAC says of its reception.
 * Its length is the MAC's header's less the FCS, never more than the MAC wrote,
 * so a reader reads only the len bytes at frame, and no header of the MAC's.
 */
struct mac_frame {
    const uint8_t *frame;
    uint32_t len;
    int rssi;         /* dBm */
    uint32_t channel; /* the channel mac_channel last tuned the radio to, or 0 if it did not */
};

typedef void mac_heard_fn(const struct mac_frame *f);

/*
 * The frame in buf, which starts with the MAC's 92 bytes about its reception, esp.h's RX_CTRL_,
 * written bytes in all, the frame among them without its FCS:
 * 1 if it was received whole, is no shorter than an acknowledgement, and fits what was written.
 * The libraries' frames come in the same layout.
 */
int mac_frame_read(const uint8_t *buf, uint32_t written, struct mac_frame *f);

/* What the receiving met since mac_rx_take, for the log; the interrupt's thread alone writes it. */
struct mac_rx_counts {
    volatile uint32_t interrupts; /* the MAC's interrupt, however caused */
    volatile uint32_t causes;     /* every cause seen, ORed */
    volatile uint32_t broken;     /* frames not received whole, dropped */
    volatile uint32_t chained;    /* frames longer than a buffer, in a chain of descriptors, dropped */
    volatile uint32_t odd;        /* frames mac_frame_read refuses, or of a header the driver does not read, dropped */
    volatile uint32_t overran;    /* buffers the MAC wrote past their end */
    volatile uint32_t restarted;  /* times the MAC had run out of descriptors and started again */
    volatile uint32_t stuck;      /* times the MAC did not read the list's links again in time */
    volatile uint32_t repointed;  /* times the take pointed the MAC at the driver's list again */
};
extern struct mac_rx_counts mac_rx_counts;

/* 0, or the step that failed. */
const char *mac_rx_take(mac_heard_fn *heard);
void mac_rx_give_back(void);

/*
 * The radio on another channel, MAC_CHANNEL_FIRST to MAC_CHANNEL_LAST, as the libraries' chm_phy_change_channel
 * retunes it, the MAC held meanwhile; 0, or why not, a channel outside them left untuned.
 */
const char *mac_channel(uint32_t channel);

/*
 * The sending's buffer, and the lock that lets one thread send at a time, made once before any thread sends;
 * 0, or what could not be made.
 */
const char *mac_tx_init(void);

/*
 * A frame sent by the driver's own code; 0, or the step that failed, a timeout or a collision among them.
 * The frame is the 802.11 frame without its checksum, which the MAC appends,
 * and mac_tx gives a management frame the station's next sequence number, as the libraries give theirs.
 * The driver programs a slot the libraries' lmac names, arms it, and finishes the frame itself:
 * it clears the hardware txq state's completion bit, which lets the slot's arm bits clear, and on a timeout's
 * or a collision's bit disarms the slot and fails; the caller may send it again.
 * It works with the libraries' interrupt or the driver's own. See mac.c.
 * Any of the driver's threads may send, the station's and the link's: one waits while another's frame goes,
 * since there is one buffer and one slot.
 */
const char *mac_tx(const uint8_t *frame, uint32_t len);

#endif
