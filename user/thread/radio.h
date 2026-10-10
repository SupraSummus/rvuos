#ifndef RVUOS_THREAD_RADIO_H
#define RVUOS_THREAD_RADIO_H

/*
 * An IEEE 802.15.4 radio, the 250 kbit/s O-QPSK one at 2.4 GHz, as a driver in user mode drives it:
 * nRF52840's RADIO, in radio.c.
 * Its channels are 802.15.4's, 11 to 26, 5 MHz apart from 2405 MHz.
 *
 * The radio raises its line for the one event the step at hand waits for;
 * the driver arms the line's Irq, waits on its notification and then asks what came,
 * so a step is started by one call and ended by another.
 * Nothing here keeps a global: the radio's state is handed in.
 */

#include <stdbool.h>
#include <stdint.h>

#define RADIO_CHANNEL_FIRST 11u
#define RADIO_CHANNEL_LAST  26u
#define RADIO_PSDU_MAX      127u /* a frame's bytes after the length, its two of FCS among them */
#define RADIO_FCS           2u

/* The centre frequency of a channel in MHz. */
#define RADIO_CHANNEL_MHZ(c) (2405u + 5u * ((c) - RADIO_CHANNEL_FIRST))

/*
 * An energy detection lasts periods of 128 us, eight symbols, and reports the most any period saw,
 * a level of 0 to 127 which the product specification converts to dBm with ED_RSSIOFFS and ED_RSSISCALE.
 */
#define RADIO_ENERGY_PERIOD_US 128u

struct radio {
    uint32_t base; /* the registers, mapped at their own address */
    /*
     * Where the radio writes what it receives, as the product specification lays it out:
     * the length, the frame without its FCS, which the radio checks and leaves out, then a byte of link quality.
     * The radio writes it by DMA while it receives, so it is read only once a frame has ended.
     */
    uint8_t rx[1u + RADIO_PSDU_MAX + 1u] __attribute__((aligned(4)));
    /* What it sends, the length first, which counts the FCS the radio appends. */
    uint8_t tx[1u + RADIO_PSDU_MAX] __attribute__((aligned(4)));
};

/* What a received frame was: its length without the FCS, its link quality, and whether its FCS was right. */
struct radio_frame {
    const uint8_t *bytes; /* the frame from its frame control field, length bytes */
    uint32_t length;
    uint32_t lqi;         /* 0 to 255, as 802.15.4 scales it */
    bool fcs_ok;
};

/* Powers the radio on, off the air, set for 802.15.4: its packet layout, its CRC and 0 dBm. */
void radio_init(struct radio *r, uint32_t base);

/* Off the air, and its line quiet. */
void radio_off(struct radio *r);

/* The channel the next step uses, RADIO_CHANNEL_FIRST to RADIO_CHANNEL_LAST. */
void radio_channel(struct radio *r, uint32_t channel);

/* Starts detecting energy on the channel for periods of RADIO_ENERGY_PERIOD_US, then off the air. */
void radio_energy_start(struct radio *r, uint32_t periods);
/* After the line: whether the detection ended, and the most energy it saw, 0 to 127. */
bool radio_energy_done(struct radio *r, uint32_t *level);

/* Starts receiving on the channel, a frame at a time. */
void radio_receive_start(struct radio *r);
/*
 * After the line: whether a frame ended, and what it was.
 * The radio then listens without keeping what it hears, so the frame stays as it is
 * until radio_receive_next has it receive the next into the same bytes.
 */
bool radio_received(struct radio *r, struct radio_frame *f);
void radio_receive_next(struct radio *r);

/*
 * Starts sending frame, length bytes without its FCS, at once, with no assessment of the channel:
 * from receiving, the radio turns round as fast as it can, so an acknowledgement goes out in 802.15.4's time.
 * Once the frame's last bit has gone, the radio receives again by itself, into the same bytes as ever,
 * so an acknowledgement 192 us later is heard, which a driver woken through the kernel would turn round for too late.
 * The radio_received after radio_sent is the first frame heard since.
 */
void radio_send_start(struct radio *r, const uint8_t *frame, uint32_t length);
/* After the line: whether the frame has gone, and the radio receives. */
bool radio_sent(struct radio *r);

#endif
