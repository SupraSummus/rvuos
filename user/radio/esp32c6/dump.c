/*
 * The modem's dump engine, see dump.h, driven as Espressif's adctrig drives it, but for its gains,
 * which the radio's PHY leaves to the AGC, and the frames and tones some of its modes send, which a sampler never does.
 */

#include "dump.h"

#include <stdint.h>

#include "devices.h"
#include "osi.h"
#include "wifi/conf.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/*
 * The engine's words, in the modem's frame at 0x600A9000, a block no header of Espressif's names,
 * and the words beside it that a capture needs; hw/dump.md says whence each is known.
 */
#define DUMP_CTRL       (MODEM_BASE + 0x9004u)
#define CTRL_RATE_SHIFT 15u           /* bits 15 and 16, the 80 MHz clock divided by 1, 2, 3 or 5 */
#define CTRL_RING       (1u << 17)    /* set, the engine writes round a ring of the samples' length */
#define CTRL_DONE       (1u << 18)    /* set once the samples are written */
#define CTRL_TRIGGER    (1u << 19)    /* written 1 and then 0, the capture starts */
#define CTRL_ENABLE     (1u << 31)
#define DUMP_MODE       (MODEM_BASE + 0x9008u)
#define MODE_SOURCE_SHIFT 15u         /* bits 15 to 18, what the lanes are taken from */
#define MODE_SOURCE     (15u << MODE_SOURCE_SHIFT)
#define MODE_INDEX      0x7fffu       /* bits 0 to 14 as read, the ring's, see dump_ring_index; 15 up the source's */
#define DUMP_LANES      (MODEM_BASE + 0x9014u)
#define LANES_MASK      0x01ffffffu   /* four lanes of six bits from bit 0, and bit 24, which adctrig clears */
#define BT_DUMP         (MODEM_BASE + 0x20b4u)
#define BT_DUMP_SIDE    1u            /* the BLE baseband's side, adctrig's mode 8 */
/*
 * The BLE side's word, which adctrig's mode 8 sets before the BLE receiver starts, and whose bit 7 it sets
 * 200 us after, the capture's start on that side; what the fields are is not known.
 */
#define BT_DUMP_CONF    (MODEM_BASE + 0x210cu)
#define BT_CONF_START   (1u << 7)
#define BT_CONF_CLEAR   ((3u << 14) | (15u << 8) | (15u << 28) | BT_CONF_START)
#define BT_CONF_SET     (4u << 28)
#define CLK_CONF        (MODEM_SYSCON_BASE + 0x4u)
#define CLK_DATA_DUMP   (1u << 31)    /* MODEM_SYSCON_CLK_DATA_DUMP_EN */
#define SRAM_USAGE      (HP_SYSTEM_BASE + 0x4u)
#define SRAM_USAGE_BANKS (15u << 8)   /* HP_SYSTEM_SRAM_USAGE, a bit a bank: the dump engine's */
#define SRAM_USAGE_HALF (1u << 16)    /* HP_SYSTEM_MAC_DUMP_ALLOC, the bank's upper 64 KiB */

/* A word no capture writes, and the four past a capture's, which no capture of its length may touch. */
#define MARK  0xa5a0055au
#define GUARD 0x5a5aa5a5u
/*
 * How long a capture is waited for: at 16 MHz the longest takes 2 ms, and a source that waits for what the receiver
 * hears has finished within milliseconds; the program answers the root task's checks once a second, so a capture that
 * never finishes must give up long before that.
 */
#define DUMP_WAIT_US 100000u
/* dumphex='s lines a burst, about 1.5 KiB of the log's 4, and the pause after each, for the root task to take them. */
#define HEX_BURST    16u
#define HEX_PAUSE_MS 10u

static volatile uint32_t *const bank = (volatile uint32_t *)(uintptr_t)DUMP_BANK_BASE;
static uint32_t captures;

void dump_configure(const char *conf, uint32_t size, struct dump_conf *c)
{
    c->samples = config_number(conf, size, "dump", 0);
    if (c->samples > DUMP_SAMPLES_MAX) {
        c->samples = DUMP_SAMPLES_MAX;
    }
    c->source = config_number(conf, size, "dumpsource", 15) & 15u;
    c->lane = config_number(conf, size, "dumplane", 0);
    if (c->lane > 60u) {
        c->lane = 0;
    }
    c->bt = config_number(conf, size, "dumpbt", 0) != 0;
    c->rate = config_number(conf, size, "dumprate", 0) & 3u;
    c->sweep = config_number(conf, size, "dumpsweep", 0) != 0;
    c->hex = config_number(conf, size, "dumphex", 0);
    if (c->hex > DUMP_HEX_MAX) {
        c->hex = DUMP_HEX_MAX;
    }
}

