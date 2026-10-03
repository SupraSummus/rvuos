/*
 * The root task of the Wi-Fi system on a Pico 2 W; see wifi.h.
 *
 * It finds the firmware blob the loader left at the start of free RAM, lends it to the driver as frames,
 * and builds the driver's process with exactly what its page lists.
 * Once the driver says the chip runs, it takes the blob's memory back,
 * builds the network process and connects the two with the link.
 * Once the network process has an address, it builds its clients, each connected to it through its hub.
 * A client that faults or fails is taken down, its channel closed, and built again once the network process let go of it,
 * a few times at most.
 * Meanwhile it copies the kernel's log to the console, its children's lines among the kernel's,
 * until the run's time is up, the driver or the network process fails or faults, or a scan alone was asked for.
 * Then it halts, which carries the console to the host.
 *
 * user/lib/ keeps what it hands out: its slots, regions, bits, units of time, free memory and room for children's data.
 */

#include "console.h"
#include "gspi.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"
#include "sntp.h"
#include "wifi.h"

/* How long the run lasts before the root task halts; the watchdog reboots the chip at about 17 s. */
#define RUN_US 14000000u

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

/* Each child's table and data: its page at the base, its state and two packets' buffers on its stack. */
#define DRV_TABLE        16u
#define DRV_DATA_SIZE    0x4000u
#define NET_TABLE        16u
#define NET_DATA_SIZE    0x2000u
#define CLIENT_TABLE     8u
#define CLIENT_DATA_SIZE 0x2000u
#define CLIENT_RESTARTS  3u
/*
 * Room for the data of the children built once the blob is back, which the root task sees through one region:
 * the network process's and its clients', each built again in the room the last left.
 * The driver comes first, in the memory the blob leaves, in a region of its own.
 */
#define ROOM_SIZE        0x10000u

/* Who gets which of the first core's units: the root task the first, mostly waiting, the driver the most. */
#define ROOT_UNITS   8u
#define DRV_UNITS    32u
#define NET_UNITS    12u
#define CLIENT_UNITS 6u

static struct self self;
static struct kernel_log klog;
static uint32_t log_bit, run_bit;
static struct child driver, network;
static struct child *const children[] = { &driver, &network };
static struct chan link;
static struct chan_hub hub;

/* The network process's clients, each on the end of the hub of its index. */
struct client {
    struct child child;
    const char *name;
    void (*entry)(struct child_page *page);
    uint32_t restarts;
    int down; /* taken down, to be built again once its end is idle */
};
enum { CLIENT_ECHO, CLIENT_CLOCK, CLIENTS };
static struct client clients[CLIENTS] = {
    [CLIENT_ECHO] = { .name = "the echo", .entry = echo_main },
    [CLIENT_CLOCK] = { .name = "the clock", .entry = clock_main },
};
static int clients_built;

/* The blob's blocks, largest first from its start, and the frames made of them, which taking the blocks back revokes. */
static struct block blob[BLOB_FRAMES];
static uint32_t blob_frames;

static void console_byte(char c)
{
    console_put_polled(c);
}

static const struct out kout = { log_write, (void *)BOOT_CAP_DEBUG };

/* The kernel's log to the console: the kernel's lines, the root task's and its children's, in their order. */
static void drain(void)
{
    kernel_log_take(&klog, console_byte);
}

static __attribute__((noreturn)) void halt(uint32_t code)
{
    drain();
    rv_halt(BOOT_CAP_DEBUG, code);
}

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&kout, "root: %s failed", what);
        if (self.what != 0) {
            say(&kout, " at %s", self.what);
        }
        say(&kout, ": %x\n", status);
        halt(2);
    }
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

/* Copies the value of a "key=value" line of the configuration into out, if the key is there. */
static void config_value(const char *conf, uint32_t len, const char *key, char *out, uint32_t room)
{
    uint32_t klen = strlen(key);
    for (uint32_t i = 0; i < len; i++) {
        if ((i == 0 || conf[i - 1] == '\n') && i + klen < len && memcmp(conf + i, key, klen) == 0 &&
            conf[i + klen] == '=') {
            uint32_t n = 0;
            for (uint32_t j = i + klen + 1; j < len && conf[j] != '\n' && conf[j] != '\0' && n + 1 < room; j++) {
                out[n++] = conf[j];
            }
            out[n] = '\0';
            return;
        }
    }
}

/*
 * What the driver is to do, from the text the loader may have left in the input region:
 * lines of mode=scan|sta|ap, ssid=, pass= and channel=. With none, it scans.
 * The passphrase lives in the driver's page and memory, never in an image.
 */
