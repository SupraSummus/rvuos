/*
 * nRF52840's RADIO in its IEEE 802.15.4 mode; see radio.h.
 *
 * What the registers are and how the mode is set comes from the nRF52840 Product Specification,
 * "IEEE 802.15.4 operation" in the chapter on RADIO:
 * the frame is the length octet, PHR, then the PSDU, which the radio sends after a preamble of four zero octets and the SFD;
 * its last two octets are the FCS, an ITU-T CRC of 16 bits starting from zero, which the radio computes when it sends,
 * and checks, without writing it, when it receives.
 * The radio reaches RAM by DMA: PACKETPTR says where the frame it sends or receives lies.
 *
 * A reception is told by CRCOK or CRCERROR, which only a received frame raises, rather than by END,
 * which a frame sent raises too, and clearing which after sending could clear an acknowledgement's.
 *
 * Receiving, the radio starts again by itself as a frame ends, END_START, into what PACKETPTR says then,
 * which a START takes as it comes; so PACKETPTR says the buffer after the one being received into,
 * written once the START into that one has come.
 * END_START is set only while the radio receives, as a frame sent raises END too.
 */

#include "radio.h"

#define REG(r, off) (*(volatile uint32_t *)((r)->base + (off)))

#define TASKS_TXEN     0x000u
#define TASKS_RXEN     0x004u
#define TASKS_START    0x008u
#define TASKS_DISABLE  0x010u
#define TASKS_RSSISTART 0x014u
#define EVENTS_READY   0x100u
#define EVENTS_END     0x10cu
#define EVENTS_DISABLED 0x110u
#define EVENTS_RSSIEND 0x11cu
#define EVENTS_CRCOK   0x130u
#define EVENTS_CRCERROR 0x134u
#define EVENTS_FRAMESTART 0x138u
#define EVENTS_EDEND   0x13cu
#define EVENTS_CCAIDLE 0x144u
#define EVENTS_CCABUSY 0x148u
#define EVENTS_TXREADY 0x154u
#define EVENTS_RXREADY 0x158u
#define SHORTS         0x200u
#define INTENSET       0x304u
#define INTENCLR       0x308u
#define PACKETPTR      0x504u
#define FREQUENCY      0x508u
#define TXPOWER        0x50cu
#define MODE           0x510u
#define PCNF0          0x514u
#define PCNF1          0x518u
#define CRCCNF         0x534u
#define CRCPOLY        0x538u
#define CRCINIT        0x53cu
#define RSSISAMPLE     0x548u
#define STATE          0x550u
#define MODECNF0       0x650u
#define EDCNT          0x664u
#define EDSAMPLE       0x668u
#define CCACTRL        0x66cu
#define POWER          0xffcu

#define SHORT_READY_START     (1u << 0)
#define SHORT_DISABLED_RXEN   (1u << 3)
#define SHORT_END_START       (1u << 5)
#define SHORT_RXREADY_CCASTART (1u << 11)
#define SHORT_CCAIDLE_TXEN    (1u << 12)
#define SHORT_CCABUSY_DISABLE (1u << 13)
#define SHORT_READY_EDSTART   (1u << 15)
#define SHORT_EDEND_DISABLE   (1u << 16)
#define SHORT_TXREADY_START   (1u << 18)
#define SHORT_RXREADY_START   (1u << 19)
#define SHORT_PHYEND_DISABLE  (1u << 20)
#define INT_DISABLED (1u << 4)
#define INT_CRCOK    (1u << 12)
#define INT_CRCERROR (1u << 13)
#define INT_EDEND    (1u << 15)
#define INT_CCABUSY  (1u << 18)
#define INT_TXREADY  (1u << 21)

