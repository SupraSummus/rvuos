/*
 * The modem's ETM, and what stops the dump engine: a survey, see hw/dump.md, "What stops the engine".
 * The MAC and the PHY come up as the ring's do, on a Wi-Fi channel, channel=, 1 by default, every frame passed on,
 * and the IEEE 802.15.4 MAC's clocks too, whose timer0's event, 59, is the one the probes fire channel 0 of the ETM on.
 * survey=1: each source a capture never finished on of itself armed, and each task from 1 to 127 fired at it in turn,
 * but the two ESP-IDF names for sending, 64 and 68; which tasks made the engine write; source 15 triggered, the control.
 * survey=2: each event from 0 to 127 made task 21's, which survey=1 found, while the ring from source 8 runs,
 * timer0 firing midway, the control; which events stopped it, when, and how many frames the Wi-Fi MAC took meanwhile.
 * survey=3: the ring from each source but 8, waited on: which sources stop it of themselves, and when.
 * rounds= repeats surveys 1 and 3; left out, the run makes the three surveys in turn, once.
 * i154=1: the 802.15.4 listener's, i154.c's, on channel=, 12 by default, and each second one of the 64 events from first=,
 * 0 or 64, made task 21's while the ring from source 8 runs, and the next second whether it stopped;
 * the events twice over, so that the nRF52840 can send through half of the run, each event once with its frames.
 * The survey sends nothing it means to: the tasks ESP-IDF names for sending are left out, and no frame is queued.
 *
 * Run it with make BOARD=esp32c6 radio PROG=etm RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "i154.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the modem's ETM", DRV_NEED_I154 | DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* The modem's ETM, ESP-IDF's ieee802154_reg.h: a channel's event and task ids, 7 bits each. */
#define ETM(off)     REG(MODEM_BASE + 0x8800u + (off))
#define ETM_CHENSET  0x04u
#define ETM_CHENCLR  0x08u
#define ETM_EVT(ch)  (0x18u + 8u * (ch))
#define ETM_TASK(ch) (0x1cu + 8u * (ch))
#define ETM_IDS      128u
#define EVENT_TIMER0 59u /* ESP-IDF's ETM_EVENT_TIMER0_OVERFLOW */
#define TASK_DUMP    21u /* survey=1's: it starts a capture from source 8, and stops the ring from it */
#define TASK_SENDS(t) ((t) == 64u || (t) == 68u) /* ESP-IDF's ETM_TASK_ED_TRIG_TX and ETM_TASK_TX_START */

/* The 802.15.4 MAC's, as i154.c names them. */
#define MODEM_SYSCON_CLK_CONF (MODEM_SYSCON_BASE + 0x04u)
#define MODEM_SYSCON_RST_CONF (MODEM_SYSCON_BASE + 0x10u)
#define CLK_ETM_EN            (1u << 22)
#define CLK_ZB_APB_EN         (1u << 23)
#define CLK_ZB_MAC_EN         (1u << 24)
#define RST_ZBMAC             (1u << 24)
#define I154(off)             REG(I154_BASE + (off))
#define COMMAND               0x000u
#define EVENT_EN              0x060u
#define EVENT_STATUS          0x064u
#define TIMER0_THRESHOLD      0x0a8u /* in microseconds */
#define CMD_STOP              0x45u
#define CMD_TIMER0_START      0x4cu
#define CMD_TIMER0_STOP       0x4du
#define EV_TIMER0             (1u << 8)

#define TIMER_US      20u
#define ARMED_US      200u    /* a capture of 1024 words at 16 MHz takes 64 us */
#define WAIT_US       100000u /* an event's or a source's wait */
#define PAUSE_MS      50u     /* between probes, for the root task to take their lines */
#define CLOCK_POLLS   64u     /* polls between two reads of the clock, which take microseconds each */
#define LISTEN_EVENTS 64u

static int up, listening;
static volatile uint32_t frames;

