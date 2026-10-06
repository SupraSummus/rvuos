/*
 * The laboratory's root task: one scenario after another, see NOTES.md.
 * A scenario is a cast of roles, each earning units of the first core, or none and running on spare time alone,
 * and asking another member, or nobody.
 * The root task builds the cast, gives each member asked a hub and connects its clients,
 * lets them run WARM_US, measures for WINDOW_US, and says for each client, in microseconds,
 *
 *   lab: <scenario> <member> n=<answers> median=<us> p90=<us> worst=<us>
 *
 * the median and p90 of the first LAB_SAMPLES answers and the worst of all; a locker's answers are its takes,
 * of the lock of lib/lock.h, or of the kernel's mutex, its waiters lending the holder their time or not.
 * Then it takes the cast down and checks that it got back what it handed out;
 * the run ends with "lab: done" and halt code 0.
 */

#include "console.h"
#include "lab.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"

#define ROOT_UNITS  8u
#define HOG_UNITS   32u /* wherever the hog runs, so that two scenarios differ only in what their names say */
#define CHILD_TABLE 16u
#define CHILD_DATA  0x2000u
#define ROOM_SIZE   0x10000u /* the cast's data, from one room, since Hazard3 gives the root task seven regions */
#define POOL_SIZE   0x1000u
#define LOCK_FRAME  0x400u
#define WARM_US     20000u
#define WINDOW_US   200000u

/* Work, in iterations of lab_work: under QEMU virt, about 25 us, half a tick, and a third of one. */
#define SMALL  4000u
#define BIG    80000u
#define HOLD   60000u
#define PERIOD 3000u  /* a periodic member's, in microseconds: three ticks */
#define JITTER 80000u /* the most a periodic member works before each ask */

enum role { NONE, STORE, GATE, UI, BULK, HOG, LOCKER };

/* What lockers share: the lock of lib/lock.h, the kernel's mutex, or the mutex with MUTEX_LEND. */
enum lock_kind { NOTE, MUTEX, LEND };

static void (*const entries[])(struct child_page *) = {
    [STORE] = store_main, [GATE] = gate_main, [UI] = ui_main,
    [BULK] = bulk_main,   [HOG] = hog_main,   [LOCKER] = locker_main,
};

#define CAST_MAX 5u

struct member {
    const char *name;
    uint8_t role;
    uint8_t units; /* of the first core; 0 for spare time alone */
    uint8_t asks;  /* the member it asks, counted from 1; 0 for nobody */
    uint8_t local;
    uint8_t lock; /* a locker's lock_kind */
    uint32_t cost, period_us, gap;
};

struct scenario {
    const char *name;
    struct member cast[CAST_MAX]; /* up to the first without a role */
};

#define SERVER(name, role, units, asks)               { name, role, units, asks, 0, 0, 0, 0, 0 }
#define CLIENT(name, role, units, asks, cost, period) { name, role, units, asks, 0, 0, cost, period, 0 }
#define LIBRARY                                       { "ui", UI, 8, 0, 1, 0, SMALL, PERIOD, 0 }
#define SHARER(name, kind, units, hold, period, gap)  { name, LOCKER, units, 0, 0, kind, hold, period, gap }
#define HOGGING                                       { "hog", HOG, HOG_UNITS, 0, 0, 0, 0, 0, 0 }
#define STORE_OF(units)                               SERVER("store", STORE, units, 0)
#define UI_OF(units, asks)                            CLIENT("ui", UI, units, asks, SMALL, PERIOD)
#define QUICK(kind)                                   SHARER("quick", kind, 16, SMALL, PERIOD, 0)
#define SLOW(kind)                                    SHARER("slow", kind, 2, HOLD, 0, HOLD)

static const struct scenario scenarios[] = {
    { "alone", { STORE_OF(8), UI_OF(8, 1) } },
    { "local", { LIBRARY } },
    { "hog", { STORE_OF(8), UI_OF(8, 1), HOGGING } },
    { "local-hog", { LIBRARY, HOGGING } },
    { "rich-ui", { STORE_OF(8), UI_OF(16, 1), HOGGING } },
    { "rich-server", { STORE_OF(16), UI_OF(8, 1), HOGGING } },
    { "poor-server", { STORE_OF(0), UI_OF(16, 1), HOGGING } },
    { "bulk", { STORE_OF(8), UI_OF(8, 1), CLIENT("bulk", BULK, 8, 1, BIG, 0) } },
    { "bulk-hog", { STORE_OF(8), UI_OF(8, 1), CLIENT("bulk", BULK, 8, 1, BIG, 0), HOGGING } },
    { "free-rider", { STORE_OF(8), UI_OF(8, 1), CLIENT("bulk", BULK, 0, 1, BIG, 0), HOGGING } },
    { "chain", { STORE_OF(8), SERVER("gate", GATE, 8, 1), UI_OF(8, 2), HOGGING } },
    { "lock", { QUICK(NOTE), SLOW(NOTE) } },
    { "lock-hog", { QUICK(NOTE), SLOW(NOTE), HOGGING } },
    { "mutex", { QUICK(MUTEX), SLOW(MUTEX) } },
    { "mutex-hog", { QUICK(MUTEX), SLOW(MUTEX), HOGGING } },
    { "lend", { QUICK(LEND), SLOW(LEND) } },
    { "lend-hog", { QUICK(LEND), SLOW(LEND), HOGGING } },
};