#define MODE_IEEE802154_250KBIT 15u
/* A length field of 8 bits, the 32-bit zero preamble, and a length that counts the FCS. */
#define PCNF0_IEEE802154 ((8u << 0) | (2u << 24) | (1u << 26))
#define PCNF1_MAXLEN     RADIO_PSDU_MAX
/* A CRC of two octets, as 802.15.4 computes it, over x^16 + x^12 + x^5 + 1 from zero. */
#define CRCCNF_IEEE802154 ((2u << 0) | (2u << 8))
#define CRCPOLY_ITU_T     0x11021u
#define STATE_DISABLED    0u
#define EDSAMPLE_LEVEL    0x7fu
#define RSSISAMPLE_DBM    0x7fu
#define PHR_LENGTH        0x7fu
/* A ramp up of 40 us rather than 130, leaving a driver woken by a frame more of the 192 us before its acknowledgement. */
#define MODECNF0_RU_FAST 1u
/* The assessment by energy alone, 802.15.4's first mode, busy at the level in CCAEDTHRES or more. */
#define CCACTRL_MODE_MASK   0x7u
#define CCACTRL_MODE_ED     0u
#define CCACTRL_EDTHRES_POS 8u
#define CCACTRL_EDTHRES     (0xffu << CCACTRL_EDTHRES_POS)

/* Link quality is reported as a fraction of 802.15.4's range, and scaled by four, saturating. */
#define LQI_SCALE 4u
#define LQI_MAX   255u
/* The link quality follows a frame of at least this many octets; it is not measured on a shorter one. */
#define LQI_MIN_PSDU 3u

/* A disable, or a ramp up, 40 us, takes some microseconds; this bounds the wait well past them. */
#define DISABLE_POLLS 100000u
#define RAMP_POLLS    100000u
/* A sample of the received power takes a quarter of a microsecond; this bounds the wait well past it. */
#define RSSI_POLLS    1000u

/* Receiving: a START once ready, and again as each frame ends. */
#define SHORTS_RX (SHORT_READY_START | SHORT_END_START)

/* The powers the chip sends at, in dBm as TXPOWER takes them, most first; its byte is the dBm's two's complement. */
static const int8_t powers[] = { 8, 7, 6, 5, 4, 3, 2, 0, -4, -8, -12, -16, -20, -40 };
#define POWERS (sizeof(powers) / sizeof(powers[0]))

/* Off the air, waited for, whatever the radio did; its events but DISABLED are left as they are. */
static void disable(struct radio *r)
{
    REG(r, SHORTS) = 0;
    if (REG(r, STATE) != STATE_DISABLED) {
        REG(r, EVENTS_DISABLED) = 0;
        REG(r, TASKS_DISABLE) = 1;
        for (uint32_t i = 0; i < DISABLE_POLLS && REG(r, EVENTS_DISABLED) == 0; i++) {
        }
    }
}

void radio_off(struct radio *r)
{
    REG(r, INTENCLR) = ~0u;
    disable(r);
    r->doubt = false;
    r->again = false;
    REG(r, EVENTS_READY) = 0;
    REG(r, EVENTS_END) = 0;
    REG(r, EVENTS_DISABLED) = 0;
    REG(r, EVENTS_CRCOK) = 0;
    REG(r, EVENTS_CRCERROR) = 0;
    REG(r, EVENTS_FRAMESTART) = 0;
    REG(r, EVENTS_EDEND) = 0;
    REG(r, EVENTS_CCAIDLE) = 0;
    REG(r, EVENTS_CCABUSY) = 0;
    REG(r, EVENTS_TXREADY) = 0;
    REG(r, EVENTS_RXREADY) = 0;
}

void radio_init(struct radio *r, uint32_t base, uint32_t cca_level)
{
    r->base = base;
    r->rx_at = 0;
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
    REG(r, MODECNF0) |= MODECNF0_RU_FAST;
    REG(r, CCACTRL) = (REG(r, CCACTRL) & ~(CCACTRL_MODE_MASK | CCACTRL_EDTHRES)) | CCACTRL_MODE_ED |
                      (cca_level << CCACTRL_EDTHRES_POS & CCACTRL_EDTHRES);
    radio_power(r, 0);
}

void radio_channel(struct radio *r, uint32_t channel)
{
    /* FREQUENCY counts MHz from 2400. */
    REG(r, FREQUENCY) = RADIO_CHANNEL_MHZ(channel) - 2400u;
}

void radio_mhz(struct radio *r, uint32_t mhz)
{
    REG(r, FREQUENCY) = mhz - 2400u;
}

void radio_carrier(struct radio *r)
{
    radio_off(r);
    /* Ramped up with nothing to start, the transmitter idles on the air sending its carrier. */
    REG(r, TASKS_TXEN) = 1;
}

