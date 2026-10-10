#ifndef RVUOS_THREAD_MAC154_H
#define RVUOS_THREAD_MAC154_H

/*
 * IEEE 802.15.4's MAC frames, as far as the nodes of rvuos send and read them:
 * data frames between two short addresses of one PAN, and the acknowledgement of one.
 * A frame here is the MPDU without its FCS, which every radio here computes and checks itself.
 * Every field is little-endian on the air, as 802.15.4 sends it.
 * The ESP32-C6's driver and the nRF52840's program include it alike; it keeps no state.
 */

#include <stdbool.h>
#include <stdint.h>

/* The frame control field's parts, 802.15.4-2006's 7.2.1.1. */
#define MAC154_TYPE_MASK     0x0007u
#define MAC154_TYPE_DATA     0x0001u
#define MAC154_TYPE_ACK      0x0002u
#define MAC154_ACK_REQUEST   0x0020u
#define MAC154_PAN_COMPRESS  0x0040u
#define MAC154_DST_SHORT     0x0800u
#define MAC154_SRC_SHORT     0x8000u
#define MAC154_ADDR_MODES    0xcc00u

/* What every data frame here carries before its payload: the FCF, the sequence, the PAN and the two addresses. */
#define MAC154_DATA_HEADER 9u
#define MAC154_ACK_LENGTH  3u
#define MAC154_FCS         2u /* the FCS's bytes, which follow a frame on the air but not here */

/*
 * The nodes of rvuos's network, until Thread gives them addresses of its own:
 * one PAN, which no network nearby was heard to use, and a short address for each board.
 */
#define MAC154_PAN_RVUOS   0x7276u /* "rv" */
#define MAC154_ADDR_C6     0xc006u
#define MAC154_ADDR_NRF    0x5284u

struct mac154_data {
    uint16_t pan, dst, src;
    uint8_t seq;
    bool ack_request;
    const uint8_t *payload;
    uint32_t length;
};

static inline void mac154_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline uint32_t mac154_get16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/* A data frame of d into out, room bytes, its payload copied after the header; its length, or 0 if it does not fit. */
static inline uint32_t mac154_data_frame(uint8_t *out, uint32_t room, const struct mac154_data *d)
{
    if (room < MAC154_DATA_HEADER || d->length > room - MAC154_DATA_HEADER) {
        return 0;
    }
    uint32_t fcf = MAC154_TYPE_DATA | MAC154_PAN_COMPRESS | MAC154_DST_SHORT | MAC154_SRC_SHORT |
                   (d->ack_request ? MAC154_ACK_REQUEST : 0);
    mac154_put16(&out[0], fcf);
    out[2] = d->seq;
    mac154_put16(&out[3], d->pan);
    mac154_put16(&out[5], d->dst);
    mac154_put16(&out[7], d->src);
    for (uint32_t i = 0; i < d->length; i++) {
        out[MAC154_DATA_HEADER + i] = d->payload[i];
    }
    return MAC154_DATA_HEADER + d->length;
}

/* Whether f, length bytes, is a data frame with short addresses in one PAN, as this file sends them, and what it says. */
static inline bool mac154_data_read(const uint8_t *f, uint32_t length, struct mac154_data *d)
{
    if (length < MAC154_DATA_HEADER) {
        return false;
    }
    uint32_t fcf = mac154_get16(f);
    if ((fcf & MAC154_TYPE_MASK) != MAC154_TYPE_DATA || (fcf & MAC154_ADDR_MODES) != (MAC154_DST_SHORT | MAC154_SRC_SHORT) ||
        !(fcf & MAC154_PAN_COMPRESS)) {
        return false;
    }
    d->ack_request = (fcf & MAC154_ACK_REQUEST) != 0;
    d->seq = f[2];
    d->pan = (uint16_t)mac154_get16(&f[3]);
    d->dst = (uint16_t)mac154_get16(&f[5]);
    d->src = (uint16_t)mac154_get16(&f[7]);
    d->payload = &f[MAC154_DATA_HEADER];
    d->length = length - MAC154_DATA_HEADER;
    return true;
}

/* The acknowledgement of the frame numbered seq, into out, three bytes. */
static inline uint32_t mac154_ack_frame(uint8_t *out, uint8_t seq)
{
    mac154_put16(&out[0], MAC154_TYPE_ACK);
    out[2] = seq;
    return MAC154_ACK_LENGTH;
}

/* Whether f, length bytes, acknowledges the frame numbered seq. */
static inline bool mac154_acks(const uint8_t *f, uint32_t length, uint8_t seq)
{
    return length == MAC154_ACK_LENGTH && (mac154_get16(f) & MAC154_TYPE_MASK) == MAC154_TYPE_ACK && f[2] == seq;
}

#endif
