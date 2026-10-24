/*
 * IEEE 802.15.4 frames made up as I and Q and sent by the dump block's player, see hw/dump.md, "A player" and "A frame made up":
 * no MAC and no modem of the radio's makes them, only the samples the bank holds.
 * The MAC and the PHY come up as the station's do, mac_up, on Wi-Fi's channel=, 13 by default, 2472 MHz,
 * and the frames are made below= MHz below that carrier, 2 by default, 802.15.4's channel 24 at 2470 MHz.
 * Each byte is two symbols, the low first, each symbol 32 chips of 802.15.4's table, at 2 Mchip/s,
 * the even chips on I and the odd on Q half a chip later, each a half sine two chips long:
 * at the player's 40 MS/s, 20 samples a chip; then turned down to the frame's frequency, sample by sample.
 *
 * kind=0, by default, sends a data frame of four bytes and no addresses: frame control 0x0001, the sequence number
 * the setting's place in the lists below, the amplitude halved, and the FCS, CRC-16 ITU-T as 802.15.4 has it;
 * 15380 samples, one play. The nRF52840's listen= of user/thread/root.c counts them by setting.
 * kind=1 sends a ping to the nRF52840 as user/radio/esp32c6/i154.c's are, PAN 0x7276 from 0xC006 to 0x5284,
 * asking to be acknowledged, its payload "ping" and a letter, a for the first setting, its sequence number the setting's;
 * 28180 samples, more than one play takes, so the player plays the first PLAY_COUNT of them from the bank's lower half,
 * and once that ends the rest from its upper half, MAC_DUMP_ALLOC set, gaps= samples of them left out first,
 * for the while the second play takes to start. The nRF52840's exchange of user/thread/root.c on channel 24 tells each.
 *
 * The settings: amps=, the samples' greatest, of 511, 300 by default; offs=, kHz above the frame's frequency, 0 by default;
 * conjs=, 1 sends the frame's mirror, its Q negated; lbs=, the loopback's gain, 0x20 by default;
 * gaps=, as above, GAP_DEFAULT by default, below 0 as many played twice;
 * dsts=, the ping's destination, the nRF52840's 0x5284, 21124, by default: another is a control, a ping it does not acknowledge.
 * slows=1 plays the frame at 16 MS/s, 8 samples a chip, CLK_DATA_DUMP_MUX cleared while it plays, see hw/dump.md, "A player":
 * a ping then fits one play.
 * dcs=1 sends the frame through the transmitter's DC word instead, with no player, the CPU storing the samples
 * one after the other at the rate a calibration times, see hw/phy.md, "The DC word".
 * Each sample is held till the next, so the stream's images stand at that rate and twice it to each side of the frame,
 * and the band must hold them: channel 7 and below=2, 2440 MHz, for one.
 * Each is a list in decimal, commas between, a minus allowed in offs=, up to SETTINGS_MAX long,
 * and a list of one holds for every setting; the frames take the settings in turn,
 * so that one run, every setting in the same while and the same air, measures them all against each other.
 * For send= seconds, up to SEND_MAX_S, a frame is made and played every gap_ms= milliseconds, 10 by default,
 * the transmitter looped back into the receiver, which is all that lets it out:
 * the loopback gives off what the transmitter sends some 22 dB below the radio's 0 dBm, see hw/phy.md, "The loopback".
 * An amplitude of 0 sends a frame of nothing, the control.
 * hear=N, every Nth frame, turns the loopback off once it ends, tunes the channel again, which brings the receiver back,
 * and takes a capture of hearwords= words, 8192 by default, at the 80 MHz clock divided as hearrate= says, 3 by default,
 * 16 MS/s, and tells hearhex= of them, 8192 by default, so that what answers the frame is in its words:
 * the nRF52840's acknowledgement, 802.15.4's 192 us after the frame and some 300 us after the play's end was seen,
 * which user/radio/esp32c6/tools/demod154.py finds and reads.
 *
 * Run it with make BOARD=esp32c6 radio PROG=synth154 RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "devices.h"
#include "drv.h"
#include "dump.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("802.15.4 frames from the player", DRV_NEED_DUMP);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define DUMP_CTRL    (MODEM_BASE + 0x9004u)
#define CTRL_RATE_SHIFT 15u
#define CTRL_DONE    (1u << 18)
#define CTRL_TRIGGER (1u << 19)
#define CTRL_ENABLE  (1u << 31)
#define PLAY         (MODEM_BASE + 0x900cu)
#define PLAY_COUNT   0x3fffu
#define PLAY_DONE    (1u << 18)
#define PLAY_ENABLE  (1u << 31)
#define SRAM_USAGE   (HP_SYSTEM_BASE + 0x4u)
#define SRAM_UPPER   (1u << 16) /* HP_SYSTEM_MAC_DUMP_ALLOC: the player reads the bank's upper half */
#define UPPER_WORD   16384u
#define FECTRL_TXRX_OFF (MODEM_BASE + 0x910u)
#define POLLS_MAX    1000000u
#define SEND_MAX_S   60u
#define GAP_MS_MAX   500u /* within the root task's second, a frame's telling beside it */
#define AMP_MAX      511
#define OFF_MAX_KHZ  4000
#define GAP_MAX      1000 /* gaps= of 0 to 8 came whole, and +-24 none; at most this keeps the upper half's words in the bank */
#define GAP_DEFAULT  4 /* the second play starts some 2 samples late, hw/dump.md "A frame made up" */
#define SETTINGS_MAX 16u
#define SAMPLE_HZ    40000000
#define SLOW_HZ      16000000 /* the player's rate with CLK_DATA_DUMP_MUX cleared, hw/dump.md "A player" */
#define CLK_CONF     (MODEM_SYSCON_BASE + 0x4u)
#define CLK_DUMP_MUX (1u << 21)

