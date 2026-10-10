/*
 * nRF52840's RADIO in its IEEE 802.15.4 mode; see radio.h.
 *
 * What the registers are and how the mode is set comes from the nRF52840 Product Specification,
 * "IEEE 802.15.4 operation" in the chapter on RADIO:
 * the frame is the length octet, PHR, then the PSDU, which the radio sends after a preamble of four zero octets and the SFD;
 * its last two octets are the FCS, an ITU-T CRC of 16 bits starting from zero, which the radio computes when it sends,
 * and checks, without writing it, when it receives.
 * The radio reaches RAM by DMA: PACKETPTR says where the frame it sends or receives lies.
 */

#include "radio.h"

#define REG(r, off) (*(volatile uint32_t *)((r)->base + (off)))

#define TASKS_TXEN     0x000u
#define TASKS_RXEN     0x004u
#define TASKS_START    0x008u
#define TASKS_DISABLE  0x010u
#define EVENTS_READY   0x100u
#define EVENTS_END     0x10cu
#define EVENTS_DISABLED 0x110u
#define EVENTS_EDEND   0x13cu
#define SHORTS         0x200u
#define INTENSET       0x304u
#define INTENCLR       0x308u
#define CRCSTATUS      0x400u
#define PACKETPTR      0x504u
#define FREQUENCY      0x508u
#define TXPOWER        0x50cu
#define MODE           0x510u
#define PCNF0          0x514u
#define PCNF1          0x518u
#define CRCCNF         0x534u
#define CRCPOLY        0x538u
#define CRCINIT        0x53cu
#define STATE          0x550u
#define EDCNT          0x664u
#define EDSAMPLE       0x668u
#define POWER          0xffcu

#define SHORT_READY_START    (1u << 0)
#define SHORT_DISABLED_RXEN  (1u << 3)
#define SHORT_PHYEND_DISABLE (1u << 20)
#define SHORT_READY_EDSTART (1u << 15)
#define SHORT_EDEND_DISABLE (1u << 16)
#define INT_END      (1u << 3)
#define INT_DISABLED (1u << 4)
#define INT_EDEND    (1u << 15)

#define MODE_IEEE802154_250KBIT 15u
/* A length field of 8 bits, the 32-bit zero preamble, and a length that counts the FCS. */
#define PCNF0_IEEE802154 ((8u << 0) | (2u << 24) | (1u << 26))
#define PCNF1_MAXLEN     RADIO_PSDU_MAX
/* A CRC of two octets, as 802.15.4 computes it, over x^16 + x^12 + x^5 + 1 from zero. */
#define CRCCNF_IEEE802154 ((2u << 0) | (2u << 8))
#define CRCPOLY_ITU_T     0x11021u
#define TXPOWER_0DBM      0u
#define STATE_DISABLED    0u
#define EDSAMPLE_LEVEL    0x7fu
#define CRCSTATUS_OK      1u
#define PHR_LENGTH        0x7fu

/* Link quality is reported as a fraction of 802.15.4's range, and scaled by four, saturating. */
#define LQI_SCALE 4u
#define LQI_MAX   255u
/* The link quality follows a frame of at least this many octets; it is not measured on a shorter one. */
#define LQI_MIN_PSDU 3u

/* A disable takes some microseconds, and a ramp up to sending 130; this bounds the waits well past them. */
#define DISABLE_POLLS 100000u
#define READY_POLLS   100000u

void radio_off(struct radio *r)
{
    REG(r, INTENCLR) = ~0u;
    REG(r, SHORTS) = 0;
    if (REG(r, STATE) != STATE_DISABLED) {
        REG(r, EVENTS_DISABLED) = 0;
        REG(r, TASKS_DISABLE) = 1;
        for (uint32_t i = 0; i < DISABLE_POLLS && REG(r, EVENTS_DISABLED) == 0; i++) {
        }
    }
    REG(r, EVENTS_READY) = 0;
    REG(r, EVENTS_END) = 0;
    REG(r, EVENTS_DISABLED) = 0;
    REG(r, EVENTS_EDEND) = 0;
}

