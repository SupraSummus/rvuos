/*
 * The root task of the Wi-Fi system on an ESP32-C6: the driver of drv.h and the network process of wifi.h,
 * each a child of its own, joined by the link, and the network process's clients, as on the Pico 2 W; see system.h.
 *
 * It reads the factory MAC address from the eFuse and what to do from the input region,
 * sets the XIAO ESP32-C6's RF switch to the antenna the configuration names,
 * and builds the driver as a child that runs the image in the window onto flash, with exactly what drv.h lists;
 * if a network is named, it builds the network process too, which runs the IP stack of netproc.c,
 * and connects the two with the link.
 * Once the driver has joined, it starts the network process, which asks for an address by DHCP,
 * and with one answers ping, and datagrams to UDP port 7777 with a line about the system;
 * then it builds the echo and the clock, the network process's clients of wifi.h.
 * The logger is not among them: the root task carries the kernel's log itself, and the log has one reader.
 * Every second it asks each child whether its loop still comes round, and feeds the watchdog;
 * a client that faults, fails or did not answer is taken down and built again, a few times at most.
 * The run lasts as long as the configuration says, by default half a minute, or for good,
 * or until the host ends it, and once it is over the driver leaves the network before the machine halts;
 * it ends early if the driver or the network process fails, faults or does not answer,
 * if the driver has not joined in time, or once a scan alone was asked for and is done.
 * A run that has the driver listen to a channel, see drv.h, builds no network process, and lasts as one that joins.
 *
 * The kernel's log goes to the console as it comes, so that the host reads the run while it lasts,
 * and the halt writes out only what was not taken yet.
 * The host may type a line on the console: "end" ends the run now, as if its time were up.
 * tools/wifi-run.py waits for the clients, checks the system from the host, and ends the run.
 *
 * user/lib/ keeps what it hands out: its slots, regions, bits, units of time, free memory and room for children's data.
 */

#include <stdint.h>

#include "console.h"
#include "drv.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"
#include "rvuos.h"
#include "wifi/conf.h"
#include "wifi/system.h"

/* The run's seconds unless the configuration says, by when the driver must have joined, and the children's checks. */
#define RUN_S    30u
#define UP_S     30u
#define CHECK_US 1000000u
#define TRACE_RUN_S 180u /* a traced run's seconds unless the configuration says: the faults slow the bring-up down */

/*
 * The driver's table, the most a child has, of which the slots past what the root task gives it are its own,
 * and its own pool, timer lines and units, for the adapter's threads, which take a unit and a line each.
 */
#define DRV_TABLE       CHILD_TABLE_MAX
#define DRV_OWN_SLOTS   40u
#define DRV_OWN_POOL    0x2000u
#define DRV_OWN_LINES   8u
#define DRV_OWN_UNITS   8u
#define ROOM_SIZE       0x8000u /* the network process's data and its clients', the driver's being a block of its own */

/*
 * Who gets which of the first core's units: the root task the first, mostly waiting,
 * and the network process and its clients theirs, system.c's.
 */
#define ROOT_UNITS 4u
#define DRV_UNITS  8u /* the driver's first thread, which serves the link */

#define EFUSE_RD_MAC_SPI_SYS_0 (EFUSE_BASE + 0x44u)
#define EFUSE_RD_MAC_SPI_SYS_1 (EFUSE_BASE + 0x48u)

/*
 * The XIAO ESP32-C6's RF switch, between the chip and its two antennas, on two pins the ROM leaves floating:
 * GPIO3 low powers it, and GPIO14 picks the antenna on the board, low, or the U.FL connector, high, as Seeed's startup does.
 * Set so, a scan heard the nearest access point 12 to 22 dB louder, and two more,
 * and the station joined at -64 dBm where it joined at -85 with the pins left alone, measured on the chip.
 * Each pin is a plain output, through the IO MUX and the GPIO matrix.
 */
#define RF_SWITCH_POWER      3u
#define RF_SWITCH_ANTENNA    14u
#define IO_MUX_GPIO(n)       (IO_MUX_BASE + 4u + 4u * (n))
#define IO_MUX_MCU_SEL       (7u << 12)
#define IO_MUX_MCU_SEL_GPIO  (1u << 12)
#define GPIO_OUT_W1TS        (GPIO_BASE + 0x08u)
#define GPIO_OUT_W1TC        (GPIO_BASE + 0x0Cu)
#define GPIO_ENABLE_W1TS     (GPIO_BASE + 0x24u)
#define GPIO_FUNC_OUT_SEL(n) (GPIO_BASE + 0x554u + 4u * (n))
#define GPIO_OUT_SEL_GPIO    128u /* the pin follows GPIO_OUT */

