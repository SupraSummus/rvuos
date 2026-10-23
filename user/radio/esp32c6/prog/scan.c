/*
 * The scan: the receiver swept off the channels' grid, a capture of the dump engine at each step, see dump.h.
 * The MAC and the PHY come up as the station's do, mac_up, calibrated on channel=, 1 by default;
 * then the RF PLL is tuned from from= to to=, in kHz, a step of step= kHz at a time, drv_phy_tune in phy.c,
 * and each step tells its frequency, the PLL's state and capacitance, and takes a capture as dump.h's keys ask,
 * pass after pass until the run ends; user/radio/esp32c6/tools/dump-spectrum.py, given --scan, puts the steps' spectra side by side.
 * gain= holds the receiver's gain index, 0 to the gain table's last, where the AGC chooses it otherwise,
 * and gainstep= raises it by that much each pass, round the table, so that one run compares the gains;
 * filter= sets the receive's analog filter's code, 0 to 63, 0 the widest, where the RC calibration's stays otherwise,
 * and filterstep= raises it each pass, round the 64, as gainstep= the gain; a step's line tells both, -1 for left alone.
 * divided=1 tunes every step with the receiver's LO divided, see drv_phy_tune, 0 none, 2 each step both ways, direct first;
 * left out, the steps below 2210 MHz alone, where the PLL does not reach direct.
 * By default 2390 to 2500 MHz in steps of 5 MHz, 1024 samples at 80 MHz, every word told:
 * at a lower rate the engine folds what lies past half of it into the band, see hw/dump.md.
 * The scan sends nothing: no frame, no tone; the hardware's frequency setting is the PHY's again at its end.
 *
 * Run it with make BOARD=esp32c6 radio PROG=scan RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the scan", DRV_NEED_DUMP);

#define TUNE_LOWEST_KHZ 1600000u /* below it the synthesizer's word, 6/5 f / 60 MHz less 32, would not fit; see phy.c */
#define TUNE_HIGHEST_KHZ 4000000u
#define NONE             0xffffffffu /* a key left out */

static int up, tuned;
static struct dump_conf conf;
static uint32_t calibrated_filter = NONE;

void drv_radio_off(void)
{
    if (calibrated_filter != NONE) {
        drv_phy_rx_filter(calibrated_filter);
        calibrated_filter = NONE;
    }
    if (tuned) {
        tuned = 0;
        drv_phy_tune_release();
    }
    if (up) {
        up = 0;
        drv_phy_rx_gain(0, 0);
        mac_stop();
    }
}

/* A step's capture, the LO divided or not. */
static void take(struct drv *d, uint32_t pass, uint32_t khz, uint32_t divided, uint32_t g, uint32_t f)
{
    int32_t cap;
    uint32_t state = drv_phy_tune(khz, divided, &cap);
    tuned = 1;
    drv_say("scan: pass %u at %u kHz, PLL %x, divided %u, capacitance %d, gain %d, filter %d\n", pass, khz, state,
            divided, (int)cap, g == NONE ? -1 : (int)g, f == NONE ? -1 : (int)f);
    dump_take(&conf);
    child_answer(&d->c);
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    dump_configure(d->conf, d->conf_len, &conf);
    if (conf.samples == 0) {
        conf = (struct dump_conf){ .samples = 1024, .source = 15, .rate = 0, .hex = 1024 };
    }
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 1);
    uint32_t from = config_number(d->conf, d->conf_len, "from", 2390000);
    uint32_t to = config_number(d->conf, d->conf_len, "to", 2500000);
    uint32_t step = config_number(d->conf, d->conf_len, "step", 5000);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("from= and to= within the PLL's reach, from= below to=",
             from >= TUNE_LOWEST_KHZ && to <= TUNE_HIGHEST_KHZ && from < to && step != 0 ? ESP_OK : ESP_FAIL);
    drv_must("no dumpbt=, the BLE side being i154listen's", conf.bt == 0 ? ESP_OK : ESP_FAIL);
    uint32_t gain = config_number(d->conf, d->conf_len, "gain", NONE);
    uint32_t gainstep = config_number(d->conf, d->conf_len, "gainstep", 0);
    uint32_t filter = config_number(d->conf, d->conf_len, "filter", NONE);
    uint32_t filterstep = config_number(d->conf, d->conf_len, "filterstep", 0);
    drv_must("a filter of 0 to 63", filter == NONE || filter <= 63 ? ESP_OK : ESP_FAIL);
    uint32_t divided = config_number(d->conf, d->conf_len, "divided", NONE);
    drv_must("divided= of 0, 1 or 2", divided == NONE || divided <= 2 ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    drv_must_mac("the channel", mac_channel(channel));
    uint32_t last = drv_phy_rx_gain_last();
    drv_must("a gain of 0 to the gain table's last", gain == NONE || gain <= last ? ESP_OK : ESP_FAIL);
    drv_say("scan: the gain table's last index %u\n", last);
    if (filter != NONE) {
        calibrated_filter = drv_phy_rx_filter(filter);
        drv_say("scan: the filter's code %u, the RC calibration's %u\n", filter, calibrated_filter);
    }
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    for (uint32_t pass = 0; !d->leave; pass++) {
        uint32_t g = gain == NONE ? NONE : (gain + pass * gainstep) % (last + 1u);
        uint32_t f = filter == NONE ? NONE : (filter + pass * filterstep) % 64u;
        if (g != NONE) {
            drv_phy_rx_gain(1, g);
        }
        if (f != NONE) {
            (void)drv_phy_rx_filter(f);
        }
        for (uint32_t khz = from; khz <= to && !d->leave; khz += step) { /* bounded by to= */
            if (divided == NONE) {
                take(d, pass, khz, khz < DRV_TUNE_DIRECT_LOWEST_KHZ, g, f);
                continue;
            }
            if (divided != 1) {
                take(d, pass, khz, 0, g, f);
            }
            if (divided != 0) {
                take(d, pass, khz, 1, g, f);
            }
        }
    }
    drv_radio_off();
    drv_say("scan: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
