/*
 * The PHY brought up and tuned, a channel of each band in turn, with nothing sent but what its calibration sends:
 * the clocks and the calibration, then each channel of channels= tuned and held a second,
 * and the writes the adapter feigned for the library to registers outside the frames, see the board's radio.h.
 * A channel the program does not know is refused; by default it tunes 1 and 36, 2412 and 5180 MHz.
 *
 * Run it with make BOARD=esp32c5 radio PROG=phyup RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "drv.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the PHY brought up", 0);

#define CHANNELS_MAX 8u

static int up;

void drv_radio_off(void)
{
    if (up) {
        up = 0;
        drv_phy_disable();
    }
}

static uint32_t now_us(uint32_t clock)
{
    uint64_t now;
    uint32_t hz;
    rv_clock_read(clock, &now, &hz);
    return (uint32_t)(now * 1000000u / hz);
}

/* The numbers of a key's value, as many as there is room for, separated by commas; 0 if the key is not there. */
static uint32_t numbers(const struct drv *d, const char *key, uint32_t *out, uint32_t room)
{
    char text[64];
    config_value(d->conf, d->conf_len, key, text, sizeof(text));
    uint32_t n = 0, v = 0, digits = 0;
    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            digits++;
        } else {
            if (digits != 0 && n < room) {
                out[n++] = v;
            }
            v = digits = 0;
            if (*p == 0) {
                return n;
            }
        }
    }
}

/* A channel of 2.4 GHz's 1 to 13 or of 5 GHz's 20 MHz channels from 36 to 165. */
static int known(uint32_t channel)
{
    return (channel >= 1 && channel <= 13) || (channel >= 36 && channel <= 165 && channel % 4 == 0) ||
           (channel >= 149 && channel <= 165 && channel % 4 == 1);
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    uint32_t channels[CHANNELS_MAX] = { 1, 36 };
    uint32_t count = numbers(d, "channels", channels, CHANNELS_MAX);
    if (count == 0) {
        count = 2;
    }
    for (uint32_t i = 0; i < count; i++) {
        drv_must("a channel the program knows", known(channels[i]) ? ESP_OK : ESP_FAIL);
    }
    uint32_t t0 = now_us(d->clock);
    drv_wifi_clock_enable();
    drv_phy_enable();
    up = 1;
    drv_say("phyup: up in %u us\n", now_us(d->clock) - t0);
    osi_feigned_dump();
    for (uint32_t i = 0; i < count; i++) {
        t0 = now_us(d->clock);
        drv_phy_channel(channels[i]);
        d->channel = channels[i];
        drv_say("phyup: channel %u, %u MHz, tuned in %u us\n", channels[i], drv_phy_channel_mhz(channels[i]),
                now_us(d->clock) - t0);
        osi_delay_ms(1000);
    }
    osi_feigned_dump();
    child_report(&d->c, DRV_LISTENING);
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("phyup: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
