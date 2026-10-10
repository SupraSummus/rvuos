/*
 * The root task of Thread on the nRF52840, so far IEEE 802.15.4's frames between it and the ESP32-C6,
 * the first steps towards it; see TODO.md, "Thread".
 *
 * It detects the energy on each channel from 11 to 26 for a while, a few rounds, and says the most each saw,
 * since Wi-Fi and everything else at 2.4 GHz share them, and a network does best on a quiet one.
 * Then it exchanges frames on one channel with the ESP32-C6, whose driver sends a ping every half second,
 * see user/wifi/esp32c6/i154.c: it acknowledges each ping, as 802.15.4 has a receiver acknowledge a frame that asks,
 * and answers it with a pong of its own, which asks to be acknowledged, and waits for that.
 * Of other frames it counts only their type, and reads nothing else of them.
 * It drives the radio itself, through radio.c, and halts once the exchange's time is up,
 * with HALT_SILENT if no ping was heard or no pong acknowledged.
 *
 * Build and run: make BOARD=nrf52840 thread, with the ESP32-C6 on i154=25, see user/wifi/esp32c6/root.c;
 * tools/thread-pair.sh runs both.
 */

#include "console.h"
#include "devices.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "lib/self.h"
#include "mac154.h"
#include "radio.h"

/* An energy detection of some 100 ms a channel, a few rounds of all. */
#define ENERGY_PERIODS 781u
#define ENERGY_ROUNDS  2u

/* A bar of the level, a mark for every few. */
#define BAR_STEP 2u
#define BAR_MAX  40u

/*
 * The exchange: on the quietest channel the survey found at home, for half a minute.
 * A pong goes some milliseconds after the ping's acknowledgement, once the C6 receives again,
 * and its acknowledgement is waited for a few, past 802.15.4's 864 us, as the kernel's tick has it.
 */
#define EXCHANGE_CHANNEL 25u
#define EXCHANGE_US      30000000u
#define PONG_DELAY_US    20000u
#define ACK_WAIT_US      3000u
#define PAYLOAD_MAX      32u

/* The halt's code when the exchange did not go both ways; a step that failed halts with 1. */
#define HALT_SILENT 2u

/* The frame types 802.15.4 names, by the low three bits of the frame control field. */
#define FRAME_TYPES 8u
static const char *const frame_type[FRAME_TYPES] = {
    "beacon", "data", "ack", "command", "reserved", "multipurpose", "fragment", "extended",
};

#define CHANNELS (RADIO_CHANNEL_LAST - RADIO_CHANNEL_FIRST + 1u)

/* What the radio is doing in the exchange, which says what its next event ends. */
enum state { LISTENING, ACKING, PONGING, AWAITING_ACK };

static struct self self;
static struct radio radio;
static uint32_t radio_irq, radio_bit, end_irq, end_bit, step_irq, step_bit, pending;

static void console_write(void *to, const char *s, uint32_t n)
{
    (void)to;
    for (uint32_t i = 0; i < n; i++) {
        console_put_polled(s[i]);
    }
}

static const struct out cout = { console_write, 0 };

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&cout, "thread: %s: failed with %u", what, status);
        if (self.what != 0) {
            say(&cout, " at %s", self.what);
        }
        say(&cout, "\n");
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

/* Waits until a bit of want is signalled, and takes those of want that were. */
static uint32_t wait_for(uint32_t want)
{
    while ((pending & want) == 0) {
        uint32_t bits;
        must("wait", rv_wait(self.inbox, &bits));
        pending |= bits;
    }
    uint32_t got = pending & want;
    pending &= ~got;
    return got;
}

static void radio_arm(void)
{
    must("arm the radio's line", rv_irq_set(radio_irq, radio_bit));
}

static uint32_t energy(uint32_t channel)
{
    radio_channel(&radio, channel);
    radio_arm();
    radio_energy_start(&radio, ENERGY_PERIODS);
    uint32_t level;
    while (wait_for(radio_bit), !radio_energy_done(&radio, &level)) {
        radio_arm();
    }
    return level;
}