enum antenna { ANTENNA_BOARD, ANTENNA_UFL, ANTENNA_NONE };

static struct self self;
static const struct out kout = { log_write, (void *)BOOT_CAP_DEBUG };
static struct system sys = {
    .self = &self,
    .out = &kout,
    .board = "an ESP32-C6",
    .clients_wanted = CLIENT_BIT(CLIENT_ECHO) | CLIENT_BIT(CLIENT_CLOCK),
};
static struct block drv_ram;
static int leaving;
static uint8_t mac[6];
static uint32_t tick_bit, seconds, run_s = RUN_S;
static enum antenna antenna;
static struct kernel_log klog;
static uint32_t log_bit, console_bit, console_irq;
static char command[16];
static uint32_t command_len;

/*
 * The trace of the libraries' bring-up, see drv.h: with trace=1 the driver maps no device, so every access it
 * makes faults, and the root task carries it out here, logs it, and answers; trace=2 runs the same window with
 * the driver mapping its frames, the baseline. There is one list of frames, so only who maps them differs.
 */
static uint32_t trace_wanted;  /* the configuration asked for the window: 1 traces, 2 runs it without a fault */
static uint32_t trace_serving; /* the trace serves the accesses; with trace=2 the driver maps the devices and none faults */
static uint32_t trace_note;    /* the notification a trace answer goes back on */
/* The device frames of the driver's own, mapped by it in a plain run or by the root under the trace. */
static const struct {
    uint32_t cap, rights;
    const char *name;
} trace_frames[] = {
    { BOOT_CAP_MODEM, RIGHT_R | RIGHT_W, "the modem" },
    { BOOT_CAP_SARADC, RIGHT_R | RIGHT_W, "the SAR ADC" },
    { BOOT_CAP_RNG, RIGHT_R, "the random number generator" },
};
static struct {
    uint32_t base, size;
} trace_dev[sizeof(trace_frames) / sizeof(trace_frames[0])];
static uint32_t trace_dev_count;
/*
 * The addresses a snapshot run reads back at the end of the window, from the configuration's snap= lines:
 * the ones the traced code read at least once. A read of only what the run read adds no access of a kind
 * the run did not make; a register whose read has an effect, or whose value moves, comes out different
 * between two dry runs and drops out of the compare. tools/mac-trace.py makes the list and compares the runs.
 */
#define SNAP_MAX 512
static uint32_t snap_addrs[SNAP_MAX];
static uint32_t snap_count;
/*
 * The stream goes straight to the console, not through the kernel's log: the kernel writes two lines of its own
 * for every fault, and the host's check symbolizes each of those, so a trace through it drowns.
 * A line is gathered whole and handed the FIFO one packet at a time, and the writer waits, unlike the board's
 * polled one, which after one long wait gives up on every byte after; the host runs wifi-run.py, so it is there.
 */
static void console_send(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n;) {
        uint32_t chunk = n - i > 64u ? 64u : n - i; /* the FIFO holds 64 bytes, so one packet each */
        for (uint32_t j = 0; j < chunk; j++) {
            while ((console_regs[USJ_EP1_CONF] & USJ_FREE) == 0) {
                console_flush();
            }
            console_regs[USJ_EP1] = (uint8_t)s[i + j];
        }
        console_flush();
        i += chunk;
    }
}

static char out_line[256];
static uint32_t out_n;

static void console_write(void *to, const char *s, uint32_t n)
{
    (void)to;
    for (uint32_t i = 0; i < n; i++) {
        if (out_n < sizeof(out_line)) {
            out_line[out_n] = s[i];
        }
        out_n++;
        if (s[i] == '\n' || out_n >= sizeof(out_line)) {
            console_send(out_line, out_n < sizeof(out_line) ? out_n : sizeof(out_line));
            out_n = 0;
        }
    }
}
static const struct out cout = { console_write, 0 };
static uint32_t trace_seq;     /* the next line's sequence number */
static uint32_t trace_records; /* the accesses served */
static struct trace_cycle trace_cyc;

