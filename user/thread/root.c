/*
 * The root task of Thread on the nRF52840, so far IEEE 802.15.4's frames between it and the ESP32-C6,
 * the first steps towards it; see TODO.md, "Thread".
 *
 * It detects the energy on each channel from 11 to 26 for a while, a few rounds, and says the most each saw,
 * since Wi-Fi and everything else at 2.4 GHz share them, and a network does best on a quiet one.
 * Then it exchanges frames on one channel with the ESP32-C6, whose driver sends a ping every half second,
 * see user/wifi/esp32c6/i154.c: its link, link.c, acknowledges each ping, as 802.15.4 has a receiver acknowledge
 * a frame that asks, and it answers each with a pong of its own, which the link sends after CSMA-CA,
 * and sends again until the C6 acknowledges it.
 * With stream= it also sends the C6 that many frames a second, which contend with the pings and pongs for the channel;
 * frames of the C6's own stream, i154stream= on its side, it only counts.
 * Of other frames it counts only how many came, and reads nothing else of them.
 * It halts once the exchange's time is up, with HALT_SILENT if no ping was heard or no pong acknowledged,
 * or, with stream=, no frame of the stream acknowledged, and tells what the link did, for the C6 as a neighbour:
 * the transmissions its frames took, and ETX from them.
 *
 * Its configuration is lines of key=value in the input region, which THREAD_CONFIG's file leaves there,
 * see user/thread/build.mk:
 * channel=, 11 to 26, the exchange's channel, 25 by default, where the C6 has to be too;
 * power=, the dBm it sends at, 0 by default, down to -40, the chip's least;
 * run=, the exchange's seconds, 30 by default; stream=, frames a second, none by default;
 * survey=0 leaves the survey of the channels out.
 *
 * Build and run: make BOARD=nrf52840 thread, with the ESP32-C6 on i154= the same channel, see user/wifi/esp32c6/root.c;
 * tools/thread-pair.sh runs both.
 */

#include "console.h"
#include "devices.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "lib/self.h"
#include "link.h"
#include "mac154.h"
#include "wifi/conf.h"

/* An energy detection of some 100 ms a channel, a few rounds of all. */
#define ENERGY_PERIODS 781u
#define ENERGY_ROUNDS  2u

/* A bar of the level, a mark for every few. */
#define BAR_STEP 2u
#define BAR_MAX  40u

/*
 * The exchange: on the quietest channel the survey found at home, for half a minute, unless configured otherwise.
 * A pong goes some milliseconds after the ping, once the C6 receives again; if it does not yet, the link sends again.
 */
#define CHANNEL_DEFAULT 25u
#define RUN_S_DEFAULT   30u
#define RUN_S_MAX       3600u
#define STREAM_MAX      200u
#define PONG_DELAY_US   20000u
#define PAYLOAD_MAX     32u
#define US_PER_S        1000000u

/* The halt's code when the exchange did not go both ways; a step that failed halts with 1. */
#define HALT_SILENT 2u

#define CHANNELS (RADIO_CHANNEL_LAST - RADIO_CHANNEL_FIRST + 1u)

static struct self self;
static struct link link;
static uint32_t radio_irq, radio_bit, link_timer, link_bit, end_irq, end_bit, step_irq, step_bit;
static uint32_t stream_irq, stream_bit, pending;

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

/* What the configuration asks for. */
struct config {
    uint32_t channel, run_s, stream;
    int32_t power;
    bool survey;
};

/* A number of the configuration that may be negative, of a few digits, or dflt. */
static int32_t config_signed(const char *conf, uint32_t size, const char *key, int32_t dflt)
{
    char value[12];
    value[0] = '\0';
    config_value(conf, size, key, value, sizeof(value));
    uint32_t i = value[0] == '-' ? 1u : 0u;
    if (value[i] < '0' || value[i] > '9') {
        return dflt;
    }
    int32_t n = 0;
    for (uint32_t digits = 0; digits < 4u && value[i] >= '0' && value[i] <= '9'; digits++, i++) {
        n = n * 10 + (value[i] - '0');
    }
    return value[0] == '-' ? -n : n;
}