/* No channel enabled, timer0 stopped, the 802.15.4 MAC idle and its events cleared. */
static void quiet(void)
{
    ETM(ETM_CHENCLR) = 0xffffffffu;
    I154(COMMAND) = CMD_TIMER0_STOP;
    I154(COMMAND) = CMD_STOP;
    I154(EVENT_STATUS) = I154(EVENT_STATUS);
}

void drv_radio_off(void)
{
    if (listening) {
        listening = 0;
        ETM(ETM_CHENCLR) = 0xffffffffu;
        drv_phy_off();
    }
    if (up) {
        up = 0;
        quiet();
        mac_stop();
    }
}

static int heard(const struct mac_frame *m)
{
    (void)m;
    frames++;
    return 0;
}

static void spin_us(uint32_t us)
{
    uint64_t start = osi_now_us();
    while (osi_now_us() - start < us) { /* bounded by us */
    }
}

/* Channel 0 from event to task. */
static void route(uint32_t event, uint32_t task)
{
    ETM(ETM_EVT(0)) = event;
    ETM(ETM_TASK(0)) = task;
    ETM(ETM_CHENSET) = 1u;
}

static void timer0(uint32_t us)
{
    I154(EVENT_STATUS) = EV_TIMER0;
    I154(TIMER0_THRESHOLD) = us;
    I154(COMMAND) = CMD_TIMER0_START;
}

/* survey=1: source armed, and each task fired at it on timer0's event, rounds times. */
static void by_task(struct dump_conf *armed, uint32_t source, uint32_t rounds)
{
    static uint8_t wrote[ETM_IDS];
    uint32_t started = 0, probes = 0;
    armed->source = source;
    for (uint32_t task = 0; task < ETM_IDS; task++) {
        wrote[task] = 0;
    }
    for (uint32_t round = 0; round < rounds; round++) { /* bounded by rounds= */
        for (uint32_t task = 1; task < ETM_IDS; task++) {
            if (TASK_SENDS(task)) {
                continue;
            }
            probes++;
            dump_arm(armed, source == 15u);
            if (source != 15u) {
                route(EVENT_TIMER0, task);
                timer0(TIMER_US);
            }
            spin_us(ARMED_US);
            quiet();
            if (dump_disarm(armed) != 0) {
                started++;
                wrote[task]++;
            }
        }
    }
    for (uint32_t task = 1; task < ETM_IDS && source != 15u; task++) {
        if (wrote[task] != 0) {
            drv_say("etm: source %u wrote on task %u, %u times of %u\n", source, task, wrote[task], rounds);
        }
    }
    drv_say("etm: source %u: %u of %u probes wrote\n", source, started, probes);
}

/* survey=2: the ring from source 8, event made TASK_DUMP's, timer0 firing midway. */
static void by_event(struct dump_conf *ring_conf, uint32_t event)
{
    ring_conf->source = 8;
    dump_ring_start(ring_conf);
    route(event, TASK_DUMP);
    timer0(WAIT_US / 2u);
    uint32_t rx_last = mac_rx_last(), moves = 0, stopped = 0, polls = 0;
    uint64_t start = osi_now_us(), now = start;
    for (; now - start < WAIT_US; polls++) { /* bounded by WAIT_US */
        if (polls % CLOCK_POLLS == 0) {
            now = osi_now_us();
        }
        if (stopped == 0 && dump_ring_stopped()) {
            stopped = (uint32_t)(now - start) + 1u;
        }
        uint32_t last = mac_rx_last();
        moves += last != rx_last;
        rx_last = last;
    }
    quiet();
    if (stopped != 0) {
        drv_say("etm: event %u stopped the ring at about %u us\n", event, stopped - 1u);
    }
    drv_say("etm: event %u: the Wi-Fi MAC took %u frames\n", event, moves);
    dump_ring_stop(ring_conf);
}

/* survey=3: the ring from source, nothing routed. */
static void by_source(struct dump_conf *ring_conf, uint32_t source)
{
    ring_conf->source = source;
    dump_ring_start(ring_conf);
    uint32_t polls = 0;
    uint64_t start = osi_now_us(), now = start;
    for (; now - start < WAIT_US && !dump_ring_stopped(); polls++) { /* bounded by WAIT_US */
        if (polls % CLOCK_POLLS == 0) {
            now = osi_now_us();
        }
    }
    if (dump_ring_stopped()) {
        drv_say("etm: source %u stopped the ring at about %u us\n", source, (uint32_t)(now - start));
    } else {
        drv_say("etm: source %u left the ring running\n", source);
    }
    dump_ring_stop(ring_conf);
}