/*
 * The kernel's log as the trace drains it: its own fault report, two lines per traced access, is dropped,
 * since the host would symbolize every one of them; everything else, the driver's steps and events among it, goes on.
 * The dropped count is printed at the end, so a fault the tracer did not serve shows as a mismatch, not as silence.
 */
static int starts_with(const char *s, const char *p)
{
    for (; *p != 0; s++, p++) {
        if (*s != *p) {
            return 0;
        }
    }
    return 1;
}

static int has(const char *s, const char *p)
{
    for (; *s != 0; s++) {
        if (starts_with(s, p)) {
            return 1;
        }
    }
    return 0;
}

static int kline_drop(const char *line)
{
    const char *s = line;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return starts_with(s, "user fault") || has(s, "mcause=");
}

static char kline[160];
static uint32_t kline_n;
static uint32_t kline_dropped;

static void kline_put(char c)
{
    if (kline_n < sizeof(kline) - 1) {
        kline[kline_n++] = c;
    }
    if (c == '\n' || kline_n >= sizeof(kline) - 1) {
        if (kline_drop(kline)) {
            kline_dropped++;
        } else {
            console_send(kline, kline_n);
        }
        kline_n = 0;
    }
}

static void must(const char *what, uint32_t status)
{
    system_must(&sys, what, status);
}

/* The factory MAC address, whose bytes the eFuse holds last first. */
static void read_mac(void)
{
    uint32_t region;
    must("install the eFuse", region_install(&self, BOOT_CAP_EFUSE, RIGHT_R, &region));
    uint32_t lo = *(volatile uint32_t *)EFUSE_RD_MAC_SPI_SYS_0;
    uint32_t hi = *(volatile uint32_t *)EFUSE_RD_MAC_SPI_SYS_1;
    must("uninstall the eFuse", region_free(&self, region));
    uint8_t fused[6] = { (uint8_t)lo, (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24), (uint8_t)hi,
                         (uint8_t)(hi >> 8) };
    for (int i = 0; i < 6; i++) {
        mac[i] = fused[5 - i];
    }
}

/* A pin made a plain output at a level, which is set before its output is enabled. */
static void pin_out(uint32_t pin, int high)
{
    volatile uint32_t *mux = (volatile uint32_t *)IO_MUX_GPIO(pin);
    *mux = (*mux & ~IO_MUX_MCU_SEL) | IO_MUX_MCU_SEL_GPIO;
    *(volatile uint32_t *)GPIO_FUNC_OUT_SEL(pin) = GPIO_OUT_SEL_GPIO;
    *(volatile uint32_t *)(high ? GPIO_OUT_W1TS : GPIO_OUT_W1TC) = 1u << pin;
    *(volatile uint32_t *)GPIO_ENABLE_W1TS = 1u << pin;
}

/* The RF switch, as the configuration's antenna= asks, before the driver's PHY first calibrates. */
static void rf_switch(void)
{
    if (antenna == ANTENNA_NONE) {
        say(&kout, "root: no RF switch to set, so the pins are left alone\n");
        return;
    }
    uint32_t mux, gpio;
    must("install the IO MUX", region_install(&self, BOOT_CAP_IO_MUX, RIGHT_R | RIGHT_W, &mux));
    must("install the GPIO matrix", region_install(&self, BOOT_CAP_GPIO, RIGHT_R | RIGHT_W, &gpio));
    pin_out(RF_SWITCH_POWER, 0);
    pin_out(RF_SWITCH_ANTENNA, antenna == ANTENNA_UFL);
    must("uninstall the GPIO matrix", region_free(&self, gpio));
    must("uninstall the IO MUX", region_free(&self, mux));
    say(&kout, antenna == ANTENNA_UFL ? "root: the RF switch picks the U.FL connector\n"
                                      : "root: the RF switch picks the antenna on the board\n");
}

/* The driver's entry, from the header its image starts with, read through the window onto flash for the moment. */
static void (*drv_entry(void))(struct child_page *)
{
    uint32_t region;
    must("install the window onto flash", region_install(&self, BOOT_CAP_FLASH, RIGHT_R, &region));
    struct drv_header h = *(const struct drv_header *)FLASH_WINDOW_BASE;
    must("uninstall the window onto flash", region_free(&self, region));
    if (h.magic != DRV_MAGIC) {
        say(&kout, "root: no driver in the window onto flash: %x where %x belongs\n", h.magic, DRV_MAGIC);
        system_halt(SYSTEM_BROKEN);
    }
    return h.entry;
}

