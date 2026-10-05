/*
 * The root task of the Wi-Fi system on a Pico 2 W; see wifi.h.
 *
 * It finds the firmware blob the loader left at the start of free RAM, lends it to the driver as frames,
 * and builds the driver's process with exactly what its page lists.
 * Once the driver says the chip runs, it takes the blob's memory back,
 * builds the network process and connects the two with the link, through system.h.
 * Once the network process has an address, it builds its clients, each connected to it through its hub,
 * the logger among them, which reads the kernel's log from then on and carries it to a host; until then nobody reads it.
 * Every second it asks each child whether its loop still comes round, and feeds the watchdog;
 * a client that faults, fails or did not answer is taken down and built again, a few times at most.
 * The run lasts as long as the configuration says, by default half a minute, or for good;
 * it ends early if the link is not up in time, the driver or the network process fails, faults or does not answer,
 * or a scan alone was asked for.
 * Then it halts, which writes out what of the kernel's log no host had.
 *
 * user/lib/ keeps what it hands out: its slots, regions, bits, units of time, free memory and room for children's data.
 */

#include "conf.h"
#include "gspi.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"
#include "system.h"
#include "wifi.h"

/* The run's seconds unless the configuration says, by when the link must be up, and the children's checks. */
#define RUN_S    30u
#define UP_S     20u
#define CHECK_US 1000000u

/* The blocks of the devices the driver needs, carved from the boot's frames; see devices.h. */
#define PIO0_SIZE          0x1000u
#define IO_BANK0_HIGH      (IO_BANK0_BASE + 0x80u) /* GPIO16 to GPIO31's status and control */
#define IO_BANK0_HIGH_SIZE 0x80u
#define PADS_BANK0_SIZE    0x1000u

#define PAD(n)         (4u + 4u * (n))
#define PAD_SLEWFAST   (1u << 0)
#define PAD_SCHMITT    (1u << 1)
#define PAD_DRIVE_4MA  (1u << 4)
#define PAD_DRIVE_12MA (3u << 4)
#define PAD_IE         (1u << 6)

/* The driver's table and data: its page at the base, its state and two packets' buffers on its stack. */
#define DRV_TABLE     16u
#define DRV_DATA_SIZE 0x4000u
/*
 * Room for the data of the children built once the blob is back, which the root task sees through one region:
 * the network process's and its clients', each built again in the room the last left.
 * The driver comes first, in the memory the blob leaves, in a region of its own.
 */
#define ROOM_SIZE 0x10000u

/*
 * Who gets which of the first core's units: the root task the first, mostly waiting, the driver the most,
 * and the network process and its clients theirs, system.c's, all 64 between them.
 */
#define ROOT_UNITS 8u
#define DRV_UNITS  32u

static struct self self;
static const struct out kout = { log_write, (void *)BOOT_CAP_DEBUG };
static struct system sys = {
    .self = &self,
    .out = &kout,
    .board = "a Pico 2 W",
    .clients_wanted = CLIENT_BIT(CLIENT_ECHO) | CLIENT_BIT(CLIENT_CLOCK) | CLIENT_BIT(CLIENT_LOGGER),
};
static uint32_t tick_bit, seconds, run_s = RUN_S;

/* The blob's blocks, largest first from its start, and the frames made of them, which taking the blocks back revokes. */
static struct block blob[BLOB_FRAMES];
static uint32_t blob_frames;

static void must(const char *what, uint32_t status)
{
    system_must(&sys, what, status);
}

/*
 * A block of a device's registers, carved from the frame the boot grants over the device, in a region of a child's.
 * The carved frame's slot is not kept: the region then hangs below the boot's frame.
 */
static void give_device(struct child *c, uint32_t granted, uint32_t offset, uint32_t size)
{
    uint32_t frame, region;
    must("a slot", slot_new(&self, &frame));
    must("a device's frame", rv_frame_carve(granted, offset, size, frame));
    must("install a device in a child", child_map(&self, c, frame, RIGHT_R | RIGHT_W, &region));
    must("a device's slot back", slot_free(&self, frame));
}