static void survey_energy(void)
{
    uint32_t most[CHANNELS] = { 0 }, sum[CHANNELS] = { 0 };
    for (uint32_t round = 0; round < ENERGY_ROUNDS; round++) {
        for (uint32_t c = 0; c < CHANNELS; c++) {
            uint32_t level = energy(RADIO_CHANNEL_FIRST + c);
            sum[c] += level;
            most[c] = level > most[c] ? level : most[c];
        }
    }
    say(&cout, "thread: the energy on each channel, %u rounds of %u us, the most and the mean level of 127\n",
        ENERGY_ROUNDS, ENERGY_PERIODS * RADIO_ENERGY_PERIOD_US);
    for (uint32_t c = 0; c < CHANNELS; c++) {
        char bar[BAR_MAX + 1];
        uint32_t n = most[c] / BAR_STEP < BAR_MAX ? most[c] / BAR_STEP : BAR_MAX;
        for (uint32_t i = 0; i < n; i++) {
            bar[i] = '#';
        }
        bar[n] = '\0';
        say(&cout, "thread: channel %u, %u MHz: most %u, mean %u %s\n", RADIO_CHANNEL_FIRST + c,
            RADIO_CHANNEL_MHZ(RADIO_CHANNEL_FIRST + c), most[c], sum[c] / ENERGY_ROUNDS, bar);
    }
}

/* What the exchange did, for its summary. */
struct tally {
    uint32_t pings, acks_sent, pongs, pongs_acked, pongs_unacked, bad_fcs, lqi_sum;
    uint32_t others[FRAME_TYPES];
};

static void pong(struct tally *t, uint8_t seq)
{
    static const char text[] = "pong from the nRF52840";
    struct mac154_data d = {
        .pan = MAC154_PAN_RVUOS, .dst = MAC154_ADDR_C6, .src = MAC154_ADDR_NRF, .seq = seq,
        .ack_request = true, .payload = (const uint8_t *)text, .length = sizeof(text) - 1u,
    };
    uint8_t frame[MAC154_DATA_HEADER + sizeof(text)];
    radio_send_start(&radio, frame, mac154_data_frame(frame, sizeof(frame), &d));
    t->pongs++;
}

/*
 * A frame heard while listening: a ping of the C6's is told about and acknowledged at once, if it asks,
 * and answered with a pong once the step's timer fires; any other frame is counted by its type.
 * Returns what the radio does next.
 */
static enum state heard(struct tally *t, const struct radio_frame *f, bool *pong_due)
{
    struct mac154_data d;
    if (!f->fcs_ok) {
        t->bad_fcs++;
    } else if (mac154_data_read(f->bytes, f->length, &d) && d.pan == MAC154_PAN_RVUOS && d.src == MAC154_ADDR_C6 &&
               d.dst == MAC154_ADDR_NRF) {
        t->pings++;
        t->lqi_sum += f->lqi;
        /* Copied first, as the radio receives into the same bytes once the acknowledgement has gone. */
        char text[PAYLOAD_MAX + 1];
        uint32_t n = d.length < PAYLOAD_MAX ? d.length : PAYLOAD_MAX;
        memcpy(text, d.payload, n);
        text[n] = 0;
        if (d.ack_request) {
            uint8_t ack[MAC154_ACK_LENGTH];
            radio_send_start(&radio, ack, mac154_ack_frame(ack, d.seq));
        } else {
            radio_receive_next(&radio);
        }
        say(&cout, "thread: heard \"%s\", seq %u, link quality %u%s\n", text, (uint32_t)d.seq, f->lqi,
            d.ack_request ? "; acknowledging" : "");
        *pong_due = true;
        return d.ack_request ? ACKING : LISTENING;
    } else if (f->length > 0) {
        t->others[f->bytes[0] & MAC154_TYPE_MASK]++;
    }
    radio_receive_next(&radio);
    return LISTENING;
}