static void configure(struct drv_page *s)
{
    uint32_t base, size, region;
    char value[16];
    s->mode = MODE_SCAN;
    s->channel = 1;
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
    value[0] = '\0';
    config_value(conf, size, "channel", value, sizeof(value));
    for (uint32_t i = 0, c = 0; value[i] >= '0' && value[i] <= '9'; i++) {
        c = c * 10u + (uint32_t)(value[i] - '0');
        s->channel = c;
    }
    must("uninstall the input", region_free(&self, region));
    if (s->mode != MODE_SCAN && s->ssid[0] == '\0') {
        say(&kout, "root: the configuration names no ssid; scanning only\n");
        s->mode = MODE_SCAN;
    }
}

/* Lends the driver the blob, and starts it. */
static void driver_build(uint32_t free_base)
{
    struct child *c = &driver;
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
    say(&kout, "root: mode %s\n", modes[p->mode]);
    must("start the driver", child_start(&self, c, driver_main, DRV_UNITS));
}

/*
 * Once the chip runs: the blob's memory back, which uninstalls it from the driver's window,
 * the network process, and the link between it and the driver, which the driver hears of as it runs;
 * and the network process's hub for its clients, and its own inbox to wake itself with.
 */
static void net_build(void)
{
    for (uint32_t i = 0; i < blob_frames; i++) {
        must("take the blob back", mem_give(&self, &blob[i]));
    }
    must("room for the children's data", self_room(&self, ROOM_SIZE));
    struct drv_page *dp = (struct drv_page *)driver.page;
    must("build the network process", child_new(&self, &network, "the network", NET_TABLE, NET_DATA_SIZE));
    struct net_page *np = (struct net_page *)network.page;
    memcpy(np->mac, dp->mac, 6);
    must("the link", chan_new(&self, &link, LINK_SIZE, LINK_SLOT));
    must("connect the driver and the network", chan_connect(&self, &link, &driver, &dp->link, &network, &np->link));
    must("the clients' hub",
         chan_hub_new(&self, &hub, &network, np->clients, SOCK_CLIENTS, CLIENT_CHAN_SIZE, CLIENT_SLOT));
    must("a bit to wake itself", child_bit(&self, &network, &np->more_bit));
    must("its inbox to wake itself", child_give_bits(&self, &network, network.inbox, np->more_bit, &np->more));
    must("start the network process", child_start(&self, &network, net_main, NET_UNITS));
}

/* A client of the network process, on end i of the hub, which is idle. */
static void client_build(uint32_t i)
{
    struct client *cl = &clients[i];
    struct child *c = &cl->child;
    must("build a client", child_new(&self, c, cl->name, CLIENT_TABLE, CLIENT_DATA_SIZE));
    must("connect a client", chan_hub_connect(&self, &hub, i, c, &((struct client_page *)c->page)->net));
    must("start a client", child_start(&self, c, cl->entry, CLIENT_UNITS));
    cl->down = 0;
}

/* A client that faulted or failed: its channel closed, taken down, and built again later, a few times at most. */
static void client_down(uint32_t i)
{
    struct client *cl = &clients[i];
    must("close a client's channel", chan_hub_close(&self, &hub, i));
    must("take a client down", child_free(&self, &cl->child));
    cl->down = ++cl->restarts <= CLIENT_RESTARTS;
    say(&kout, "root: %s %s\n", cl->name, cl->down ? "is taken down, to be built again" : "is taken down for good");
}

/* What a client's new state says, on the console. */
static void client_state(uint32_t i, uint32_t state)
{
    struct client *cl = &clients[i];
    if (state == CHILD_FAILED) {
        say(&kout, "root: %s failed at step %u, read %x\n", cl->name, cl->child.page->step, cl->child.page->detail);
        client_down(i);
    } else if (i == CLIENT_ECHO && state == ECHO_SERVING) {
        say(&kout, "root: %s answers on UDP port %u\n", cl->name, ECHO_PORT);
    } else if (i == CLIENT_CLOCK && state == CLOCK_SET) {
        const struct clock_page *p = (const struct clock_page *)cl->child.page;
        char text[UTC_TEXT];
        utc_format(text, p->seconds);
        say(&kout, "root: %s says it is %s, by %I, and tells it on UDP port %u\n", cl->name, text, p->server,
            CLOCK_PORT);
    }
}

