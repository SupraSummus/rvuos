/*
 * The transmitter's analog filter, as a hypothesis block 0x67's registers 12 and 13, and 14 and 15, which
 * jochenhammes's esp32-sdr-trx found on the ESP32-S3 and esp-sdr-zephyr names on the ESP32-C6 as the transmit
 * low-pass pairs; see hw/phy.md, "The transmitter's filter".
 * The MAC and the PHY come up as the station's do, mac_up, the receiver on channel=, 6 by default,
 * its gain held at gain=, 30 by default, and its own filter at rxf=, 0 by default, the widest, drv_phy_rx_filter.
 * Each second, for each of codes=, written into both registers of the pair regs= names, 12 by default or 14,
 * and for each of steps=, the transmit tone generator's, drv_phy_tone, looped back as prog/loopback.c loops it,
 * a capture of 2048 words at 80 MHz is taken and the line's power told, a single bin of its spectrum,
 * where a step of 39.0625 kHz falls on a bin of its own, and at its mirror's, below the carrier, the control;
 * the code the PHY left is put back after each.
 * The tone goes out some 22 dB below the radio's 0 dBm for the capture's 26 us, see hw/phy.md, "The loopback".
 * codes= and steps= are hexadecimal, small letters, commas between, up to LIST_MAX words; a step is ten bits,
 * unsigned, and every one must reach within the band and lie below 512, see drv_phy_tone_fits.
 *
 * rhythm=, seconds up to RHYTHM_MAX_S, first keeps the loopback and the tone of step=, in decimal, on that long,
 * the tone switched off and on every tonehalf= milliseconds, 71 by default, or with 0 on throughout,
 * and the pair's code between codes='s first two every codehalf=, 50 by default,
 * for the nRF52840's watch= of user/thread/root.c to find each at its rate in what the antenna gives off:
 * the tone's rhythm the run's own control, the code's what the filter does to the air.
 *
 * Run it with make BOARD=esp32c6 radio PROG=txfilter RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the transmitter's filter", DRV_NEED_DUMP);

#define LIST_MAX   8u
#define WORDS      2048u /* 80 MHz over 2048 is 39.0625 kHz, the tone's step */
#define TONE_ATTEN 0x30u /* the calibration's, prog/loopback.c's least */
#define I2C_BBTOP  0x67u
#define I2C_HOST   1u
#define CODE_KEEP  0x100u
#define RHYTHM_MAX_S 60u
#define HALF_MIN_MS  5u
#define HALF_MAX_MS  500u
#define ANSWER_MS    1u

typedef void (*romfunc)(void);
romfunc *phy_get_romfuncs(void);
#define ROMFUNC_I2C_WRITE_REG 22 /* rom_i2c_writeReg, as prog/bbpll.c reaches it */

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static int up, looped, toned, held;
static uint32_t pair, was_a = CODE_KEEP, was_b = CODE_KEEP;

static void i2c_write(uint8_t reg, uint8_t data)
{
    ((void (*)(uint8_t, uint8_t, uint8_t, uint8_t))phy_get_romfuncs()[ROMFUNC_I2C_WRITE_REG])(I2C_BBTOP, I2C_HOST, reg, data);
}

static void put_back(void)
{
    if (was_a != CODE_KEEP) {
        i2c_write((uint8_t)pair, (uint8_t)was_a);
        i2c_write((uint8_t)(pair + 1u), (uint8_t)was_b);
    }
}

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
        put_back();
        if (held) {
            held = 0;
            drv_phy_rx_gain(0, 0);
        }
        up = 0;
        mac_stop();
    }
}

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

static int32_t field10(uint32_t w, uint32_t at)
{
    int32_t v = (int32_t)((w >> at) & 0x3ffu);
    return v >= 512 ? v - 1024 : v;
}

/* 10 log10 v in tenths of a dB, through log2 by repeated squaring, twelve bits of its fraction. */
static uint32_t db10(uint64_t v)
{
    uint32_t msb = 0;
    for (uint64_t t = v; t > 1u; t >>= 1) { /* bounded by the word's 64 bits */
        msb++;
    }
    uint64_t x = msb >= 30u ? v >> (msb - 30u) : v << (30u - msb); /* v over 2^msb, Q30 in [1, 2) */
    uint32_t frac = 0;
    for (uint32_t k = 0; k < 12u; k++) {
        x = (x * x) >> 30;
        frac <<= 1;
        if (x >= (2ull << 30)) {
            x >>= 1;
            frac |= 1u;
        }
    }
    uint64_t log2q = (uint64_t)msb << 12 | frac;
    return (uint32_t)(log2q * 30103u / 4096000u);
}

