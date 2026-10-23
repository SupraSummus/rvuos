/*
 * The observer: the nRF52840's radio in BLE's 1 Mbit mode, listening on one channel for the packets of an access address,
 * as an adapter in BLE's receiver test does, to say how many came whole and how strong.
 * user/observer/le-observer.py runs it and reads what it says;
 * the CYW43439's experiments, user/wifi/BT.md, are its first use.
 *
 * The radio receives one packet after another by itself, END_START.
 * Two routes, DESIGN.md's "Routes", count what it heard with no processor in between:
 * CRCOK counts in TIMER3 and CRCERROR in TIMER4, so the counts are whole however late the program wakes.
 * The program wakes at each packet's end for its strength, RSSISAMPLE, which ADDRESS starts measuring,
 * and keeps the strongest and the sum of those whose CRC was right; a packet it wakes too late for is not measured,
 * so the line says how many were.
 * Every window= ms it says one line, the window's end on the clock and what the window heard:
 *
 *   observer: at US ok N bad M rssi SUM of K, best B
 *
 * SUM the sum of K strengths in dBm, negative, B the strongest; the host divides.
 * The packets are not whitened, as BLE's test packets are not, and whatever their length, up to 255 bytes, they are taken.
 *
 * Its configuration, lines of key=value in the input region, OBSERVER_CONFIG's file, see user/observer/build.mk:
 * channel=, 0 to 39, BLE's RF channel at 2402 + 2 * channel MHz, 19 by default, 2440 MHz;
 * aa=, the access address in hex, the test's 71764129 by default; crcinit=, the CRC's start in hex, 555555 by default;
 * window=, ms, 100 by default, 10 to 10000; run=, seconds, 30 by default, at most 3600.
 * It sends nothing.
 */

#include "console.h"
#include "devices.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "lib/self.h"
#include "wifi/conf.h"

#define REG(off)         (*(volatile uint32_t *)(RADIO_BASE + (off)))
#define TREG(base, off)  (*(volatile uint32_t *)((base) + (off)))

/* The radio's registers, nRF52840 Product Specification, RADIO. */
#define TASKS_RXEN       0x004u
#define TASKS_DISABLE    0x010u
#define EVENTS_END       0x10cu
#define EVENTS_DISABLED  0x110u
#define EVENTS_CRCOK     0x130u
#define EVENTS_CRCERROR  0x134u
#define SHORTS           0x200u
#define INTENSET         0x304u
#define INTENCLR         0x308u
#define PACKETPTR        0x504u
#define FREQUENCY        0x508u
#define MODE             0x510u
#define PCNF0            0x514u
#define PCNF1            0x518u
#define BASE0            0x51cu
#define PREFIX0          0x524u
#define RXADDRESSES      0x530u
#define CRCCNF           0x534u
#define CRCPOLY          0x538u
#define CRCINIT          0x53cu
#define RSSISAMPLE       0x548u
#define STATE            0x550u
#define POWER            0xffcu

#define SHORT_READY_START     (1u << 0)
#define SHORT_END_START       (1u << 5)
#define SHORT_ADDRESS_RSSISTART (1u << 4)
#define INT_END          (1u << 3)
#define MODE_BLE_1MBIT   3u
/* A length of 8 bits after a header byte, S0, and the preamble of one byte. */
#define PCNF0_BLE        ((8u << 0) | (1u << 8))
/* At most 255 bytes, an address of three bytes after the prefix, little endian, not whitened. */
#define PCNF1_BLE        ((255u << 0) | (3u << 16))
/* BLE's CRC: three bytes, over the PDU alone, x^24 + x^10 + x^9 + x^6 + x^4 + x^3 + x + 1. */
#define CRCCNF_BLE       (3u | (1u << 8))
#define CRCPOLY_BLE      0x65bu
#define STATE_DISABLED   0u
#define DISABLE_POLLS    100000u

/* A TIMER's registers, as offsets from its base, counting what a route hands it. */
#define TIMER_START      0x000u
#define TIMER_STOP       0x004u
#define TIMER_COUNT      0x008u
#define TIMER_CLEAR      0x00cu
#define TIMER_CAPTURE0   0x040u
#define TIMER_MODE       0x504u
#define TIMER_BITMODE    0x508u
#define TIMER_CC0        0x540u
#define MODE_COUNTER     1u
#define BITMODE_32       3u

#define CHANNEL_LAST     39u
#define CHANNEL_DEFAULT  19u
#define AA_DEFAULT       0x71764129u
#define CRCINIT_DEFAULT  0x555555u
#define WINDOW_MS_DEFAULT 100u
#define WINDOW_MS_MIN    10u
#define WINDOW_MS_MAX    10000u
#define RUN_S_DEFAULT    30u
#define RUN_S_MAX        3600u
#define US_PER_MS        1000u

static struct self self;
static uint8_t packet[260] __attribute__((aligned(4)));