static struct self self;
static struct kernel_log klog;
static const struct out out = { log_write, (void *)BOOT_CAP_DEBUG };
static uint32_t timer, timer_bit;
static struct child kids[CAST_MAX];
static struct chan_hub hubs[CAST_MAX];
static uint32_t connected[CAST_MAX]; /* the clients of each member's hub, 0 for no hub */
static uint32_t end_of[CAST_MAX];    /* the end of its server's hub a client is connected to */
static uint32_t sorted[LAB_SAMPLES];

static void console_byte(char c)
{
    console_put_polled(c);
}

/* The kernel's log to the console: the kernel's lines, the root task's and its children's, in their order. */
static void drain(void)
{
    kernel_log_take(&klog, console_byte);
    console_flush();
}

static __attribute__((noreturn)) void halt(uint32_t code)
{
    drain();
    rv_halt(BOOT_CAP_DEBUG, code);
}

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&out, "lab: %s failed", what);
        if (self.what != 0) {
            say(&out, " at %s", self.what);
        }
        say(&out, ": %x\n", status);
        halt(2);
    }
}

static struct lab_page *page_of(uint32_t i)
{
    return (struct lab_page *)kids[i].page;
}

/* Waits us, and halts if a member of the cast faults or fails meanwhile. */
static void sleep_us(const struct scenario *sc, uint32_t count, uint32_t us)
{
    must("set the root's timer", rv_timer_set(timer, timer_bit, us));
    for (;;) {
        uint32_t bits, state;
        must("wait on the root's inbox", rv_wait(self.inbox, &bits));
        for (uint32_t i = 0; i < count; i++) {
            if ((bits & kids[i].bit_fault) || (child_poll(&kids[i], &state) && state == CHILD_FAILED)) {
                say(&out, "lab: %s: %s faulted or failed\n", sc->name, sc->cast[i].name);
                halt(1);
            }
        }
        if (bits & timer_bit) {
            return;
        }
    }
}

static void set_phase(uint32_t count, uint32_t phase)
{
    for (uint32_t i = 0; i < count; i++) {
        __atomic_store_n(&page_of(i)->phase, phase, __ATOMIC_RELEASE);
    }
}

