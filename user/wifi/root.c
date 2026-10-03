/*
 * The root task of the Wi-Fi system on a Pico 2 W; see wifi.h.
 *
 * It finds the firmware blob the loader left at the start of free RAM, lends it to the driver as frames,
 * and builds the driver's process with exactly what wifi.h lists.
 * Once the driver says the chip runs, it takes the blob's memory back,
 * builds the network process and the link between the two, and hands the driver the link.
 * Meanwhile it copies the kernel's log and its children's to the console,
 * until the run's time is up, a child fails or faults, or a scan alone was asked for.
 * Then it halts, which carries the console to the host.
 *
 * Memory comes out of BOOT_CAP_FREE_RAM by halving: a block is taken from the free one that holds it,
 * whose other halves stay free, each an Untyped in a slot of its own.
 */

#include "console.h"
#include "lib.h"
#include "ring.h"
#include "rvuos.h"
#include "wifi.h"

/* The root task's regions. */
enum {
    ROOT_REGION_CODE,
    ROOT_REGION_DATA,
    ROOT_REGION_CONSOLE,
    ROOT_REGION_LOG,
    ROOT_REGION_PEEK,   /* a frame looked at for a moment: the blob's header, the pads, the input, the link */
    ROOT_REGION_PAGES,  /* a child's page, one region each */
};

/* How long the run lasts before the root task halts; the watchdog reboots the chip at about 17 s. */
#define RUN_US 14000000u

/* The blocks of the devices the driver needs, carved from the boot's frames; see devices.h. */
#define PIO0_SIZE       0x1000u
#define IO_BANK0_HIGH   (IO_BANK0_BASE + 0x80u) /* GPIO16 to GPIO31's status and control */
#define IO_BANK0_HIGH_SIZE 0x80u
#define PADS_BANK0_SIZE 0x1000u

#define PAD(n)       (4u + 4u * (n))
#define PAD_SLEWFAST (1u << 0)
#define PAD_SCHMITT  (1u << 1)
#define PAD_DRIVE_4MA  (1u << 4)
#define PAD_DRIVE_12MA (3u << 4)
#define PAD_IE       (1u << 6)

#define PAGE_SIZE 0x1000u
#define POOL_SIZE 0x1000u
#define DRV_DATA_SIZE 0x4000u /* its state, two packets' buffers among it, and its stack */
#define NET_DATA_SIZE 0x2000u

/* Who gets which of the first core's units: the root task the first, mostly waiting, the driver the most. */
#define ROOT_UNITS   16u
#define DRV_UNITS    32u
#define NET_UNITS    16u

/* The root task's own slots, from BOOT_CAP_COUNT up; counted from zero, since a program has no initialised data. */
static uint32_t slots_used;
static uint32_t inbox;

static void kput(void *to, char c)
{
    (void)to;
    rv_putc(BOOT_CAP_DEBUG, c);
}

static const struct out kout = { kput, 0 };

static const char *const child_name[CHILDREN] = { "the driver", "the network" };

/* A child of the root task, as child_new builds it; see wifi.h for what every child starts with. */
struct child {
    uint32_t index;
    uint32_t pool, table, process, thread, inbox, timer; /* the root task's slots */
    uint32_t data_base, data_size, page_frame;
    struct child_page *page;
    uint32_t log_taken, told;
};
static struct child children[CHILDREN];

/* The kernel's log, and how far it has been copied to the console. */
static volatile struct rvuos_log *log_header;
static const volatile uint8_t *log_ring;
static uint32_t log_taken;

static void console_byte(char c)
{
    console_put_polled(c);
}

static void drain(void)
{
    uint32_t head = log_header->head;
    uint32_t size = log_header->size;
    if (head - log_taken > size) {
        log_taken = head - size;
    }
    for (; log_taken != head; log_taken++) {
        console_put_polled((char)log_ring[log_taken % size]);
    }
    log_header->taken = log_taken;
    for (uint32_t i = 0; i < CHILDREN; i++) {
        if (children[i].page != 0) {
            children[i].log_taken = plog_take(&children[i].page->log, children[i].log_taken, console_byte);
        }
    }
}

static __attribute__((noreturn)) void halt(uint32_t code)
{
    drain();
    rv_halt(BOOT_CAP_DEBUG, code);
}

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&kout, "root: %s failed: %x\n", what, status);
        halt(2);
    }
}

/*
 * The table has ROOT_TABLE_SLOTS slots, and halving memory takes two at each step,
 * so slots come back: those a call consumed or a revoke emptied, and those deleted.
 */
static uint32_t spare_slots[24], spare_count;