/* The captured words' power at a bin of WORDS, its I in bits 10 to 19 and Q in 0 to 9, mean taken out, in tenths of a dB. */
static uint32_t bin_power(uint32_t bin, int32_t *peak)
{
    int64_t si = 0, sq = 0, re = 0, im = 0;
    int32_t top = 0;
    for (uint32_t n = 0; n < WORDS; n++) {
        si += field10(bank[n], 10);
        sq += field10(bank[n], 0);
    }
    int32_t mi = (int32_t)(si / WORDS), mq = (int32_t)(sq / WORDS);
    for (uint32_t n = 0; n < WORDS; n++) {
        int32_t i = field10(bank[n], 10) - mi, q = field10(bank[n], 0) - mq;
        uint32_t phase = (uint32_t)(((uint64_t)bin * n * 64u / WORDS) & 63u);
        int32_t c = sine(phase + 16u), s = sine(phase);
        /* (i + jq) times e^(-j phase) */
        re += (int64_t)i * c + (int64_t)q * s;
        im += (int64_t)q * c - (int64_t)i * s;
        int32_t a = i < 0 ? -i : i;
        top = a > top ? a : top;
    }
    *peak = top;
    re /= 1024;
    im /= 1024;
    return db10((uint64_t)(re * re + im * im) + 1u);
}

static void set_code(uint32_t code)
{
    uint32_t a = code == CODE_KEEP ? was_a : code, b = code == CODE_KEEP ? was_b : code;
    i2c_write((uint8_t)pair, (uint8_t)a);
    i2c_write((uint8_t)(pair + 1u), (uint8_t)b);
}

