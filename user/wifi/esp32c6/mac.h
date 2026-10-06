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
 * A frame sent by the driver's own code; 0, or the step that failed, a timeout or a collision among them.
 * The frame is the 802.11 frame without its checksum, which the MAC appends.
 * The driver programs a slot the libraries' lmac names, arms it, and finishes the frame itself:
 * it clears the hardware txq state's completion bit, which lets the slot's arm bits clear, and on a timeout's
 * or a collision's bit disarms the slot and fails; the caller may send it again.
 * It works with the libraries' interrupt or the driver's own. See mac.c.
 */
const char *mac_tx(const uint8_t *frame, uint32_t len);

#endif