#define SAMPLES_A_CHIP 20u /* 40 MS/s at 2 Mchip/s */
#define SLOW_A_CHIP    8u  /* 16 MS/s */
#define FRAME_MAX      24u
#define FCS_POLY       0x8408u /* CRC-16 ITU-T, its bits reversed, as 802.15.4 sends them */
#define PAN_RVUOS      0x7276u
#define ADDR_C6        0xc006u
#define ADDR_NRF       0x5284u

/* 802.15.4's chips of each symbol, the first chip in bit 0. */
static const uint32_t chips_of[16] = {
    0x744ac39bu, 0x44ac39b7u, 0x4ac39b74u, 0xac39b744u, 0xc39b744au, 0x39b744acu, 0x9b744ac3u, 0xb744ac39u,
    0xdee06931u, 0xee06931du, 0xe06931deu, 0x06931deeu, 0x6931dee0u, 0x931dee06u, 0x31dee069u, 0x1dee0693u,
};

/* A half sine two chips long, 1024 at its peak. */
static const int16_t half_sine[2u * SAMPLES_A_CHIP] = {
    0, 80, 160, 239, 316, 392, 465, 535, 602, 665, 724, 779, 828, 873, 912, 946, 974, 996, 1011, 1021,
    1024, 1021, 1011, 996, 974, 946, 912, 873, 828, 779, 724, 665, 602, 535, 465, 392, 316, 239, 160, 80,
};

/* A quarter turn of a sine, 64 steps and its end, 1024 at its peak: a turn is 256 steps. */
static const int16_t quarter[65] = {
    0, 25, 50, 75, 100, 125, 150, 175, 200, 224, 249, 273, 297, 321, 345, 369, 392, 415, 438, 460, 483, 505,
    526, 548, 569, 590, 610, 630, 650, 669, 688, 706, 724, 742, 759, 775, 792, 807, 822, 837, 851, 865, 878,
    891, 903, 915, 926, 936, 946, 955, 964, 972, 980, 987, 993, 999, 1004, 1009, 1013, 1016, 1019, 1021, 1023,
    1024, 1024,
};

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static int up, looped;
static uint32_t looped_lb;

static uint32_t txdc_was;