int32_t radio_power(struct radio *r, int32_t dbm)
{
    uint32_t i = 0;
    while (i + 1u < POWERS && powers[i] > dbm) {
        i++;
    }
    REG(r, TXPOWER) = (uint8_t)powers[i];
    return powers[i];
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

/* The buffer after the one the radio receives into, for the START as that frame ends. */
static void receive_ahead(struct radio *r)
{
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx[(r->rx_at + 1u) % RADIO_RX_BUFFERS];
}

/*
 * Once the radio, ramping up to receive, is ready, and the START that ready makes has taken PACKETPTR,
 * the buffer after; if it was ready before, it has started already.
 */
static void receive_ready(struct radio *r)
{
    for (uint32_t i = 0; i < RAMP_POLLS && REG(r, EVENTS_RXREADY) == 0; i++) {
    }
    receive_ahead(r);
}

void radio_receive_start(struct radio *r)
{
    radio_off(r);
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx[r->rx_at];
    REG(r, SHORTS) = SHORTS_RX;
    REG(r, INTENSET) = INT_CRCOK | INT_CRCERROR;
    REG(r, TASKS_RXEN) = 1;
    receive_ready(r);
}

bool radio_received(struct radio *r, struct radio_frame *f)
{
    /* Each event stands for every frame that ended since the last call, so one bad makes them all doubtful. */
    bool ok = REG(r, EVENTS_CRCOK) != 0, bad = REG(r, EVENTS_CRCERROR) != 0;
    if (!ok && !bad) {
        return false;
    }
    const uint8_t *rx = r->rx[r->rx_at];
    bool doubt = r->doubt;
    r->rx_at = (r->rx_at + 1u) % RADIO_RX_BUFFERS;
    REG(r, EVENTS_END) = 0;
    if (r->again) {
        /*
         * The frame was the first after a send, and ended before radio_sending set END_START,
         * so the radio is ready, or receives again into the frame's own buffer:
         * it is stopped, what ended meanwhile forgotten, and started into the buffer after.
         */
        r->again = false;
        disable(r);
        REG(r, EVENTS_CRCOK) = 0;
        REG(r, EVENTS_CRCERROR) = 0;
        REG(r, EVENTS_FRAMESTART) = 0;
        REG(r, EVENTS_RXREADY) = 0;
        REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx[r->rx_at];
        REG(r, SHORTS) = SHORTS_RX;
        REG(r, TASKS_RXEN) = 1;
        receive_ready(r);
        r->doubt = false;
    } else {
        /*
         * The radio receives into the buffer after already, and is told the one after that,
         * which the frame the last call returned leaves free; then the events are cleared.
         * A frame that ends between the two loses its events to the clear,
         * and the next call would return it with the verdict of the frame after it;
         * END, cleared before, tells of it, and the next frame returned is taken as bad.
         */
        receive_ahead(r);
        REG(r, EVENTS_CRCOK) = 0;
        REG(r, EVENTS_CRCERROR) = 0;
        REG(r, EVENTS_FRAMESTART) = 0;
        r->doubt = REG(r, EVENTS_END) != 0;
    }
    uint32_t psdu = rx[0] & PHR_LENGTH;
    f->bytes = &rx[1];
    f->length = psdu >= RADIO_FCS ? psdu - RADIO_FCS : 0;
    f->fcs_ok = ok && !bad && !doubt;
    f->lqi = 0;
    if (psdu >= LQI_MIN_PSDU) {
        uint32_t lqi = rx[1u + f->length] * LQI_SCALE;
        f->lqi = lqi > LQI_MAX ? LQI_MAX : lqi;
    }
    return true;
}

uint32_t radio_rssi(struct radio *r)
{
    REG(r, EVENTS_RSSIEND) = 0;
    REG(r, TASKS_RSSISTART) = 1;
    for (uint32_t i = 0; i < RSSI_POLLS && REG(r, EVENTS_RSSIEND) == 0; i++) {
    }
    return REG(r, EVENTS_RSSIEND) != 0 ? REG(r, RSSISAMPLE) & RSSISAMPLE_DBM : RADIO_RSSI_NONE;
}

bool radio_receiving(struct radio *r)
{
    return REG(r, EVENTS_FRAMESTART) != 0 && REG(r, EVENTS_CRCOK) == 0 && REG(r, EVENTS_CRCERROR) == 0;
}

/* The frame into the radio's own bytes, the length first. */
static void frame_put(struct radio *r, const uint8_t *frame, uint32_t length)
{
    r->tx[0] = (uint8_t)(length + RADIO_FCS);
    for (uint32_t i = 0; i < length; i++) {
        r->tx[1u + i] = frame[i];
    }
}

/*
 * Once the radio has started sending, whatever it was told to do before: once the last bit is on the air, PHYEND,
 * it turns itself off, which is the line, and from off straight on to receiving, into the buffer next in turn.
 * The START that ready made took PACKETPTR as it was; from then the register is the next START's, the reception's.
 * The assessment before sending was ready to receive too, so its ready is cleared for the reception's.
 */
static void then_receive(struct radio *r)
{
    REG(r, EVENTS_RXREADY) = 0;
    REG(r, SHORTS) = SHORT_PHYEND_DISABLE | SHORT_DISABLED_RXEN | SHORT_RXREADY_START;
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->rx[r->rx_at];
    REG(r, INTENCLR) = INT_TXREADY | INT_CCABUSY;
    REG(r, INTENSET) = INT_DISABLED;
}

void radio_send_start(struct radio *r, const uint8_t *frame, uint32_t length)
{
    radio_off(r);
    frame_put(r, frame, length);
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->tx;
    REG(r, SHORTS) = SHORT_TXREADY_START | SHORT_PHYEND_DISABLE;
    REG(r, INTENSET) = INT_TXREADY;
    REG(r, TASKS_TXEN) = 1;
}

void radio_send_clear(struct radio *r, const uint8_t *frame, uint32_t length)
{
    radio_off(r);
    frame_put(r, frame, length);
    REG(r, PACKETPTR) = (uint32_t)(uintptr_t)r->tx;
    /*
     * Ready to receive, the radio assesses the channel; clear, it turns straight to sending, busy, it turns off.
     * Receiving after the frame waits for then_receive: with DISABLED_RXEN set now, busy would assess again and again.
     */
    REG(r, SHORTS) = SHORT_RXREADY_CCASTART | SHORT_CCAIDLE_TXEN | SHORT_CCABUSY_DISABLE | SHORT_TXREADY_START |
                     SHORT_PHYEND_DISABLE;
    REG(r, INTENSET) = INT_CCABUSY | INT_TXREADY;
    REG(r, TASKS_RXEN) = 1;
}

enum radio_send radio_sending(struct radio *r)
{
    if (REG(r, EVENTS_CCABUSY) != 0) {
        radio_off(r);
        return RADIO_BUSY;
    }
    if (REG(r, EVENTS_TXREADY) != 0) {
        REG(r, EVENTS_TXREADY) = 0;
        then_receive(r);
    }
    if (REG(r, EVENTS_DISABLED) == 0) {
        return RADIO_GOING;
    }
    REG(r, EVENTS_DISABLED) = 0;
    REG(r, EVENTS_FRAMESTART) = 0;
    REG(r, INTENCLR) = INT_DISABLED;
    REG(r, INTENSET) = INT_CRCOK | INT_CRCERROR;
    /* Off still, the frame ended before then_receive came, and the radio is turned to receiving here. */
    if (REG(r, STATE) == STATE_DISABLED) {
        r->late++;
        REG(r, TASKS_RXEN) = 1;
    } else if (REG(r, EVENTS_CRCOK) != 0 || REG(r, EVENTS_CRCERROR) != 0) {
        /*
         * A frame was received already, with END_START not yet set,
         * and PHYEND may have turned the radio off and on again into the frame's own buffer:
         * the radio is left to radio_received, which the line has already come for, to start again.
         */
        r->again = true;
        REG(r, SHORTS) = SHORT_READY_START;
        return RADIO_SENT;
    }
    /*
     * Receiving from here on, and no later off turns the radio on again;
     * the buffer after is told before END_START is set, so a frame ending meanwhile is not received over.
     */
    receive_ready(r);
    REG(r, SHORTS) = SHORTS_RX;
    return RADIO_SENT;
}