/* One hexadecimal number at s, into *out; returns whether it read one, leaving *end past it. */
static int snap_hex(const char *s, uint32_t *out, const char **end)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    uint32_t v = 0;
    const char *p = s;
    for (; *p != '\0'; p++) {
        int d;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            d = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            d = *p - 'A' + 10;
        } else {
            break;
        }
        v = v * 16u + (uint32_t)d;
    }
    *out = v;
    *end = p;
    return p != s;
}

/* The configuration's snap= lines into snap_addrs: one address, or one range lo-hi, both four apart. */
static void snap_parse(const char *conf, uint32_t len)
{
    snap_count = 0;
    for (uint32_t i = 0; i < len && conf[i] != '\0'; i++) {
        if ((i != 0 && conf[i - 1] != '\n') || i + 5 > len || memcmp(conf + i, "snap=", 5) != 0) {
            continue;
        }
        const char *p;
        uint32_t lo, hi;
        if (!snap_hex(conf + i + 5, &lo, &p)) {
            continue;
        }
        if (*p == '-') {
            if (!snap_hex(p + 1, &hi, &p)) {
                continue;
            }
        } else {
            hi = lo;
        }
        for (lo &= ~3u; lo <= hi && snap_count < SNAP_MAX; lo += 4) {
            snap_addrs[snap_count++] = lo;
        }
    }
}

/*
 * What to do, from the text the loader may have left in the input region: lines of ssid= and pass=,
 * the network to join, and with no ssid, a scan; bssid=, which of the network's access points to join;
 * and run=, how many seconds the run lasts, 0 for good.
 * debug= names what the driver tells besides its steps, any of frames, wpa and rekey, see drv.h,
 * and lib= the libraries' log level, 4 for debug and 5 for verbose;
 * pmf=0 joins without protecting management frames, and sae=0 without WPA3's SAE.
 * listen=, a channel, with no ssid has the driver hear that channel without joining, and tell what it heard;
 * probe=, with listen=, has it ask for that network on the channel, and count the answers; see drv.h.
 * antenna=ufl has the XIAO's RF switch pick its U.FL connector rather than the antenna on the board,
 * and antenna=none leaves the pins alone, on a board without that switch.
 * The passphrase lives in the driver's page and memory, never in an image.
 */
static void configure(struct drv *p)
{
    uint32_t base, size, region;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    must("install the input", region_install(&self, BOOT_CAP_INPUT, RIGHT_R, &region));
    const char *conf = (const char *)(uintptr_t)base;
    config_value(conf, size, "ssid", p->ssid, sizeof(p->ssid));
    config_value(conf, size, "pass", p->pass, sizeof(p->pass));
    p->bssid_set = (uint8_t)config_mac(conf, size, "bssid", p->bssid);
    p->debug = (config_has(conf, size, "debug", "frames") ? DRV_DEBUG_FRAMES : 0) |
               (config_has(conf, size, "debug", "wpa") ? DRV_DEBUG_WPA : 0) |
               (config_has(conf, size, "debug", "rekey") ? DRV_DEBUG_REKEY : 0);
    p->lib_log = config_number(conf, size, "lib", 0);
    p->no_pmf = config_number(conf, size, "pmf", 1) == 0;
    p->no_sae = config_number(conf, size, "sae", 1) == 0;
    uint32_t channel = config_number(conf, size, "listen", 0);
    p->listen = (uint8_t)(channel <= 13 ? channel : 0);
    config_value(conf, size, "probe", p->probe, sizeof(p->probe));
    p->trace = config_number(conf, size, "trace", 0); /* 1: the root serves each access; 2: the window with no fault */
    trace_wanted = p->trace != 0;
    trace_serving = p->trace == 1;
    run_s = config_number(conf, size, "run", trace_wanted ? TRACE_RUN_S : RUN_S);
    antenna = config_has(conf, size, "antenna", "ufl")    ? ANTENNA_UFL
              : config_has(conf, size, "antenna", "none") ? ANTENNA_NONE
                                                           : ANTENNA_BOARD;
    snap_parse(conf, size);
    must("uninstall the input", region_free(&self, region));
}