void drv_radio_off(void)
{
    REG(PLAY) = 0;
    if (up) {
        REG(MODEM_BASE + 0x444u) = txdc_was;
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

static int32_t sine(uint32_t step) /* step of 256 a turn */
{
    uint32_t r = step & 63u;
    switch ((step >> 6) & 3u) {
    case 0:
        return quarter[r];
    case 1:
        return quarter[64u - r];
    case 2:
        return -quarter[r];
    default:
        return -quarter[64u - r];
    }
}

static uint16_t fcs(const uint8_t *b, uint32_t n)
{
    uint16_t c = 0;
    for (uint32_t k = 0; k < n; k++) {
        c ^= b[k];
        for (uint32_t i = 0; i < 8u; i++) {
            c = (c & 1u) ? (uint16_t)((c >> 1) ^ FCS_POLY) : (uint16_t)(c >> 1);
        }
    }
    return c;
}

static int8_t chip_of[FRAME_MAX * 64u]; /* the frame's chips, +1 or -1 */

static void spell(const uint8_t *frame, uint32_t bytes)
{
    for (uint32_t m = 0; m < bytes * 64u; m++) {
        uint32_t symbol = m >> 5, byte = frame[symbol >> 1];
        uint32_t nibble = symbol & 1u ? byte >> 4 : byte & 15u;
        chip_of[m] = (chips_of[nibble] >> (m & 31u)) & 1u ? 1 : -1;
    }
}

struct setting {
    int32_t amp, off_khz, conj, lb, gap, dst, slow, dc;
};

/* The half sine's kth sample of 2 spc, of 20 or 8 samples a chip. */
static int32_t half_of(uint32_t k, uint32_t spc)
{
    return spc == SAMPLES_A_CHIP ? half_sine[k] : sine(k * 8u);
}

/*
 * The frame's samples into the bank, I in bits 0 to 9 and Q in 10 to 19, as the player takes them:
 * the first PLAY_COUNT from the lower half's first word, and from PLAY_COUNT and gap on from the upper half's,
 * gap below 0 playing as many again; how many the upper half holds.
 */
static uint32_t make(const uint8_t *frame, uint32_t bytes, const struct setting *s, int32_t shift_hz)
{
    uint32_t spc = s->slow ? SLOW_A_CHIP : SAMPLES_A_CHIP;
    uint32_t chips = bytes * 64u, total = chips * spc + spc;
    uint32_t phase = 0, rest = 0, from = (uint32_t)((int32_t)PLAY_COUNT + s->gap);
    spell(frame, bytes);
    uint32_t step = (uint32_t)(int32_t)(((int64_t)shift_hz << 32) / (s->slow ? SLOW_HZ : SAMPLE_HZ));
    for (uint32_t n = 0, even = 0, at_even = 0, odd = 1, at_odd = 0; n < total; n++) {
        int32_t i = even < chips ? chip_of[even] * half_of(at_even, spc) : 0;
        int32_t q = n >= spc && odd < chips ? chip_of[odd] * half_of(at_odd, spc) : 0;
        if (++at_even == 2u * spc) {
            at_even = 0;
            even += 2u;
        }
        if (n >= spc && ++at_odd == 2u * spc) {
            at_odd = 0;
            odd += 2u;
        }
        uint32_t turn = phase >> 24;
        int32_t c = sine(turn + 64u), sn = sine(turn);
        phase += step;
        int32_t si = ((i * c - q * sn) >> 10) * s->amp >> 10, sq = ((i * sn + q * c) >> 10) * s->amp >> 10;
        if (s->conj) {
            sq = -sq;
        }
        uint32_t word = ((uint32_t)sq & 0x3ffu) << 10 | ((uint32_t)si & 0x3ffu);
        if (n < PLAY_COUNT) {
            bank[n] = word;
        }
        if (total > PLAY_COUNT && n >= from) {
            bank[UPPER_WORD + rest++] = word;
        }
    }
    return rest;
}

/* The DC word, FEDATA 0x600A0444, which the CPU writes a sample at a time; no clock but the loop's paces it. */
#define FE_TXDC      (MODEM_BASE + 0x444u)
#define TXDC_IQ      0xfffffu
#define CHIP_HZ      2000000u
#define DC_RATE_MIN  4000000u /* two samples a chip at least */
#define DC_RATE_MAX  20000000u
#define DC_SHORT     1000u    /* the calibration's two streams, the difference their time's */
#define DC_LONG      6000u
#define DC_SPAN_US   4000u    /* at least: 6.6 ms as a power of two of 160 MHz's counts, 820 us of it the eight units' */
#define DC_TRIES     20u
#define BAND_LOW_KHZ  2400000
#define BAND_HIGH_KHZ 2483500

/*
 * The frame's samples into the bank from its first word, at rate samples a second, as make's are at 40 MS/s,
 * each at its own time: the chip it falls in and where in that chip's half sine, I the even chips and Q the odd
 * half a chip later; how many.
 */
static uint32_t make_dc(const uint8_t *frame, uint32_t bytes, const struct setting *s, int32_t shift_hz, uint32_t rate)
{
    uint32_t chips = bytes * 64u;
    uint64_t chip_step = ((uint64_t)CHIP_HZ << 32) / rate; /* chips a sample, 32 bits of fraction */
    uint32_t total = (uint32_t)(((uint64_t)(chips + 1u) << 32) / chip_step) + 1u;
    uint32_t step = (uint32_t)(int32_t)(((int64_t)shift_hz << 32) / (int64_t)rate), phase = 0;
    spell(frame, bytes);
    for (uint32_t n = 0; n < total; n++) {
        uint64_t at = (uint64_t)n * chip_step; /* the sample's time in chips, 32 bits of fraction */
        uint32_t pair = (uint32_t)(at >> 33), into = (uint32_t)(at >> 26) & 127u; /* the two-chip half sine and where in it */
        int32_t i = 2u * pair < chips ? chip_of[2u * pair] * sine(into) : 0, q = 0;
        if (at >= (1ull << 32)) {
            uint64_t late = at - (1ull << 32);
            uint32_t qpair = (uint32_t)(late >> 33), qinto = (uint32_t)(late >> 26) & 127u;
            q = 2u * qpair + 1u < chips ? chip_of[2u * qpair + 1u] * sine(qinto) : 0;
        }
        uint32_t turn = phase >> 24;
        int32_t c = sine(turn + 64u), sn = sine(turn);
        phase += step;
        int32_t si = ((i * c - q * sn) >> 10) * s->amp >> 10, sq = ((i * sn + q * c) >> 10) * s->amp >> 10;
        if (s->conj) {
            sq = -sq;
        }
        bank[n] = (txdc_was & ~TXDC_IQ) | ((uint32_t)sq & 0x3ffu) << 10 | ((uint32_t)si & 0x3ffu);
    }
    return total;
}

/* The bank's first n words stored into the DC word, as fast as the CPU stores; the calibration's and the frames' one loop. */
__attribute__((noinline)) static void dc_stream(uint32_t n)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)FE_TXDC;
    const volatile uint32_t *from = bank;
    for (uint32_t k = 0; k < n; k++) { /* bounded by n, the bank's words at most */
        *w = from[k];
    }
    *w = txdc_was;
}

