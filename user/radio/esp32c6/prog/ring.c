/*
 * The ring: the dump engine writing round a ring of the bank's words, see dump.h, stopped on an event,
 * so that it holds what the receiver heard before it, where a capture holds only what came after its start.
 * The MAC and the PHY come up as the sampler's do, on a Wi-Fi channel, channel=, 1 by default, every frame passed on;
 * each second the ring starts from source 2, which the engine stops of itself as the AGC lets a packet go, see hw/dump.md,
 * and is kept if the Wi-Fi MAC took a frame, its RX_LAST moving, mac_rx_last, and started over if not;
 * or, with after= in microseconds, from source 15, which the program stops that long after it started;
 * any=1 keeps the first stop of dumpsource='s, whatever stopped it;
 * altsource= takes every other ring from that source instead, and altmac= with that dumpmac=, see dump.h,
 * so that one run holds two ways side by side;
 * broken=1 has the MAC list the frames it did not receive whole too, which the ring then keeps as frames, and counts.
 * Its newest words are told as dump.h's keys ask, by default 4096 at 80 MHz.
 * Each ring tells how long the program waited, how many packets it let go that the MAC did not take,
 * how many polls before it saw the stop RX_LAST moved, if it did since the ring started, and the state of that frame,
 * how many broken frames the MAC listed meanwhile,
 * and how often its thread ran out of its time, OP_THREAD_OVERRUNS, a thread away from the core missing frames.
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

/*
 * One ring, from its start to a frame the MAC took, or after_us;
 * RX_LAST is read at every poll, so that a kept ring tells how many polls before its stop was seen the MAC named the frame.
 */
static void ring(struct drv *d, const struct dump_conf *conf, uint32_t after_us, uint32_t any)
{
    uint32_t overruns[2] = { 0, 0 }, restarts = 0, kept = 0, moved = 0, restarted = 0, polls = 1;
    uint32_t broken = mac_rx_counts.broken;
    dump_ring_start(conf);
    uint32_t rx_last = mac_rx_last();
    uint64_t start = osi_now_us(), now = start;
    (void)rv_thread_overruns(d->own_thread, &overruns[0]);
    for (; now - start < RING_WAIT_US; polls++) { /* bounded by RING_WAIT_US */
        if (polls % CLOCK_POLLS == 0 || after_us) {
            now = osi_now_us();
        }
        uint32_t last = mac_rx_last();
        if (last != rx_last) {
            rx_last = last;
            moved = polls;
        }
        if (after_us ? now - start < after_us : !dump_ring_stopped()) {
            continue;
        }
        if (after_us || any || moved > restarted) {
            kept = 1;
            break;
        }
        restarts++;
        dump_ring_restart(conf);
        restarted = polls;
        rx_last = mac_rx_last();
    }
    int named = kept && moved > restarted; /* a frame RX_LAST named since the ring last started */
    uint32_t state = named ? mac_rx_last_state() : 0xffffffffu;
    (void)rv_thread_overruns(d->own_thread, &overruns[1]);
    uint32_t took = (uint32_t)(osi_now_us() - start);
    dump_ring_stop(conf); /* by its end the MAC's thread has read the frame */
    drv_say("ring: %s in %u us, %u polls, %u packets let go; RX_LAST moved %d polls before the stop was seen, "
            "its frame's state %x, %u broken frames; the thread ran out of its time %u times\n",
            kept ? "stopped" : "gave up", took, polls, restarts, named ? (int)(polls - moved) : -1, state,
            mac_rx_counts.broken - broken, overruns[1] - overruns[0]);
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
    uint32_t any = config_number(d->conf, d->conf_len, "any", 0);
    conf.source = config_number(d->conf, d->conf_len, "dumpsource", after_us ? 15 : 2) & 15u;
    uint32_t altsource = config_number(d->conf, d->conf_len, "altsource", conf.source) & 15u, rings = 0;
    uint32_t altmac = config_number(d->conf, d->conf_len, "altmac", conf.mac);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("no dumpbt=, the BLE side being i154listen's", conf.bt == 0 ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    mac_sniffer(1);
    mac_sniffer_broken(config_number(d->conf, d->conf_len, "broken", 0) != 0);
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
            struct dump_conf c = conf;
            c.source = rings % 2u ? altsource : conf.source;
            c.mac = rings++ % 2u ? altmac : conf.mac;
            ring(d, &c, after_us, any);
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    mac_sniffer(0);
    mac_rx_give_back();
    drv_radio_off();
    drv_say("ring: %u frames heard; the radio is off\n", frames);
    child_stop(&d->c, DRV_LEFT);
}