/* The trace's capabilities, which the driver's page needs before it starts: its first thread, and the answer. */
static void trace_give(struct child *c)
{
    struct drv *p = (struct drv *)c->page;
    must("give the driver its first thread", child_give(&self, c, c->thread, RIGHT_W, &p->own_thread));
    must("a slot for the trace's answer", slot_new(&self, &trace_note));
    must("the trace's answer", rv_pool_alloc(self.pool, CAP_NOTIFICATION, trace_note, 0));
    must("give the driver the trace's answer", child_give(&self, c, trace_note, RIGHT_R, &p->trace_note));
}

/*
 * The devices the root serves from, installed after rf_switch has let the IO MUX and the GPIO go again,
 * so the momentary peak of those two does not run out of regions.
 * The eight the process has hold: the code and the data in the first two, the console's, the driver's data,
 * and these three, seven, leaving one; a trace builds no network process nor clients, so self_room is not made.
 */
static void trace_map(void)
{
    for (uint32_t i = 0; i < sizeof(trace_frames) / sizeof(trace_frames[0]); i++) {
        uint32_t base, size, region;
        must("a device frame's place", rv_frame_info(trace_frames[i].cap, &base, &size));
        must(trace_frames[i].name, region_install(&self, trace_frames[i].cap, trace_frames[i].rights, &region));
        trace_dev[trace_dev_count].base = base;
        trace_dev[trace_dev_count].size = size;
        trace_dev_count++;
    }
}

/* Whether a request names an access the root may carry out: a device it maps, a whole width, and aligned. */
static int trace_covered(uint32_t address, uint32_t width)
{
    if (width != 1 && width != 2 && width != 4) {
        return 0;
    }
    if ((address & (width - 1)) != 0) {
        return 0;
    }
    for (uint32_t i = 0; i < trace_dev_count; i++) {
        if (address >= trace_dev[i].base && address - trace_dev[i].base <= trace_dev[i].size - width) {
            return 1;
        }
    }
    return 0;
}

static uint32_t dev_read(uint32_t address, uint32_t width)
{
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)address;
    if (width == 1) {
        return *p;
    }
    if (width == 2) {
        return *(volatile uint16_t *)p;
    }
    return *(volatile uint32_t *)p;
}

static void dev_write(uint32_t address, uint32_t value, uint32_t width)
{
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)address;
    if (width == 1) {
        *p = (uint8_t)value;
    } else if (width == 2) {
        *(volatile uint16_t *)p = (uint16_t)value;
    } else {
        *(volatile uint32_t *)p = value;
    }
}

/* One access out, with its sequence number; tools/mac-trace.py reads this line and the snapshot's, so the
 * format is theirs too, and a change to it is a change to what they parse. */
static void trace_line(const struct trace_rec *e)
{
    say(&cout, "[%u] pc %x t%u %s%u %x = %x%s%s\n", trace_seq++, e->pc, e->thread,
        e->op == TRACE_WRITE ? "W" : "R", e->width, e->address, e->value,
        e->flags & TRACE_FLAG_BATCH ? " batch" : "", e->flags & TRACE_FLAG_CHANGED ? " changed" : "");
}

static void trace_out_record(void *ctx, const struct trace_rec *e)
{
    (void)ctx;
    trace_line(e);
}

/* A cycle's one turn, and how many times it turned, out in place of each turn. */
static void trace_out_cycle(void *ctx, uint32_t turns, uint32_t len, const struct trace_rec *turn)
{
    (void)ctx;
    say(&cout, "[%u] cycle x%u of %u:\n", trace_seq++, turns, len);
    for (uint32_t i = 0; i < len; i++) {
        trace_line(&turn[i]);
    }
}

static const struct trace_sink trace_sink = { trace_out_record, trace_out_cycle, 0 };

/* One access into the stream: the short-cycle compression of trace_cycle_feed does the rest. */
static void trace_stream(const struct trace_req *r)
{
    struct trace_rec e = { .op = r->op, .address = r->address, .width = r->width, .value = r->value,
                           .pc = r->pc, .thread = r->thread, .flags = r->flags };
    trace_records++;
    trace_cycle_feed(&trace_cyc, &e, &trace_sink);
}

static void log_to_console(void);