/* The counter's count now, and its rate into hz: the CPU's cycles on the C6, where osi_now_us keeps whole microseconds. */
static uint64_t dc_counts(struct drv *d, uint32_t *hz)
{
    uint64_t now;
    rv_clock_read(d->clock, &now, hz);
    return now;
}

/*
 * A sleep of ms on the program's own timer, the request its wake begins due DC_SPAN_US later or up to twice that:
 * the thread's units' part of it is the stream's to run whole, see MANUAL.md, section 5.11,
 * once the sleep has brought its virtual time back to the clock, so ms is the span at least.
 * At the default span the request is some 128 us, and a stream of 390 us lost the core for 100 to 800 us.
 * The span holds for the thread's later waits too, which give none. What the parent wrote meanwhile is answered.
 */
static void dc_sleep(struct drv *d, uint32_t ms)
{
    uint32_t hz, bits = 0, other = 0;
    (void)dc_counts(d, &hz);
    uint64_t counts = (uint64_t)hz * DC_SPAN_US / 1000000u;
    uint32_t span = rv_span(counts > 0x40000000u ? 0x80000000u : (uint32_t)(2u * counts - 1u));
    rv_timer_set(CHILD_TIMER, CHILD_BIT_TIMER, ms * 1000u);
    while (!(bits & CHILD_BIT_TIMER)) { /* bounded by the timer's ms */
        if (rv_wait_within(CHILD_INBOX, span, &bits) != KERR_OK) {
            break;
        }
        other |= bits;
    }
    if (other & ~CHILD_BIT_TIMER) {
        child_answer(&d->c);
    }
}

/* A stream of the bank's first n words, each after a sleep of ms, the shortest of DC_TRIES; in counts. */
static uint32_t dc_time(struct drv *d, uint32_t n, uint32_t ms)
{
    uint32_t best = 0xffffffffu, hz;
    for (uint32_t t = 0; t < DC_TRIES; t++) {
        dc_sleep(d, ms);
        uint64_t start = dc_counts(d, &hz);
        dc_stream(n);
        uint32_t took = (uint32_t)(dc_counts(d, &hz) - start);
        best = took < best ? took : best;
    }
    return best;
}

/*
 * The store loop's rate, from two streams' times, so that what the clock's reading costs drops out,
 * the loopback on, with the quiet value, as for the frames: a frame drifts by its chips times the rate's error,
 * so the counter's counts time it, not microseconds, whose rounding alone was a few tenths of a percent.
 */
