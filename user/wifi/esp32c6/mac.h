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
 * and the libraries' interrupt finishes the frame; see mac.c.
 */

#include <stdint.h>

/* A received frame: the 92 bytes the MAC writes about its reception, esp.h's RX_CTRL_, and the frame after them. */
typedef void mac_heard_fn(const uint8_t *buf);

/* What the receiving met since mac_rx_take, for the log; the interrupt's thread alone writes it. */
struct mac_rx_counts {
    volatile uint32_t interrupts; /* the MAC's interrupt, however caused */
    volatile uint32_t causes;     /* every cause seen, ORed */
    volatile uint32_t chained;    /* frames longer than a buffer, in a chain of descriptors, dropped */
    volatile uint32_t odd;        /* frames whose header the driver does not read yet, or too short for one, dropped */
    volatile uint32_t overran;    /* buffers the MAC wrote past their end */
    volatile uint32_t restarted;  /* times the MAC had run out of descriptors and started again */
    volatile uint32_t stuck;      /* times the MAC did not read the list's links again in time */
    volatile uint32_t repointed;  /* times the take pointed the MAC at the driver's list again */
};
extern struct mac_rx_counts mac_rx_counts;

/* 0, or the step that failed. */
const char *mac_rx_take(mac_heard_fn *heard);
void mac_rx_give_back(void);

/* The radio on another channel, 1 to 13, as the libraries' chm_phy_change_channel retunes it, the MAC held meanwhile. */
void mac_channel(uint32_t channel);

/*
 * A frame sent by the driver's own code; 0, or the step that failed.
 * The frame is the 802.11 frame without its checksum, which the MAC appends.
 * The driver programs a slot the libraries' lmac uses and arms it, and does not finish the frame;
 * their interrupt does, and their lmac_stop_hw_txq waits on the queue's state, so not with own_rx. See mac.c.
 */
const char *mac_tx(const uint8_t *frame, uint32_t len);

/*
 * What the libraries wrote to the MAC to send a frame, for debug=tx, which is to hold the driver's own sending against theirs:
 * their hal_mac_txq_enable, which starts one of the MAC's slots sending, is wrapped at the link,
 * and once armed, the next start records the slot's registers, its descriptor and the frame's first bytes, before it starts.
 */
#define MAC_TX_PPDU_WORDS 29u
#define MAC_TX_DESC_WORDS 8u
#define MAC_TX_FRAME_SIZE 48u
struct mac_tx_seen {
    volatile uint32_t armed, full;
    uint32_t slot;
    uint32_t queue[4];                /* from 0x600a4d60, less 0x10 a slot */
    uint32_t ppdu[MAC_TX_PPDU_WORDS]; /* from 0x600a5488, less 0x74 a slot */
    uint32_t state[4];                /* 0x600a4c5c, 0x600a4ca8, 0x600a4cb0 and 0x600a4cb8 */
    uint32_t desc_at;                 /* 0 if the slot named no descriptor in RAM */
    uint32_t desc[MAC_TX_DESC_WORDS];
    uint32_t frame_at; /* 0 if the descriptor named no buffer in RAM */
    uint8_t frame[MAC_TX_FRAME_SIZE];
};
extern struct mac_tx_seen mac_tx_seen;

/* The slot's PPDU registers as they are now, as mac_tx_seen's ppdu holds them at the start. */
void mac_tx_ppdu(uint32_t slot, uint32_t *words);

#endif
