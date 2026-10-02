/*
 * The root task of the Wi-Fi system on a Pico 2 W; see wifi.h.
 *
 * It finds the firmware blob the loader left at the start of free RAM,
 * lends it to the driver as frames, builds the driver's process with exactly what wifi.h lists,
 * and copies the kernel's log and the driver's text to the console until the driver reports or faults.
 * Then it halts, which carries the console to the host.
 *
 * Memory comes out of BOOT_CAP_FREE_RAM by halving: a block is taken from the free one that holds it,
 * whose other halves stay free, each an Untyped in a slot of its own.
 */

#include "console.h"
#include "lib.h"
#include "rvuos.h"
#include "wifi.h"

/* The root task's regions. */
enum {
    ROOT_REGION_CODE,
    ROOT_REGION_DATA,
    ROOT_REGION_CONSOLE,
    ROOT_REGION_LOG,
    ROOT_REGION_PEEK,   /* a frame looked at for a moment: the blob's header, the pads */
    ROOT_REGION_SHARED, /* the page shared with the driver */
};

/* How long the run lasts before the root task gives up; the watchdog reboots the chip at about 17 s. */
#define RUN_US 13000000u

/* The devices the driver needs, through OP_DEBUG_FRAME; see the board's DEBUG_RANGE_LIST. */
#define PIO0_BASE       0x50200000u
#define PIO0_SIZE       0x1000u
#define IO_BANK0_HIGH   0x40028080u /* GPIO16 to GPIO31's status and control */
#define IO_BANK0_HIGH_SIZE 0x80u
#define PADS_BANK0_BASE 0x40038000u
#define PADS_BANK0_SIZE 0x1000u

#define PAD(n)       (4u + 4u * (n))
#define PAD_SLEWFAST (1u << 0)
#define PAD_SCHMITT  (1u << 1)
#define PAD_PDE      (1u << 2)
#define PAD_DRIVE_4MA  (1u << 4)
#define PAD_DRIVE_12MA (3u << 4)
#define PAD_IE       (1u << 6)

#define DRV_DATA_SIZE   0x4000u
#define DRV_POOL_SIZE   0x1000u
#define DRV_SHARED_SIZE 0x1000u

/* The root task's own slots, from BOOT_CAP_COUNT up; counted from zero, since a program has no initialised data. */
static uint32_t slots_used;

static void kput(void *to, char c)
{
    (void)to;
    rv_putc(BOOT_CAP_DEBUG, c);
}

static const struct out kout = { kput, 0 };

/* The kernel's log and the driver's ring, and how far each has been copied to the console. */
static volatile struct rvuos_log *log_header;
static const volatile uint8_t *log_ring;
static uint32_t log_taken;
static struct drv_shared *shared;
static uint32_t drv_taken;

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
    if (shared != 0) {
        uint32_t dhead = shared->log_head;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (dhead - drv_taken > DRV_LOG_SIZE) {
            drv_taken = dhead - DRV_LOG_SIZE;
        }
        for (; drv_taken != dhead; drv_taken++) {
            console_put_polled(shared->log[drv_taken % DRV_LOG_SIZE]);
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
        print(&kout, "root: ");
        print(&kout, what);
        print(&kout, " failed: ");
        print_hex(&kout, status);
        print(&kout, "\n");
        halt(2);
    }
}

static uint32_t new_slot(void)
{
    if (BOOT_CAP_COUNT + slots_used >= 64u) {
        must("a free slot", KERR_LIMIT);
    }
    return BOOT_CAP_COUNT + slots_used++;
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

static uint32_t device(uint32_t base, uint32_t size)
{
    uint32_t frame = new_slot();
    must("a device's frame", rv_invoke(OP_DEBUG_FRAME, BOOT_CAP_DEBUG, base, size, frame));
    return frame;
}

/* The pads of the Wi-Fi chip's pins: inputs enabled, isolation off, the bus's two lines fast and strong. */
static void pads_set(void)
{
    uint32_t pads = device(PADS_BANK0_BASE, PADS_BANK0_SIZE);
    must("install the pads", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, pads, RIGHT_R | RIGHT_W));
    REG32(PADS_BANK0_BASE + PAD(23)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(25)) = PAD_IE | PAD_DRIVE_4MA | PAD_SCHMITT;
    REG32(PADS_BANK0_BASE + PAD(24)) = PAD_IE | PAD_DRIVE_12MA | PAD_SCHMITT | PAD_SLEWFAST;
    REG32(PADS_BANK0_BASE + PAD(29)) = PAD_IE | PAD_DRIVE_12MA | PAD_SLEWFAST;
    must("uninstall the pads", rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, 0, 0));
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
    must("install the blob's header", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, head, RIGHT_R));
    struct blob_header h = *(const struct blob_header *)free_base;
    must("uninstall the blob's header", rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, 0, 0));
    must("give the free RAM back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, BOOT_CAP_FREE_RAM, 0, 0));
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
 * The passphrase lives in the shared page and the driver's memory, never in an image.
 */
static void configure(struct drv_shared *s)
{
    uint32_t base, size;
    char value[16];
    s->mode = MODE_SCAN;
    s->channel = 1;
    must("input", rv_frame_info(BOOT_CAP_INPUT, &base, &size));
    must("install the input", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, BOOT_CAP_INPUT, RIGHT_R));
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
    must("uninstall the input", rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_REGION_PEEK, 0, 0));
    if (s->mode != MODE_SCAN && s->ssid[0] == '\0') {
        print(&kout, "root: the configuration names no ssid; scanning only\n");
        s->mode = MODE_SCAN;
    }
}

