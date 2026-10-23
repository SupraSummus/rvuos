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
 * Only a disable or a ramp up, some microseconds, is waited for in the call that asks it.
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
 * a level of 0 to 127.
 * The product specification converts it to dBm with constants of its electrical specification;
 * beside the ESP32-C6's survey of the same channels, which reads dBm, a level is the dBm above -94, to a few dB.
 */
#define RADIO_ENERGY_PERIOD_US 128u
#define RADIO_LEVEL_DBM        (-94)

/*
 * The receive buffers, which take turns:
 * the one a frame was last returned in, which stays as it is until the next call that returns one,
 * the one the radio receives into, and the one after, which the radio starts receiving into by itself
 * as soon as the frame before ends, without waiting for the driver.
 */
#define RADIO_RX_BUFFERS 3u

struct radio {
    uint32_t base; /* the registers, mapped at their own address */
    /*
     * Where the radio writes what it receives, as the product specification lays it out:
     * the length, the frame without its FCS, which the radio checks and leaves out, then a byte of link quality.
     * The radio writes it by DMA while it receives, so it is read only once a frame has ended.
     */
    uint8_t rx[RADIO_RX_BUFFERS][1u + RADIO_PSDU_MAX + 1u] __attribute__((aligned(4)));
    uint32_t rx_at; /* the buffer the radio receives into, and the next frame returned comes from */
    /* What it sends, the length first, which counts the FCS the radio appends. */
    uint8_t tx[1u + RADIO_PSDU_MAX] __attribute__((aligned(4)));
    uint32_t late; /* frames sent that ended before the driver woke to set the radio to receive after them */
    bool doubt;    /* the next frame returned may carry another's verdict, see radio_received */
    bool again;    /* the frame received after a send ended before receiving was set up, see radio_sending */
};

/* What a received frame was: its length without the FCS, its link quality, and whether its FCS was right. */
struct radio_frame {
    const uint8_t *bytes; /* the frame from its frame control field, length bytes */
    uint32_t length;
    uint32_t lqi;         /* 0 to 255, as 802.15.4 scales it */
    bool fcs_ok;
};

/*
 * Powers the radio on, off the air, set for 802.15.4: its packet layout, its CRC and 0 dBm,
 * its fast ramp up, and its assessment of the channel, which finds it busy at cca_level or more.
 */
void radio_init(struct radio *r, uint32_t base, uint32_t cca_level);

/* Off the air, and its line quiet. */
void radio_off(struct radio *r);

/* The channel the next step uses, RADIO_CHANNEL_FIRST to RADIO_CHANNEL_LAST. */
void radio_channel(struct radio *r, uint32_t channel);

/* The power the radio sends at from now on: the most of the chip's steps that is no more than dbm; it returns that. */
int32_t radio_power(struct radio *r, int32_t dbm);

/* Starts detecting energy on the channel for periods of RADIO_ENERGY_PERIOD_US, then off the air. */
void radio_energy_start(struct radio *r, uint32_t periods);
/* After the line: whether the detection ended, and the most energy it saw, 0 to 127. */
bool radio_energy_done(struct radio *r, uint32_t *level);

/* Starts receiving on the channel, a frame at a time. */
void radio_receive_start(struct radio *r);
/*
 * After the line: whether a frame ended, and what it was;
 * the frame stays as it is until the next call that returns one.
 * The radio receives the next frame by itself as soon as one ends, into the buffer after,
 * so how soon the driver wakes for a frame matters only once the next has ended too:
 * the radio is one buffer ahead of the driver, and receives a third frame into the second's buffer.
 * The second is lost then, and its verdict is the first's too, so a frame is told whole
 * only if every frame that ended with it was; a whole one is now and then told otherwise, never the reverse.
 */
bool radio_received(struct radio *r, struct radio_frame *f);
/*
 * While receiving: the power the radio hears on the channel now, as a sample of a quarter of a microsecond,
 * in dB below a milliwatt, 0 to 127, as the product specification has RSSISAMPLE say it;
 * RADIO_RSSI_NONE if the sample never ended, the radio not receiving.
 */
#define RADIO_RSSI_MAX  127u
#define RADIO_RSSI_NONE 0xffu
uint32_t radio_rssi(struct radio *r);

/*
 * Whether a frame is arriving: its length heard, its end not yet, which a send would cut off.
 * A frame whose length came before the call that returned the frame before it goes untold.
 */
bool radio_receiving(struct radio *r);

/*
 * Starts sending frame, length bytes without its FCS, at once, with no assessment of the channel:
 * from receiving, the radio turns round as fast as it can, so an acknowledgement goes out in 802.15.4's time.
 */
void radio_send_start(struct radio *r, const uint8_t *frame, uint32_t length);

/* The same, after an assessment of the channel, eight symbols, which finds it clear or busy. */
void radio_send_clear(struct radio *r, const uint8_t *frame, uint32_t length);

/* How far a frame being sent has come. */
enum radio_send {
    RADIO_GOING, /* still being assessed or sent */
    RADIO_BUSY,  /* the channel was busy, and the radio is off the air, which radio_receive_start ends */
    RADIO_SENT,  /* gone, and the radio receives */
};

/*
 * After the line: how far the frame has come.
 * The line comes once the radio is ready to send, and the call sets it to receive once the frame's last bit has gone,
 * so an acknowledgement 192 us later is heard, which a driver woken by the frame's end would turn round for too late.
 * The wake has to come while the frame is on the air, 352 us for an acknowledgement, the shortest;
 * after a later one the radio turns to receiving only then, and counts the frame late.
 * The radio_received after RADIO_SENT is the first frame heard since.
 */
enum radio_send radio_sending(struct radio *r);

#endif