/* Every request the driver's watcher left, carried out on the root's own mapping and logged. */
static void trace_answer(void)
{
    struct drv *d = (struct drv *)sys.driver.page;
    if (!trace_serving) {
        return;
    }
    int logged = 0;
    while (d->share.asked != d->share.answered) {
        /* The request copied out first, so what the root checks is what it carries out. */
        struct trace_req r = d->share.req;
        if (trace_covered(r.address, r.width)) {
            if (r.op == TRACE_WRITE) {
                dev_write(r.address, r.value, r.width);
            } else {
                r.value = dev_read(r.address, r.width);
            }
            r.status = TRACE_DONE;
            trace_stream(&r);
        } else {
            r.status = TRACE_REFUSED;
            say(&kout, "root: the trace refuses thread %u pc %x %s%u at %x\n", r.thread, r.pc,
                r.op == TRACE_WRITE ? "W" : "R", r.width, r.address);
        }
        d->share.req.value = r.value;
        d->share.req.status = r.status;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->share.answered = d->share.asked;
        rv_signal(trace_note, 0x1);
        logged = 1;
    }
    if (logged) {
        /* A long trace outruns the kernel's log, so what it holds is carried to the console after each serving. */
        log_to_console();
    }
}

/* The stream's tail, the snapshot the configuration asked for, and what it saw: the accesses served, the lines. */
static void trace_dump(void)
{
    trace_cycle_end(&trace_cyc, &trace_sink);
    say(&cout, "[%u] trace over: %u accesses\n", trace_seq++, trace_records);
    if (snap_count != 0) {
        say(&cout, "[%u] snapshot of %u addresses\n", trace_seq++, snap_count);
        for (uint32_t i = 0; i < snap_count; i++) {
            uint32_t address = snap_addrs[i];
            if (trace_covered(address, 4)) {
                say(&cout, "snap %x = %x\n", address, dev_read(address, 4));
            } else {
                /* A stale list, or a frame the root does not map; named so the compare tells it from a value. */
                say(&cout, "snap %x = none\n", address);
            }
        }
    }
    say(&kout, "root: the trace saw %u accesses in %u lines\n", trace_records, trace_seq);
    say(&kout, "root: the trace dropped %u kernel fault lines for %u accesses\n", kline_dropped, trace_records);
}

/*
 * The driver, not started: its image's code, its block of RAM, the ROM, the modem, the SAR ADC,
 * the random number generator, what it builds its threads from, the clock and the modem's lines.
 * The block of RAM is the driver's alone: the root task never installs it.
 */
static void driver_build(void)
{
    struct child *c = &sys.driver;
    uint32_t region, lines;
    must("build the driver", child_new(&self, c, "the driver", DRV_TABLE, DRV_DATA_SIZE));
    struct drv *p = (struct drv *)c->page;
    /* Before the device frames, since the configuration says whether the driver gets them or the root keeps them. */
    configure(p);
    must("the driver's own code", child_code(&self, c, BOOT_CAP_FLASH));
    must("a frame of the driver's RAM", mem_make(&self, &drv_ram, CAP_FRAME));
    must("give the driver its RAM", child_map(&self, c, drv_ram.made, RIGHT_R | RIGHT_W, &region));
    must("give the driver the ROM", child_map(&self, c, BOOT_CAP_ROM, RIGHT_R | RIGHT_X, &region));
    if (p->trace == 1) {
        trace_give(c);
    } else {
        for (uint32_t i = 0; i < sizeof(trace_frames) / sizeof(trace_frames[0]); i++) {
            must(trace_frames[i].name, child_map(&self, c, trace_frames[i].cap, trace_frames[i].rights, &region));
        }
    }
    must("what the driver builds of its own",
         child_give_own(&self, c, DRV_OWN_POOL, DRV_OWN_LINES, DRV_OWN_UNITS, DRV_OWN_SLOTS, &p->own));
    must("give the driver the clock", child_give(&self, c, BOOT_CAP_CLOCK, RIGHT_R, &p->clock));
    must("a slot", slot_new(&self, &lines));
    must("the modem's lines", rv_irq_carve(BOOT_CAP_IRQ_LINES, DRV_LINE_FIRST, DRV_LINES, lines));
    must("give the driver the modem's lines", child_give(&self, c, lines, RIGHT_W, &p->lines));
    must("the lines' slot back", slot_free(&self, lines));
    memcpy(p->mac, mac, 6);
}

static void summary(void)
{
    const struct drv *d = (const struct drv *)sys.driver.page;
    say(&kout, "root: driver frames in %u, out %u, dropped in %u, out %u", d->rx_frames, d->tx_frames, d->rx_dropped,
        d->tx_dropped);
    system_summary(&sys);
}

