/*
 * The loopback: the transmitter looped back into the receiver, as the receive IQ calibration loops it at every bring-up,
 * and what the receiver hears of it through the modem's dump engine, see dump.h.
 * The MAC and the PHY come up as the station's do, mac_up, the receiver on a Wi-Fi channel, channel=, 6 by default,
 * the calibration's; each second three captures are taken and told, as dump.h's keys ask:
 * the receiver alone, then looped back with nothing sent, then looped back with the transmit tone generator's tone,
 * the calibration's own, step= its frequency, 0x80 by default, and atten= its attenuation, 0x30 by default, drv_phy_tone in phy.c:
 * the generator's level is 256 less atten=, so a larger one is a weaker tone, and none below the calibration's 0x30 is taken.
 * stepstep= moves the step on by that much each capture, round its ten bits, so that one run measures what a step is.
 * lb= is the loopback's gain, 0x20 by default, the calibration's first; gain= holds the receiver's gain index,
 * which the AGC chooses otherwise.
 * hold=, in milliseconds, up to HOLD_MAX_MS, keeps the tone and the loopback on that long after the capture,
 * so that a receiver outside, the nRF52840's survey of the energy, has time to hear what the antenna gives off of it.
 * rhythm=, seconds up to RHYTHM_MAX_S, first keeps the loopback on that long and switches the tone on and off
 * every half= milliseconds, 50 by default, before the captures begin:
 * a receiver outside that samples the power through it, the nRF52840's watch= of user/thread/root.c,
 * finds what the antenna gives off as a line at the rhythm's rate, against the rates beside it.
 * The tone lasts as long as its capture, tens of microseconds at 80 MHz, a second apart, at the calibration's level,
 * the capture told only once the tone and the loopback are off;
 * the loopback does not keep it off the air, which hears it about 22 dB below the radio's 0 dBm, see hw/phy.md, "The loopback".
 * muxes=1 clears MODEM_SYSCON's CLK_DATA_DUMP_MUX in the odd seconds, the dump block's clock 32 MHz instead of 80,
 * and moves dumprate= on by one every two seconds, round its four, so that one run takes every clock and divider.
 * It sends no frame.
 *
 * Run it with make BOARD=esp32c6 radio PROG=loopback RADIO_CONFIG=file, see build.mk,
 * and read the captures with user/radio/esp32c6/tools/dump-spectrum.py.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the loopback", DRV_NEED_DUMP);

#define NONE 0xffffffffu /* a key left out */
#define HOLD_MAX_MS 500u  /* half of each second, so that the program still answers the root task each second */
#define RHYTHM_MAX_S 60u
#define HALF_MIN_MS  5u
#define HALF_MAX_MS  500u
#define ATTEN_LEAST  0x30u /* the calibration's own, the loudest this program sends */
#define ATTEN_MOST   0xffu
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLK_CONF     (MODEM_SYSCON_BASE + 0x4u)
#define CLK_DUMP_MUX (1u << 21) /* MODEM_SYSCON_CLK_DATA_DUMP_MUX, 1 after the start, see hw/dump.md "A player" */

static int up, looped, toned, held;

void drv_radio_off(void)
{
    if (toned) {
        toned = 0;
        drv_phy_tone(0, 0, 0);
    }
    if (looped) {
        looped = 0;
        drv_phy_loopback(0, 0);
    }
    if (up) {
        up = 0;
        if (held) {
            held = 0;
            drv_phy_rx_gain(0, 0);
        }
        mac_stop();
    }
}

/*
 * The rhythm: the loopback on for seconds, and the tone on for the first half of each period and off for the second,
 * the edges taken from the clock, so that their rate is the crystal's however late a wake comes.
 */
