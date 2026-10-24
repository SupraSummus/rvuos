/*
 * The transmitter's DC word, FEDATA 0x600A0444, which librftest's set_txdc_dig writes, I in bits 0 to 9 and Q in 10 to 19,
 * and which cyber-murmel's esp-sdr-zephyr drives from the CPU, a sample at a time, as a transmitter with no bank;
 * see hw/phy.md, "The DC word". The MAC and the PHY come up as the station's do, mac_up, the receiver on channel=, 6 by default,
 * and the transmitter is looped back into the receiver, lb= its gain, 0x20 by default, as prog/loopback.c does,
 * which radiates what the transmitter sends some 22 dB below the radio's 0 dBm, see hw/phy.md, "The loopback".
 *
 * Each second the front end's words around it are told, and then, for each of raws=, the DC word's twenty bits,
 * the receiver's mean through the modem's IQ estimate, phy.c's dc_iq_est_new,
 * with the word as it was and then as raws= has it, as prog/player.c's est=1 takes it.
 *
 * stream=N instead writes the DC word N times from a table of period= words, a tone of amp= counts,
 * as fast as the CPU stores, while the dump engine captures dump= words at dumprate=, see dump.h, and tells them:
 * the tone's line stands at the stores' rate over period=, and the capture says how even they come.
 * play=, a word in hexadecimal, has the player round and round over PLAY_WORDS of it, and raws= each alone and with it,
 * to tell whether the two add up.
 * modemreset=1 pulses the modem's resets before the bring-up, as ESP-IDF's restart does, and tells some words before and after;
 * modemreset=2 first marks the dump block's words, to tell whether the pulse clears them; see hw/README.md, "The modem's resets".
 * pokeat=, an offset in the modem's frame, hexadecimal, has each of pokes= written there in turn, around each second's survey,
 * the word put back after: a probe of what else shapes what the transmitter sends, such as a scale.
 * raws= and pokes= are hexadecimal, small letters, commas between, up to LIST_MAX words.
 *
 * Run it with make BOARD=esp32c6 radio PROG=txdc RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "esp.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the DC word", DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define FE_TXDC     (MODEM_BASE + 0x444u)
#define TXDC_IQ     0xfffffu
#define DUMP_CTRL   (MODEM_BASE + 0x9004u)
#define CTRL_DONE   (1u << 18)
#define LIST_MAX    8u
#define EST_COUNT   1023u
#define EST_SETTLE_US 20u
#define TABLE_MAX   64u
#define STREAM_MAX  1000000u
#define WAIT_POLLS  2000000u

static int up, looped, poked;
static uint32_t txdc_was, poke_at, poke_was;

void drv_radio_off(void)
{
    /* The modem keeps its words through a reset, so whatever this program moved is put back. */
    if (poked) {
        poked = 0;
        REG(MODEM_BASE + poke_at) = poke_was;
    }
    if (up) {
        REG(FE_TXDC) = txdc_was;
    }
    if (looped) {
        looped = 0;
        drv_phy_loopback(0, 0);
    }
    if (up) {
        up = 0;
        mac_stop();
    }
}

/* phy.c's, libphy's dc_iq_est_new: the receiver's mean I and Q over a count, and the power about them in dB. */
void dc_iq_est_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3);

static const int16_t quarter[17] = { 0, 100, 200, 297, 392, 483, 569, 650, 724, 792, 851, 903, 946, 980, 1004, 1019, 1024 };

static int32_t sine(uint32_t phase) /* phase in 1/64 of a turn */
{
    phase &= 63u;
    if (phase < 16u) {
        return quarter[phase];
    }
    if (phase < 32u) {
        return quarter[32u - phase];
    }
    if (phase < 48u) {
        return -quarter[phase - 32u];
    }
    return -quarter[64u - phase];
}

