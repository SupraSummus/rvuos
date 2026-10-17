/*
 * The ESP32-C6's IEEE 802.15.4 radio, a MAC of its own beside Wi-Fi's, at 0x600A3000 in the modem; see i154.h.
 *
 * It comes up as ESP-IDF's esp_ieee802154_enable brings it, components/ieee802154/:
 * the modem's clocks of 802.15.4 and of the BT baseband it shares, as modem_clock_impl.c enables them,
 * the PHY on, calibrated by libphy.a as for Wi-Fi, but without Wi-Fi's part of it, phy.c's drv_phy_on,
 * the baseband by the driver's own btbb_init, Espressif's bt_bb_v2_init_cmplx taken from libbtbb.a one function at a
 * time, and the MAC reset and set as ieee802154_mac_init sets it,
 * its delay before sending again the archive's ieee802154_txon_delay_set, until the driver's own replaces it.
 * Everything RF is the libraries': the channel and the power are numbers the MAC hands them,
 * the power an index into the archive's own table, until the driver's own replaces it.
 * The MAC does the rest in hardware: it sends a frame, with a clear channel assessment first,
 * receives one through its address filter, acknowledges one that asks, and waits for the acknowledgement of its own.
 * It assesses once and waits once, so the backoffs and retries of 802.15.4's CSMA-CA are the driver's, mac154.h's,
 * each backoff timed in microseconds by the MAC's timer0, whose event comes on the MAC's line.
 * Its registers are ESP-IDF's soc/esp32c6/register/soc/ieee802154_reg.h and ieee802154_struct.h,
 * its commands and events ieee802154_common_ll.h's.
 *
 * Its line, the ZB_MAC source's, is bound to a notification of the driver's own, with a timer beside it,
 * and the driver's first thread waits on both, answering the root task's checks as it wakes.
 * Frames are the MPDU after a length octet, as the MAC's DMA takes and gives them, see mac154.h:
 * on receiving, the two bytes of the FCS are the frame's RSSI in dBm and its LQI.
 */

#include <stdbool.h>
#include <stdint.h>

#include "i154.h"
#include "btbb.h"
#include "lib/child.h"
#include "lib/libc.h"
#include "osi.h"
#include "thread/mac154.h"

#define REG(addr) (*(volatile uint32_t *)(addr))

/* The modem's clocks and resets of 802.15.4, ESP-IDF's soc/esp32c6/include/modem/. */
#define MODEM_SYSCON_CLK_CONF  (MODEM_SYSCON_BASE + 0x04u)
#define MODEM_SYSCON_RST_CONF  (MODEM_SYSCON_BASE + 0x10u)
#define MODEM_SYSCON_CLK_CONF1 (MODEM_SYSCON_BASE + 0x14u)
#define MODEM_LPCON_CLK_CONF   (MODEM_LPCON_BASE + 0x18u)
#define CLK_ETM_EN           (1u << 22)
#define CLK_ZB_APB_EN        (1u << 23)
#define CLK_ZB_MAC_EN        (1u << 24)
#define CLK_MODEM_SEC_APB_EN (1u << 28)
#define CLK_BT_APB_EN        (1u << 17)
#define CLK_BT_EN            (1u << 18)
#define RST_ZBMAC            (1u << 24)
#define CLK_COEX_EN          (1u << 1)

/* The MAC's registers. */
#define I154_BASE (MODEM_BASE + 0x3000u)
#define I154(off) REG(I154_BASE + (off))
#define COMMAND         0x000u
#define CTRL_CFG        0x004u
#define INF0_SHORT_ADDR 0x008u
#define INF0_PAN_ID     0x00cu
#define INF0_EXT_ADDR0  0x010u
#define INF0_EXT_ADDR1  0x014u
#define CHANNEL         0x048u
#define TX_POWER        0x04cu
#define ED_DURATION     0x050u
#define ED_CFG          0x054u
#define ACK_TIMEOUT     0x05cu
#define ACK_TIMEOUT_MASK 0xffffu /* in symbols */
#define EVENT_EN        0x060u
#define EVENT_STATUS    0x064u
#define RX_ABORT_EN     0x068u
#define COEX_PTI        0x070u
#define TX_ABORT_EN     0x078u
#define RX_STATUS       0x080u
#define TX_STATUS       0x084u
#define TXDMA_ADDR      0x0d0u
#define RXDMA_ADDR      0x0e0u
#define TIMER0_THRESHOLD 0x0a8u /* in microseconds */
#define CRC_ERROR_CNT      0x148u
#define RX_FILTER_FAIL_CNT 0x154u