static void console_write(void *to, const char *s, uint32_t n)
{
    (void)to;
    for (uint32_t i = 0; i < n; i++) {
        console_put_polled(s[i]);
    }
}

static const struct out cout = { console_write, 0 };

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&cout, "observer: %s: failed with %u", what, status);
        if (self.what != 0) {
            say(&cout, " at %s", self.what);
        }
        say(&cout, "\n");
        rv_halt(BOOT_CAP_HALT, 1);
    }
}

struct config {
    uint32_t channel, aa, crcinit, window_ms, run_s;
};

/* A number of the configuration in hex, with or without 0x, or dflt. */
static uint32_t config_hex(const char *conf, uint32_t size, const char *key, uint32_t dflt)
{
    char value[12];
    value[0] = '\0';
    config_value(conf, size, key, value, sizeof(value));
    uint32_t i = value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 2u : 0u;
    uint32_t n = 0, digits = 0;
    for (; digits < 8u && value[i] != '\0'; digits++, i++) {
        char c = value[i];
        uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                     : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16u;
        if (d == 16u) {
            return dflt;
        }
        n = n << 4 | d;
    }
    return digits == 0 ? dflt : n;
}

static void configure(struct config *c)
{
    uint32_t base, size, region;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    must("install the input", region_install(&self, BOOT_CAP_INPUT, RIGHT_R, &region));
    const char *conf = (const char *)(uintptr_t)base;
    c->channel = config_number(conf, size, "channel", CHANNEL_DEFAULT);
    c->channel = c->channel > CHANNEL_LAST ? CHANNEL_DEFAULT : c->channel;
    c->aa = config_hex(conf, size, "aa", AA_DEFAULT);
    c->crcinit = config_hex(conf, size, "crcinit", CRCINIT_DEFAULT) & 0xffffffu;
    c->window_ms = config_number(conf, size, "window", WINDOW_MS_DEFAULT);
    c->window_ms = c->window_ms < WINDOW_MS_MIN ? WINDOW_MS_MIN : c->window_ms > WINDOW_MS_MAX ? WINDOW_MS_MAX
                                                                                                : c->window_ms;
    c->run_s = config_number(conf, size, "run", RUN_S_DEFAULT);
    c->run_s = c->run_s == 0 ? RUN_S_DEFAULT : c->run_s > RUN_S_MAX ? RUN_S_MAX : c->run_s;
    must("uninstall the input", region_free(&self, region));
}

/* Off and on again, which resets the radio, then set for BLE's 1 Mbit packets of the access address, off the air. */
static void radio_set(const struct config *c)
{
    REG(POWER) = 0;
    REG(POWER) = 1;
    REG(MODE) = MODE_BLE_1MBIT;
    REG(PCNF0) = PCNF0_BLE;
    REG(PCNF1) = PCNF1_BLE;
    REG(BASE0) = c->aa << 8;
    REG(PREFIX0) = c->aa >> 24;
    REG(RXADDRESSES) = 1;
    REG(CRCCNF) = CRCCNF_BLE;
    REG(CRCPOLY) = CRCPOLY_BLE;
    REG(CRCINIT) = c->crcinit;
    REG(FREQUENCY) = 2u + 2u * c->channel; /* MHz above 2400 */
    REG(PACKETPTR) = (uint32_t)(uintptr_t)packet;
}

static void radio_stop(void)
{
    REG(INTENCLR) = ~0u;
    REG(SHORTS) = 0;
    if (REG(STATE) != STATE_DISABLED) {
        REG(EVENTS_DISABLED) = 0;
        REG(TASKS_DISABLE) = 1;
        for (uint32_t i = 0; i < DISABLE_POLLS && REG(EVENTS_DISABLED) == 0; i++) { /* bounded: DISABLE_POLLS */
        }
    }
}

/* A TIMER that counts what its route hands it, from zero. */
static void counter_start(uint32_t base)
{
    TREG(base, TIMER_MODE) = MODE_COUNTER;
    TREG(base, TIMER_BITMODE) = BITMODE_32;
    TREG(base, TIMER_CLEAR) = 1;
    TREG(base, TIMER_START) = 1;
}

static uint32_t counter_read(uint32_t base)
{
    TREG(base, TIMER_CAPTURE0) = 1;
    return TREG(base, TIMER_CC0);
}

/* A route from an event of the radio's to a TIMER's count, on the router's channel. */
static void route_count(uint32_t channel_no, uint32_t event, uint32_t timer_cap)
{
    uint32_t channel, route;
    must("a slot for the channel", slot_new(&self, &channel));
    must("a channel", rv_invoke(OP_ROUTE_CARVE, BOOT_CAP_ROUTES, channel_no, 1, channel));
    must("a slot for the route", slot_new(&self, &route));
    must("bind the route", rv_invoke(OP_ROUTE_BIND, channel, self.pool, route, 0));
    must("the route's event", rv_invoke(OP_ROUTE_END, route, ROUTE_EVENT, BOOT_CAP_RADIO, event));
    must("the route's task", rv_invoke(OP_ROUTE_END, route, ROUTE_TASK, timer_cap, TIMER_COUNT));
    must("arm the route", rv_invoke(OP_ROUTE_SET, route, 1, 0, 0));
}