/* The rhythm: the tone off and on every tone_ms, and the code between two every code_ms, each edge from the clock. */
static void rhythm(struct drv *d, uint32_t seconds, uint32_t step, uint32_t lb, const uint32_t codes[2], uint32_t tone_ms,
                   uint32_t code_ms)
{
    uint64_t start = osi_now_us(), end = start + (uint64_t)seconds * 1000000u;
    uint32_t edges = 0, code = 2;
    drv_phy_loopback(1, lb);
    looped = 1;
    drv_say("txfilter: rhythm for %u s, step %u, the tone every %u ms, the code %x or %x every %u ms\n", seconds, step,
            tone_ms, codes[0], codes[1], code_ms);
    for (uint64_t now = start; now < end && !d->leave; now = osi_now_us()) { /* bounded by seconds */
        int on = tone_ms == 0 || ((now - start) / (tone_ms * 1000u)) % 2u == 0;
        uint32_t c = (uint32_t)(((now - start) / (code_ms * 1000u)) % 2u);
        if (on != toned) {
            if (on) {
                drv_phy_tone(1, step, TONE_ATTEN);
            } else {
                drv_phy_tone(0, 0, 0);
            }
            toned = on;
            edges++;
        }
        if (c != code) {
            set_code(codes[c]);
            code = c;
            edges++;
        }
        child_answer(&d->c);
        osi_delay_ms(ANSWER_MS);
    }
    drv_phy_tone(0, 0, 0);
    toned = 0;
    drv_phy_loopback(0, 0);
    looped = 0;
    put_back();
    drv_say("txfilter: rhythm over, %u edges in %u us\n", edges, (uint32_t)(osi_now_us() - start));
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
    struct dump_conf conf = { 0 };
    uint32_t codes[LIST_MAX], steps[LIST_MAX];
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 6);
    uint32_t gain = config_number(d->conf, d->conf_len, "gain", 30);
    uint32_t rxf = config_number(d->conf, d->conf_len, "rxf", 0);
    pair = config_number(d->conf, d->conf_len, "regs", 12);
    uint32_t lb = config_number(d->conf, d->conf_len, "lb", 0x20);
    uint32_t ncodes = hex_list(d, "codes", codes), nsteps = hex_list(d, "steps", steps);
    uint32_t seconds = config_number(d->conf, d->conf_len, "rhythm", 0);
    uint32_t step = config_number(d->conf, d->conf_len, "step", 0x80);
    uint32_t tone_ms = config_number(d->conf, d->conf_len, "tonehalf", 71);
    uint32_t code_ms = config_number(d->conf, d->conf_len, "codehalf", 50);
    drv_must("a rhythm of at most RHYTHM_MAX_S", seconds <= RHYTHM_MAX_S ? ESP_OK : ESP_FAIL);
    drv_must("halves of HALF_MIN_MS to HALF_MAX_MS, or tonehalf=0", (tone_ms == 0 || tone_ms >= HALF_MIN_MS) && tone_ms <= HALF_MAX_MS &&
             code_ms >= HALF_MIN_MS && code_ms <= HALF_MAX_MS ? ESP_OK : ESP_FAIL);
    drv_must("a rhythm with two codes", seconds == 0 || ncodes >= 2 ? ESP_OK : ESP_FAIL);
    if (seconds != 0) {
        steps[nsteps < LIST_MAX ? nsteps : LIST_MAX - 1u] = step;
        nsteps += nsteps < LIST_MAX;
    }
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("regs= 12 or 14", pair == 12 || pair == 14 ? ESP_OK : ESP_FAIL);
    drv_must("an rxf of 0 to 63", rxf <= 63 ? ESP_OK : ESP_FAIL);
    drv_must("codes= and steps=", ncodes != 0 && nsteps != 0 ? ESP_OK : ESP_FAIL);
    for (uint32_t k = 0; k < ncodes; k++) {
        drv_must("a code of a byte, or 100 for the PHY's own", codes[k] <= CODE_KEEP ? ESP_OK : ESP_FAIL);
    }
    for (uint32_t k = 0; k < nsteps; k++) {
        drv_must("every step within the band, below 512", steps[k] < 1024u && drv_phy_tone_fits(channel, steps[k]) ? ESP_OK : ESP_FAIL);
    }
    conf.samples = WORDS;
    conf.source = 15;
    mac_up(d->mac);
    up = 1;
    dump_known();
    drv_must_mac("the channel", mac_channel(channel));
    drv_must("a gain within the table", gain <= drv_phy_rx_gain_last() ? ESP_OK : ESP_FAIL);
    drv_phy_rx_gain(1, gain);
    held = 1;
    uint32_t rxf_was = drv_phy_rx_filter(rxf);
    was_a = drv_phy_i2c_read(I2C_BBTOP, I2C_HOST, (uint8_t)pair);
    was_b = drv_phy_i2c_read(I2C_BBTOP, I2C_HOST, (uint8_t)(pair + 1u));
    drv_say("txfilter: registers %u and %u read %x and %x; the receive filter was %u, now %u; gain %u\n", pair, pair + 1u,
            was_a, was_b, rxf_was, rxf, gain);
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    if (seconds != 0) {
        rhythm(d, seconds, step, lb, codes, tone_ms, code_ms);
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
            for (uint32_t c = 0; c < ncodes; c++) {
                uint32_t a = codes[c] == CODE_KEEP ? was_a : codes[c], b = codes[c] == CODE_KEEP ? was_b : codes[c];
                i2c_write((uint8_t)pair, (uint8_t)a);
                i2c_write((uint8_t)(pair + 1u), (uint8_t)b);
                for (uint32_t s = 0; s < nsteps; s++) {
                    drv_phy_loopback(1, lb);
                    looped = 1;
                    drv_phy_tone(1, steps[s], TONE_ATTEN);
                    toned = 1;
                    dump_capture(&conf);
                    drv_phy_tone(0, 0, 0);
                    toned = 0;
                    drv_phy_loopback(0, 0);
                    looped = 0;
                    int32_t peak;
                    /* The line's bin, and its mirror's, the control, which the transmitter's and the receiver's corrections keep low. */
                    uint32_t line = steps[s], mirror = WORDS - steps[s];
                    uint32_t pl = bin_power(line, &peak), pm = bin_power(mirror, &peak);
                    drv_say("txfilter: second %u code %x,%x step %x: at %u kHz %u.%u dB, its mirror %u.%u dB, peak %d\n",
                            second, a, b, steps[s], line * 390625u / 10000u, pl / 10u, pl % 10u, pm / 10u, pm % 10u, peak);
                }
                child_answer(&d->c);
            }
            put_back();
            second++;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_phy_rx_filter(rxf_was);
    drv_radio_off();
    drv_say("txfilter: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
