/*
 * The ring: the dump engine writing round a ring of the bank's words, see dump.h, stopped on an event,
 * so that it holds what the receiver heard before it, where a capture holds only what came after its start.
 * The MAC and the PHY come up as the sampler's do, on a Wi-Fi channel, channel=, 1 by default, every frame passed on;
 * each second the ring starts and stops as the MAC ends a frame, its RX_LAST moving, mac_rx_last,
 * or, with after= in microseconds, that long after it started, and its newest words are told as dump.h's keys ask.
 * By default 4096 words of source 15 at 80 MHz.
 * Each stop tells how long the program polled, the most words the engine wrote between two polls,
 * and how often its thread ran out of its time meanwhile, OP_THREAD_OVERRUNS: a thread away from the core stops late.
 * The ring sends nothing: no frame, no tone.
 *
 * Run it with make BOARD=esp32c6 radio PROG=ring RADIO_CONFIG=file, see build.mk,
 * and read the words with tools/dump-spectrum.py.
 */

#include <stdint.h>

#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the ring", DRV_NEED_DUMP);

#define RING_WAIT_US 100000u /* a second's answer to the root task must not wait on a channel that stays quiet */
#define CLOCK_POLLS  64u     /* polls between two reads of the clock, which take microseconds each */

static int up;
static volatile uint32_t frames;

void drv_radio_off(void)
{
    if (up) {
        up = 0;
        mac_stop();
    }
}

static int heard(const struct mac_frame *m)
{
    (void)m;
    frames++;
    return 0;
}

/* One ring, from its start to a frame's end or after_us; the words the engine writes between two polls time them. */
static void ring(struct drv *d, const struct dump_conf *conf, uint32_t after_us)
{
    uint32_t rx_last = mac_rx_last(), overruns[2] = { 0, 0 }, longest = 0, gap = 0, stopped = 0;
    dump_ring_start(conf);
    uint64_t start = osi_now_us(), now = start;
    (void)rv_thread_overruns(d->own_thread, &overruns[0]);
    uint32_t index = dump_ring_index();
    for (uint32_t polls = 1; now - start < RING_WAIT_US; polls++) { /* bounded by RING_WAIT_US */
        uint32_t i = dump_ring_index();
        gap = (i - index) % DUMP_RING; /* modulo a round, as bit 14 stays set */
        longest = gap > longest ? gap : longest;
        index = i;
        if (polls % CLOCK_POLLS == 0 || after_us) {
            now = osi_now_us();
        }
        if (after_us ? now - start >= after_us : mac_rx_last() != rx_last) {
            stopped = 1;
            break;
        }
    }
    (void)rv_thread_overruns(d->own_thread, &overruns[1]);
    uint32_t took = (uint32_t)(osi_now_us() - start);
    dump_ring_stop(conf);
    drv_say("ring: %s in %u us; between polls at most %u words, the last %u; the thread ran out of its time %u times\n",
            stopped ? "stopped" : "gave up", took, longest, gap, overruns[1] - overruns[0]);
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    if (conf.hex == 0) {
        conf.hex = 4096;
    }
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    uint32_t after_us = config_number(d->conf, d->conf_len, "after", 0);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("no dumpbt=, the BLE side being i154listen's", conf.bt == 0 ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    mac_sniffer(1);
    drv_must_mac("the channel", mac_channel(channel));
    drv_must_mac("the MAC receives into the ring's list", mac_rx_take(heard));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    uint64_t told = osi_now_us();
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        if (osi_now_us() - told >= 1000000u) {
            told = osi_now_us();
            ring(d, &conf, after_us);
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    mac_sniffer(0);
    mac_rx_give_back();
    drv_radio_off();
    drv_say("ring: %u frames heard; the radio is off\n", frames);
    child_stop(&d->c, DRV_LEFT);
}