/* i154=1: called once a second by the listener. */
static struct dump_conf listen_ring;
static uint32_t listen_probe, listen_first;

static void listen_second(void)
{
    if (listen_probe != 0) {
        ETM(ETM_CHENCLR) = 1u;
        drv_say("etm: event %u %s\n", listen_first + (listen_probe - 1u) % LISTEN_EVENTS,
                dump_ring_stopped() ? "stopped the ring" : "left the ring running");
        dump_ring_stop(&listen_ring);
    }
    if (listen_probe < 2u * LISTEN_EVENTS) {
        dump_ring_start(&listen_ring);
        route(listen_first + listen_probe++ % LISTEN_EVENTS, TASK_DUMP);
    } else if (listen_probe++ == 2u * LISTEN_EVENTS) {
        drv_say("etm: the survey is done\n");
    }
}

void drv_run(struct drv *d, struct self *own)
{
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    conf.rate = 3;
    if (config_number(d->conf, d->conf_len, "i154", 0)) {
        uint32_t channel = config_number(d->conf, d->conf_len, "channel", 12);
        drv_must("a channel of 11 to 26",
                 channel >= I154_CHANNEL_FIRST && channel <= I154_CHANNEL_LAST ? ESP_OK : ESP_FAIL);
        listen_ring = conf;
        listen_ring.source = 8;
        listen_first = config_number(d->conf, d->conf_len, "first", 0) & LISTEN_EVENTS;
        listening = 1;
        i154_listen_run(d, own, channel, listen_second);
    }
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    uint32_t survey = config_number(d->conf, d->conf_len, "survey", 0);
    uint32_t rounds = config_number(d->conf, d->conf_len, "rounds", 1);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("survey= of 1, 2 or 3", survey <= 3 ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    mac_sniffer(1);
    drv_must_mac("the channel", mac_channel(channel));
    drv_must_mac("the MAC receives into the survey's list", mac_rx_take(heard));
    REG(MODEM_SYSCON_CLK_CONF) |= CLK_ZB_APB_EN | CLK_ZB_MAC_EN | CLK_ETM_EN;
    REG(MODEM_SYSCON_RST_CONF) |= RST_ZBMAC;
    REG(MODEM_SYSCON_RST_CONF) &= ~RST_ZBMAC;
    I154(EVENT_EN) = EV_TIMER0;
    quiet();
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);

    /* The probes in turn, survey=1's, 2's and 3's, of which survey= picks one. */
    static const uint8_t waiting[] = { 15, 3, 4, 6, 7, 8, 9, 11, 12, 13, 14 };
    static const uint8_t ring_sources[] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15 };
    struct dump_conf armed = conf;
    armed.samples = 1024;
    uint32_t ones = sizeof waiting, twos = ETM_IDS, threes = rounds * sizeof ring_sources;
    uint32_t probe = survey <= 1 ? 0 : survey == 2 ? ones : ones + twos;
    uint32_t probes = survey == 1 ? ones : survey == 2 ? ones + twos : ones + twos + threes;
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        if (probe < probes) {
            if (probe < ones) {
                by_task(&armed, waiting[probe], rounds);
            } else if (probe < ones + twos) {
                by_event(&conf, probe - ones);
            } else {
                by_source(&conf, ring_sources[(probe - ones - twos) % sizeof ring_sources]);
            }
            if (++probe == probes) {
                drv_say("etm: the survey is done\n");
            }
            osi_delay_ms(PAUSE_MS);
            continue;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    mac_sniffer(0);
    mac_rx_give_back();
    drv_radio_off();
    drv_say("etm: %u frames heard; the radio is off\n", frames);
    child_stop(&d->c, DRV_LEFT);
}