static void configure(struct config *c)
{
    uint32_t base, size, region;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    must("install the input", region_install(&self, BOOT_CAP_INPUT, RIGHT_R, &region));
    const char *conf = (const char *)(uintptr_t)base;
    c->channel = config_number(conf, size, "channel", CHANNEL_DEFAULT);
    if (c->channel < RADIO_CHANNEL_FIRST || c->channel > RADIO_CHANNEL_LAST) {
        c->channel = CHANNEL_DEFAULT;
    }
    c->power = config_signed(conf, size, "power", 0);
    c->run_s = config_number(conf, size, "run", RUN_S_DEFAULT);
    c->run_s = c->run_s == 0 ? RUN_S_DEFAULT : c->run_s > RUN_S_MAX ? RUN_S_MAX : c->run_s;
    c->stream = config_number(conf, size, "stream", 0);
    c->stream = c->stream > STREAM_MAX ? STREAM_MAX : c->stream;
    c->survey = config_number(conf, size, "survey", 1) != 0;
    must("uninstall the input", region_free(&self, region));
}

static uint32_t energy(uint32_t channel)
{
    radio_channel(&link.radio, channel);
    must("arm the radio's line", rv_irq_set(radio_irq, radio_bit));
    radio_energy_start(&link.radio, ENERGY_PERIODS);
    uint32_t level;
    while (wait_for(radio_bit), !radio_energy_done(&link.radio, &level)) {
        must("arm the radio's line", rv_irq_set(radio_irq, radio_bit));
    }
    return level;
}