#define CMD_RX_START     0x42u
#define CMD_CCA_TX_START 0x43u
#define CMD_ED_START     0x44u
#define CMD_STOP         0x45u
#define CMD_TIMER0_START 0x4cu
#define CMD_TIMER0_STOP  0x4du

#define CFG_AUTO_ACK_TX   (1u << 0)
#define CFG_AUTO_ENHACK   (1u << 1)
#define CFG_AUTO_ACK_RX   (1u << 3)
#define CFG_COORDINATOR   (1u << 6)
#define CFG_PROMISCUOUS   (1u << 7)
#define CFG_MULTIPAN_MASK (0xfu << 28)
#define CFG_MULTIPAN_INF0 (1u << 28) /* the first interface's PAN and addresses filter what it receives */

#define EV_TX_DONE     (1u << 0)
#define EV_RX_DONE     (1u << 1)
#define EV_ACK_TX_DONE (1u << 2)
#define EV_ACK_RX_DONE (1u << 3)
#define EV_RX_ABORT    (1u << 4)
#define EV_TX_ABORT    (1u << 5)
#define EV_ED_DONE     (1u << 6)
#define EV_TIMER0      (1u << 8) /* timer0 reached its threshold */
#define EV_ALL (EV_TX_DONE | EV_RX_DONE | EV_ACK_TX_DONE | EV_ACK_RX_DONE | EV_RX_ABORT | EV_TX_ABORT | EV_ED_DONE | \
                EV_TIMER0)

/* A frame is arriving once its SFD is heard, as ESP-IDF's ieee802154_ll_is_current_rx_frame reads RX_STATUS. */
#define RX_STATE(status) (((status) >> 16) & 0x7u)
#define RX_STATE_SFD     1u

/* An abort's reason, as TX_STATUS gives it, and its bit in the abort events' enables, the reason less one. */
#define ABORT_REASON(status)       (((status) >> 4) & 0x1fu)
#define ABORT_BIT(reason)          (1u << ((reason) - 1u))
#define RX_ABORT_TX_ACK_TIMEOUT    16u
#define RX_ABORT_TX_ACK_COEX_BREAK 18u
#define TX_ABORT_RX_ACK_TIMEOUT    16u
#define TX_ABORT_TX_COEX_BREAK     18u
#define TX_ABORT_SECURITY_ERROR    19u
#define TX_ABORT_CCA_FAILED        24u
#define TX_ABORT_CCA_BUSY          25u

#define ED_CCA_THRESHOLD (0xffu << 0)
#define ED_SAMPLE_AVG    (1u << 13)
#define ED_CCA_MODE      (3u << 14)
#define ED_CCA_MODE_ED   (1u << 14)
#define ED_RSS(cfg)      ((int8_t)((cfg) >> 16))
#define ED_DURATION_MASK 0xffffffu

/* No coexistence: its frames and acknowledgements at priority 3, as ieee802154_ll_disable_coex leaves them. */
#define COEX_PTI_MASK 0xffu
#define COEX_PTI_NONE 0x33u

/* The channel's register, in MHz from 2402, ESP-IDF's ieee802154_channel_to_freq. */
#define CHANNEL_FIRST 11u
#define CHANNEL_LAST  26u
#define FREQ(c)       (((c) - CHANNEL_FIRST) * 5u + 3u)

/*
 * A symbol is 16 us. The survey detects for 100 ms a channel, the assessment before sending for eight symbols,
 * ESP-IDF's CCA_DETECTION_TIME, busy at -75 dBm or more, 10 dB above the sensitivity 802.15.4 asks of a receiver,
 * as the nRF52840's link has it, rather than ESP-IDF's -60; and the radio sends at 0 dBm, or the nearest below.
 * An acknowledgement is waited for macAckWaitDuration, half ESP-IDF's default of 108 symbols,
 * which holds the nRF52840's, sent from a wake, to what 802.15.4 has a sender wait.
 */
#define ENERGY_SYMBOLS     6250u
#define CCA_SYMBOLS        8u
#define CCA_THRESHOLD_DBM  (-75)
#define TX_POWER_DBM       0