int main(void);
int main(void)
{
    uint32_t base, size, region;
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("the root's own", self_root(&self, TIME_UNITS));
    must("map the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    struct config c;
    configure(&c);
    must("map the radio", region_install(&self, BOOT_CAP_RADIO, RIGHT_R | RIGHT_W, &region));
    must("map TIMER3", region_install(&self, BOOT_CAP_TIMER3, RIGHT_R | RIGHT_W, &region));
    must("map TIMER4", region_install(&self, BOOT_CAP_TIMER4, RIGHT_R | RIGHT_W, &region));

    uint32_t radio_bit, radio_irq, window_bit, window_irq;
    must("a bit for the radio", bit_new(&self, &radio_bit));
    must("a slot for the radio's line", slot_new(&self, &radio_irq));
    must("the radio's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, RADIO_IRQ, 1, radio_irq));
    must("bind the radio's line", rv_irq_bind(radio_irq, self.pool, self.inbox, radio_irq));
    must("a bit for the windows", bit_new(&self, &window_bit));
    must("a timer for the windows", timer_new(&self, &window_irq));

    radio_set(&c);
    counter_start(TIMER3_BASE);
    counter_start(TIMER4_BASE);
    route_count(0, EVENTS_CRCOK, BOOT_CAP_TIMER3);
    route_count(1, EVENTS_CRCERROR, BOOT_CAP_TIMER4);
    say(&cout, "observer: BLE 1 Mbit on %u MHz, access address %x, CRC from %x, a line every %u ms for %u s\n",
        2402u + 2u * c.channel, c.aa, c.crcinit, c.window_ms, c.run_s);

    REG(EVENTS_END) = 0;
    REG(SHORTS) = SHORT_READY_START | SHORT_END_START | SHORT_ADDRESS_RSSISTART;
    REG(INTENSET) = INT_END;
    REG(TASKS_RXEN) = 1;
    must("arm the radio's line", rv_irq_set(radio_irq, radio_bit));
    uint32_t skipped;
    must("the windows' timer", rv_timer_period(window_irq, window_bit, c.window_ms * US_PER_MS, &skipped));

    uint32_t windows = c.run_s * 1000u / c.window_ms;
    uint32_t ok_before = 0, bad_before = 0, ok_all = 0, bad_all = 0, measured = 0, best = 0;
    int32_t sum = 0;
    for (uint32_t w = 0; w < windows;) { /* bounded: windows, each a period of the timer */
        uint32_t bits;
        must("wait", rv_wait(self.inbox, &bits));
        if (bits & radio_bit) {
            if (REG(EVENTS_END) != 0) {
                REG(EVENTS_END) = 0;
                /*
                 * The radio receives again by the time the program wakes, so the packet's verdict is its event's,
                 * which only this clears; the routes count the event as it comes, whatever this does to it after.
                 */
                bool whole = REG(EVENTS_CRCOK) != 0;
                REG(EVENTS_CRCOK) = 0;
                if (whole) {
                    uint32_t rssi = REG(RSSISAMPLE) & 0x7fu; /* the strength's magnitude, -dBm */
                    sum -= (int32_t)rssi;
                    best = measured == 0 || rssi < best ? rssi : best;
                    measured++;
                }
            }
            must("arm the radio's line", rv_irq_set(radio_irq, radio_bit));
        }
        if (bits & window_bit) {
            uint64_t at;
            uint32_t hz, counter;
            must("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &at, &hz, &counter));
            uint32_t ok = counter_read(TIMER3_BASE), bad = counter_read(TIMER4_BASE);
            say(&cout, "observer: at %u ok %u bad %u rssi %d of %u, best -%u\n", (uint32_t)at, ok - ok_before,
                bad - bad_before, (int)sum, measured, best);
            ok_all += ok - ok_before;
            bad_all += bad - bad_before;
            ok_before = ok;
            bad_before = bad;
            sum = 0;
            measured = 0;
            best = 0;
            /* The next window, a period after this one's deadline; a window the program slept through is counted in. */
            must("the windows' timer", rv_timer_period(window_irq, window_bit, c.window_ms * US_PER_MS, &skipped));
            w += 1u + skipped;
        }
    }
    radio_stop();
    TREG(TIMER3_BASE, TIMER_STOP) = 1;
    TREG(TIMER4_BASE, TIMER_STOP) = 1;
    say(&cout, "observer: in all ok %u bad %u\n", ok_all, bad_all);
    rv_halt(BOOT_CAP_HALT, 0);
}