/* Whether the exchange went both ways: a ping heard, and a pong acknowledged. */
static bool exchange(uint32_t channel)
{
    struct tally t = { 0 };
    enum state state = LISTENING;
    bool pong_due = false;
    uint8_t seq = 0;
    say(&cout, "thread: exchanging frames with the ESP32-C6 on channel %u for %u s, as %x in PAN %x\n", channel,
        EXCHANGE_US / 1000000u, MAC154_ADDR_NRF, MAC154_PAN_RVUOS);
    radio_channel(&radio, channel);
    radio_arm();
    radio_receive_start(&radio);
    must("set the exchange's end", rv_timer_set(end_irq, end_bit, EXCHANGE_US));
    for (;;) {
        uint32_t got = wait_for(radio_bit | end_bit | step_bit);
        if (got & end_bit) {
            break;
        }
        if (got & radio_bit) {
            struct radio_frame f;
            if (state == LISTENING && radio_received(&radio, &f)) {
                state = heard(&t, &f, &pong_due);
                if (state == LISTENING && pong_due) {
                    must("set the pong's time", rv_timer_set(step_irq, step_bit, PONG_DELAY_US));
                }
            } else if (state == ACKING && radio_sent(&radio)) {
                t.acks_sent++;
                state = LISTENING;
                must("set the pong's time", rv_timer_set(step_irq, step_bit, PONG_DELAY_US));
            } else if (state == PONGING && radio_sent(&radio)) {
                state = AWAITING_ACK;
                must("set the acknowledgement's wait", rv_timer_set(step_irq, step_bit, ACK_WAIT_US));
            } else if (state == AWAITING_ACK && radio_received(&radio, &f)) {
                if (f.fcs_ok && mac154_acks(f.bytes, f.length, (uint8_t)(seq - 1u))) {
                    t.pongs_acked++;
                    state = LISTENING;
                }
                radio_receive_next(&radio);
            }
            radio_arm();
        }
        if (got & step_bit) {
            if (state == LISTENING && pong_due) {
                pong_due = false;
                pong(&t, seq++);
                state = PONGING;
            } else if (state == AWAITING_ACK) {
                t.pongs_unacked++;
                state = LISTENING;
            }
        }
    }
    radio_off(&radio);
    must("quiet the radio's line", rv_irq_set(radio_irq, 0));
    must("stop the step's timer", rv_timer_set(step_irq, 0, 0));

    say(&cout, "thread: heard %u pings at link quality %u in the mean, acknowledged %u; "
               "sent %u pongs, %u acknowledged, %u not; %u frames with a bad FCS",
        t.pings, t.pings ? t.lqi_sum / t.pings : 0, t.acks_sent, t.pongs, t.pongs_acked, t.pongs_unacked, t.bad_fcs);
    for (uint32_t i = 0; i < FRAME_TYPES; i++) {
        if (t.others[i] > 0) {
            say(&cout, "; other %s %u", frame_type[i], t.others[i]);
        }
    }
    say(&cout, "\n");
    return t.pings != 0 && t.pongs_acked != 0;
}

int main(void);
int main(void)
{
    uint32_t base, size, region;
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("the root's own", self_root(&self, TIME_UNITS));
    must("map the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("map the radio", region_install(&self, BOOT_CAP_RADIO, RIGHT_R | RIGHT_W, &region));

    must("a bit for the radio", bit_new(&self, &radio_bit));
    must("a slot for the radio's line", slot_new(&self, &radio_irq));
    must("the radio's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, RADIO_IRQ, 1, radio_irq));
    must("bind the radio's line", rv_irq_bind(radio_irq, self.pool, self.inbox, radio_irq));
    must("a bit for the end", bit_new(&self, &end_bit));
    must("a timer for the end", timer_new(&self, &end_irq));
    must("a bit for the steps", bit_new(&self, &step_bit));
    must("a timer for the steps", timer_new(&self, &step_irq));

    radio_init(&radio, RADIO_BASE);
    say(&cout, "thread: the radio is on, set for IEEE 802.15.4\n");
    survey_energy();
    bool both_ways = exchange(EXCHANGE_CHANNEL);
    radio_off(&radio);
    say(&cout, both_ways ? "thread: done\n" : "thread: no ping heard, or no pong acknowledged\n");
    rv_halt(BOOT_CAP_DEBUG, both_ways ? 0 : HALT_SILENT);
}