/*
 * The exchange: a ping to the nRF52840 every half second, with a stream of frames between if the page asks,
 * and what was heard told every few.
 */
#define PING_US      500000u
#define TELL_US      5000000u
#define PAYLOAD_MAX  32u
#define US_PER_S     1000000u


/* The bits the radio's notification is signalled with. */
#define BIT_RADIO   0x1u
#define BIT_TICK    0x2u

/* What the MAC is doing, which says what its next event ends; ACKING, sending the acknowledgement of a frame received. */
enum state { IDLE, RX, ACKING, TX, ED };

/*
 * Where the frame being sent is: in a backoff, timed by the MAC's timer0 while it receives,
 * or with the MAC, assessed, sent and waited for.
 */
enum sending { SEND_NONE, SEND_BACKOFF, SEND_TX };

struct radio {
    uint32_t note, irq, timer;
    enum state state;
    enum sending sending;
    struct mac154_csma csma;
    bool ping_due, stream_due;
    /* The frames, a length octet first, as the MAC's DMA reads and writes them. */
    uint8_t rx[1u + 127u + 1u] __attribute__((aligned(4)));
    uint8_t tx[1u + 127u] __attribute__((aligned(4)));
    uint8_t seq;
    bool heard_seq; /* whether last_seq holds the sequence number of the nRF52840's last data frame */
    uint8_t last_seq;
    /* What happened, for the log. */
    uint32_t pings, streamed, tries, acked, no_ack, busy, blocked, tx_aborted;
    uint32_t heard, repeats, heard_other, acks_sent, rx_aborted;
    int32_t rssi_sum;
    uint32_t rssi_count;
};

static void must(const char *step, uint32_t status)
{
    if (status != KERR_OK) {
        drv_failed(step, status);
        drv_stop(CHILD_FAILED);
    }
}

/* The events since the last, cleared as ESP-IDF's ISR clears them; the line armed again. */
static uint32_t events_take(struct radio *r)
{
    uint32_t events = I154(EVENT_STATUS);
    I154(EVENT_STATUS) = events;
    must("arm the radio's line", rv_irq_set(r->irq, BIT_RADIO));
    return events;
}

/* The index of libbtbb.a's power table nearest below dbm, as ESP-IDF's ieee802154_txpower_convert picks it. */
static uint32_t power_index(int dbm)
{
    uint8_t length = 0;
    const int8_t *table = bt_bb_get_tx_pwr_table(&length);
    if (table == 0 || length == 0) {
        return 0;
    }
    if (dbm <= table[0]) {
        return 0;
    }
    if (dbm >= table[length - 1]) {
        return length - 1u;
    }
    uint32_t i = length - 1u;
    while (i != 0 && table[i] > dbm) {
        i--;
    }
    return i;
}

/* The clocks of the MAC and of the baseband it shares, modem_clock_impl.c's IEEE802154_CLOCK_DEPS. */
static void clocks_on(void)
{
    REG(MODEM_SYSCON_CLK_CONF) |= CLK_ZB_APB_EN | CLK_ZB_MAC_EN | CLK_ETM_EN | CLK_MODEM_SEC_APB_EN;
    REG(MODEM_SYSCON_CLK_CONF1) |= CLK_BT_EN | CLK_BT_APB_EN;
    REG(MODEM_LPCON_CLK_CONF) |= CLK_COEX_EN;
}

/*
 * The MAC as ieee802154_mac_init and ieee802154_pib_update leave it, with the driver's own choices:
 * its frames' acknowledgements in hardware both ways, not enhanced ones, which only 802.15.4-2015's frames ask for,
 * and the address filter on, for the PAN and the short address of mac154.h's, rather than ESP-IDF's promiscuous default.
 */