static uint32_t percentile(uint32_t n, uint32_t per_cent)
{
    for (uint32_t i = 1; i < n; i++) {
        uint32_t v = sorted[i], j = i;
        for (; j > 0 && sorted[j - 1] > v; j--) {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = v;
    }
    return n == 0 ? 0 : sorted[(n - 1) * per_cent / 100u];
}

static void report(const char *scenario, const char *member, const struct lab_stats *st)
{
    uint64_t t;
    uint32_t hz, counter;
    must("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &t, &hz, &counter));
    uint32_t per_us = hz >= 1000000u ? hz / 1000000u : 1u;
    uint32_t n = __atomic_load_n(&st->n, __ATOMIC_ACQUIRE), kept = n < LAB_SAMPLES ? n : LAB_SAMPLES;
    memcpy(sorted, st->sample, kept * sizeof(sorted[0]));
    say(&out, "lab: %s %s n=%u median=%u p90=%u worst=%u\n", scenario, member, n, percentile(kept, 50) / per_us,
        percentile(kept, 90) / per_us, st->worst / per_us);
}

/*
 * The lock the lockers share: its word in a frame of its own, and a notification from a pool of its own,
 * and the mutex they take instead, from that pool too.
 */
struct shared_lock {
    struct block frame, pool;
    uint32_t note, mutex;
};

static void lock_new(struct shared_lock *l)
{
    uint32_t region;
    must("the lock's frame", mem_frame(&self, LOCK_FRAME, &l->frame));
    must("install the lock's frame", region_install(&self, l->frame.made, RIGHT_R | RIGHT_W, &region));
    lock_init(&(struct lock){ l->frame.base, 0 });
    must("uninstall the lock's frame", region_free(&self, region));
    must("the lock's pool", mem_take(&self, POOL_SIZE, &l->pool));
    must("make the lock's pool", mem_make(&self, &l->pool, CAP_POOL));
    must("a slot", slot_new(&self, &l->note));
    must("the lock's notification", rv_pool_alloc(l->pool.made, CAP_NOTIFICATION, l->note, 0));
    must("a slot", slot_new(&self, &l->mutex));
    must("the mutex", rv_pool_alloc(l->pool.made, CAP_MUTEX, l->mutex, 0));
}

static void lock_give_to(struct shared_lock *l, uint32_t i, uint8_t kind)
{
    struct lab_page *p = page_of(i);
    if (kind != NOTE) {
        must("give the mutex", child_give(&self, &kids[i], l->mutex, RIGHT_W, &p->mutex));
        p->lend = kind == LEND ? MUTEX_LEND : 0;
        return;
    }
    uint32_t region;
    must("map the lock", child_map(&self, &kids[i], l->frame.made, RIGHT_R | RIGHT_W, &region));
    p->lock.word = l->frame.base;
    must("give the lock's notification", child_give(&self, &kids[i], l->note, RIGHT_R | RIGHT_W, &p->lock.note));
}

static void lock_free(struct shared_lock *l)
{
    must("the lock's frame back", mem_give(&self, &l->frame));
    must("the lock's notification back", slot_free(&self, l->note));
    must("the mutex back", slot_free(&self, l->mutex));
    must("the lock's pool back", mem_give(&self, &l->pool));
}

static void run(const struct scenario *sc)
{
    uint32_t count = 0;
    while (count < CAST_MAX && sc->cast[count].role != NONE) {
        count++;
    }
    for (uint32_t i = 0; i < count; i++) {
        const struct member *m = &sc->cast[i];
        must("build a member", child_new(&self, &kids[i], m->name, CHILD_TABLE, CHILD_DATA));
        struct lab_page *p = page_of(i);
        must("give a member the clock", child_give(&self, &kids[i], BOOT_CAP_CLOCK, RIGHT_R, &p->clock));
        p->cost = m->cost;
        p->period_us = m->period_us;
        p->jitter = m->period_us != 0 ? JITTER : 0;
        p->gap = m->gap;
        p->local = m->local;
        connected[i] = 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = sc->cast[i].asks;
        if (s-- == 0) {
            continue;
        }
        if (connected[s] == 0) {
            must("a hub", chan_hub_new(&self, &hubs[s], &kids[s], page_of(s)->ends, LAB_ENDS, LAB_CHAN_SIZE, LAB_SLOT));
        }
        end_of[i] = connected[s]++;
        must("connect a client", chan_hub_connect(&self, &hubs[s], end_of[i], &kids[i], &page_of(i)->up));
    }
    struct shared_lock lock = { 0 };
    for (uint32_t i = 0; i < count; i++) {
        if (sc->cast[i].role == LOCKER) {
            if (lock.note == 0) {
                lock_new(&lock);
            }
            lock_give_to(&lock, i, sc->cast[i].lock);
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        must("start a member", child_start(&self, &kids[i], entries[sc->cast[i].role], sc->cast[i].units));
    }

    sleep_us(sc, count, WARM_US);
    set_phase(count, LAB_MEASURE);
    sleep_us(sc, count, WINDOW_US);
    set_phase(count, LAB_OVER);
    for (uint32_t i = 0; i < count; i++) {
        const struct member *m = &sc->cast[i];
        /* The clients, and the lockers: one that wants the lock in time, and one whose holds the other waits out. */
        if (m->role == UI || m->role == BULK || m->role == LOCKER) {
            report(sc->name, m->name, &page_of(i)->st);
        }
    }
    drain();

    /* Each member down before the one it asks, and its end of that one's hub closed; the hubs last. */
    for (uint32_t alive = (1u << count) - 1u; alive != 0;) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t asked = 0;
            for (uint32_t j = 0; j < count; j++) {
                asked |= (alive >> j & 1u) && sc->cast[j].asks == i + 1u;
            }
            if (!(alive >> i & 1u) || asked) {
                continue;
            }
            must("take a member down", child_free(&self, &kids[i]));
            alive &= ~(1u << i);
            if (sc->cast[i].asks != 0) {
                must("close a client's channel", chan_hub_close(&self, &hubs[sc->cast[i].asks - 1u], end_of[i]));
            }
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (connected[i] != 0) {
            must("free a hub", chan_hub_free(&self, &hubs[i]));
        }
    }
    if (lock.note != 0) {
        lock_free(&lock);
    }
}

int main(void)
{
    uint32_t base, size, region;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));
    must("room for the cast's data", self_room(&self, ROOM_SIZE));
    must("the root's timer", timer_new(&self, &timer));
    must("the root's timer bit", bit_new(&self, &timer_bit));

    struct self_tally before = self_tally(&self);
    for (uint32_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        run(&scenarios[i]);
        struct self_tally after = self_tally(&self);
        if (memcmp(&after, &before, sizeof(after)) != 0) {
            say(&out, "lab: %s did not give back what it was given\n", scenarios[i].name);
            halt(1);
        }
    }
    say(&out, "lab: done\n");
    halt(0);
}