static uint32_t new_slot(void)
{
    if (spare_count > 0) {
        return spare_slots[--spare_count];
    }
    if (BOOT_CAP_COUNT + slots_used >= ROOT_TABLE_SLOTS) {
        must("a free slot", KERR_LIMIT);
    }
    return BOOT_CAP_COUNT + slots_used++;
}

/* A slot back, deleting what it holds; what hung below it goes to its parent. */
static void give_slot(uint32_t slot)
{
    must("delete a slot", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, slot, 0, 0));
    if (spare_count < sizeof(spare_slots) / sizeof(spare_slots[0])) {
        spare_slots[spare_count++] = slot;
    }
}

/* Free memory: Untypeds, each a block, none overlapping. */
struct block {
    uint32_t slot, base, size;
};
static struct block free_blocks[32];
static uint32_t free_count;

static void free_add(struct block b)
{
    if (free_count == sizeof(free_blocks) / sizeof(free_blocks[0])) {
        must("room for a free block", KERR_LIMIT);
    }
    free_blocks[free_count++] = b;
}

/* An Untyped of exactly the block at base, halved out of the free block that holds it. */
static uint32_t take_at(uint32_t base, uint32_t size)
{
    for (uint32_t i = 0; i < free_count; i++) {
        struct block b = free_blocks[i];
        if (b.base <= base && base + size <= b.base + b.size) {
            free_blocks[i] = free_blocks[--free_count];
            while (b.size > size) {
                uint32_t lower = new_slot(), upper = new_slot();
                must("split free memory", rv_split(b.slot, lower, upper));
                /* The halves hang below what was split; with it deleted they hang a step higher, and its slot is free. */
                if (b.slot != BOOT_CAP_FREE_RAM) {
                    give_slot(b.slot);
                }
                uint32_t half = b.size / 2;
                struct block lo = { lower, b.base, half }, hi = { upper, b.base + half, half };
                if (base < hi.base) {
                    free_add(hi);
                    b = lo;
                } else {
                    free_add(lo);
                    b = hi;
                }
            }
            return b.slot;
        }
    }
    must("free memory for a block", KERR_NO_MEMORY);
    return 0;
}

/* An Untyped of size, from the smallest free block that has room. */
static uint32_t take(uint32_t size, uint32_t *base)
{
    uint32_t best = free_count;
    for (uint32_t i = 0; i < free_count; i++) {
        if (free_blocks[i].size >= size && (best == free_count || free_blocks[i].size < free_blocks[best].size)) {
            best = i;
        }
    }
    if (best == free_count) {
        must("free memory", KERR_NO_MEMORY);
    }
    *base = free_blocks[best].base;
    return take_at(*base, size);
}

static uint32_t frame_of(uint32_t untyped)
{
    uint32_t frame = new_slot(), base;
    must("make a frame", rv_retype(untyped, CAP_FRAME, frame, &base));
    return frame;
}

/* A block of a device's registers, carved from the frame the boot grants over the device. */
static uint32_t device(uint32_t granted, uint32_t offset, uint32_t size)
{
    uint32_t frame = new_slot();
    must("a device's frame", rv_invoke(OP_FRAME_CARVE, granted, offset, size, frame));
    return frame;
}

static void peek(uint32_t frame, uint32_t rights)
{
    must("install a frame to look at", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, frame, rights));
}

static void unpeek(void)
{
    must("uninstall the frame looked at", rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, 0, 0));
}

/*
 * A child: its data, its page, a pool for its objects, its table, process, thread and inbox, and a timer line,
 * with the regions and slots every child starts with; the page is the root task's to fill before child_start.
 */