static void mac_init(const uint8_t *mac)
{
    REG(MODEM_SYSCON_RST_CONF) |= RST_ZBMAC;
    REG(MODEM_SYSCON_RST_CONF) &= ~RST_ZBMAC;
    I154(EVENT_EN) |= EV_ALL;
    I154(TX_ABORT_EN) |= ABORT_BIT(TX_ABORT_RX_ACK_TIMEOUT) | ABORT_BIT(TX_ABORT_TX_COEX_BREAK) |
                         ABORT_BIT(TX_ABORT_SECURITY_ERROR) | ABORT_BIT(TX_ABORT_CCA_FAILED) |
                         ABORT_BIT(TX_ABORT_CCA_BUSY);
    I154(RX_ABORT_EN) |= ABORT_BIT(RX_ABORT_TX_ACK_TIMEOUT) | ABORT_BIT(RX_ABORT_TX_ACK_COEX_BREAK);
    I154(ED_CFG) |= ED_SAMPLE_AVG;
    I154(COEX_PTI) = (I154(COEX_PTI) & ~COEX_PTI_MASK) | COEX_PTI_NONE;
    ieee802154_txon_delay_set();

    /* The extended address, EUI-64 from the factory MAC, little-endian as it goes on the air. */
    uint8_t eui[8] = { mac[0], mac[1], mac[2], 0xff, 0xfe, mac[3], mac[4], mac[5] };
    I154(INF0_EXT_ADDR0) = (uint32_t)eui[7] | (uint32_t)eui[6] << 8 | (uint32_t)eui[5] << 16 | (uint32_t)eui[4] << 24;
    I154(INF0_EXT_ADDR1) = (uint32_t)eui[3] | (uint32_t)eui[2] << 8 | (uint32_t)eui[1] << 16 | (uint32_t)eui[0] << 24;
    I154(INF0_SHORT_ADDR) = MAC154_ADDR_C6;
    I154(INF0_PAN_ID) = MAC154_PAN_RVUOS;
    I154(CTRL_CFG) = (I154(CTRL_CFG) & ~(CFG_AUTO_ENHACK | CFG_COORDINATOR | CFG_PROMISCUOUS | CFG_MULTIPAN_MASK)) |
                     CFG_AUTO_ACK_TX | CFG_AUTO_ACK_RX | CFG_MULTIPAN_INF0;
    I154(ED_CFG) = (I154(ED_CFG) & ~(ED_CCA_MODE | ED_CCA_THRESHOLD)) | ED_CCA_MODE_ED |
                   ((uint32_t)(uint8_t)CCA_THRESHOLD_DBM);
    I154(TX_POWER) = power_index(TX_POWER_DBM);
    I154(ACK_TIMEOUT) = (I154(ACK_TIMEOUT) & ~ACK_TIMEOUT_MASK) | MAC154_ACK_WAIT_SYMBOLS;
}

/* The MAC held and the events of that cleared, as its stop and the trace window's end both make it. */
static void radio_hold(void)
{
    I154(COMMAND) = CMD_STOP;
    I154(EVENT_STATUS) = I154(EVENT_STATUS);
}

/* The MAC stopped, whatever it did, and the events of that cleared, as ESP-IDF's stop_current_operation. */
static void stop(struct radio *r)
{
    radio_hold();
    r->state = IDLE;
}

static void receive(struct radio *r)
{
    I154(RXDMA_ADDR) = (uint32_t)(uintptr_t)r->rx;
    I154(COMMAND) = CMD_RX_START;
    r->state = RX;
}

/* The most energy on channel over ENERGY_SYMBOLS, in dBm. */
static int8_t energy(struct radio *r, uint32_t channel)
{
    stop(r);
    I154(CHANNEL) = FREQ(channel);
    I154(ED_DURATION) = (I154(ED_DURATION) & ~ED_DURATION_MASK) | ENERGY_SYMBOLS;
    r->state = ED;
    I154(COMMAND) = CMD_ED_START;
    while (r->state == ED) {
        uint32_t bits;
        rv_wait(r->note, &bits);
        if ((bits & BIT_RADIO) && (events_take(r) & EV_ED_DONE)) {
            r->state = IDLE;
        }
    }
    return ED_RSS(I154(ED_CFG));
}

/*
 * The survey keeps the most any sample saw, as the nRF52840's does, since Wi-Fi's bursts averaged over 100 ms
 * vanish into the noise; the assessment before sending averages again, as ESP-IDF has it.
 */
static void survey(struct radio *r)
{
    drv_say("i154: the energy on each channel, %u ms each, the most in dBm\n", ENERGY_SYMBOLS * 16u / 1000u);
    I154(ED_CFG) &= ~ED_SAMPLE_AVG;
    for (uint32_t c = CHANNEL_FIRST; c <= CHANNEL_LAST; c++) {
        int8_t dbm = energy(r, c);
        drv_say("i154: channel %u, %u MHz: %d dBm\n", (unsigned)c, (unsigned)(2405u + 5u * (c - CHANNEL_FIRST)), dbm);
    }
    I154(ED_CFG) |= ED_SAMPLE_AVG;
}