void radio_init(struct radio *r, uint32_t base)
{
    r->base = base;
    /* Off and on again resets the peripheral, its registers to what reset leaves. */
    REG(r, POWER) = 0;
    REG(r, POWER) = 1;
    radio_off(r);
    REG(r, MODE) = MODE_IEEE802154_250KBIT;
    REG(r, PCNF0) = PCNF0_IEEE802154;
    REG(r, PCNF1) = PCNF1_MAXLEN;
    REG(r, CRCCNF) = CRCCNF_IEEE802154;
    REG(r, CRCPOLY) = CRCPOLY_ITU_T;
    REG(r, CRCINIT) = 0;
    REG(r, TXPOWER) = TXPOWER_0DBM;
}

void radio_channel(struct radio *r, uint32_t channel)
{
    /* FREQUENCY counts MHz from 2400. */
    REG(r, FREQUENCY) = RADIO_CHANNEL_MHZ(channel) - 2400u;
}

void radio_energy_start(struct radio *r, uint32_t periods)
{
    radio_off(r);
    /* Ready to receive, the radio detects energy without decoding anything, then turns itself off. */
    REG(r, EDCNT) = periods - 1u;
    REG(r, SHORTS) = SHORT_READY_EDSTART | SHORT_EDEND_DISABLE;
    REG(r, INTENSET) = INT_EDEND;
    REG(r, TASKS_RXEN) = 1;
}

bool radio_energy_done(struct radio *r, uint32_t *level)
{
    if (REG(r, EVENTS_EDEND) == 0) {
        return false;
    }
    REG(r, EVENTS_EDEND) = 0;
    REG(r, INTENCLR) = INT_EDEND;
    *level = REG(r, EDSAMPLE) & EDSAMPLE_LEVEL;
    return true;
}

void radio_receive_start(struct radio *r)
{
    radio_off(r);
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx;
    REG(r, SHORTS) = SHORT_READY_START;
    REG(r, INTENSET) = INT_END;
    REG(r, TASKS_RXEN) = 1;
}

bool radio_received(struct radio *r, struct radio_frame *f)
{
    if (REG(r, EVENTS_END) == 0) {
        return false;
    }
    REG(r, EVENTS_END) = 0;
    uint32_t psdu = r->rx[0] & PHR_LENGTH;
    f->bytes = &r->rx[1];
    f->length = psdu >= RADIO_FCS ? psdu - RADIO_FCS : 0;
    f->fcs_ok = (REG(r, CRCSTATUS) & CRCSTATUS_OK) != 0;
    f->lqi = 0;
    if (psdu >= LQI_MIN_PSDU) {
        uint32_t lqi = r->rx[1u + f->length] * LQI_SCALE;
        f->lqi = lqi > LQI_MAX ? LQI_MAX : lqi;
    }
    return true;
}

void radio_receive_next(struct radio *r)
{
    REG(r, TASKS_START) = 1;
}

void radio_send_start(struct radio *r, const uint8_t *frame, uint32_t length)
{
    radio_off(r);
    r->tx[0] = (uint8_t)(length + RADIO_FCS);
    for (uint32_t i = 0; i < length; i++) {
        r->tx[1u + i] = frame[i];
    }
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->tx;
    /*
     * Ready, it sends; once the last bit is on the air, PHYEND, it turns itself off, which is the line,
     * and from off straight on to receiving, and from ready to receiving the frame.
     */
    REG(r, SHORTS) = SHORT_READY_START | SHORT_PHYEND_DISABLE | SHORT_DISABLED_RXEN;
    REG(r, INTENSET) = INT_DISABLED;
    REG(r, TASKS_TXEN) = 1;
    /*
     * The START that ready makes takes PACKETPTR as it is; from then the register is the next START's,
     * the reception's, which may then point where frames are received.
     */
    for (uint32_t i = 0; i < READY_POLLS && REG(r, EVENTS_READY) == 0; i++) {
    }
    REG(r, EVENTS_READY) = 0;
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx;
}

bool radio_sent(struct radio *r)
{
    if (REG(r, EVENTS_DISABLED) == 0) {
        return false;
    }
    REG(r, EVENTS_DISABLED) = 0;
    /* Receiving from here on: the frame's own END is not a reception's, and no later off turns the radio on again. */
    REG(r, EVENTS_END) = 0;
    REG(r, SHORTS) = SHORT_READY_START;
    REG(r, INTENCLR) = INT_DISABLED;
    REG(r, INTENSET) = INT_END;
    return true;
}