static void child_new(struct child *c, uint32_t index, uint32_t table_slots, uint32_t data_size)
{
    uint32_t page_base, pool_base, line = new_slot();
    c->index = index;
    c->data_size = data_size;
    uint32_t data = frame_of(take(data_size, &c->data_base));
    c->page_frame = frame_of(take(PAGE_SIZE, &page_base));
    c->pool = new_slot();
    must("a child's pool", rv_retype(take(POOL_SIZE, &pool_base), CAP_POOL, c->pool, &pool_base));
    c->table = new_slot();
    c->process = new_slot();
    c->thread = new_slot();
    c->inbox = new_slot();
    c->timer = new_slot();
    must("a child's table", rv_invoke(OP_POOL_ALLOC, c->pool, CAP_CAPTABLE, c->table, table_slots));
    must("a child's process", rv_invoke(OP_POOL_ALLOC, c->pool, CAP_PROCESS, c->process, c->table));
    must("a child's thread", rv_invoke(OP_POOL_ALLOC, c->pool, CAP_THREAD, c->thread, c->process));
    must("a child's inbox", rv_invoke(OP_POOL_ALLOC, c->pool, CAP_NOTIFICATION, c->inbox, 0));
    must("a child's timer line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 1u + index, 1, line));
    must("bind a child's timer", rv_invoke(OP_IRQ_BIND, line, c->pool, c->inbox, c->timer));
    give_slot(line); /* the bind consumed it */

    must("install a child's code", rv_invoke(OP_PROCESS_INSTALL, c->process, CHILD_REGION_CODE, BOOT_CAP_CODE, RIGHT_R | RIGHT_X));
    must("install a child's data", rv_invoke(OP_PROCESS_INSTALL, c->process, CHILD_REGION_DATA, data, RIGHT_R | RIGHT_W));
    must("install a child's page", rv_invoke(OP_PROCESS_INSTALL, c->process, CHILD_REGION_PAGE, c->page_frame, RIGHT_R | RIGHT_W));
    must("give a child its inbox", rv_invoke(OP_CAP_COPY, c->table, CHILD_INBOX, c->inbox, RIGHT_R));
    must("give a child the root's inbox", rv_invoke(OP_CAP_COPY, c->table, CHILD_ROOT, inbox, RIGHT_W));
    must("give a child its timer", rv_invoke(OP_CAP_COPY, c->table, CHILD_TIMER, c->timer, RIGHT_W));
    must("give a child its process", rv_invoke(OP_CAP_COPY, c->table, CHILD_SELF, c->process, RIGHT_W));

    must("install a child's page here", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PAGES + index,
                                                  c->page_frame, RIGHT_R | RIGHT_W));
    memset((void *)page_base, 0, PAGE_SIZE);
    c->page = (struct child_page *)page_base;
    c->page->data_base = c->data_base;
    c->page->data_size = data_size;
    c->page->state = CHILD_STARTING;
}

static void child_give(struct child *c, uint32_t child_slot, uint32_t root_slot, uint32_t rights)
{
    must("give a child a capability", rv_invoke(OP_CAP_COPY, c->table, child_slot, root_slot, rights));
}

static void child_map(struct child *c, uint32_t region, uint32_t frame, uint32_t rights)
{
    must("install a frame in a child", rv_invoke(OP_PROCESS_INSTALL, c->process, region, frame, rights));
}

/* Starts a child at entry, with a0 at its page, its stack at the top of its data, on units of the first core. */
static void child_start(struct child *c, void (*entry)(void *), uint32_t first_unit, uint32_t units)
{
    uint32_t time = new_slot();
    must("carve a child's time", rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, first_unit, units, time));
    must("watch a child", rv_invoke(OP_THREAD_WATCH, c->thread, inbox, ROOT_BIT_FAULT(c->index), 0));
    must("configure a child", rv_invoke(OP_THREAD_CONFIGURE, c->thread, (uint32_t)entry, c->data_base + c->data_size, 0));
    must("hand a child its page", rv_invoke(OP_THREAD_WRITE_REG, c->thread, RV_REG_A0, (uint32_t)c->page, 0));
    must("bind a child", rv_invoke(OP_TIME_BIND, time, c->thread, 0, units));
    must("start a child", rv_invoke(OP_THREAD_RESUME, c->thread, 0, 0, 0));
}

/* The pads of the Wi-Fi chip's pins: inputs enabled, isolation off, the bus's two lines fast and strong. */
static void pads_set(void)
{
    uint32_t pads = device(BOOT_CAP_PADS_BANK0, 0, PADS_BANK0_SIZE);
    peek(pads, RIGHT_R | RIGHT_W);
    REG32(PADS_BANK0_BASE + PAD(23)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(25)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(24)) = PAD_IE | PAD_DRIVE_12MA | PAD_SCHMITT | PAD_SLEWFAST;
    REG32(PADS_BANK0_BASE + PAD(29)) = PAD_IE | PAD_DRIVE_12MA | PAD_SLEWFAST;
    unpeek();
    give_slot(pads);
}

/*
 * The blob's header, read through a frame made of all free RAM for the moment;
 * the revoke after it leaves the free RAM as it was.
 */