/* A backoff before the frame in tx is sent, receiving meanwhile; one of no periods lasts the timer's least. */
static void backoff_start(struct radio *r)
{
    uint8_t random;
    drv_random(&random, sizeof(random));
    uint32_t us = mac154_backoff_us(&r->csma, random);
    r->sending = SEND_BACKOFF;
    I154(TIMER0_THRESHOLD) = us != 0 ? us : 1u;
    I154(COMMAND) = CMD_TIMER0_START;
}

/* The next frame to the nRF52840, a ping before the stream's, which asks to be acknowledged. */
static void frame_start(struct radio *r)
{
    char text[PAYLOAD_MAX];
    if (r->ping_due) {
        snprintf(text, sizeof(text), "ping %u from the C6", (unsigned)r->pings++);
        r->ping_due = false;
    } else {
        snprintf(text, sizeof(text), "frame %u of the C6", (unsigned)r->streamed++);
        r->stream_due = false;
    }
    struct mac154_data d = {
        .pan = MAC154_PAN_RVUOS, .dst = MAC154_ADDR_NRF, .src = MAC154_ADDR_C6, .seq = r->seq++,
        .ack_request = true, .payload = (const uint8_t *)text, .length = (uint32_t)strlen(text),
    };
    uint32_t n = mac154_data_frame(&r->tx[1], sizeof(r->tx) - 1u, &d);
    r->tx[0] = (uint8_t)(n + MAC154_FCS);
    mac154_csma_start(&r->csma);
    backoff_start(r);
}

/* The channel was busy: another backoff, or the frame given up once it was busy too often. */
static void channel_busy(struct radio *r)
{
    r->busy++;
    if (mac154_csma_busy(&r->csma)) {
        backoff_start(r);
    } else {
        r->blocked++;
        r->sending = SEND_NONE;
    }
}

/* The frame went unacknowledged: sent again after a backoff, or given up once it went often enough. */
static void unacknowledged(struct radio *r)
{
    if (mac154_csma_retry(&r->csma)) {
        backoff_start(r);
    } else {
        r->no_ack++;
        r->sending = SEND_NONE;
    }
}

/* The frame in tx to the MAC, sent after a clear channel assessment; the acknowledgement is received where frames are. */
static void transmit(struct radio *r)
{
    stop(r);
    I154(RXDMA_ADDR) = (uint32_t)(uintptr_t)r->rx;
    I154(TXDMA_ADDR) = (uint32_t)(uintptr_t)r->tx;
    I154(ED_DURATION) = (I154(ED_DURATION) & ~ED_DURATION_MASK) | CCA_SYMBOLS;
    r->state = TX;
    r->sending = SEND_TX;
    I154(COMMAND) = CMD_CCA_TX_START;
    r->csma.tries++;
    r->tries++;
}

/* A frame received: told about if it is the nRF52840's pong, counted if it is another of its, or anyone else's. */
static void received(struct radio *r)
{
    uint32_t psdu = r->rx[0] & 0x7fu;
    if (psdu < MAC154_FCS + 1u) {
        return;
    }
    uint32_t length = psdu - MAC154_FCS;
    int8_t rssi = (int8_t)r->rx[1u + length];
    uint8_t lqi = r->rx[2u + length];
    struct mac154_data d;
    if (!mac154_data_read(&r->rx[1], length, &d) || d.pan != MAC154_PAN_RVUOS || d.src != MAC154_ADDR_NRF) {
        r->heard_other++;
        return;
    }
    /* A repeat, the MAC's acknowledgement of the first lost on the way: acknowledged again, but not taken twice. */
    if (r->heard_seq && d.seq == r->last_seq) {
        r->repeats++;
        return;
    }
    r->heard_seq = true;
    r->last_seq = d.seq;
    r->heard++;
    r->rssi_sum += rssi;
    r->rssi_count++;
    if (d.length < 4u || memcmp(d.payload, "pong", 4) != 0) {
        return;
    }
    char text[PAYLOAD_MAX + 1];
    uint32_t n = d.length < PAYLOAD_MAX ? d.length : PAYLOAD_MAX;
    memcpy(text, d.payload, n);
    text[n] = 0;
    drv_say("i154: heard \"%s\" from %04x, seq %u, %d dBm, LQI %u\n", text, (unsigned)d.src, (unsigned)d.seq, rssi,
            (unsigned)lqi);
}