void driver_main(struct drv_shared *s);

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

    uint32_t inbox = new_slot(), line = new_slot(), log_irq = new_slot(), tline = new_slot(), timer = new_slot();
    must("the root's inbox", rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, inbox, 0));
    must("the log's line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, LOG_IRQ_LINE, 1, line));
    must("bind the log", rv_invoke(OP_IRQ_BIND, line, BOOT_CAP_POOL, inbox, log_irq));
    must("a timer line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, tline));
    must("bind the timer", rv_invoke(OP_IRQ_BIND, tline, BOOT_CAP_POOL, inbox, timer));
    print(&kout, "root: wifi system\n");

    /* The blob, and frames that cover it, largest first from its start. */
    must("free RAM", rv_untyped_info(BOOT_CAP_FREE_RAM, &base, &size, &made));
    uint32_t free_base = base;
    struct blob_header h = blob_header(free_base);
    if (h.magic != BLOB_MAGIC || h.size > size) {
        print(&kout, "root: no firmware blob at ");
        print_hex(&kout, free_base);
        print(&kout, "; load one with make BOARD=rp2350 wifi\n");
        halt(2);
    }
    free_add((struct block){ BOOT_CAP_FREE_RAM, base, size });

    uint32_t page = 0x1000u;
    uint32_t covered = (h.size + page - 1) & ~(page - 1);
    uint32_t blob_frame[DRV_SLOTS - DRV_FIRMWARE], blob_size[DRV_SLOTS - DRV_FIRMWARE], blob_frames = 0;
    for (uint32_t at = free_base, block = size / 2; at < free_base + covered; block /= 2) {
        if (at + block <= free_base + covered || block == page) {
            if (blob_frames == DRV_SLOTS - DRV_FIRMWARE) {
                must("frames enough for the blob", KERR_LIMIT);
            }
            blob_frame[blob_frames] = frame_of(take_at(at, block));
            blob_size[blob_frames++] = block;
            at += block;
        }
    }
    print(&kout, "root: blob of ");
    print_dec(&kout, h.size);
    print(&kout, " bytes in ");
    print_dec(&kout, blob_frames);
    print(&kout, " frames\n");

    /* The driver's memory, devices and objects. */
    uint32_t data_base, shared_base, pool_base;
    uint32_t data = frame_of(take(DRV_DATA_SIZE, &data_base));
    uint32_t shared_frame = frame_of(take(DRV_SHARED_SIZE, &shared_base));
    uint32_t pool = new_slot();
    must("the driver's pool", rv_retype(take(DRV_POOL_SIZE, &pool_base), CAP_POOL, pool, &pool_base));
    uint32_t pio = device(PIO0_BASE, PIO0_SIZE);
    uint32_t pins = device(IO_BANK0_HIGH, IO_BANK0_HIGH_SIZE);
    pads_set();

    uint32_t table = new_slot(), process = new_slot(), thread = new_slot(), drv_inbox = new_slot();
    uint32_t drv_line = new_slot(), drv_timer = new_slot(), drv_time = new_slot();
    must("the driver's table", rv_invoke(OP_POOL_ALLOC, pool, CAP_CAPTABLE, table, DRV_SLOTS));
    must("the driver's process", rv_invoke(OP_POOL_ALLOC, pool, CAP_PROCESS, process, table));
    must("the driver's thread", rv_invoke(OP_POOL_ALLOC, pool, CAP_THREAD, thread, process));
    must("the driver's inbox", rv_invoke(OP_POOL_ALLOC, pool, CAP_NOTIFICATION, drv_inbox, 0));
    must("the driver's timer line", rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 1, 1, drv_line));
    must("bind the driver's timer", rv_invoke(OP_IRQ_BIND, drv_line, pool, drv_inbox, drv_timer));

    must("install the driver's code", rv_invoke(OP_PROCESS_INSTALL, process, DRV_REGION_CODE, BOOT_CAP_CODE, RIGHT_R | RIGHT_X));
    must("install the driver's data", rv_invoke(OP_PROCESS_INSTALL, process, DRV_REGION_DATA, data, RIGHT_R | RIGHT_W));
    must("install PIO0", rv_invoke(OP_PROCESS_INSTALL, process, DRV_REGION_PIO, pio, RIGHT_R | RIGHT_W));
    must("install the pins", rv_invoke(OP_PROCESS_INSTALL, process, DRV_REGION_PINS, pins, RIGHT_R | RIGHT_W));
    must("install the shared page", rv_invoke(OP_PROCESS_INSTALL, process, DRV_REGION_SHARED, shared_frame, RIGHT_R | RIGHT_W));

    must("copy the driver's process", rv_invoke(OP_CAP_COPY, table, DRV_SELF, process, RIGHT_W));
    must("copy the driver's inbox", rv_invoke(OP_CAP_COPY, table, DRV_INBOX, drv_inbox, RIGHT_R));
    must("copy the root's inbox", rv_invoke(OP_CAP_COPY, table, DRV_ROOT, inbox, RIGHT_W));
    must("copy the driver's timer", rv_invoke(OP_CAP_COPY, table, DRV_TIMER, drv_timer, RIGHT_W));
    for (uint32_t i = 0; i < blob_frames; i++) {
        must("lend the blob", rv_invoke(OP_CAP_COPY, table, DRV_FIRMWARE + i, blob_frame[i], RIGHT_R));
    }

    /* The shared page: what the driver must know before it starts. */
    must("install the shared page", rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_REGION_SHARED, shared_frame, RIGHT_R | RIGHT_W));
    shared = (struct drv_shared *)shared_base;
    memset(shared, 0, sizeof(*shared));
    shared->data_base = data_base;
    shared->data_size = DRV_DATA_SIZE;
    shared->pio_base = PIO0_BASE;
    shared->pins_base = IO_BANK0_HIGH;
    shared->blob_base = free_base;
    shared->blob_frames = blob_frames;
    for (uint32_t i = 0; i < blob_frames; i++) {
        shared->blob_frame_size[i] = blob_size[i];
    }
    shared->state = DRV_STARTING;
    configure(shared);
    static const char *const modes[] = { "scan", "station", "access point" };
    print(&kout, "root: mode ");
    print(&kout, modes[shared->mode]);
    print(&kout, "\n");

    /* Half the first core for the driver, and spare time; its faults to the root's inbox. */
    must("keep half the core", rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, TIME_UNITS / 2));
    must("carve the driver's time", rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS / 2, TIME_UNITS / 2, drv_time));
    must("watch the driver", rv_invoke(OP_THREAD_WATCH, thread, inbox, ROOT_BIT_FAULT, 0));
    must("configure the driver", rv_invoke(OP_THREAD_CONFIGURE, thread, (uint32_t)&driver_main, data_base + DRV_DATA_SIZE, 0));
    must("hand the driver its page", rv_invoke(OP_THREAD_WRITE_REG, thread, RV_REG_A0, shared_base, 0));
    must("bind the driver", rv_invoke(OP_TIME_BIND, drv_time, thread, 0, TIME_UNITS / 2));
    must("start the driver", rv_invoke(OP_THREAD_RESUME, thread, 0, 0, 0));

    must("arm the run's end", rv_timer_set(timer, ROOT_BIT_TIMER, RUN_US));
    uint32_t told = DRV_STARTING;
    for (;;) {
        uint32_t bits = 0;
        must("arm the log", rv_irq_set(log_irq, ROOT_BIT_LOG));
        must("wait", rv_wait(inbox, &bits));
        drain();
        if (bits & ROOT_BIT_FAULT) {
            uint32_t cause, pc, addr, status;
            rv_thread_fault(thread, &cause, &pc, &addr, &status);
            print(&kout, "root: the driver faulted, cause ");
            print_hex(&kout, cause);
            print(&kout, " at ");
            print_hex(&kout, pc);
            print(&kout, " address ");
            print_hex(&kout, addr);
            print(&kout, "\n");
            halt(3);
        }
        uint32_t state = shared->state;
        if (state != told && state == DRV_SCANNED) {
            print(&kout, "root: ");
            print_dec(&kout, shared->net_count);
            print(&kout, " networks heard\n");
            for (uint32_t i = 0; i < shared->net_count; i++) {
                const struct drv_net *n = &shared->nets[i];
                print(&kout, "  ch ");
                print_dec(&kout, n->channel);
                print(&kout, n->rssi < 0 ? "  rssi -" : "  rssi ");
                print_dec(&kout, (uint32_t)(n->rssi < 0 ? -n->rssi : n->rssi));
                print(&kout, "  ");
                print(&kout, n->ssid);
                print(&kout, "\n");
            }
            if (shared->mode == MODE_SCAN) {
                halt(0);
            }
        }
        if (state != told && state == DRV_JOINED) {
            print(&kout, "root: joined ");
            print(&kout, shared->ssid);
            print(&kout, "\n");
        }
        if (state != told && state == DRV_AP) {
            print(&kout, "root: access point ");
            print(&kout, shared->ssid);
            print(&kout, " on channel ");
            print_dec(&kout, shared->channel);
            print(&kout, "\n");
        }
        told = state;
        if (shared->state == DRV_FAILED) {
            print(&kout, "root: the driver failed at step ");
            print_dec(&kout, shared->step);
            print(&kout, ", read ");
            print_hex(&kout, shared->detail);
            print(&kout, "\n");
            halt(1);
        }
        if (bits & ROOT_BIT_TIMER) {
            /* A link or an access point that lasted the run is what was asked for. */
            if (state == DRV_JOINED || state == DRV_AP) {
                print(&kout, "root: the run is over\n");
                halt(0);
            }
            print(&kout, "root: out of time\n");
            halt(4);
        }
    }
}
