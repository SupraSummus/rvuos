/*
 * The player: the dump block's word 0x600A900C, which plays the bank's words to the transmitter's DAC,
 * as Espressif's RF test library's dactrig drives its own on the ESP32-C3; see hw/dump.md, "A player", for what is known.
 * The MAC and the PHY come up as the station's do, mac_up, the receiver on channel=, 6 by default,
 * and the word is written 0 first, as a player left round and round, bit 19, plays on through a reset.
 *
 * The bank is filled with count= words, 4096 by default, up to the word's 14 bits:
 * a tone of period= words, 16 by default, amp= counts, 64 by default, its cosine in bits 10 to 19 and its sine in 0 to 9,
 * swap=1 the other way round, which is the player's I and Q; period=0 a constant amp= in both; ramp=1 dactrig's ramp.
 * Each second the word is written count=, bits=, in hexadecimal, and bit 31, and the time to its bit 18 told
 * beside the time a capture of as many words at 80 MHz takes.
 * loop=1 loops the transmitter back into the receiver meanwhile, lb= its gain, 0x20 by default, as prog/loopback.c does,
 * which radiates what the transmitter sends some 22 dB below the radio's 0 dBm, see hw/phy.md, "The loopback".
 * tone=, a step of the transmit tone generator's, drv_phy_tone in phy.c, has it on meanwhile, at the calibration's level.
 * grab=, a capture of that many words from source 15 at 80 MHz, starts grabat= polls into the play,
 * and the words of the bank it changed are told, dumphex= of them for user/radio/esp32c6/tools/dump-spectrum.py.
 *
 * muxes=1 clears MODEM_SYSCON's CLK_DATA_DUMP_MUX every other second, the odd ones, while it plays,
 * the block's clock 32 MHz instead of 80, see hw/dump.md, "A player".
 * timebits=1 times a play with each bit that might slow it, see time_bits, in the first second.
 * est=1 instead takes the modem's IQ estimate each second, looped back, with nothing played
 * and then with the player round and round over count= words of each of raws=, from the bank's word at=, 0 before it,
 * for each of w10s=, the block's word 0x10 meanwhile, and with upper=1 SRAM_USAGE's MAC_DUMP_ALLOC set; see estimate.
 * est=2 swings the player from the bank's upper half to its lower one under the CPU's writes; see swing.
 * raws=, w10s= and bits= are hexadecimal, small letters, commas between, up to LIST_MAX words.
 *
 * rhythm=, seconds up to RHYTHM_MAX_S, first plays over and over for half= milliseconds, 50 by default,
 * and stays quiet as long, or with half=0 plays throughout, the loopback on,
 * for the nRF52840's watch= or sweep= of user/thread/root.c to find what goes out.
 *
 * Run it with make BOARD=esp32c6 radio PROG=player RADIO_CONFIG=file, see build.mk.
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

DRV_PROGRAM("the player", DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL    (MODEM_BASE + 0x9004u)
#define CLK_CONF     (MODEM_SYSCON_BASE + 0x4u)
#define CLK_DUMP_MUX (1u << 21) /* MODEM_SYSCON_CLK_DATA_DUMP_MUX, 1 by default */
#define CTRL_DONE    (1u << 18)
#define CTRL_TRIGGER (1u << 19)
#define CTRL_ENABLE  (1u << 31)
#define PLAY         (MODEM_BASE + 0x900cu)
#define PLAY_COUNT   0x3fffu    /* bits 0 to 13, the words played */
#define PLAY_FAST    (1u << 15) /* a word every 12.5 ns, else every 25 */
#define PLAY_DONE    (1u << 18)
#define PLAY_LOOP    (1u << 19) /* round and round, bit 18 never set */
#define PLAY_ENABLE  (1u << 31)
#define PLAY_PAGE    (MODEM_BASE + 0x9010u) /* bits 28 and 29, the first word in pages of 4096 */
#define SRAM_USAGE   (HP_SYSTEM_BASE + 0x4u)
#define SRAM_UPPER   (1u << 16) /* HP_SYSTEM_MAC_DUMP_ALLOC, the bank's upper 64 KiB */
#define UPPER_WORD   16384u
#define POLLS_MAX    2000000u
#define RHYTHM_MAX_S 60u
#define HALF_MIN_MS  5u
#define HALF_MAX_MS  500u
#define TONE_ATTEN   0x30u /* the calibration's, prog/loopback.c's least */
#define NO_WORD      0xffffffffu
#define LIST_MAX     8u
#define EST_COUNT    1023u
#define EST_SETTLE_US 20u

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static int up, looped, toned;