static void play_rhythm(struct drv *d, uint32_t seconds, uint32_t half, uint32_t step, uint32_t atten, uint32_t lb)
{
    uint64_t start = osi_now_us(), end = start + (uint64_t)seconds * 1000000u;
    uint32_t edges = 0;
    drv_phy_loopback(1, lb);
    looped = 1;
    drv_say("loopback: rhythm for %u s, the tone %u ms on and %u ms off, step %u atten %u\n", seconds, half, half, step,
            atten);
    for (uint64_t now = start; now < end && !d->leave; now = osi_now_us()) { /* bounded by seconds */
        int on = ((now - start) / (half * 1000u)) % 2u == 0;
        if (on != toned) {
            drv_phy_tone((uint32_t)on, step, atten);
            toned = on;
            edges++;
        }
        child_answer(&d->c);
        osi_delay_ms(1);
    }
    drv_phy_tone(0, 0, 0);
    toned = 0;
    drv_phy_loopback(0, 0);
    looped = 0;
    drv_say("loopback: rhythm over, %u edges in %u us\n", edges, (uint32_t)(osi_now_us() - start));
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 6);
    uint32_t step = config_number(d->conf, d->conf_len, "step", 0x80);
    uint32_t stepstep = config_number(d->conf, d->conf_len, "stepstep", 0);
    uint32_t atten = config_number(d->conf, d->conf_len, "atten", ATTEN_LEAST);
    uint32_t lb = config_number(d->conf, d->conf_len, "lb", 0x20);
    uint32_t gain = config_number(d->conf, d->conf_len, "gain", NONE);
    uint32_t hold = config_number(d->conf, d->conf_len, "hold", 0);
    uint32_t rhythm = config_number(d->conf, d->conf_len, "rhythm", 0);
    uint32_t half = config_number(d->conf, d->conf_len, "half", 50);
    uint32_t muxes = config_number(d->conf, d->conf_len, "muxes", 0);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("no dumpbt=, the BLE side being i154listen's", conf.bt == 0 ? ESP_OK : ESP_FAIL);
    drv_must("an atten of ATTEN_LEAST, the calibration's, to ATTEN_MOST", atten >= ATTEN_LEAST && atten <= ATTEN_MOST ? ESP_OK : ESP_FAIL);
    drv_must("a hold of at most HOLD_MAX_MS", hold <= HOLD_MAX_MS ? ESP_OK : ESP_FAIL);
    drv_must("a rhythm of at most RHYTHM_MAX_S", rhythm <= RHYTHM_MAX_S ? ESP_OK : ESP_FAIL);
    drv_must("a half of HALF_MIN_MS to HALF_MAX_MS", half >= HALF_MIN_MS && half <= HALF_MAX_MS ? ESP_OK : ESP_FAIL);
    mac_up(d->mac);
    up = 1;
    drv_must_mac("the channel", mac_channel(channel));
    if (gain != NONE) {
        drv_must("a gain within the table", gain <= drv_phy_rx_gain_last() ? ESP_OK : ESP_FAIL);
        drv_phy_rx_gain(1, gain);
        held = 1;
    }
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    if (rhythm != 0) {
        play_rhythm(d, rhythm, half, step, atten, lb);
    }
    uint64_t told = osi_now_us();
    for (uint32_t second = 0;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            told = now;
            uint32_t s = (step + second * stepstep) & 0x3ffu, clk = REG(CLK_CONF);
            if (muxes) {
                conf.rate = (second / 2u) % 4u;
                REG(CLK_CONF) = second % 2u ? clk & ~CLK_DUMP_MUX : clk | CLK_DUMP_MUX;
                drv_say("loopback: second %u: the dump clock's mux %u, dumprate %u\n", second,
                        (REG(CLK_CONF) & CLK_DUMP_MUX) != 0, conf.rate);
            }
            drv_say("loopback: second %u: alone\n", second);
            dump_take(&conf);
            drv_phy_loopback(1, lb);
            looped = 1;
            drv_say("loopback: second %u: looped, quiet\n", second);
            dump_take(&conf);
            drv_phy_tone(1, s, atten);
            toned = 1;
            dump_capture(&conf);
            osi_delay_ms(hold);
            drv_phy_tone(0, 0, 0);
            toned = 0;
            drv_phy_loopback(0, 0);
            looped = 0;
            REG(CLK_CONF) = clk;
            drv_say("loopback: second %u: looped, tone step %u atten %u\n", second, s, atten);
            dump_tell(&conf);
            second++;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("loopback: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