static void tell_words(const char *when)
{
    drv_say("txdc: %s: 0x430 %x 0x434 %x 0x438 %x 0x43c %x 0x440 %x 0x448 %x\n", when, REG(MODEM_BASE + 0x430u),
            REG(MODEM_BASE + 0x434u), REG(MODEM_BASE + 0x438u), REG(MODEM_BASE + 0x43Cu), REG(MODEM_BASE + 0x440u),
            REG(MODEM_BASE + 0x448u));
    drv_say("txdc: %s: 0x410 %x 0x414 %x 0x418 %x 0x420 %x 0x424 %x 0x42c %x 0x444 %x 0x44c %x 0x450 %x\n", when,
            REG(MODEM_BASE + 0x410u), REG(MODEM_BASE + 0x414u), REG(MODEM_BASE + 0x418u), REG(MODEM_BASE + 0x420u),
            REG(MODEM_BASE + 0x424u), REG(MODEM_BASE + 0x42Cu), REG(FE_TXDC), REG(MODEM_BASE + 0x44Cu), REG(MODEM_BASE + 0x450u));
}

/* Words of the modem's that an image before may leave, read before the bring-up. */
static void tell_cold(const char *when)
{
    drv_say("txdc: %s: 0x9804 %x 0x9004 %x 0x900c %x 0x9010 %x 0x4400 %x 0x3004 %x 0x0444 %x 0x0414 %x 0x702c %x 0x0450 %x\n", when,
            REG(MODEM_SYSCON_BASE + 0x4u), REG(MODEM_BASE + 0x9004u), REG(MODEM_BASE + 0x900Cu), REG(MODEM_BASE + 0x9010u),
            REG(MODEM_BASE + 0x4400u), REG(MODEM_BASE + 0x3004u), REG(FE_TXDC), REG(MODEM_BASE + 0x414u),
            REG(MODEM_BASE + 0x702Cu), REG(MODEM_BASE + 0x450u));
}

/* The modem's resets pulsed, as ESP-IDF's restart does, modem_syscon_ll_reset_all and modem_lpcon_ll_reset_all. */
#define MODEM_RST_CONF (MODEM_SYSCON_BASE + 0x10u)
#define LPCON_RST_CONF (MODEM_LPCON_BASE + 0x24u)

static void modem_reset(void)
{
    REG(MODEM_RST_CONF) = 0xffffffffu;
    REG(MODEM_RST_CONF) = 0;
    REG(LPCON_RST_CONF) = 0xfu;
    REG(LPCON_RST_CONF) = 0;
}

#define PLAY        (MODEM_BASE + 0x900Cu)
#define PLAY_FAST   (1u << 15)
#define PLAY_LOOP   (1u << 19)
#define PLAY_ENABLE (1u << 31)
#define PLAY_WORDS  4096u
#define NONE        0xffffffffu

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;

/*
 * The player round and round over PLAY_WORDS of played, and the DC word as raws= has it, each alone and both:
 * whether the DC word adds to what the player sends, as the receiver's mean through the IQ estimate says.
 */
static void with_player(uint32_t played, const uint32_t *raws, uint32_t nraws)
{
    for (uint32_t r = 0; r < nraws; r++) {
        int32_t e[4][3];
        for (uint32_t k = 0; k < PLAY_WORDS; k++) {
            bank[k] = played;
        }
        for (uint32_t m = 0; m < 4u; m++) {
            uint32_t usage = 0;
            if (m & 1u) {
                usage = dump_bank_give();
                REG(PLAY) = 0;
                REG(PLAY) = PLAY_WORDS | PLAY_FAST | PLAY_LOOP | PLAY_ENABLE;
            }
            REG(FE_TXDC) = m & 2u ? (txdc_was & ~TXDC_IQ) | (raws[r] & TXDC_IQ) : txdc_was;
            ets_delay_us(EST_SETTLE_US);
            dc_iq_est_new(1, EST_COUNT, e[m], 0);
            REG(FE_TXDC) = txdc_was;
            if (m & 1u) {
                REG(PLAY) = 0;
                dump_bank_take(usage);
            }
        }
        drv_say("txdc: played %x, raw %x: none I %d Q %d; player I %d Q %d; DC I %d Q %d; both I %d Q %d; "
                "sum of the two moves I %d Q %d\n", played, raws[r], e[0][0], e[0][1], e[1][0], e[1][1], e[2][0], e[2][1],
                e[3][0], e[3][1], e[1][0] + e[2][0] - e[0][0], e[1][1] + e[2][1] - e[0][1]);
    }
}