void drv_radio_off(void)
{
    REG(PLAY) = 0;
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
        mac_stop();
    }
}

static void loop_back(uint32_t on, uint32_t lb)
{
    drv_phy_loopback(on, lb);
    looped = (int)on;
}

/* phy.c's, libphy's dc_iq_est_new: the receiver's mean I and Q over a count, and the power about them in dB. */
void dc_iq_est_new(uint32_t a0, uint32_t a1, int32_t out[3], uint32_t a3);

/* A quarter period's table of a sine, 16 steps, scaled to 1024. */
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

struct shape {
    uint32_t period, amp, ramp, swap;
};

static uint32_t pattern(const struct shape *s, uint32_t k)
{
    if (s->ramp) {
        return (k >> 3) + 0x100u;
    }
    int32_t hi = (int32_t)s->amp, lo = (int32_t)s->amp;
    if (s->period != 0) {
        uint32_t phase = (uint32_t)((uint64_t)k * 64u / s->period);
        hi = sine(phase + 16u) * (int32_t)s->amp / 1024;
        lo = sine(phase) * (int32_t)s->amp / 1024;
    }
    if (s->swap) {
        int32_t t = hi;
        hi = lo;
        lo = t;
    }
    return ((uint32_t)hi & 0x3ffu) << 10 | ((uint32_t)lo & 0x3ffu);
}

struct play {
    uint32_t count, bits, grab, grabat;
};

static void capture_start(uint32_t n)
{
    REG(DUMP_CTRL) = n | CTRL_DONE;
    REG(DUMP_CTRL) = n | CTRL_ENABLE;
    REG(DUMP_CTRL) = n | CTRL_ENABLE | CTRL_TRIGGER;
    REG(DUMP_CTRL) = n | CTRL_ENABLE;
}

/* The microseconds a capture of n words at 80 MHz takes, timed as play times the player, the bank the engine's. */
static uint32_t capture_us(uint32_t n)
{
    uint64_t start = osi_now_us();
    capture_start(n);
    for (uint32_t polls = 0; (REG(DUMP_CTRL) & CTRL_DONE) == 0 && polls < POLLS_MAX; polls++) { /* bounded by POLLS_MAX */
    }
    uint64_t end = osi_now_us();
    REG(DUMP_CTRL) = 0;
    return (uint32_t)(end - start);
}

/* One play, the bank the engine's, and a capture beside it if grab; the microseconds it took, NO_WORD if it never ended. */
static uint32_t play(const struct play *p)
{
    uint32_t polls = 0;
    REG(PLAY) = 0;
    uint64_t start = osi_now_us();
    REG(PLAY) = p->count | p->bits | PLAY_ENABLE;
    for (; (REG(PLAY) & PLAY_DONE) == 0 && polls < POLLS_MAX; polls++) { /* bounded by POLLS_MAX */
        if (p->grab != 0 && polls == p->grabat) {
            capture_start(p->grab);
        }
    }
    uint32_t us = polls < POLLS_MAX ? (uint32_t)(osi_now_us() - start) : NO_WORD;
    for (uint32_t k = 0; p->grab != 0 && (REG(DUMP_CTRL) & CTRL_DONE) == 0 && k < POLLS_MAX; k++) { /* bounded */
    }
    REG(DUMP_CTRL) = 0;
    REG(PLAY) = 0;
    return us;
}

/*
 * Each bit that might slow the player, one at a time, a play of count words timed with it set:
 * CLK_CONF's bits 0 to 20, which ESP-IDF leaves unnamed, the page word's 0 to 27 and the player's word's own unexplained ones;
 * those whose play took other than the plain one's time told, and the mux cleared, the run's control.
 */
static void time_bits(const struct play *p)
{
    struct play q = *p;
    q.grab = 0;
    uint32_t usage = dump_bank_give(), base = play(&q), clk = REG(CLK_CONF), moved = 0;
    for (uint32_t b = 0; b < 21u + 28u + 12u; b++) { /* bounded by the bits tried */
        uint32_t at = b < 21u ? CLK_CONF : b < 49u ? PLAY_PAGE : PLAY;
        uint32_t bit = b < 21u ? 1u << b : b < 49u ? 1u << (b - 21u) : b < 50u ? 1u << 16 : b < 51u ? 1u << 17
                     : b < 52u ? 1u << 30 : 1u << (20u + b - 52u);
        uint32_t us;
        if (at == PLAY) {
            q.bits = p->bits | bit;
            us = play(&q);
            q.bits = p->bits;
        } else {
            uint32_t was = REG(at);
            REG(at) = was | bit;
            us = play(&q);
            REG(at) = was;
        }
        if (us == NO_WORD || us + us / 20u < base || us > base + base / 20u) {
            drv_say("player: bit %u of %x, %u us against %u\n", __builtin_ctz(bit), at, us, base);
            moved++;
        }
    }
    REG(CLK_CONF) = clk & ~CLK_DUMP_MUX;
    uint32_t slow = play(&q);
    REG(CLK_CONF) = clk;
    dump_bank_take(usage);
    drv_say("player: of the bits tried %u moved the play's %u us; the mux cleared, the control, %u us\n", moved, base, slow);
}

