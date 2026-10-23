/*
 * The sampler: what the receiver hears, a capture a second through the modem's dump engine, see dump.h.
 * The MAC and the PHY come up as the station's do, mac_up, the receiver on a Wi-Fi channel, channel=, 1 by default,
 * every frame the MAC passes counted, and each second a capture taken and told, as dump.h's keys ask;
 * the run ends when the root task asks the program to leave.
 * The sampler sends nothing: no frame, no tone.
 *
 * Run it with make BOARD=esp32c6 radio PROG=sampler RADIO_CONFIG=file, see build.mk,
 * and read the captures with user/radio/esp32c6/tools/dump-spectrum.py.
 */

#include <stdint.h>

#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the sampler", DRV_NEED_DUMP);

static int up; /* whether the MAC and the PHY are up, so that the radio is to be turned off */
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

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("no dumpbt=, the BLE side being i154listen's", conf.bt == 0 ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    mac_sniffer(1);
    drv_must_mac("the channel", mac_channel(channel));
    drv_must_mac("the MAC receives into the sampler's list", mac_rx_take(heard));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    uint64_t told = osi_now_us();
    uint32_t then = 0;
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            told = now;
            uint32_t f = frames;
            drv_say("sampler: %u frames in the last second\n", f - then);
            then = f;
            dump_take(&conf);
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    mac_sniffer(0);
    mac_rx_give_back();
    drv_radio_off();
    drv_say("sampler: %u frames heard; the radio is off\n", frames);
    child_stop(&d->c, DRV_LEFT);
}