/* The run's summary, then the halt; system_halt halts without one. */
static __attribute__((noreturn)) void halt(uint32_t code)
{
    summary();
    system_halt(code);
}

/* What the driver's new state says, in the log; its join starts the network process. */
static void driver_state(uint32_t state)
{
    const struct drv *p = (const struct drv *)sys.driver.page;
    if (state == CHILD_FAILED) {
        if (trace_wanted && p->share.cannot) {
            say(&kout, "root: the trace cannot serve thread %u: cause %x, pc %x, address %x\n",
                p->share.giveup.thread, p->share.giveup.cause, p->share.giveup.pc, p->share.giveup.address);
        }
        say(&kout, "root: the driver failed at %s: %d (%x)\n", p->failed, (int)p->c.detail, p->c.detail);
        halt(SYSTEM_FAILED);
    }
    if (state == DRV_UP) {
        say(&kout, "root: the driver's libraries run\n");
    } else if (state == DRV_SCANNED) {
        system_scanned(&sys, p->nets, p->net_count);
        system_halt(0);
    } else if (state == DRV_JOINED) {
        say(&kout, "root: joined %s at %M, channel %u, authentication mode %u\n", p->joined.ssid, p->joined.bssid,
            p->joined.channel, (uint32_t)p->authmode);
        system_net_start(&sys);
    } else if (state == DRV_LISTENING) {
        say(&kout, "root: the driver listens on channel %u\n", (uint32_t)p->listen);
    } else if (state == DRV_LEFT) {
        say(&kout, "root: the driver left the network\n");
        system_halt(0);
    } else if (state == DRV_TRACED) {
        trace_dump();
        system_halt(0);
    }
}

/* Whether the driver serves, joined or listening, and so answers the root task's checks. */
static int driver_serves(void)
{
    return sys.driver.told == DRV_JOINED || sys.driver.told == DRV_LISTENING;
}

/*
 * The run over, its time up or as the host asked: the driver asked to leave the network, if it joined or listens,
 * and the machine halted once it has; a run that never joined halts at once.
 */
static void run_over(void)
{
    summary();
    if (!driver_serves()) {
        system_halt(SYSTEM_LATE);
    }
    if (!leaving) {
        ((struct drv *)sys.driver.page)->leave = 1;
        must("ask the driver to leave", child_tell(&sys.driver));
        leaving = 1;
    }
}

/*
 * A second gone: the children checked and the watchdog fed, system_check,
 * and the run ended if the driver has not joined in time or the run's time is up.
 * The driver answers once it serves the link, its first thread waiting on the libraries' bring-up until then;
 * once asked to leave, its station leaves, and the machine halts at the next second whether it has left or not.
 */
static void second(void)
{
    if (trace_wanted) {
        /* A traced run has no join: only the watchdog, and the run's own end, which the driver does not answer. */
        uint32_t code = system_check(&sys, 0);
        if (code != 0) {
            halt(code);
        }
        seconds++;
        if (seconds == run_s) {
            say(&kout, "root: the trace is over after %u s\n", run_s);
            trace_dump();
            system_halt(0);
        }
        return;
    }
    int serves = driver_serves();
    if (leaving) {
        say(&kout, "root: the driver has not left the network\n");
        system_halt(0);
    }
    uint32_t code = system_check(&sys, serves);
    if (code != 0) {
        halt(code);
    }
    seconds++;
    if (seconds == UP_S && !serves) {
        say(&kout, "root: the driver has not joined after %u s\n", UP_S);
        halt(SYSTEM_LATE);
    }
    if (seconds == run_s) {
        say(&kout, "root: the run is over after %u s\n", run_s);
        run_over();
    }
}

/* The console, and the kernel's log through it as the log fills; and what the host types, a line at a time. */
static void console_open(void)
{
    uint32_t base, size, region;
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));
    must("a bit for the log", bit_new(&self, &log_bit));
    must("arm the log", kernel_log_arm(&klog, log_bit));
    must("a bit for the console", bit_new(&self, &console_bit));
    must("a slot", slot_new(&self, &console_irq));
    must("the console's line", rv_irq_carve(BOOT_CAP_IRQ_LINES, CONSOLE_IRQ, 1, console_irq));
    must("bind the console's line", rv_irq_bind(console_irq, self.pool, self.inbox, console_irq));
    console_listen();
    must("arm the console", rv_irq_set(console_irq, console_bit));
}