static uint32_t dc_calibrate(struct drv *d, uint32_t ms)
{
    uint32_t hz;
    (void)dc_counts(d, &hz);
    for (uint32_t k = 0; k < DC_LONG; k++) {
        bank[k] = txdc_was;
    }
    uint32_t short_c = dc_time(d, DC_SHORT, ms), long_c = dc_time(d, DC_LONG, ms);
    uint32_t rate = long_c > short_c ? (uint32_t)((uint64_t)(DC_LONG - DC_SHORT) * hz / (long_c - short_c)) : 0;
    drv_say("synth154: the DC word's loop: %u words in %u counts, %u in %u, at %u Hz: %u samples a second\n", DC_SHORT,
            short_c, DC_LONG, long_c, hz, rate);
    return rate;
}

static uint64_t played_end; /* when the last play's end was seen */
static uint32_t captures;

/* A capture of n words from source 15, the receiver's own, at the 80 MHz clock divided by rate's, at once: its microseconds. */
static uint32_t capture(uint32_t n, uint32_t rate)
{
    uint32_t usage = dump_bank_give(), ctrl = n | rate << CTRL_RATE_SHIFT, polls = 0;
    uint64_t start = osi_now_us();
    REG(DUMP_CTRL) = ctrl | CTRL_DONE;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE | CTRL_TRIGGER;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    while ((REG(DUMP_CTRL) & CTRL_DONE) == 0 && polls < POLLS_MAX) { /* bounded by POLLS_MAX */
        polls++;
    }
    uint32_t us = (uint32_t)(osi_now_us() - start);
    REG(DUMP_CTRL) = 0;
    dump_bank_take(usage);
    return us;
}