/* Each of raws= written in turn: the receiver's mean with the word as it was, and as written. */
static void survey(const uint32_t *raws, uint32_t nraws)
{
    for (uint32_t r = 0; r < nraws; r++) {
        int32_t quiet[3], heard[3];
        REG(FE_TXDC) = txdc_was;
        ets_delay_us(EST_SETTLE_US);
        dc_iq_est_new(1, EST_COUNT, quiet, 0);
        REG(FE_TXDC) = (txdc_was & ~TXDC_IQ) | (raws[r] & TXDC_IQ);
        ets_delay_us(EST_SETTLE_US);
        dc_iq_est_new(1, EST_COUNT, heard, 0);
        REG(FE_TXDC) = txdc_was;
        drv_say("txdc: raw %x: was I %d Q %d %d dB, written I %d Q %d %d dB, moved I %d Q %d\n", raws[r], quiet[0],
                quiet[1], quiet[2], heard[0], heard[1], heard[2], heard[0] - quiet[0], heard[1] - quiet[1]);
    }
}

/* The DC word written n times round a table, as fast as the CPU stores, under a capture; the capture told. */
static void stream(struct dump_conf *c, uint32_t n, uint32_t period, uint32_t amp)
{
    uint32_t table[TABLE_MAX];
    for (uint32_t k = 0; k < period; k++) {
        uint32_t phase = k * 64u / period;
        int32_t i = sine(phase + 16u) * (int32_t)amp / 1024, q = sine(phase) * (int32_t)amp / 1024;
        table[k] = (txdc_was & ~TXDC_IQ) | ((uint32_t)q & 0x3ffu) << 10 | ((uint32_t)i & 0x3ffu);
    }
    dump_arm(c, 1);
    uint64_t start = osi_now_us();
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)FE_TXDC;
    for (uint32_t k = 0, j = 0; k < n; k++) { /* bounded by n, STREAM_MAX at most */
        *w = table[j];
        j = j + 1u == period ? 0 : j + 1u;
    }
    uint64_t end = osi_now_us();
    REG(FE_TXDC) = txdc_was;
    uint32_t polls = 0;
    for (; (REG(DUMP_CTRL) & CTRL_DONE) == 0 && polls < WAIT_POLLS; polls++) { /* bounded by WAIT_POLLS */
    }
    uint32_t written = dump_disarm(c);
    uint32_t us = (uint32_t)(end - start);
    drv_say("txdc: stream of %u stores, period %u, amp %u, in %u us, %u kS/s; the capture %u of %u written, %u polls after\n",
            n, period, amp, us, us ? (uint32_t)((uint64_t)n * 1000u / us) : 0, written, c->samples, polls);
    dump_tell(c);
}

