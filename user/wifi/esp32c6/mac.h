#ifndef RVUOS_WIFI_MAC_H
#define RVUOS_WIFI_MAC_H

/*
 * The Wi-Fi MAC driven by the driver's own code rather than by Espressif's libraries,
 * on the way to a driver that leaves them the PHY alone; see TODO.md.
 * The driver's own path reaches the MAC through this file alone, so that giving the libraries up rewrites mac.c and
 * macstart.c alone, the second holding the bring-up's register sequences.
 * It receives and retunes the radio, once the libraries have brought the MAC up:
 * mac_sniffer has the MAC pass on every frame heard on the channel, or the station's alone,
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
    int decrypted;    /* the MAC's cipher took it: the payload is in the clear and the Protected bit is kept,
                         so a frame is the cipher's only with that bit set and this flag set; see ccmp_frame_kind */
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

/*
 * The MAC passes on every management and data frame heard on the channel, whoever it is to, while the sniffer is on,
 * as a scan and listen= want, and only the station's while it is off, as the station's receiving wants.
 * Of control frames it passes none: they pass by a mask of their subtypes, which the driver leaves empty, as the
 * libraries' default did.
 */
void mac_sniffer(int on);

/* 0, or the step that failed. */
const char *mac_rx_take(mac_heard_fn *heard);
void mac_rx_give_back(void);

/*
 * The MAC's receive off for the bring-up, in place of the libraries' hal_mac_rx_disable: RX_CTRL's enable and its
 * reload cleared, as the MAC is reset, before the driver's own list is taken.
 */
void mac_rx_off(void);

/*
 * The MAC's tx-block hold cleared, as the libraries' hal_mac_init clears it when the interface is brought up
 * (their ic_mac_init) and mac_channel clears it after a retune.
 */
void mac_tx_block_clear(void);

/*
 * The MAC's receive on, in place of the libraries' ic_enable_rx: RX_CTRL's enable bit set, after the bring-up has
 * set the receive policy.
 */
void mac_rx_on(void);

/*
 * The register sequences of the libraries' bring-up, written out in macstart.c, which holds their detail:
 * their hal_init's words and groups, their hal_crypto_init's cipher words, and their coex PTI.
 * main.c calls them where the libraries' hal_init called the groups, in drv_mac_config's order; see macstart.c.
 */
void mac_config_start(void);
void mac_rx_policy_word(uint32_t iface);
void mac_config_finish(void);
void mac_rx_set_policy(uint32_t iface);
void mac_queues_init(void);
void mac_rx_base_init(void);
void mac_low_rate_disable(void);
void mac_crypto_init(void);
void mac_coex_pti_init(void);
void mac_rx_active_pti(uint32_t pti);
void mac_rx_ack_pti(uint32_t pti);
void mac_wifi_default_pti(uint32_t pti);

/*
 * The own station's receiving, in place of the libraries': the address and the BSSID (mac_station),
 * a key entry the MAC's cipher holds (mac_key_set), and the station-mode receive policy (mac_receive),
 * written with the sniffer off, before the driver's list is taken; see mac.c.
 */
void mac_key_set(uint32_t entry, const uint8_t addr[6], uint8_t id, const uint8_t tk[16]);
void mac_key_clear(uint32_t entry);
void mac_receive(uint16_t aid);

/*
 * The default receive policy the bring-up writes before anything is joined, in place of the libraries'
 * wifi_set_rx_policy(0): both interfaces' addresses from the factory MAC and the soft AP's, and their filters
 * left invalid and empty; see mac.c.
 */
void mac_default_policy(const uint8_t mac[6]);

/*
 * The MAC's own station address and the access point's. The libraries set both when their station associates;
 * the own path associates by its own code, so it must set them, or the MAC matches no unicast frame to the station.
 */
void mac_station(const uint8_t sta[6], const uint8_t bssid[6]);

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
 * and mac_tx gives a management or a data frame the station's next sequence number, from one counter,
 * as a station that does not do QoS numbers them.
 * The driver programs a slot the libraries' lmac names, arms it, and finishes the frame itself:
 * it clears the hardware txq state's completion bit, which lets the slot's arm bits clear, and on a timeout's
 * or a collision's bit disarms the slot and fails; the caller may send it again.
 * It works with the libraries' interrupt or the driver's own. See mac.c.
 * Any of the driver's threads may send, the station's and the link's: one waits while another's frame goes,
 * since there is one buffer and one slot.
 */
const char *mac_tx(const uint8_t *frame, uint32_t len);

/*
 * The driver's own stop, in place of Espressif's esp_wifi_stop: it turns the vif's receive off, holds the MAC
 * still and closes the PHY. Call it where the library's stop was called, in the trace window and when a run
 * turns the radio off; why it can leave some of the library's writes out is in mac.c.
 */
void mac_stop(void);

#endif