static uint32_t grabs;

/* The words a capture beside the play changed, which it writes from the player's place: their span, and hex of them. */
static void tell_changed(const struct play *p, const struct shape *s, uint32_t hex)
{
    uint32_t lo = NO_WORD, hi = 0, n = 0;
    for (uint32_t k = 0; k < p->count; k++) {
        if (bank[k] != pattern(s, k)) {
            lo = lo == NO_WORD ? k : lo;
            hi = k;
            n++;
        }
    }
    drv_say("player: %u words changed, from %u to %u\n", n, lo, hi);
    if (n == 0) {
        return;
    }
    uint32_t words = hi - lo + 1u < hex ? hi - lo + 1u : hex;
    drv_say("dump: capture %u source 15 lane 0 rate 0: beside the player, from word %u\n", grabs, lo);
    for (uint32_t k = 0, lines = 0; k + 8u <= words; k += 8u) { /* bounded by hex */
        if (lines++ % 16u == 0) {
            osi_delay_ms(10);
        }
        const volatile uint32_t *w = &bank[lo + k];
        drv_say("dumphex: %u %u %x %x %x %x %x %x %x %x\n", grabs, k, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    grabs++;
}

/* The second's play, timed, beside a capture as long, and what a capture beside it changed. */
static void play_second(uint32_t second, const struct play *p, const struct shape *s, uint32_t loop, uint32_t lb,
                        uint32_t tone, uint32_t hex)
{
    for (uint32_t k = 0; k < p->count; k++) {
        bank[k] = pattern(s, k);
    }
    if (loop) {
        loop_back(1, lb);
    }
    if (tone != NO_WORD) {
        drv_phy_tone(1, tone, TONE_ATTEN);
        toned = 1;
    }
    uint32_t usage = dump_bank_give();
    uint32_t us = play(p);
    dump_bank_take(usage);
    if (toned) {
        drv_phy_tone(0, 0, 0);
        toned = 0;
    }
    uint32_t kept = 0;
    for (uint32_t k = 0; k < p->count; k++) {
        kept += bank[k] == pattern(s, k);
    }
    if (p->grab != 0) {
        tell_changed(p, s, hex);
    }
    /* The capture writes the bank's first words, so it comes after what reads them. */
    usage = dump_bank_give();
    uint32_t cal = capture_us(p->count);
    dump_bank_take(usage);
    if (looped) {
        loop_back(0, 0);
    }
    drv_say("player: second %u: word %x, %u us, a capture as long %u us; %u of %u words as they were\n", second,
            p->count | p->bits | PLAY_ENABLE, us, cal, kept, p->count);
}

struct est {
    uint32_t raws[LIST_MAX], nraws, w10s[LIST_MAX], nw10s, at, upper;
};

/*
 * The estimate: looped back, the receiver's mean and the power about it, from the modem's IQ estimate, phy.c's dc_iq_est_new,
 * with nothing played and then with the player round and round over count words each of one raw word,
 * for each of raws= and w10s=; a witness of what the player sends that keeps out of the bank.
 */
static void estimate(const struct play *p, const struct est *e, uint32_t lb)
{
    for (uint32_t w = 0; w < e->nw10s; w++) {
        for (uint32_t r = 0; r < e->nraws; r++) {
            int32_t quiet[3], heard[3];
            for (uint32_t k = 0; k < e->at + p->count; k++) {
                bank[k] = k < e->at ? 0 : e->raws[r];
            }
            loop_back(1, lb);
            dc_iq_est_new(1, EST_COUNT, quiet, 0);
            uint32_t usage = dump_bank_give();
            if (e->upper) {
                REG(SRAM_USAGE) |= SRAM_UPPER;
            }
            uint32_t page = REG(PLAY_PAGE);
            REG(PLAY_PAGE) = e->w10s[w];
            REG(PLAY) = 0;
            REG(PLAY) = p->count | p->bits | PLAY_LOOP | PLAY_ENABLE;
            ets_delay_us(EST_SETTLE_US);
            dc_iq_est_new(1, EST_COUNT, heard, 0);
            REG(PLAY) = 0;
            REG(PLAY_PAGE) = page;
            dump_bank_take(usage);
            loop_back(0, 0);
            drv_say("player: est %x, 0x10 %x: quiet I %d Q %d %d dB, played I %d Q %d %d dB\n", e->raws[r], e->w10s[w],
                    quiet[0], quiet[1], quiet[2], heard[0], heard[1], heard[2]);
        }
    }
}

/*
 * The swing: the player round and round over count= words of the bank's upper half, MAC_DUMP_ALLOC set, all of them 0,
 * while the CPU writes the lower half's first count= words with raw and reads them back;
 * then MAC_DUMP_ALLOC cleared under the playing player, and the IQ estimate after each step:
 * whether the CPU keeps the half the engine does not play, and whether the player moves to it without a stop.
 */
static void swing(const struct play *p, uint32_t raw, uint32_t lb)
{
    int32_t e0[3], e1[3], e2[3], e3[3];
    for (uint32_t k = 0; k < p->count; k++) {
        bank[k] = 0;
        bank[UPPER_WORD + k] = 0;
    }
    loop_back(1, lb);
    dc_iq_est_new(1, EST_COUNT, e0, 0);
    uint32_t usage = dump_bank_give();
    REG(SRAM_USAGE) |= SRAM_UPPER;
    REG(PLAY) = 0;
    REG(PLAY) = p->count | p->bits | PLAY_LOOP | PLAY_ENABLE;
    ets_delay_us(EST_SETTLE_US);
    dc_iq_est_new(1, EST_COUNT, e1, 0);
    for (uint32_t k = 0; k < p->count; k++) {
        bank[k] = raw;
    }
    uint32_t kept = 0;
    for (uint32_t k = 0; k < p->count; k++) {
        kept += bank[k] == raw;
    }
    dc_iq_est_new(1, EST_COUNT, e2, 0);
    REG(SRAM_USAGE) &= ~SRAM_UPPER;
    ets_delay_us(10u * EST_SETTLE_US);
    dc_iq_est_new(1, EST_COUNT, e3, 0);
    REG(PLAY) = 0;
    dump_bank_take(usage);
    loop_back(0, 0);
    uint32_t after = 0;
    for (uint32_t k = 0; k < p->count; k++) {
        after += bank[k] == raw;
    }
    drv_say("player: swing %x: quiet I %d Q %d, upper played I %d Q %d\n", raw, e0[0], e0[1], e1[0], e1[1]);
    drv_say("player: swing: the lower written, %u of %u read back, I %d Q %d; the upper let go I %d Q %d; %u kept after\n",
            kept, p->count, e2[0], e2[1], e3[0], e3[1], after);
}

static void play_rhythm(struct drv *d, const struct play *p, uint32_t seconds, uint32_t half)
{
    uint64_t start = osi_now_us(), end = start + (uint64_t)seconds * 1000000u;
    uint32_t plays = 0, unended = 0;
    struct play quiet = *p;
    quiet.grab = 0;
    uint32_t usage = dump_bank_give();
    for (uint64_t now = start; now < end && !d->leave; now = osi_now_us()) { /* bounded by seconds */
        if (half == 0 || ((now - start) / (half * 1000u)) % 2u == 0) {
            unended += play(&quiet) == NO_WORD;
            if (++plays % 1024u == 0) {
                child_answer(&d->c);
            }
        } else {
            child_answer(&d->c);
            osi_delay_ms(1);
        }
    }
    dump_bank_take(usage);
    drv_say("player: rhythm over, %u plays, %u unended, in %u us\n", plays, unended, (uint32_t)(osi_now_us() - start));
}

/* A key's words in hexadecimal, small letters, commas between, up to LIST_MAX of them; how many. */
static uint32_t hex_list(struct drv *d, const char *key, uint32_t *out)
{
    uint32_t n = 0;
    char list[96];
    list[0] = '\0';
    config_value(d->conf, d->conf_len, key, list, sizeof(list));
    for (const char *c = list; *c != '\0' && n < LIST_MAX;) {
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
    return n;
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    struct dump_conf conf;
    dump_configure(d->conf, d->conf_len, &conf);
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 6);
    struct play p;
    struct shape s;
    struct est e;
    p.count = config_number(d->conf, d->conf_len, "count", 4096);
    p.grab = config_number(d->conf, d->conf_len, "grab", 0);
    p.grabat = config_number(d->conf, d->conf_len, "grabat", 0);
    uint32_t bits[LIST_MAX];
    p.bits = hex_list(d, "bits", bits) != 0 ? bits[0] : 0;
    s.period = config_number(d->conf, d->conf_len, "period", 16);
    s.amp = config_number(d->conf, d->conf_len, "amp", 64);
    s.ramp = config_number(d->conf, d->conf_len, "ramp", 0);
    s.swap = config_number(d->conf, d->conf_len, "swap", 0);
    uint32_t loop = config_number(d->conf, d->conf_len, "loop", 0);
    uint32_t lb = config_number(d->conf, d->conf_len, "lb", 0x20);
    uint32_t tone = config_number(d->conf, d->conf_len, "tone", NO_WORD);
    uint32_t est = config_number(d->conf, d->conf_len, "est", 0);
    e.at = config_number(d->conf, d->conf_len, "at", 0);
    e.upper = config_number(d->conf, d->conf_len, "upper", 0);
    e.nraws = hex_list(d, "raws", e.raws);
    e.nw10s = hex_list(d, "w10s", e.w10s);
    if (e.nw10s == 0) {
        e.w10s[0] = 0;
        e.nw10s = 1;
    }
    uint32_t muxes = config_number(d->conf, d->conf_len, "muxes", 0);
    uint32_t timebits = config_number(d->conf, d->conf_len, "timebits", 0);
    uint32_t rhythm = config_number(d->conf, d->conf_len, "rhythm", 0);
    uint32_t half = config_number(d->conf, d->conf_len, "half", 50);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("a count of 1 to PLAY_COUNT", p.count >= 1 && p.count <= PLAY_COUNT ? ESP_OK : ESP_FAIL);
    drv_must("bits= none of the count's, bit 19's or bit 31's",
             (p.bits & (PLAY_COUNT | PLAY_LOOP | PLAY_ENABLE)) == 0 ? ESP_OK : ESP_FAIL);
    drv_must("an amp of at most 511", s.amp <= 511 ? ESP_OK : ESP_FAIL);
    drv_must("a grab within the count", p.grab <= p.count ? ESP_OK : ESP_FAIL);
    drv_must("an at= and count= within the bank", e.at + p.count <= DUMP_SAMPLES_MAX ? ESP_OK : ESP_FAIL);
    drv_must("upper=1 with a count= within the bank's upper half", !e.upper || p.count <= UPPER_WORD ? ESP_OK : ESP_FAIL);
    drv_must("a rhythm of at most RHYTHM_MAX_S", rhythm <= RHYTHM_MAX_S ? ESP_OK : ESP_FAIL);
    drv_must("a half of 0, or HALF_MIN_MS to HALF_MAX_MS",
             (half == 0 || half >= HALF_MIN_MS) && half <= HALF_MAX_MS ? ESP_OK : ESP_FAIL);
    /* A player left round and round, bit 19, before the last reset plays on through it, as the modem keeps its state. */
    drv_say("player: the word was %x\n", REG(PLAY));
    REG(PLAY) = 0;
    mac_up(d->mac);
    up = 1;
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    if (rhythm != 0) {
        for (uint32_t k = 0; k < p.count; k++) {
            bank[k] = pattern(&s, k);
        }
        loop_back(1, lb);
        drv_say("player: rhythm for %u s, %u ms playing and %u ms not, count %u bits %x\n", rhythm, half, half, p.count,
                p.bits);
        play_rhythm(d, &p, rhythm, half);
        loop_back(0, 0);
    }
    uint64_t told = osi_now_us();
    for (uint32_t second = 0;;) {
        uint32_t wake;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            told = now;
            if (est == 2) {
                swing(&p, e.nraws ? e.raws[0] : 0x200u, lb);
            } else if (est) {
                estimate(&p, &e, lb);
            } else {
                if (timebits && second == 0) {
                    for (uint32_t k = 0; k < p.count; k++) {
                        bank[k] = pattern(&s, k);
                    }
                    time_bits(&p);
                }
                uint32_t clk = REG(CLK_CONF);
                if (muxes) {
                    REG(CLK_CONF) = second % 2u ? clk & ~CLK_DUMP_MUX : clk | CLK_DUMP_MUX;
                    drv_say("player: the dump clock's mux %u\n", (REG(CLK_CONF) & CLK_DUMP_MUX) != 0);
                }
                play_second(second, &p, &s, loop, lb, tone, conf.hex);
                REG(CLK_CONF) = clk;
            }
            second++;
        }
        rv_wait(CHILD_INBOX, &wake);
    }
    drv_radio_off();
    drv_say("player: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