/* A key's words in hexadecimal, small letters, commas between, up to LIST_MAX of them; how many. */
static uint32_t hex_list(struct drv *d, const char *key, uint32_t *out)
{
    uint32_t n = 0;
    char list[96];
    list[0] = '\0';
    config_value(d->conf, d->conf_len, key, list, sizeof(list));
    const char *c = list;
    for (; *c != '\0' && n < LIST_MAX;) {
        uint32_t v = 0, digits = 0;
        for (; (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'); c++, digits++) {
            v = v << 4 | (uint32_t)(*c <= '9' ? *c - '0' : *c - 'a' + 10);
        }
        drv_must("a list's words in hexadecimal, small letters, commas between",
                 digits != 0 && digits <= 8 ? ESP_OK : ESP_FAIL);
        out[n++] = v;
        if (*c == ',') {
            c++;
        } else {
            drv_must("a list's words separated by commas alone", *c == '\0' ? ESP_OK : ESP_FAIL);
            break;
        }
    }
    drv_must("a list of at most LIST_MAX words", *c == '\0' ? ESP_OK : ESP_FAIL);
    return n;
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 6);
    uint32_t lb = config_number(d->conf, d->conf_len, "lb", 0x20);
    uint32_t n = config_number(d->conf, d->conf_len, "stream", 0);
    uint32_t period = config_number(d->conf, d->conf_len, "period", 16);
    uint32_t amp = config_number(d->conf, d->conf_len, "amp", 64);
    uint32_t raws[LIST_MAX], nraws = hex_list(d, "raws", raws);
    drv_must("a channel of 3 to 11, the stream's tone within the band", channel >= 3 && channel <= 11 ? ESP_OK : ESP_FAIL);
    drv_must("a period of 2 to TABLE_MAX", period >= 2 && period <= TABLE_MAX ? ESP_OK : ESP_FAIL);
    drv_must("an amp of at most 511", amp <= 511 ? ESP_OK : ESP_FAIL);
    drv_must("a stream of at most STREAM_MAX", n <= STREAM_MAX ? ESP_OK : ESP_FAIL);
    drv_must("a stream with a capture", n == 0 || conf.samples != 0 ? ESP_OK : ESP_FAIL);
    uint32_t reset = config_number(d->conf, d->conf_len, "modemreset", 0);
    uint32_t pokes[LIST_MAX], npokes = hex_list(d, "pokes", pokes), pokeat = 0;
    {
        uint32_t at[LIST_MAX];
        if (hex_list(d, "pokeat", at) != 0) {
            pokeat = at[0];
        }
    }
    drv_must("pokeat= a word of the modem's FEDATA block, 0x400 to 0x4fc", npokes == 0 ||
             (pokeat >= 0x400u && pokeat <= 0x4fcu && (pokeat & 3u) == 0) ? ESP_OK : ESP_FAIL);
    uint32_t played = NONE, plays[LIST_MAX];
    if (hex_list(d, "play", plays) != 0) {
        played = plays[0];
    }
    tell_cold("cold");
    if (reset) {
        if (reset == 2) {
            /* Marks in the dump block's words, the player's count and loop bit without bit 31, and its page. */
            REG(MODEM_BASE + 0x900Cu) = 0x00081000u;
            REG(MODEM_BASE + 0x9010u) = 0x10000000u;
            REG(MODEM_BASE + 0x9004u) = 0x00001000u;
            tell_cold("marked");
        }
        modem_reset();
        tell_cold("reset");
    }
    mac_up(d->mac);
    dump_known();
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    txdc_was = REG(FE_TXDC);
    up = 1;
    child_report(&d->c, DRV_LISTENING);
    tell_words("up");
    uint64_t told = osi_now_us();
    for (;;) {
        uint32_t wake;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            told = now;
            drv_phy_loopback(1, lb);
            looped = 1;
            txdc_was = REG(FE_TXDC);
            tell_words("looped");
            for (uint32_t k = 0; k < (npokes != 0 ? npokes : 1u); k++) {
                if (npokes != 0) {
                    poke_at = pokeat;
                    poke_was = REG(MODEM_BASE + pokeat);
                    poked = 1;
                    REG(MODEM_BASE + pokeat) = pokes[k];
                    drv_say("txdc: %x was %x, now %x, reads %x\n", pokeat, poke_was, pokes[k], REG(MODEM_BASE + pokeat));
                }
                if (n != 0) {
                    stream(&conf, n, period, amp);
                } else if (played != NONE) {
                    with_player(played, raws, nraws);
                } else {
                    survey(raws, nraws);
                }
                if (npokes != 0) {
                    REG(MODEM_BASE + pokeat) = poke_was;
                    poked = 0;
                }
                child_answer(&d->c);
            }
            drv_phy_loopback(0, 0);
            looped = 0;
        }
        rv_wait(CHILD_INBOX, &wake);
    }
    drv_radio_off();
    drv_say("txdc: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