static struct blob_header blob_header(uint32_t free_base)
{
    uint32_t all = frame_of(BOOT_CAP_FREE_RAM), head = new_slot(), min;
    must("the smallest region", rv_frame_min_size(all, &min));
    must("carve the blob's header", rv_invoke(OP_FRAME_CARVE, all, 0, 64u < min ? min : 64u, head));
    peek(head, RIGHT_R);
    struct blob_header h = *(const struct blob_header *)free_base;
    unpeek();
    must("give the free RAM back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, BOOT_CAP_FREE_RAM, 0, 0));
    give_slot(head);
    give_slot(all);
    return h;
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
    uint32_t base, size;
    char value[16];
    s->mode = MODE_SCAN;
    s->channel = 1;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    peek(BOOT_CAP_INPUT, RIGHT_R);
    const char *conf = (const char *)base;
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
    unpeek();
    if (s->mode != MODE_SCAN && s->ssid[0] == '\0') {
        say(&kout, "root: the configuration names no ssid; scanning only\n");
        s->mode = MODE_SCAN;
    }
}

void driver_main(struct drv_page *s);
void net_main(struct net_page *s);

/* The blob's frames, and the Untypeds they were made of, which revoking gives back. */
static uint32_t blob_untyped[DRV_SLOTS - DRV_FIRMWARE], blob_base[DRV_SLOTS - DRV_FIRMWARE];
static uint32_t blob_size[DRV_SLOTS - DRV_FIRMWARE], blob_frame[DRV_SLOTS - DRV_FIRMWARE], blob_frames;

/* Lends the driver the blob, and starts it. */
static void driver_build(uint32_t free_base)
{
    struct child *c = &children[CHILD_DRIVER];
    child_new(c, CHILD_DRIVER, DRV_SLOTS, DRV_DATA_SIZE);
    struct drv_page *p = (struct drv_page *)c->page;
    child_map(c, DRV_REGION_PIO, device(BOOT_CAP_PIO0, 0, PIO0_SIZE), RIGHT_R | RIGHT_W);
    child_map(c, DRV_REGION_PINS, device(BOOT_CAP_IO_BANK0, IO_BANK0_HIGH - IO_BANK0_BASE, IO_BANK0_HIGH_SIZE),
              RIGHT_R | RIGHT_W);
    pads_set();
    for (uint32_t i = 0; i < blob_frames; i++) {
        blob_frame[i] = frame_of(blob_untyped[i]);
        child_give(c, DRV_FIRMWARE + i, blob_frame[i], RIGHT_R);
        p->blob_frame_size[i] = blob_size[i];
    }
    p->pio_base = PIO0_BASE;
    p->pins_base = IO_BANK0_HIGH;
    p->blob_base = free_base;
    p->blob_frames = blob_frames;
    configure(p);
    static const char *const modes[] = { "scan", "station", "access point" };
    say(&kout, "root: mode %s\n", modes[p->mode]);
    child_start(c, (void (*)(void *))driver_main, ROOT_UNITS, DRV_UNITS);
}

/*
 * Once the chip runs: the blob's memory back, the network process, and the link between it and the driver,
 * which the driver learns of from its page and a signal, since it runs already.
 */
static void net_build(void)
{
    struct child *d = &children[CHILD_DRIVER], *c = &children[CHILD_NET];
    for (uint32_t i = 0; i < blob_frames; i++) {
        must("take the blob back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, blob_untyped[i], 0, 0));
        free_add((struct block){ blob_untyped[i], blob_base[i], blob_size[i] });
        give_slot(blob_frame[i]); /* the revoke emptied it */
    }

    child_new(c, CHILD_NET, NET_SLOTS, NET_DATA_SIZE);
    uint32_t link_base;
    uint32_t link = frame_of(take(LINK_SIZE, &link_base));
    peek(link, RIGHT_R | RIGHT_W);
    ring_init(LINK_RX(link_base), LINK_SIZE / 2u, LINK_SLOT);
    ring_init(LINK_TX(link_base), LINK_SIZE / 2u, LINK_SLOT);
    unpeek();
    child_map(c, NET_REGION_LINK, link, RIGHT_R | RIGHT_W);
    child_map(d, DRV_REGION_LINK, link, RIGHT_R | RIGHT_W);
    child_give(c, NET_DRIVER, d->inbox, RIGHT_W);
    child_give(d, DRV_NET, c->inbox, RIGHT_W);

    struct net_page *np = (struct net_page *)c->page;
    np->link_base = link_base;
    memcpy(np->mac, ((struct drv_page *)d->page)->mac, 6);
    child_start(c, (void (*)(void *))net_main, ROOT_UNITS + DRV_UNITS, NET_UNITS);

    ((struct drv_page *)d->page)->link_base = link_base;
    must("tell the driver", rv_signal(d->inbox, CHILD_BIT_ROOT));
}

/* What a child's new state says, on the console; 1 if the run is to end. */
static int child_state(struct child *c, uint32_t state)
{
    if (state == CHILD_FAILED) {
        say(&kout, "root: %s failed at step %u, read %x\n", child_name[c->index], c->page->step, c->page->detail);
        halt(1);
    }
    if (c->index == CHILD_NET) {
        if (state == NET_BOUND) {
            say(&kout, "root: the system answers at %I: ping it, or send a datagram to UDP port %u for its status\n",
                ((struct net_page *)c->page)->ip, NET_PORT_STATUS);
        }
        return 0;
    }
    struct drv_page *p = (struct drv_page *)c->page;
    /* The driver may be past DRV_UP before the root task looks, so any state of a running driver will do. */
    if (state >= DRV_UP && children[CHILD_NET].page == 0) {
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
    const struct drv_page *d = (const struct drv_page *)children[CHILD_DRIVER].page;
    const struct net_page *n = (const struct net_page *)children[CHILD_NET].page;
    say(&kout, "root: driver frames in %u, out %u, dropped %u", d->rx_frames, d->tx_frames, d->rx_dropped);
    if (n != 0) {
        say(&kout, "; network frames in %u, out %u, pings %u, datagrams %u", n->rx_frames, n->tx_frames, n->pings,
            n->datagrams);
    }
    say(&kout, "\n");
}

int main(void)
{
    uint32_t base, size, made;

    /* The console and the log. */
    must("console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_CONSOLE, BOOT_CAP_UART, RIGHT_R | RIGHT_W));
    console_init(base);
    must("log", rv_frame_info(BOOT_CAP_LOG, &base, &size));
    must("install the log", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_LOG, BOOT_CAP_LOG, RIGHT_R | RIGHT_W));
    log_header = (volatile struct rvuos_log *)base;
    log_ring = (const volatile uint8_t *)(base + RVUOS_LOG_HEADER);

    uint32_t line = new_slot(), log_irq = new_slot(), tline = new_slot(), timer = new_slot();
    inbox = new_slot();
    must("the root's inbox", rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, inbox, 0));
    must("the log's line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, LOG_IRQ_LINE, 1, line));
    must("bind the log", rv_invoke(OP_IRQ_BIND, line, BOOT_CAP_POOL, inbox, log_irq));
    must("a timer line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, tline));
    must("bind the timer", rv_invoke(OP_IRQ_BIND, tline, BOOT_CAP_POOL, inbox, timer));
    give_slot(line);
    give_slot(tline);
    must("keep the root's units", rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, ROOT_UNITS));
    say(&kout, "root: wifi system\n");

    /* The blob, and frames that cover it, largest first from its start. */
    must("free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &base, &size, &made));
    uint32_t free_base = base;
    struct blob_header h = blob_header(free_base);
    if (h.magic != BLOB_MAGIC || h.size > size) {
        say(&kout, "root: no firmware blob at %x; load one with make BOARD=rp2350 wifi\n", free_base);
        halt(2);
    }
    free_add((struct block){ BOOT_CAP_FREE_RAM, base, size });
    uint32_t covered = (h.size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint32_t at = free_base, block = size / 2; at < free_base + covered; block /= 2) {
        if (at + block <= free_base + covered || block == PAGE_SIZE) {
            if (blob_frames == DRV_SLOTS - DRV_FIRMWARE) {
                must("frames enough for the blob", KERR_LIMIT);
            }
            blob_untyped[blob_frames] = take_at(at, block);
            blob_base[blob_frames] = at;
            blob_size[blob_frames++] = block;
            at += block;
        }
    }
    say(&kout, "root: blob of %u bytes in %u frames\n", h.size, blob_frames);

    driver_build(free_base);

    must("arm the run's end", rv_timer_set(timer, ROOT_BIT_TIMER, RUN_US));
    for (;;) {
        uint32_t bits = 0;
        must("arm the log", rv_irq_set(log_irq, ROOT_BIT_LOG));
        must("wait", rv_wait(inbox, &bits));
        drain();
        for (uint32_t i = 0; i < CHILDREN; i++) {
            struct child *c = &children[i];
            if (c->page == 0) {
                continue;
            }
            if (bits & ROOT_BIT_FAULT(i)) {
                uint32_t cause, pc, addr, status;
                rv_thread_fault(c->thread, &cause, &pc, &addr, &status);
                say(&kout, "root: %s faulted, cause %x at %x, address %x\n", child_name[i], cause, pc, addr);
                halt(3);
            }
            uint32_t state = c->page->state;
            if (state != c->told) {
                c->told = state;
                if (child_state(c, state)) {
                    halt(0);
                }
            }
        }
        if (bits & ROOT_BIT_TIMER) {
            /* A link or an access point that lasted the run is what was asked for. */
            uint32_t state = children[CHILD_DRIVER].page->state;
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