/* The pads of the Wi-Fi chip's pins: inputs enabled, isolation off, the bus's two lines fast and strong. */
static void pads_set(void)
{
    uint32_t pads, region;
    must("a slot", slot_new(&self, &pads));
    must("the pads' frame", rv_frame_carve(BOOT_CAP_PADS_BANK0, 0, PADS_BANK0_SIZE, pads));
    must("install the pads", region_install(&self, pads, RIGHT_R | RIGHT_W, &region));
    REG32(PADS_BANK0_BASE + PAD(GSPI_PIN_ON)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(GSPI_PIN_CS)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(GSPI_PIN_DATA)) = PAD_IE | PAD_DRIVE_12MA | PAD_SCHMITT | PAD_SLEWFAST;
    REG32(PADS_BANK0_BASE + PAD(GSPI_PIN_CLK)) = PAD_IE | PAD_DRIVE_12MA | PAD_SLEWFAST;
    must("uninstall the pads", region_free(&self, region));
    must("the pads' slot back", slot_free(&self, pads));
}

/*
 * What the driver is to do, from the text the loader may have left in the input region:
 * lines of mode=scan|sta|ap, ssid=, pass= and channel=. With none, it scans.
 * bssid= names which of the network's access points a station joins,
 * and debug=dhcp turns power save off and has the driver tell each DHCP message it hears, whoever it is for.
 * A line run= says how many seconds the run lasts, 0 for good.
 * The passphrase lives in the driver's page and memory, never in an image.
 */
static void configure(struct drv_page *s)
{
    uint32_t base, size, region;
    char value[16];
    s->mode = MODE_SCAN;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    must("install the input", region_install(&self, BOOT_CAP_INPUT, RIGHT_R, &region));
    const char *conf = (const char *)(uintptr_t)base;
    value[0] = '\0';
    config_value(conf, size, "mode", value, sizeof(value));
    if (memcmp(value, "sta", 4) == 0) {
        s->mode = MODE_STA;
    } else if (memcmp(value, "ap", 3) == 0) {
        s->mode = MODE_AP;
    }
    config_value(conf, size, "ssid", s->ssid, sizeof(s->ssid));
    config_value(conf, size, "pass", s->pass, sizeof(s->pass));
    s->channel = config_number(conf, size, "channel", 1);
    s->bssid_set = (uint8_t)config_mac(conf, size, "bssid", s->bssid);
    s->dhcp_log = (uint8_t)config_has(conf, size, "debug", "dhcp");
    run_s = config_number(conf, size, "run", RUN_S);
    must("uninstall the input", region_free(&self, region));
    if (s->mode != MODE_SCAN && s->ssid[0] == '\0') {
        say(&kout, "root: the configuration names no ssid; scanning only\n");
        s->mode = MODE_SCAN;
    }
}

/* Lends the driver the blob, and starts it. */
static void driver_build(uint32_t free_base)
{
    struct child *c = &sys.driver;
    must("build the driver", child_new(&self, c, "the driver", DRV_TABLE, DRV_DATA_SIZE));
    struct drv_page *p = (struct drv_page *)c->page;
    give_device(c, BOOT_CAP_PIO0, 0, PIO0_SIZE);
    give_device(c, BOOT_CAP_IO_BANK0, IO_BANK0_HIGH - IO_BANK0_BASE, IO_BANK0_HIGH_SIZE);
    pads_set();
    for (uint32_t i = 0; i < blob_frames; i++) {
        must("a frame of the blob", mem_make(&self, &blob[i], CAP_FRAME));
        must("lend the driver the blob", child_give(&self, c, blob[i].made, RIGHT_R, &p->blob_slot[i]));
        p->blob_size[i] = blob[i].size;
    }
    must("a window for the blob", child_region(&self, c, &p->window));
    p->pio_base = PIO0_BASE;
    p->pins_base = IO_BANK0_HIGH;
    p->blob_base = free_base;
    p->blob_frames = blob_frames;
    configure(p);
    static const char *const modes[] = { "scan", "station", "access point" };
    if (p->mode == MODE_SCAN) {
        say(&kout, "root: mode scan\n");
    } else {
        say(&kout, run_s != 0 ? "root: mode %s, for %u s\n" : "root: mode %s, for good\n", modes[p->mode], run_s);
    }
    must("start the driver", child_start(&self, c, driver_main, DRV_UNITS));
}

/*
 * Once the chip runs: the blob's memory back, which uninstalls it from the driver's window,
 * and the network process, with the link between it and the driver, which the driver hears of as it runs.
 */
static void net_build(void)
{
    for (uint32_t i = 0; i < blob_frames; i++) {
        must("take the blob back", mem_give(&self, &blob[i]));
    }
    must("room for the children's data", self_room(&self, ROOM_SIZE));
    struct drv_page *dp = (struct drv_page *)sys.driver.page;
    system_net_build(&sys, &dp->link, dp->mac);
    system_net_start(&sys);
}