/* One wake's news of the clients: faults, states, and ends idle again for those to be built again. */
static void clients_heard(uint32_t bits)
{
    for (uint32_t i = 0; i < CLIENTS; i++) {
        struct client *cl = &clients[i];
        struct child *c = &cl->child;
        uint32_t state;
        if (c->page == 0) {
            if (cl->down && chan_hub_idle(&hub, i)) {
                client_build(i);
            }
            continue;
        }
        if (bits & c->bit_fault) {
            uint32_t cause, pc, addr, status;
            rv_thread_fault(c->thread, &cause, &pc, &addr, &status);
            say(&kout, "root: %s faulted, cause %x at %x, address %x\n", cl->name, cause, pc, addr);
            client_down(i);
        } else if (child_poll(c, &state)) {
            client_state(i, state);
        }
    }
}

/* What a child's new state says, on the console; 1 if the run is to end. */
static int child_state(struct child *c, uint32_t state)
{
    if (state == CHILD_FAILED) {
        say(&kout, "root: %s failed at step %u, read %x\n", c->name, c->page->step, c->page->detail);
        halt(1);
    }
    if (c == &network) {
        if (state == NET_BOUND && !clients_built) {
            clients_built = 1;
            say(&kout, "root: the system answers at %I: ping it, or send a datagram to UDP port %u for its status\n",
                ((struct net_page *)c->page)->ip, NET_PORT_STATUS);
            for (uint32_t i = 0; i < CLIENTS; i++) {
                client_build(i);
            }
        }
        return 0;
    }
    struct drv_page *p = (struct drv_page *)c->page;
    /* The driver may be past DRV_UP before the root task looks, so any state of a running driver will do. */
    if (state >= DRV_UP && network.page == 0) {
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

static void summary(void)
{
    const struct drv_page *d = (const struct drv_page *)driver.page;
    const struct net_page *n = (const struct net_page *)network.page;
    say(&kout, "root: driver frames in %u, out %u, dropped %u", d->rx_frames, d->tx_frames, d->rx_dropped);
    if (n != 0) {
        say(&kout, "; network frames in %u, out %u, pings %u, datagrams %u", n->rx_frames, n->tx_frames, n->pings,
            n->datagrams);
    }
    const struct echo_page *e = (const struct echo_page *)clients[CLIENT_ECHO].child.page;
    if (e != 0) {
        say(&kout, "; echoed %u", e->echoed);
    }
    say(&kout, "; %u bytes and %u slots unused\n", mem_unused(&self), slots_unused(&self));
}

int main(void)
{
    uint32_t base, size, region, run;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));
    must("a bit for the log", bit_new(&self, &log_bit));
    must("a bit for the run's end", bit_new(&self, &run_bit));
    must("a timer", timer_new(&self, &run));
    say(&kout, "root: wifi system\n");

    /* The blob, and frames that cover it, largest first from its start. */
    struct blob_header h;
    uint32_t free_base, free_size, made;
    must("free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &free_base, &free_size, &made));
    must("the blob's header", mem_read(&self, free_base, &h, sizeof(h)));
    if (h.magic != BLOB_MAGIC || h.size > free_size) {
        say(&kout, "root: no firmware blob at %x; load one with make BOARD=rp2350 wifi\n", free_base);
        halt(2);
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

    must("arm the run's end", rv_timer_set(run, run_bit, RUN_US));
    for (;;) {
        uint32_t bits = 0;
        must("arm the log", kernel_log_arm(&klog, log_bit));
        must("wait", rv_wait(self.inbox, &bits));
        drain();
        for (uint32_t i = 0; i < sizeof(children) / sizeof(children[0]); i++) {
            struct child *c = children[i];
            uint32_t state;
            if (c->page == 0) {
                continue;
            }
            if (bits & c->bit_fault) {
                uint32_t cause, pc, addr, status;
                rv_thread_fault(c->thread, &cause, &pc, &addr, &status);
                say(&kout, "root: %s faulted, cause %x at %x, address %x\n", c->name, cause, pc, addr);
                halt(3);
            }
            if (child_poll(c, &state) && child_state(c, state)) {
                halt(0);
            }
        }
        clients_heard(bits);
        if (bits & run_bit) {
            /* A link or an access point that lasted the run is what was asked for. */
            uint32_t state = driver.page->state;
            summary();
            if (state == DRV_JOINED || state == DRV_AP) {
                say(&kout, "root: the run is over\n");
                halt(0);
            }
            say(&kout, "root: out of time\n");
            halt(4);
        }
    }
}