/* A 10-bit two's complement field of a word, from bit at. */
static int32_t field10(uint32_t w, uint32_t at)
{
    int32_t v = (int32_t)((w >> at) & 0x3ffu);
    return v >= 512 ? v - 1024 : v;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0;
    for (uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

/* The engine stopped and set up: its clock, its lanes, its source and its side, and n words of the bank marked. */
static void setup(const struct dump_conf *c, uint32_t source, uint32_t n)
{
    uint32_t l = c->lane;
    REG(CLK_CONF) |= CLK_DATA_DUMP;
    REG(DUMP_CTRL) = 0;
    REG(DUMP_LANES) = (REG(DUMP_LANES) & ~LANES_MASK) | l | (l + 1u) << 6 | (l + 2u) << 12 | (l + 3u) << 18;
    REG(DUMP_MODE) = (REG(DUMP_MODE) & ~MODE_SOURCE) | source << MODE_SOURCE_SHIFT;
    REG(BT_DUMP) = c->bt ? REG(BT_DUMP) | BT_DUMP_SIDE : REG(BT_DUMP) & ~BT_DUMP_SIDE;
    for (uint32_t i = 0; i < n; i++) {
        bank[i] = MARK;
    }
}

/* The bank the engine's, and SRAM_USAGE as it was, for bank_to_cpu. */
static uint32_t bank_to_engine(void)
{
    uint32_t usage = REG(SRAM_USAGE);
    REG(SRAM_USAGE) = (usage & ~(SRAM_USAGE_BANKS | SRAM_USAGE_HALF)) | (1u << (8u + DUMP_BANK));
    __asm__ volatile("fence rw, rw" ::: "memory");
    (void)REG(SRAM_USAGE);
    return usage;
}

static void bank_to_cpu(uint32_t usage)
{
    REG(SRAM_USAGE) = usage;
    __asm__ volatile("fence rw, rw" ::: "memory");
    (void)REG(SRAM_USAGE);
}

/* The engine's run: the bank its, the capture started, waited for, and the bank the CPU's again; the reads it took. */
static uint32_t capture(const struct dump_conf *c, uint32_t source)
{
    uint32_t n = c->samples;
    setup(c, source, n);
    for (uint32_t i = 0; i < 4u; i++) {
        bank[n + i] = GUARD ^ i;
    }
    uint32_t usage = bank_to_engine();
    uint32_t ctrl = n | c->rate << CTRL_RATE_SHIFT;
    /* A done the last capture left is cleared first, as ESP-SDR found the engine wants. */
    REG(DUMP_CTRL) = ctrl | CTRL_DONE;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    uint32_t bt_conf = REG(BT_DUMP_CONF);
    if (c->bt) {
        /* The receiver is already on, the 802.15.4 MAC's, so the BLE receiver's start is left out. */
        REG(BT_DUMP_CONF) = (bt_conf & ~BT_CONF_CLEAR) | BT_CONF_SET;
        REG(BT_DUMP_CONF) |= BT_CONF_START;
    } else {
        REG(DUMP_CTRL) = ctrl | CTRL_ENABLE | CTRL_TRIGGER;
        REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    }
    uint32_t polls = 0;
    uint64_t start = osi_now_us();
    while ((REG(DUMP_CTRL) & CTRL_DONE) == 0 && osi_now_us() - start < DUMP_WAIT_US) { /* bounded by DUMP_WAIT_US */
        polls++;
    }
    REG(DUMP_CTRL) = 0;
    if (c->bt) {
        REG(BT_DUMP_CONF) = bt_conf;
    }
    bank_to_cpu(usage);
    return polls;
}

/*
 * What the n words from the bank's word base hold, after a line of the caller's: the written ones' spread and gains,
 * and the first dumphex= of them whole.
 */
static void tell(const struct dump_conf *c, uint32_t base, uint32_t n)
{
    uint32_t seen = 0, changes = 0;
    int32_t sum_i = 0, sum_q = 0, min_i = 511, max_i = -512, min_q = 511, max_q = -512;
    uint32_t sq_i = 0, sq_q = 0, gain_min = 255, gain_max = 0, agc_or = 0, agc_and = 15;
    for (uint32_t k = base; k < base + n; k++) {
        uint32_t w = bank[k];
        if (w == MARK) {
            continue;
        }
        seen++;
        int32_t i = field10(w, 10), q = field10(w, 0);
        sum_i += i;
        sum_q += q;
        sq_i += (uint32_t)(i * i) >> 2;
        sq_q += (uint32_t)(q * q) >> 2;
        min_i = i < min_i ? i : min_i;
        max_i = i > max_i ? i : max_i;
        min_q = q < min_q ? q : min_q;
        max_q = q > max_q ? q : max_q;
        uint32_t gain = (w >> 20) & 0xffu;
        gain_min = gain < gain_min ? gain : gain_min;
        gain_max = gain > gain_max ? gain : gain_max;
        agc_or |= w >> 28;
        agc_and &= w >> 28;
        changes += k != base && w != bank[k - 1u];
    }
    int32_t d = seen ? (int32_t)seen : 1;
    drv_say("dump: I mean %d rms %u range %d to %d; Q mean %d rms %u range %d to %d\n", sum_i / d,
        isqrt(sq_i / (uint32_t)d * 4u), min_i, max_i, sum_q / d, isqrt(sq_q / (uint32_t)d * 4u), min_q, max_q);
    drv_say("dump: gain %u to %u, AGC bits or %x and %x, words that change %u\n", gain_min, gain_max, agc_or,
        agc_and, changes);
    uint32_t words = c->hex < n ? c->hex : n;
    for (uint32_t k = 0, lines = 0; k < words; k += 8u) { /* bounded by words, DUMP_HEX_MAX at most */
        if (lines++ % HEX_BURST == 0) {
            osi_delay_ms(HEX_PAUSE_MS);
        }
        const volatile uint32_t *w = &bank[base + k];
        drv_say("dumphex: %u %u %x %x %x %x %x %x %x %x\n", captures, k, w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                w[7]);
    }
    captures++;
}

void dump_take(struct dump_conf *c)
{
    if (c->samples == 0) {
        return;
    }
    uint32_t source = c->sweep ? captures & 15u : c->source;
    uint32_t polls = capture(c, source);
    uint32_t n = c->samples, written = 0, guards = 0;
    for (uint32_t k = 0; k < n; k++) {
        written += bank[k] != MARK;
    }
    for (uint32_t k = 0; k < 4u; k++) {
        guards += bank[n + k] == (GUARD ^ k);
    }
    drv_say("dump: capture %u source %u lane %u%s rate %u: %u of %u written, guards %u of 4, %u polls\n", captures,
        source, c->lane, c->bt ? " bt" : "", c->rate, written, n, guards, polls);
    tell(c, 0, n);
}

static uint32_t ring_usage; /* SRAM_USAGE before dump_ring_start, which dump_ring_stop puts back */

void dump_ring_start(const struct dump_conf *c)
{
    setup(c, c->source, DUMP_RING);
    ring_usage = bank_to_engine();
    uint32_t ctrl = DUMP_RING | c->rate << CTRL_RATE_SHIFT | CTRL_RING;
    REG(DUMP_CTRL) = ctrl | CTRL_DONE;
    REG(DUMP_CTRL) = ctrl;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
}

uint32_t dump_ring_index(void)
{
    return REG(DUMP_MODE) & MODE_INDEX;
}

int dump_ring_stopped(void)
{
    return (REG(DUMP_CTRL) & CTRL_DONE) != 0;
}

void dump_ring_restart(const struct dump_conf *c)
{
    REG(DUMP_CTRL) = 0;
    bank_to_cpu(ring_usage);
    dump_ring_start(c);
}

void dump_ring_stop(const struct dump_conf *c)
{
    /* The index reads 0 once the engine stops, so it is read before. */
    uint32_t last = dump_ring_index();
    REG(DUMP_CTRL) = 0;
    bank_to_cpu(ring_usage);
    uint32_t written = 0;
    for (uint32_t k = 0; k < DUMP_RING; k++) {
        written += bank[k] != MARK;
    }
    drv_say("dump: ring %u source %u rate %u: index %x at the stop, %u of %u written\n", captures, c->source, c->rate,
            last, written, DUMP_RING);
    /* The newest words, the oldest first, into the bank's upper half, which the ring does not reach. */
    uint32_t told = c->hex && c->hex < written ? c->hex : written;
    for (uint32_t k = 0; k < told; k++) { /* bounded by DUMP_RING */
        bank[DUMP_RING + k] = bank[(last + DUMP_RING - told + k) % DUMP_RING];
    }
    tell(c, DUMP_RING, told);
}

static uint32_t armed_usage; /* SRAM_USAGE before dump_arm, which dump_disarm puts back */

void dump_arm(const struct dump_conf *c, int trigger)
{
    setup(c, c->source, c->samples);
    armed_usage = bank_to_engine();
    uint32_t ctrl = c->samples | c->rate << CTRL_RATE_SHIFT;
    REG(DUMP_CTRL) = ctrl | CTRL_DONE;
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE | (trigger ? CTRL_TRIGGER : 0);
    REG(DUMP_CTRL) = ctrl | CTRL_ENABLE;
}

uint32_t dump_disarm(const struct dump_conf *c)
{
    REG(DUMP_CTRL) = 0;
    bank_to_cpu(armed_usage);
    uint32_t written = 0;
    for (uint32_t k = 0; k < c->samples; k++) {
        written += bank[k] != MARK;
    }
    return written;
}
