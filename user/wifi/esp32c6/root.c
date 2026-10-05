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
 *
 * The kernel's log goes to the console as it comes, so that the host reads the run while it lasts,
 * and the halt writes out only what was not taken yet.
 * The host may type a line on the console: "end" ends the run now, as if its time were up,
 * and "stats" has the driver tell the libraries' counters of the radio, once it has joined.
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

/*
 * What to do, from the text the loader may have left in the input region: lines of ssid= and pass=,
 * the network to join, and with no ssid, a scan; bssid=, which of the network's access points to join;
 * and run=, how many seconds the run lasts, 0 for good.
 * debug= names what the driver tells besides its steps, any of frames, stats and wpa, see drv.h,
 * and lib= the libraries' log level, 4 for debug and 5 for verbose;
 * ax=0 joins without 802.11ax, pmf=0 without protecting management frames, and ps=1 sleeps between beacons.
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
               (config_has(conf, size, "debug", "stats") ? DRV_DEBUG_STATS : 0) |
               (config_has(conf, size, "debug", "wpa") ? DRV_DEBUG_WPA : 0);
    p->lib_log = config_number(conf, size, "lib", 0);
    p->no_ax = config_number(conf, size, "ax", 1) == 0;
    p->no_pmf = config_number(conf, size, "pmf", 1) == 0;
    p->modem_sleep = config_number(conf, size, "ps", 0) != 0;
    run_s = config_number(conf, size, "run", RUN_S);
    antenna = config_has(conf, size, "antenna", "ufl")    ? ANTENNA_UFL
              : config_has(conf, size, "antenna", "none") ? ANTENNA_NONE
                                                           : ANTENNA_BOARD;
    must("uninstall the input", region_free(&self, region));
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
    must("the driver's own code", child_code(&self, c, BOOT_CAP_FLASH));
    must("a frame of the driver's RAM", mem_make(&self, &drv_ram, CAP_FRAME));
    must("give the driver its RAM", child_map(&self, c, drv_ram.made, RIGHT_R | RIGHT_W, &region));
    must("give the driver the ROM", child_map(&self, c, BOOT_CAP_ROM, RIGHT_R | RIGHT_X, &region));
    must("give the driver the modem", child_map(&self, c, BOOT_CAP_MODEM, RIGHT_R | RIGHT_W, &region));
    must("give the driver the SAR ADC", child_map(&self, c, BOOT_CAP_SARADC, RIGHT_R | RIGHT_W, &region));
    must("give the driver the random number generator", child_map(&self, c, BOOT_CAP_RNG, RIGHT_R, &region));
    must("what the driver builds of its own",
         child_give_own(&self, c, DRV_OWN_POOL, DRV_OWN_LINES, DRV_OWN_UNITS, DRV_OWN_SLOTS, &p->own));
    must("give the driver the clock", child_give(&self, c, BOOT_CAP_CLOCK, RIGHT_R, &p->clock));
    must("a slot", slot_new(&self, &lines));
    must("the modem's lines", rv_irq_carve(BOOT_CAP_IRQ_LINES, DRV_LINE_FIRST, DRV_LINES, lines));
    must("give the driver the modem's lines", child_give(&self, c, lines, RIGHT_W, &p->lines));
    must("the lines' slot back", slot_free(&self, lines));
    memcpy(p->mac, mac, 6);
    configure(p);
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
        say(&kout, "root: the driver failed at %s: %d (%x)\n", p->failed, (int)p->c.detail, p->c.detail);
        halt(SYSTEM_FAILED);
    }
    if (state == DRV_UP) {
        say(&kout, "root: the driver's libraries run\n");
    } else if (state == DRV_SCANNED) {
        say(&kout, "root: %u access points heard\n", p->net_count);
        for (uint32_t i = 0; i < p->net_count; i++) {
            const struct drv_net *n = &p->nets[i];
            say(&kout, "  %M  ch %u  rssi %d  %s\n", n->bssid, n->channel, n->rssi, n->ssid[0] ? n->ssid : "(hidden)");
        }
        system_halt(0);
    } else if (state == DRV_JOINED) {
        say(&kout, "root: joined %s at %M, channel %u, authentication mode %u\n", p->joined.ssid, p->joined.bssid,
            p->joined.channel, (uint32_t)p->authmode);
        system_net_start(&sys);
    } else if (state == DRV_LEFT) {
        say(&kout, "root: the driver left the network\n");
        system_halt(0);
    }
}

/*
 * The run over, its time up or as the host asked: the driver asked to leave the network, if it joined,
 * and the machine halted once it has; a run that never joined halts at once.
 */
static void run_over(void)
{
    summary();
    if (sys.driver.told != DRV_JOINED) {
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
 * The driver answers once it serves the link: until it joins, its first thread waits on the libraries,
 * and once asked to leave, the libraries again; the machine halts at the next second whether it has left or not.
 */
static void second(void)
{
    int joined = sys.driver.told == DRV_JOINED;
    if (leaving) {
        say(&kout, "root: the driver has not left the network\n");
        system_halt(0);
    }
    uint32_t code = system_check(&sys, joined);
    if (code != 0) {
        halt(code);
    }
    seconds++;
    if (seconds == UP_S && !joined) {
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
    kernel_log_take(&klog, console_put_polled);
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
    struct drv *p = (struct drv *)sys.driver.page;
    if (typed("end")) {
        say(&kout, "root: the run is over, as the host asked\n");
        run_over();
    } else if (typed("stats")) {
        p->stats++;
        must("ask the driver for the counters", child_tell(&sys.driver));
    } else {
        say(&kout, "root: no command %s; there are end and stats\n", command);
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
    must("room for the children's data", self_room(&self, ROOM_SIZE));
    struct drv *dp = (struct drv *)sys.driver.page;
    if (dp->ssid[0]) {
        system_net_build(&sys, &dp->link, mac);
        say(&kout, run_s != 0 ? "root: joining %s, for %u s\n" : "root: joining %s, for good\n", dp->ssid, run_s);
    } else {
        say(&kout, "root: scanning\n");
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