/*
 * The backoff over: the frame to the MAC, unless a frame is arriving or being acknowledged,
 * which makes the channel as busy as an assessment would find it, and which stopping the MAC to send would cut off.
 */
static void backoff_end(struct radio *r)
{
    if (r->state == RX && RX_STATE(I154(RX_STATUS)) <= RX_STATE_SFD) {
        transmit(r);
    } else {
        channel_busy(r);
    }
}

/*
 * What the MAC's events end, as ESP-IDF's ieee802154_isr takes them:
 * a frame received and acknowledged if it asked, a frame sent and acknowledged or not; then receiving again,
 * and the frame sent again after a backoff if the channel was busy or no acknowledgement came, as mac154.h counts.
 * Every frame sent asks for an acknowledgement, so the MAC waits for one after TX_DONE, which ends nothing here.
 */
static void events(struct radio *r)
{
    uint32_t ev = events_take(r);
    if (ev & EV_RX_DONE) {
        received(r);
        /* If the frame asked for an acknowledgement, the MAC sends it and ACK_TX_DONE ends the reception. */
        if ((I154(CTRL_CFG) & CFG_AUTO_ACK_TX) && (mac154_get16(&r->rx[1]) & MAC154_ACK_REQUEST)) {
            r->state = ACKING;
        } else {
            receive(r);
        }
    }
    if (ev & EV_ACK_TX_DONE) {
        r->acks_sent++;
        receive(r);
    }
    if (ev & EV_RX_ABORT) {
        r->rx_aborted++;
        receive(r);
    }
    if (ev & EV_ACK_RX_DONE) {
        r->acked++;
        r->sending = SEND_NONE;
        receive(r);
    }
    if (ev & EV_TX_ABORT) {
        uint32_t reason = ABORT_REASON(I154(TX_STATUS));
        receive(r);
        if (reason == TX_ABORT_CCA_BUSY || reason == TX_ABORT_CCA_FAILED) {
            channel_busy(r);
        } else if (reason == TX_ABORT_RX_ACK_TIMEOUT) {
            unacknowledged(r);
        } else {
            r->tx_aborted++;
            r->sending = SEND_NONE;
            drv_say("i154: sending aborted, reason %u\n", (unsigned)reason);
        }
    }
    /* Last, so that a frame received with it, and being acknowledged, makes the channel busy. */
    if (ev & EV_TIMER0) {
        I154(COMMAND) = CMD_TIMER0_STOP;
        if (r->sending == SEND_BACKOFF) {
            backoff_end(r);
        }
    }
}

/* What was sent and heard so far, in two lines, as a line of the log holds 160 characters. */
static void tell(const struct radio *r)
{
    drv_say("i154: sent %u frames in %u transmissions, acknowledged %u, unacknowledged %u; "
            "channel busy %u times, given up %u; aborted %u\n",
            (unsigned)(r->pings + r->streamed), (unsigned)r->tries, (unsigned)r->acked, (unsigned)r->no_ack,
            (unsigned)r->busy, (unsigned)r->blocked, (unsigned)r->tx_aborted);
    drv_say("i154: heard %u from the nRF52840, %u repeats, at %d dBm in the mean, acknowledged %u; %u others, "
            "%u CRC errors, %u filtered out\n",
            (unsigned)r->heard, (unsigned)r->repeats, r->rssi_count ? (int)(r->rssi_sum / (int32_t)r->rssi_count) : 0,
            (unsigned)r->acks_sent, (unsigned)r->heard_other, (unsigned)I154(CRC_ERROR_CNT),
            (unsigned)I154(RX_FILTER_FAIL_CNT));
}

/*
 * The radio up: the modem's clocks and the PHY, the baseband by bt_bb_v2_init_cmplx and the MAC set after it,
 * which the exchange and the trace window both make, in one place so that the two cannot drift apart.
 * The osi_trace markers frame the bring-up for the trace window alone, since osi_trace does nothing without one.
 */
static void radio_up(struct drv *d)
{
    clocks_on();
    /* The Wi-Fi power block's slow clock, which the PHY's calibration waits on, as for Wi-Fi. */
    drv_wifi_clock_enable();
    drv_phy_on();
    osi_trace("baseband", 0, 0);
    btbb_init(1);
    osi_trace("mac", 0, 0);
    mac_init(d->mac);
}