static void summary(void)
{
    const struct drv_page *d = (const struct drv_page *)sys.driver.page;
    say(&kout, "root: driver frames in %u, out %u, dropped %u", d->rx_frames, d->tx_frames, d->rx_dropped);
    system_summary(&sys);
}

/* The run's summary, then the halt; system_halt halts without one. */
static __attribute__((noreturn)) void halt(uint32_t code)
{
    summary();
    system_halt(code);
}

/* What the driver's new state says, in the log; 1 if the run is to end. */
static int driver_state(uint32_t state)
{
    struct drv_page *p = (struct drv_page *)sys.driver.page;
    if (state == CHILD_FAILED) {
        say(&kout, "root: the driver failed at step %u, read %x\n", p->c.step, p->c.detail);
        halt(SYSTEM_FAILED);
    }
    /* The driver may be past DRV_UP before the root task looks, so any state of a running driver will do. */
    if (state >= DRV_UP && sys.network.page == 0) {
        net_build();
    }
    if (state == DRV_SCANNED) {
        say(&kout, "root: %u access points heard\n", p->net_count);
        for (uint32_t i = 0; i < p->net_count; i++) {
            const struct drv_net *n = &p->nets[i];
            say(&kout, "  %M  ch %u  rssi %d  %s\n", n->bssid, n->channel, n->rssi, n->ssid[0] ? n->ssid : "(hidden)");
        }
        return p->mode == MODE_SCAN;
    }
    if (state == DRV_JOINED) {
        say(&kout, "root: joined %s\n", p->ssid);
    } else if (state == DRV_AP) {
        say(&kout, "root: access point %s on channel %u\n", p->ssid, p->channel);
    }
    return 0;
}

/*
 * A second gone: the children checked and the watchdog fed, system_check,
 * and the run ended if the link is not up in time or the run's time is up.
 */
static void second(void)
{
    uint32_t code = system_check(&sys, 1);
    if (code != 0) {
        halt(code);
    }
    seconds++;
    uint32_t state = sys.driver.page->state;
    int up = state == DRV_JOINED || state == DRV_AP;
    if (seconds == UP_S && !up) {
        say(&kout, "root: the link is not up after %u s\n", UP_S);
        halt(SYSTEM_LATE);
    }
    if (seconds == run_s) {
        /* A link or an access point that lasted the run is what was asked for. */
        say(&kout, "root: the run is over after %u s\n", run_s);
        halt(up ? 0 : SYSTEM_LATE);
    }
}

int main(void)
{
    uint32_t size, tick, skipped;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the kernel's log", rv_frame_info(BOOT_CAP_LOG, &sys.log_base, &size));
    must("a bit for the seconds", bit_new(&self, &tick_bit));
    must("a timer", timer_new(&self, &tick));
    say(&kout, "root: wifi system\n");

    /* The blob, and frames that cover it, largest first from its start. */
    struct blob_header h;
    uint32_t free_base, free_size, made;
    must("free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &free_base, &free_size, &made));
    must("the blob's header", mem_read(&self, free_base, &h, sizeof(h)));
    if (h.magic != BLOB_MAGIC || h.size > free_size) {
        say(&kout, "root: no firmware blob at %x; load one with make BOARD=rp2350 wifi\n", free_base);
        system_halt(SYSTEM_BROKEN);
    }
    uint32_t covered = (h.size + 0xfffu) & ~0xfffu;
    for (uint32_t at = free_base, block = free_size / 2; at < free_base + covered; block /= 2) {
        if (at + block <= free_base + covered || block == 0x1000u) {
            if (blob_frames == BLOB_FRAMES) {
                must("frames enough for the blob", KERR_LIMIT);
            }
            must("a block of the blob", mem_take_at(&self, at, block, &blob[blob_frames++]));
            at += block;
        }
    }
    say(&kout, "root: blob of %u bytes in %u frames\n", h.size, blob_frames);

    driver_build(free_base);

    must("count the seconds", rv_timer_period(tick, tick_bit, CHECK_US, &skipped));
    for (;;) {
        uint32_t bits = 0;
        must("wait", rv_wait(self.inbox, &bits));
        uint32_t state, code;
        if (child_poll(&sys.driver, &state) && driver_state(state)) {
            system_halt(0);
        }
        if ((code = system_heard(&sys, bits)) != 0) {
            halt(code);
        }
        if (bits & tick_bit) {
            must("count the seconds", rv_timer_period(tick, tick_bit, CHECK_US, &skipped));
            second();
        }
    }
}
