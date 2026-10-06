#ifndef RVUOS_WIFI_MAC_H
#define RVUOS_WIFI_MAC_H

/*
 * The Wi-Fi MAC driven by the driver's own code rather than by Espressif's libraries,
 * on the way to a driver that leaves them the PHY alone; see TODO.md.
 * So far it receives and retunes the radio, once the libraries have brought the MAC up and set its filters:
 * mac_rx_take gives the MAC a list of descriptors of its own and takes its interrupt,
 * so that each frame goes to heard, in the interrupt's thread, and none to the libraries;
 * mac_rx_give_back stops the MAC receiving and gives the libraries their interrupt back, before they stop.
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
};
extern struct mac_rx_counts mac_rx_counts;

/* 0, or the step that failed. */
const char *mac_rx_take(mac_heard_fn *heard);
void mac_rx_give_back(void);

/* The radio on another channel, 1 to 13, as the libraries' chm_phy_change_channel retunes it, the MAC held meanwhile. */
void mac_channel(uint32_t channel);

#endif