void i154_run(struct drv *d, struct self *own)
{
    static struct radio radio;
    struct radio *r = &radio;
    drv_say("i154: bringing IEEE 802.15.4 up, channel %u\n", (unsigned)d->i154);
    must("a slot for the radio's notification", slot_new(own, &r->note));
    must("the radio's notification", rv_pool_alloc(own->pool, CAP_NOTIFICATION, r->note, 0));
    must("a slot for the radio's line", slot_new(own, &r->irq));
    must("the radio's line", rv_irq_carve(d->i154_line, 0, 1, r->irq));
    must("bind the radio's line", rv_irq_bind(r->irq, own->pool, r->note, r->irq));
    must("the radio's timer", timer_bind(own, own->pool, r->note, &r->timer));

    radio_up(d);
    drv_say("i154: up; short address %04x in PAN %04x, power index %u, acknowledgement timeout %u symbols\n",
            MAC154_ADDR_C6, MAC154_PAN_RVUOS, (unsigned)I154(TX_POWER), (unsigned)I154(ACK_TIMEOUT));
    must("arm the radio's line", rv_irq_set(r->irq, BIT_RADIO));
    survey(r);

    I154(CHANNEL) = FREQ(d->i154);
    receive(r);
    child_report(&d->c, DRV_LISTENING);
    /* The timer ticks at the stream's period, or at the pings' without one. */
    uint32_t tick_us = d->i154_stream ? US_PER_S / d->i154_stream : PING_US, skipped;
    uint64_t pinged_at = osi_now_us(), told_at = pinged_at;
    must("the radio's timer", rv_timer_period(r->timer, BIT_TICK, tick_us, &skipped));
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(r->note, &bits);
        if (bits & BIT_RADIO) {
            events(r);
        }
        if (bits & BIT_TICK) {
            must("the radio's timer", rv_timer_period(r->timer, BIT_TICK, tick_us, &skipped));
            r->stream_due = d->i154_stream != 0;
        }
        uint64_t now = osi_now_us();
        if (now - pinged_at >= PING_US) {
            pinged_at = now;
            r->ping_due = true;
        }
        /* Not while a frame is half received, or while the MAC acknowledges one. */
        if (r->sending == SEND_NONE && r->state == RX && (r->ping_due || r->stream_due)) {
            frame_start(r);
        }
        if (now - told_at >= TELL_US) {
            told_at = now;
            tell(r);
        }
    }
    stop(r);
    must("quiet the radio's line", rv_irq_set(r->irq, 0));
    tell(r);
    drv_phy_off();
    drv_say("driver: 802.15.4 is off\n");
    drv_must("a frame of the nRF52840's heard", r->heard != 0 ? ESP_OK : ESP_FAIL);
    drv_must("a frame acknowledged by the nRF52840", r->acked != 0 ? ESP_OK : ESP_FAIL);
    child_stop(&d->c, DRV_LEFT);
}

/*
 * The trace window of the 802.15.4 radio: the bring-up that precedes the exchange and nothing else,
 * so that every device access is its own; the library's bt_bb_v2_init_cmplx's among them, which the driver's own
 * bt_bb_* take one function at a time and are held to, see user/wifi/esp32c6/NOTES.md.
 * With trace=1 the root task maps no device, so each access faults to the watcher and the root carries it out and logs it;
 * with trace=2 the devices are mapped and the same window runs with no fault, and the configuration's snap= addresses
 * are read back at its end, so that tools/mac-trace.py compare holds the two runs' final state.
 * It ends with DRV_TRACED, as the Wi-Fi window does.
 */
__attribute__((noreturn)) void i154_trace_run(struct drv *d)
{
    drv_say("driver: the 802.15.4 trace window starts, %u bytes of heap free\n", (unsigned)osi_heap_free());
    osi_trace_init(128);
    osi_trace("window", 0, 0);
    osi_trace("start", 0, 0);
    radio_up(d);
    osi_trace("stop", 0, 0);
    radio_hold();
    drv_phy_off();
    osi_trace("done", 0, 0);
    osi_trace_dump();
    drv_say("driver: the window is done; the tracer served %u faults, %u interrupts\n", osi_faults_served(),
            osi_interrupts());
    drv_stop(DRV_TRACED);
}