/* The survey, which returns what it measured folded into a word, as no other node measured the same. */
static uint32_t survey_energy(void)
{
    uint32_t most[CHANNELS] = { 0 }, sum[CHANNELS] = { 0 }, fold = 0;
    for (uint32_t round = 0; round < ENERGY_ROUNDS; round++) {
        for (uint32_t c = 0; c < CHANNELS; c++) {
            uint32_t level = energy(RADIO_CHANNEL_FIRST + c);
            sum[c] += level;
            most[c] = level > most[c] ? level : most[c];
            fold = (fold << 5 | fold >> 27) ^ level;
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
    return fold;
}

/* What the frames of one kind did: how many the link took, and how each ended. */
struct sent {
    uint32_t frames, ended[LINK_RESULTS];
};

/* What the exchange did, for its summary. */
struct tally {
    uint32_t pings, lqi_sum, heard_stream, stream_skipped;
    struct sent pongs, stream;
};

/* Text into a buffer, as much as fits, for a frame's payload. */
struct text {
    char *at;
    uint32_t n, room;
};

static void text_write(void *to, const char *s, uint32_t n)
{
    struct text *t = to;
    for (uint32_t i = 0; i < n && t->n < t->room; i++) {
        t->at[t->n++] = s[i];
    }
}

/* A pong, or the stream's next frame, to the C6; what its sending will count towards, or 0 if the link is busy. */
static struct sent *send(struct tally *t, bool pong)
{
    struct sent *s = pong ? &t->pongs : &t->stream;
    char payload[PAYLOAD_MAX];
    struct text text = { payload, 0, sizeof(payload) };
    const struct out o = { text_write, &text };
    if (pong) {
        say(&o, "pong from the nRF52840");
    } else {
        say(&o, "frame %u of the nRF52840", s->frames);
    }
    if (!link_send(&link, MAC154_ADDR_C6, (const uint8_t *)payload, text.n)) {
        return 0;
    }
    s->frames++;
    return s;
}

/* Counts how the frame being sent ended, if it has. */
static void finished(struct sent **sending)
{
    enum link_result result;
    if (*sending != 0 && link_done(&link, &result)) {
        (*sending)->ended[result]++;
        *sending = 0;
    }
}

/*
 * A frame the link took: a ping of the C6's is told about, and answered with a pong once the step's timer fires;
 * a frame of its stream is counted.
 */
static bool heard(struct tally *t, const struct link_frame *f)
{
    if (f->src != MAC154_ADDR_C6) {
        return false;
    }
    if (f->length < 4u || memcmp(f->payload, "ping", 4) != 0) {
        t->heard_stream++;
        return false;
    }
    t->pings++;
    t->lqi_sum += f->lqi;
    char text[PAYLOAD_MAX + 1];
    uint32_t n = f->length < PAYLOAD_MAX ? f->length : PAYLOAD_MAX;
    memcpy(text, f->payload, n);
    text[n] = 0;
    say(&cout, "thread: heard \"%s\", seq %u, link quality %u\n", text, (uint32_t)f->seq, f->lqi);
    return true;
}

static void tell(const char *what, const struct sent *s)
{
    say(&cout, "thread: %s %u, %u acknowledged, %u not, %u never sent for a busy channel", what, s->frames,
        s->ended[LINK_ACKED], s->ended[LINK_NO_ACK], s->ended[LINK_BUSY]);
}

static void summary(const struct tally *t, const struct config *c)
{
    const struct link_counts *k = &link.counts;
    say(&cout, "thread: heard %u pings at link quality %u in the mean, and %u frames of the C6's stream\n", t->pings,
        t->pings ? t->lqi_sum / t->pings : 0, t->heard_stream);
    tell("sent pongs", &t->pongs);
    say(&cout, "\n");
    if (c->stream != 0) {
        tell("streamed frames", &t->stream);
        say(&cout, ", %u not sent as the one before was still going\n", t->stream_skipped);
    }
    say(&cout, "thread: the link sent %u frames in %u transmissions, the channel busy at %u assessments; "
               "sent %u acknowledgements; %u frames with a bad FCS, %u stray acknowledgements, %u others, %u dropped\n",
        k->frames, k->tries, k->busy, k->acks_sent, k->bad_fcs, k->stray_acks, k->others, k->dropped);
    for (uint32_t i = 0; i < LINK_NEIGHBORS; i++) {
        const struct link_neighbor *n = &link.neighbors[i];
        if (!n->used) {
            continue;
        }
        say(&cout, "thread: neighbour %x: heard %u, %u of them repeats, at link quality %u; "
                   "sent %u frames in %u transmissions, %u acknowledged, %u not",
            (uint32_t)n->addr, n->heard, n->repeats, n->lqi, n->frames, n->tries, n->acked, n->failed);
        if (n->etx != 0) {
            uint32_t h = n->etx * 100u / LINK_ETX_ONE;
            say(&cout, "; ETX %u.%u%u", h / 100u, h / 10u % 10u, h % 10u);
        }
        say(&cout, "\n");
    }
}

/* Whether the exchange went both ways: a ping heard, and a pong acknowledged, and with a stream a frame of it. */
static bool exchange(const struct config *c)
{
    struct tally t = { 0 };
    struct sent *sending = 0;
    bool pong_due = false, pong_ready = false, stream_due = false;
    int32_t dbm = link_power(&link, c->power);
    say(&cout, "thread: exchanging frames with the ESP32-C6 on channel %u for %u s at %d dBm, as %x in PAN %x",
        c->channel, c->run_s, dbm, MAC154_ADDR_NRF, MAC154_PAN_RVUOS);
    if (c->stream != 0) {
        say(&cout, ", and streaming %u frames a second", c->stream);
    }
    say(&cout, "\n");
    link_listen(&link, c->channel);
    must("set the exchange's end", rv_timer_set(end_irq, end_bit, c->run_s * US_PER_S));
    if (c->stream != 0) {
        uint32_t skipped;
        must("set the stream's period", rv_timer_period(stream_irq, stream_bit, US_PER_S / c->stream, &skipped));
    }
    for (;;) {
        uint32_t got = wait_for(radio_bit | link_bit | end_bit | step_bit | stream_bit);
        if (got & end_bit) {
            break;
        }
        if (got & (radio_bit | link_bit)) {
            link_wake(&link, got);
        }
        struct link_frame f;
        while (link_take(&link, &f)) {
            if (heard(&t, &f) && !pong_due) {
                pong_due = true;
                must("set the pong's time", rv_timer_set(step_irq, step_bit, PONG_DELAY_US));
            }
        }
        if (got & step_bit) {
            pong_ready = pong_due;
        }
        if (got & stream_bit) {
            uint32_t skipped;
            must("set the stream's period", rv_timer_period(stream_irq, stream_bit, US_PER_S / c->stream, &skipped));
            t.stream_skipped += skipped + (stream_due ? 1u : 0u);
            stream_due = true;
        }
        finished(&sending);
        /* A pong first, as the C6 counts on it; the stream takes what is left. */
        if (sending == 0 && pong_ready) {
            sending = send(&t, true);
            pong_ready = pong_due = false;
        } else if (sending == 0 && stream_due) {
            sending = send(&t, false);
            stream_due = false;
        }
        /* A frame the channel refused at every assessment may end within link_send. */
        finished(&sending);
        must("the link's calls", link.fault);
    }
    link_off(&link);
    must("stop the step's timer", rv_timer_set(step_irq, 0, 0));
    must("stop the stream", rv_timer_set(stream_irq, 0, 0));
    must("the link's calls", link.fault);
    summary(&t, c);
    return t.pings != 0 && t.pongs.ended[LINK_ACKED] != 0 && (c->stream == 0 || t.stream.ended[LINK_ACKED] != 0);
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
    struct config c;
    configure(&c);

    /* The clock's counter, which the link times what is shorter than a tick by, a load away. */
    uint64_t at;
    uint32_t hz, counter, counter_frame;
    must("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &at, &hz, &counter));
    must("the link's clock counts microseconds", hz == US_PER_S ? KERR_OK : KERR_INVALID_ARG);
    must("a slot for the counter", slot_new(&self, &counter_frame));
    must("the counter's frame", rv_clock_frame(BOOT_CAP_CLOCK, counter_frame));
    must("map the counter", region_install(&self, counter_frame, RIGHT_R, &region));

    must("a bit for the radio", bit_new(&self, &radio_bit));
    must("a slot for the radio's line", slot_new(&self, &radio_irq));
    must("the radio's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, RADIO_IRQ, 1, radio_irq));
    must("bind the radio's line", rv_irq_bind(radio_irq, self.pool, self.inbox, radio_irq));
    must("a bit for the link", bit_new(&self, &link_bit));
    must("a timer for the link", timer_new(&self, &link_timer));
    must("a bit for the end", bit_new(&self, &end_bit));
    must("a timer for the end", timer_new(&self, &end_irq));
    must("a bit for the steps", bit_new(&self, &step_bit));
    must("a timer for the steps", timer_new(&self, &step_irq));
    must("a bit for the stream", bit_new(&self, &stream_bit));
    must("a timer for the stream", timer_new(&self, &stream_irq));

    link_init(&link, RADIO_BASE, counter, radio_irq, radio_bit, link_timer, link_bit, MAC154_PAN_RVUOS,
              MAC154_ADDR_NRF, (uint32_t)at ^ rv_counter_low(counter));
    say(&cout, "thread: the radio is on, set for IEEE 802.15.4\n");
    if (c.survey) {
        link_stir(&link, survey_energy());
    }
    bool both_ways = exchange(&c);
    say(&cout, both_ways ? "thread: done\n" : "thread: no ping heard, or nothing of ours acknowledged\n");
    rv_halt(BOOT_CAP_DEBUG, both_ways ? 0 : HALT_SILENT);
}