/* The capture's first hex words, as dump.c tells one, for user/radio/esp32c6/tools/dump-spectrum.py and the like. */
static void tell(uint32_t n, uint32_t hex)
{
    uint32_t words = hex < n ? hex : n;
    drv_say("dump: capture %u source 15 lane 0: after a frame\n", captures);
    for (uint32_t k = 0, lines = 0; k + 8u <= words; k += 8u) { /* bounded by hex */
        if (lines++ % 16u == 0) {
            osi_delay_ms(10);
        }
        const volatile uint32_t *w = &bank[k];
        drv_say("dumphex: %u %u %x %x %x %x %x %x %x %x\n", captures, k, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    captures++;
}

/* One play of the first count words, and of rest more from the upper half if any, the second as soon as the first ends. */
static uint32_t play(uint32_t count, uint32_t rest, uint32_t slow)
{
    uint32_t usage = dump_bank_give(), polls = 0, clk = REG(CLK_CONF);
    if (slow) {
        REG(CLK_CONF) = clk & ~CLK_DUMP_MUX;
    }
    uint32_t lower = REG(SRAM_USAGE), upper = lower | SRAM_UPPER;
    REG(PLAY) = 0;
    REG(PLAY) = count | PLAY_ENABLE;
    while ((REG(PLAY) & PLAY_DONE) == 0 && polls < POLLS_MAX) { /* bounded by POLLS_MAX */
        polls++;
    }
    if (rest != 0) {
        REG(SRAM_USAGE) = upper;
        REG(PLAY) = 0;
        REG(PLAY) = rest | PLAY_ENABLE;
        while ((REG(PLAY) & PLAY_DONE) == 0 && polls < POLLS_MAX) { /* bounded by POLLS_MAX */
            polls++;
        }
        REG(SRAM_USAGE) = lower;
    }
    played_end = osi_now_us();
    REG(PLAY) = 0;
    REG(CLK_CONF) = clk;
    dump_bank_take(usage);
    return polls < POLLS_MAX;
}

/* A key's list of decimal numbers, a minus allowed, commas between, up to SETTINGS_MAX; how many. */
static uint32_t list_of(struct drv *d, const char *key, int32_t *out)
{
    uint32_t n = 0;
    char list[128];
    list[0] = '\0';
    config_value(d->conf, d->conf_len, key, list, sizeof(list));
    const char *c = list;
    for (; *c != '\0' && n < SETTINGS_MAX;) {
        int32_t sign = *c == '-' ? -1 : 1, v = 0;
        uint32_t digits = 0;
        c += *c == '-';
        for (; *c >= '0' && *c <= '9' && digits < 6u; c++, digits++) {
            v = v * 10 + (*c - '0');
        }
        drv_must("a list's numbers in decimal, commas between", digits != 0 ? ESP_OK : ESP_FAIL);
        out[n++] = sign * v;
        if (*c != ',') {
            drv_must("a list's numbers separated by commas alone", *c == '\0' ? ESP_OK : ESP_FAIL);
            break;
        }
        c++;
    }
    drv_must("a list of at most SETTINGS_MAX numbers", *c == '\0' ? ESP_OK : ESP_FAIL);
    return n;
}

/* The settings' field from a key's list: its one value for all, or a value each; dflt if the key is absent. */
static void spread(struct drv *d, const char *key, int32_t dflt, struct setting *s, uint32_t n, uint32_t field)
{
    int32_t v[SETTINGS_MAX];
    uint32_t got = list_of(d, key, v);
    drv_must("a setting's list of one, or of as many as the longest", got <= 1u || got == n ? ESP_OK : ESP_FAIL);
    for (uint32_t k = 0; k < n; k++) {
        int32_t x = got == 0 ? dflt : got == 1 ? v[0] : v[k];
        int32_t *f = field == 0 ? &s[k].amp : field == 1 ? &s[k].off_khz : field == 2 ? &s[k].conj
                   : field == 3 ? &s[k].lb : field == 4 ? &s[k].gap : field == 5 ? &s[k].dst : field == 6 ? &s[k].slow
                   : &s[k].dc;
        *f = x;
    }
}

static uint32_t settings(struct drv *d, struct setting *s)
{
    static const char *const keys[8] = { "amps", "offs", "conjs", "lbs", "gaps", "dsts", "slows", "dcs" };
    uint32_t n = 1;
    for (uint32_t k = 0; k < 8u; k++) {
        int32_t v[SETTINGS_MAX];
        uint32_t got = list_of(d, keys[k], v);
        n = got > n ? got : n;
    }
    spread(d, "amps", 300, s, n, 0);
    spread(d, "offs", 0, s, n, 1);
    spread(d, "conjs", 0, s, n, 2);
    spread(d, "lbs", 0x20, s, n, 3);
    spread(d, "gaps", GAP_DEFAULT, s, n, 4);
    spread(d, "dsts", ADDR_NRF, s, n, 5);
    spread(d, "slows", 0, s, n, 6);
    spread(d, "dcs", 0, s, n, 7);
    for (uint32_t k = 0; k < n; k++) {
        drv_must("amps= of 0 to AMP_MAX", s[k].amp >= 0 && s[k].amp <= AMP_MAX ? ESP_OK : ESP_FAIL);
        drv_must("offs= within OFF_MAX_KHZ", s[k].off_khz >= -OFF_MAX_KHZ && s[k].off_khz <= OFF_MAX_KHZ ? ESP_OK : ESP_FAIL);
        drv_must("conjs= of 0 or 1", s[k].conj == 0 || s[k].conj == 1 ? ESP_OK : ESP_FAIL);
        drv_must("lbs= of 0 to 255", s[k].lb >= 0 && s[k].lb <= 255 ? ESP_OK : ESP_FAIL);
        drv_must("slows= of 0 or 1", s[k].slow == 0 || s[k].slow == 1 ? ESP_OK : ESP_FAIL);
        drv_must("dcs= of 0 or 1", s[k].dc == 0 || s[k].dc == 1 ? ESP_OK : ESP_FAIL);
        drv_must("dsts= of 16 bits", s[k].dst >= 0 && s[k].dst <= 0xffff ? ESP_OK : ESP_FAIL);
        drv_must("gaps= within GAP_MAX", s[k].gap >= -GAP_MAX && s[k].gap <= GAP_MAX ? ESP_OK : ESP_FAIL);
    }
    return n;
}

/* The frame of a kind for a setting, from its preamble on; its bytes. */
static uint32_t frame_of(uint32_t kind, uint32_t k, const struct setting *s, uint8_t *f)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        f[n++] = 0;
    }
    f[n++] = 0xa7;
    uint32_t phr = n++, psdu = n;
    if (kind == 0) {
        f[n++] = 0x01;
        f[n++] = 0x00;
        f[n++] = (uint8_t)k;
        f[n++] = (uint8_t)(s->amp / 2);
    } else {
        f[n++] = 0x61; /* data, ack request, PAN compressed */
        f[n++] = 0x88; /* short addresses both */
        f[n++] = (uint8_t)k;
        f[n++] = (uint8_t)PAN_RVUOS;
        f[n++] = (uint8_t)(PAN_RVUOS >> 8);
        f[n++] = (uint8_t)s->dst;
        f[n++] = (uint8_t)(s->dst >> 8);
        f[n++] = (uint8_t)ADDR_C6;
        f[n++] = (uint8_t)(ADDR_C6 >> 8);
        f[n++] = 'p';
        f[n++] = 'i';
        f[n++] = 'n';
        f[n++] = 'g';
        f[n++] = (uint8_t)('a' + k);
    }
    uint16_t c = fcs(&f[psdu], n - psdu);
    f[n++] = (uint8_t)c;
    f[n++] = (uint8_t)(c >> 8);
    f[phr] = (uint8_t)(n - psdu);
    return n;
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    uint32_t channel = config_number(d->conf, d->conf_len, "channel", 13);
    uint32_t below = config_number(d->conf, d->conf_len, "below", 2);
    uint32_t kind = config_number(d->conf, d->conf_len, "kind", 0);
    uint32_t send = config_number(d->conf, d->conf_len, "send", 20);
    uint32_t gap_ms = config_number(d->conf, d->conf_len, "gap_ms", 10);
    uint32_t hear = config_number(d->conf, d->conf_len, "hear", 0);
    uint32_t hear_words = config_number(d->conf, d->conf_len, "hearwords", 8192);
    uint32_t hear_rate = config_number(d->conf, d->conf_len, "hearrate", 3);
    uint32_t hear_hex = config_number(d->conf, d->conf_len, "hearhex", 8192);
    struct setting s[SETTINGS_MAX];
    uint32_t n = settings(d, s);
    drv_must("hearwords= of 8 to DUMP_SAMPLES_MAX", hear_words >= 8u && hear_words <= DUMP_SAMPLES_MAX ? ESP_OK : ESP_FAIL);
    drv_must("hearrate= of 0 to 3", hear_rate <= 3u ? ESP_OK : ESP_FAIL);
    drv_must("hearhex= of at most hearwords=", hear_hex <= hear_words ? ESP_OK : ESP_FAIL);
    drv_must("a channel of 1 to 13", channel >= 1 && channel <= 13 ? ESP_OK : ESP_FAIL);
    drv_must("a below= of 0 to 4", below <= 4u ? ESP_OK : ESP_FAIL);
    drv_must("a kind of 0 or 1", kind <= 1u ? ESP_OK : ESP_FAIL);
    drv_must("a send of 1 to SEND_MAX_S", send >= 1 && send <= SEND_MAX_S ? ESP_OK : ESP_FAIL);
    drv_must("a gap_ms of 1 to GAP_MS_MAX", gap_ms >= 1 && gap_ms <= GAP_MS_MAX ? ESP_OK : ESP_FAIL);
    _Static_assert(FRAME_MAX * 64u * SAMPLES_A_CHIP + SAMPLES_A_CHIP <= 2u * PLAY_COUNT, "a frame within two plays");
    _Static_assert(UPPER_WORD + FRAME_MAX * 64u * SAMPLES_A_CHIP + SAMPLES_A_CHIP - PLAY_COUNT + GAP_MAX <= DUMP_BANK_SIZE / 4u,
                   "the upper half's words within the bank, gaps= at its least");
    /* A player left round and round before the last reset plays on through it, see hw/dump.md. */
    REG(PLAY) = 0;
    mac_up(d->mac);
    up = 1;
    dump_known();
    drv_must_mac("the channel", mac_channel(channel));
    d->channel = channel;
    child_report(&d->c, DRV_LISTENING);
    drv_say("synth154: kind %u, %u MHz below channel %u, %u settings\n", kind, below, channel, n);
    txdc_was = REG(FE_TXDC);
    uint32_t dc_rate = 0, any_dc = 0;
    for (uint32_t k = 0; k < n; k++) {
        any_dc |= (uint32_t)s[k].dc;
    }
    drv_must("a gap_ms= of twice DC_SPAN_US at least, the sleep that brings the thread's virtual time back",
             !any_dc || gap_ms * 1000u >= 2u * DC_SPAN_US ? ESP_OK : ESP_FAIL);
    if (any_dc) {
        drv_phy_loopback(1, (uint32_t)s[0].lb);
        dc_rate = dc_calibrate(d, gap_ms);
        drv_phy_loopback(0, 0);
    }
    drv_must("the calibration's rate of DC_RATE_MIN to DC_RATE_MAX",
             !any_dc || (dc_rate >= DC_RATE_MIN && dc_rate <= DC_RATE_MAX) ? ESP_OK : ESP_FAIL);
    drv_must("a DC frame's words within the bank's",
             !any_dc || (uint64_t)FRAME_MAX * 64u * dc_rate / CHIP_HZ + dc_rate / CHIP_HZ + 2u <= DUMP_BANK_SIZE / 4u ? ESP_OK
                                                                                                         : ESP_FAIL);
    for (uint32_t k = 0; k < n; k++) {
        /* The frame's MHz each side, and the stream's first two images, the rate and twice it to each side of the frame. */
        int32_t at = 2407000 + 5000 * (int32_t)channel - 1000 * (int32_t)below + s[k].off_khz;
        int32_t reach = 1000 + 2 * (int32_t)(dc_rate / 1000u);
        drv_must("a DC setting's frame and its stream's first two images within the band, 2400 to 2483.5 MHz",
                 !s[k].dc || (at - reach >= BAND_LOW_KHZ && at + reach <= BAND_HIGH_KHZ) ? ESP_OK : ESP_FAIL);
    }

    uint32_t sent[SETTINGS_MAX] = { 0 }, plays = 0, unended = 0, made_us = 0;
    uint32_t dc_least = 0xffffffffu, dc_most = 0, dc_words_last = 0;
    uint8_t frame[FRAME_MAX];
    uint64_t start = osi_now_us(), end = start + (uint64_t)send * 1000000u;
    for (uint64_t now = start; now < end && !d->leave; now = osi_now_us()) { /* bounded by send */
        uint32_t k = plays % n;
        const struct setting *t = &s[k];
        uint32_t bytes = frame_of(kind, k, t, frame);
        uint64_t made = osi_now_us();
        int32_t shift_hz = -(int32_t)below * 1000000 + t->off_khz * 1000;
        uint32_t rest = 0, dc_words = 0;
        if (t->dc) {
            dc_words = make_dc(frame, bytes, t, shift_hz, dc_rate);
            dc_words_last = dc_words;
        } else {
            rest = make(frame, bytes, t, shift_hz);
        }
        made_us = (uint32_t)(osi_now_us() - made);
        if (looped && looped_lb != (uint32_t)t->lb) {
            drv_phy_loopback(0, 0);
            looped = 0;
        }
        if (!looped) {
            drv_phy_loopback(1, (uint32_t)t->lb);
            looped = 1;
            looped_lb = (uint32_t)t->lb;
        }
        uint32_t spc = t->slow ? SLOW_A_CHIP : SAMPLES_A_CHIP, first = bytes * 64u * spc + spc;
        if (t->dc) {
            uint32_t hz;
            dc_sleep(d, gap_ms);
            uint64_t at = dc_counts(d, &hz);
            dc_stream(dc_words);
            uint32_t took = (uint32_t)(dc_counts(d, &hz) - at);
            played_end = osi_now_us();
            dc_least = took < dc_least ? took : dc_least;
            dc_most = took > dc_most ? took : dc_most;
        } else {
            unended += !play(first < PLAY_COUNT ? first : PLAY_COUNT, rest, (uint32_t)t->slow);
        }
        sent[k]++;
        plays++;
        if (hear != 0 && plays % hear == 0) {
            /*
             * The antenna's path back: the loopback off, and the channel tuned again, as the receiver stays deaf
             * after the loopback until something moves it, tens of milliseconds at random; then a capture at once.
             */
            drv_phy_loopback(0, 0);
            looped = 0;
            (void)mac_channel(channel);
            uint32_t back = (uint32_t)(osi_now_us() - played_end);
            uint32_t took = capture(hear_words, hear_rate);
            drv_say("synth154: frame %u, setting %u: the receiver back %u us after its end, a capture of %u words in %u us\n",
                    plays - 1u, k, back, hear_words, took);
            tell(hear_words, hear_hex);
            child_answer(&d->c);
        }
        child_answer(&d->c);
        osi_delay_ms(gap_ms);
        child_answer(&d->c);
    }
    if (looped) {
        drv_phy_loopback(0, 0);
        looped = 0;
    }
    drv_say("synth154: %u frames played, %u unended, in %u us, the last made in %u us\n", plays, unended,
            (uint32_t)(osi_now_us() - start), made_us);
    if (any_dc) {
        drv_say("synth154: the DC word's streams of %u words: %u to %u counts\n", dc_words_last, dc_least, dc_most);
    }
    for (uint32_t k = 0; k < n; k++) {
        drv_say("synth154: setting %u: amp %d, off %d kHz, conj %d, lb %d, gap %d, dst %x, slow %d, dc %d: %u frames\n", k,
                s[k].amp, s[k].off_khz, s[k].conj, s[k].lb, s[k].gap, (uint32_t)s[k].dst, s[k].slow, s[k].dc, sent[k]);
    }

    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    drv_radio_off();
    drv_say("synth154: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