/* What the kernel's log holds that the console has not had, out at once. */
static void log_to_console(void)
{
    /* In a trace the kernel's fault report is dropped here, so the host never has to symbolize it. */
    kernel_log_take(&klog, trace_serving ? kline_put : console_put_polled);
    console_flush();
    must("arm the log", kernel_log_arm(&klog, log_bit));
}

/* Whether the line the host typed is word. */
static int typed(const char *word)
{
    return command_len == strlen(word) && memcmp(command, word, command_len) == 0;
}

static void obey(void)
{
    if (typed("end")) {
        say(&kout, "root: the run is over, as the host asked\n");
        run_over();
    } else {
        say(&kout, "root: no command %s; there is end\n", command);
    }
}

/* The bytes the host sent, a command at the end of each line. */
static void heard(void)
{
    char c;
    console_listen();
    while (console_get(&c)) {
        if (c == '\n' || c == '\r') {
            command[command_len] = 0;
            if (command_len != 0) {
                obey();
            }
            command_len = 0;
        } else if (command_len + 1 < sizeof(command)) {
            command[command_len++] = c;
        }
    }
    must("arm the console", rv_irq_set(console_irq, console_bit));
}

int main(void)
{
    uint32_t tick, skipped;
    must("the root's own", self_root(&self, ROOT_UNITS));
    console_open();
    must("a bit for the seconds", bit_new(&self, &tick_bit));
    must("a timer", timer_new(&self, &tick));
    /* The driver's block first, before anything else is taken from free RAM, which might take part of it. */
    must("the driver's RAM", mem_take_at(&self, DRV_RAM_BASE, DRV_RAM_SIZE, &drv_ram));
    read_mac();
    say(&kout, "root: wifi system, MAC address %M\n", mac);
    void (*entry)(struct child_page *) = drv_entry();

    /* The driver's data a block of its own, built before the room is, which is the other children's. */
    driver_build();
    rf_switch();
    if (trace_wanted) {
        /* After rf_switch, which takes the IO MUX and the GPIO for a moment and gives them back;
         * with trace=2 the root serves nothing, but reads the frames back there for the snapshot. */
        trace_map();
    }
    if (!trace_wanted) {
        /* A trace builds no network process nor clients, so it needs no room for their data. */
        must("room for the children's data", self_room(&self, ROOM_SIZE));
    }
    struct drv *dp = (struct drv *)sys.driver.page;
    if (dp->trace) {
        say(&kout, run_s != 0 ? "root: tracing the libraries' bring-up, for %u s\n"
                              : "root: tracing the libraries' bring-up, for good\n",
            run_s);
    } else if (dp->ssid[0]) {
        system_net_build(&sys, &dp->link, mac);
        say(&kout, run_s != 0 ? "root: joining %s, for %u s\n" : "root: joining %s, for good\n", dp->ssid, run_s);
    } else if (dp->listen) {
        say(&kout, run_s != 0 ? "root: listening on channel %u, for %u s\n" : "root: listening on channel %u, for good\n",
            (uint32_t)dp->listen, run_s);
    } else {
        say(&kout, "root: scanning\n");
    }
    if (trace_wanted) {
        /* The root is the only reader of the kernel's log, so a fault of its own would hide the log's tail; the
         * watchdog halts the machine and writes it out. Fed every second by second() as long as the root turns. */
        must("the watchdog", rv_clock_watchdog(BOOT_CAP_CLOCK, 2000000u));
    }
    must("start the driver", child_start(&self, &sys.driver, entry, DRV_UNITS));
    must("count the seconds", rv_timer_period(tick, tick_bit, CHECK_US, &skipped));
    for (;;) {
        uint32_t bits = 0;
        must("wait", rv_wait(self.inbox, &bits));
        uint32_t state, code;
        if (child_poll(&sys.driver, &state)) {
            driver_state(state);
        }
        if ((code = system_heard(&sys, bits)) != 0) {
            halt(code);
        }
        trace_answer();
        if (bits & tick_bit) {
            must("count the seconds", rv_timer_period(tick, tick_bit, CHECK_US, &skipped));
            second();
        }
        if (bits & console_bit) {
            heard();
        }
        if (bits & log_bit) {
            log_to_console();
        }
    }
}
