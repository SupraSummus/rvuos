/*
 * The library's test: a root task that does with user/lib/ what a program of several processes does,
 * and checks that everything it hands out comes back.
 *
 * It checks the copies and fills against byte by byte ones,
 * and reads bytes left in free memory, as a loader leaves a program's files there;
 * connects two peers with a channel whose rings hold a few packets, and has each send the other more than that;
 * builds a child that stores where it has no region, hears it fault and takes it down, twice;
 * checks a child that answers its checks and one that spins, which it finds and takes down;
 * builds a server with a hub in room for children's data, and clients on its ends,
 * and takes one down and connects another in its place, twice;
 * has children add to a count they share under a lock, sleeping now and then while they hold it,
 * and again under a lock that names its holder, whose takers lend the holder their time;
 * has a thread publish a snapshot through a seqlock, on the second core where there is one, while a child reads it,
 * and stops the writer while it writes;
 * hands a buffer between two children, and hears the last fault on it once it is taken;
 * lets a child build a thread of its own from a pool, timer lines and units it gave it,
 * hears that thread's fault as the child's, and takes it all back, twice;
 * then takes the rest down.
 * It ends with "libtest: ok" and halt code 0, which tests/libtest.sh checks, or with what failed and another code.
 */

#include "console.h"
#include "libtest.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"

#define ROOT_UNITS  16u
#define CHILD_UNITS 8u
#define CHILD_TABLE 16u
#define CHILD_DATA  0x2000u
#define RUN_US      10000000u /* the test's deadline */
#define PAUSE_US    100000u   /* between two checks */
#define ROOM_SIZE   0x8000u   /* the server's data and its clients' */

static struct self self, after;
static struct kernel_log klog;
static uint32_t log_bit, deadline_bit, pause_bit, pause_timer;

static struct child a, b, faulty, answering, spinning, server, clients[HUB_CLIENTS], lockers[LOCKERS], snap_reader,
    passers[2], builder;
static struct child *const children[] = { &a, &b, &faulty, &answering, &spinning, &server, &clients[0], &clients[1],
                                          &lockers[0], &lockers[1], &lockers[2], &snap_reader, &passers[0], &passers[1],
                                          &builder };
static struct child *may_fault; /* the one child whose fault is asked for */
static uint32_t faulted;        /* the bits of the faults the root task heard */

static void console_byte(char c)
{
    console_put_polled(c);
}

static const struct out out = { log_write, (void *)BOOT_CAP_DEBUG };

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
        say(&out, "libtest: %s failed", what);
        if (self.what != 0) {
            say(&out, " at %s", self.what);
        }
        say(&out, ": %x\n", status);
        halt(2);
    }
}

static void check(int ok, const char *what)
{
    if (!ok) {
        say(&out, "libtest: FAIL: %s\n", what);
        halt(1);
    }
}

/* One wait, and what it brought: the logs copied, a child's failure or an unasked fault reported, the deadline. */
static uint32_t step(void)
{
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
        if (child_poll(c, &state) && state == CHILD_FAILED) {
            say(&out, "libtest: FAIL: %s failed at step %u, detail %u\n", c->name, c->page->step, c->page->detail);
            halt(1);
        }
        if ((bits & c->bit_fault) && c != may_fault) {
            say(&out, "libtest: FAIL: %s faulted\n", c->name);
            halt(1);
        }
        faulted |= bits & c->bit_fault;
    }
    check(!(bits & deadline_bit), "done before the deadline");
    return bits;
}

/* Steps until a pause of us microseconds is over. */
static void pause_for(uint32_t us)
{
    must("arm a pause", rv_timer_set(pause_timer, pause_bit, us));
    while (!(step() & pause_bit)) {
    }
}

/* What the root task holds, see self_tally. */
static struct self_tally tally(void)
{
    return self_tally(&self);
}

static int same(struct self_tally x, struct self_tally y)
{
    return memcmp(&x, &y, sizeof(x)) == 0;
}

/* Whether the root task holds the same slots and free blocks as it did at was, the blocks in any order. */
static int as_at(const struct self *was)
{
    if (self.free_count != was->free_count || memcmp(self.slots, was->slots, sizeof(self.slots)) != 0) {
        return 0;
    }
    for (uint32_t i = 0; i < was->free_count; i++) {
        int found = 0;
        for (uint32_t j = 0; j < self.free_count; j++) {
            found |= memcmp(&was->free[i], &self.free[j], sizeof(struct block)) == 0;
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

/* Bytes written through a frame, the frame given back, and the bytes read out of free memory. */
static void read_back(void)
{
    struct block blk;
    uint32_t region, base, size;
    uint8_t got[64];
    must("a frame", mem_frame(&self, 0x100u, &blk));
    must("where it lies", rv_frame_info(blk.made, &base, &size));
    must("install it", region_install(&self, blk.made, RIGHT_R | RIGHT_W, &region));
    for (uint32_t i = 0; i < sizeof(got); i++) {
        ((volatile uint8_t *)(uintptr_t)base)[i] = packet_byte(3, i);
    }
    must("uninstall it", region_free(&self, region));
    must("give it back", mem_give(&self, &blk));
    memset(got, 0, sizeof(got));
    must("read free memory", mem_read(&self, base, got, sizeof(got)));
    for (uint32_t i = 0; i < sizeof(got); i++) {
        check(got[i] == packet_byte(3, i), "free memory reads back what was written before it was given back");
    }
    say(&out, "libtest: free memory read: ok\n");
}

/*
 * memcpy, memmove and memset, which move whole words where both ends are aligned, against byte by byte ones
 * at every offset of either end within two words, every length up to a few words, and overlaps either way;
 * the whole buffer is compared, so that nothing is written beside what was asked.
 */
static void copy_round(void)
{
    static uint8_t buf[40], src[40], want[40], moved[24];
    for (uint32_t from = 0; from < 8; from++) {
        for (uint32_t to = 0; to < 8; to++) {
            for (uint32_t n = 0; n <= sizeof(moved); n++) {
                for (uint32_t i = 0; i < sizeof(buf); i++) {
                    buf[i] = want[i] = (uint8_t)(i + 1);
                    src[i] = (uint8_t)(i + 0x80);
                }
                memcpy(buf + to, src + from, n);
                for (uint32_t i = 0; i < n; i++) {
                    want[to + i] = src[from + i];
                }
                check(memcmp(buf, want, sizeof(buf)) == 0, "memcpy copies what a byte loop copies");
                memmove(buf + to, buf + from, n);
                for (uint32_t i = 0; i < n; i++) {
                    moved[i] = want[from + i];
                }
                for (uint32_t i = 0; i < n; i++) {
                    want[to + i] = moved[i];
                }
                check(memcmp(buf, want, sizeof(buf)) == 0, "memmove moves what a byte loop through a copy moves");
                memset(buf + to, (int)(0x100 | from), n);
                for (uint32_t i = 0; i < n; i++) {
                    want[to + i] = (uint8_t)from;
                }
                check(memcmp(buf, want, sizeof(buf)) == 0, "memset fills with the value's low byte, as a byte loop does");
            }
        }
    }
    say(&out, "libtest: copies and fills: ok\n");
}

/*
 * A child that answers its parent's checks and one that spins, each checked twice with a pause between:
 * the first check finds both well, since neither was asked before, and the second finds the one that spins.
 * Taken down, the one that spins leaves nothing behind, its units among what comes back.
 */
static void check_round(void)
{
    struct self_tally before = tally();
    must("build a child that answers", child_new(&self, &answering, "answering", CHILD_TABLE, CHILD_DATA));
    must("build a child that spins", child_new(&self, &spinning, "spinning", CHILD_TABLE, CHILD_DATA));
    must("start a child that answers", child_start(&self, &answering, answer_main, CHILD_UNITS));
    must("start a child that spins", child_start(&self, &spinning, spin_main, CHILD_UNITS));
    while (answering.told != CHILD_RUNNING || spinning.told != CHILD_RUNNING) {
        step();
    }
    check(child_check(&answering) && child_check(&spinning), "a child never asked counts as having answered");
    pause_for(PAUSE_US);
    check(child_check(&answering), "a child whose loop comes round answers the check its parent asked meanwhile");
    check(!child_check(&spinning), "a child that spins answers no check");
    must("take a child down", child_free(&self, &answering));
    must("take a child that spins down", child_free(&self, &spinning));
    check(same(tally(), before), "a child that spins, taken down, gives back what it was given");
    say(&out, "libtest: a child that answers its checks, and one that spins found by them: ok\n");
}

/* A client on end i of the hub, built, connected and started; seed makes its packets its own. */
static void client_go(struct chan_hub *hub, uint32_t i, uint32_t seed)
{
    must("build a client", child_new(&self, &clients[i], "client", CHILD_TABLE, CHILD_DATA));
    struct client_page *p = (struct client_page *)clients[i].page;
    p->seed = seed;
    must("connect a client", chan_hub_connect(&self, hub, i, &clients[i], &p->link));
    must("start a client", child_start(&self, &clients[i], client_main, CHILD_UNITS));
}

/*
 * A server with a hub, in room for children's data, and a client on each end at once;
 * then the first end's client taken down and another connected in its place once the server let go of it,
 * twice, the second leaving the root task as the first did; then all of it back, the room too.
 */
static void hub_round(void)
{
    struct self_tally unroomed = tally(), down = unroomed;
    struct chan_hub hub;
    must("room for children's data", self_room(&self, ROOM_SIZE));
    must("build the server", child_new(&self, &server, "server", CHILD_TABLE, CHILD_DATA));
    check(server.in_room, "a child built once there is room is built in it");
    struct server_page *ps = (struct server_page *)server.page;
    must("a hub", chan_hub_new(&self, &hub, &server, ps->ends, HUB_CLIENTS, HUB_CHAN_SIZE, PACKET_MAX));
    must("start the server", child_start(&self, &server, server_main, CHILD_UNITS));
    client_go(&hub, 0, 0);
    client_go(&hub, 1, 1);
    while (clients[0].told != CLIENT_DONE || clients[1].told != CLIENT_DONE) {
        step();
    }
    say(&out, "libtest: two clients of a hub, %u packets each and back: ok\n", PACKETS);
    for (uint32_t round = 0; round < 2; round++) {
        must("close a client's channel", chan_hub_close(&self, &hub, 0));
        must("take a client down", child_free(&self, &clients[0]));
        while (!chan_hub_idle(&hub, 0)) {
            step();
        }
        check(round == 0 || (same(tally(), down) && as_at(&after)),
              "a client connected again leaves the root task as the last did once taken down");
        down = tally();
        after = self;
        client_go(&hub, 0, 2u + round);
        while (clients[0].told != CLIENT_DONE) {
            step();
        }
    }
    check(ps->let_go == 2, "the server let go of each client the root task closed");
    say(&out, "libtest: a client of a hub taken down and another connected in its place, twice: ok\n");
    for (uint32_t i = 0; i < HUB_CLIENTS; i++) {
        must("close a client's channel", chan_hub_close(&self, &hub, i));
        must("take a client down", child_free(&self, &clients[i]));
    }
    must("take the server down", child_free(&self, &server));
    must("free the hub", chan_hub_free(&self, &hub));
    must("give the room back", self_room_free(&self));
    check(same(tally(), unroomed), "the hub, its server and clients and the room gave back what they were given");
}

/*
 * Lockers in room for children's data, the count carved from it,
 * and the lock's notification from the root task's pool, where it stays, as every object does until its pool goes.
 * With named the lock names its holder, locker i as i + 1, and each locker holds every other's thread to lend it time.
 */
static void lock_round(int named)
{
    struct self_tally unroomed = tally();
    struct block shared;
    uint32_t note, waited = 0;
    must("room for children's data", self_room(&self, ROOM_SIZE));
    must("memory the lockers share", room_take(&self, SELF_ROOM_UNIT, &shared));
    struct locked *l = (struct locked *)(uintptr_t)shared.base;
    must("a slot for the lock's notification", slot_new(&self, &note));
    must("the lock's notification", rv_pool_alloc(self.pool, CAP_NOTIFICATION, note, 0));
    struct lock lock = { (uint32_t)(uintptr_t)&l->word, note };
    lock_init(&lock);
    l->count = 0;
    for (uint32_t i = 0; i < LOCKERS; i++) {
        uint32_t region;
        must("build a locker", child_new(&self, &lockers[i], "locker", CHILD_TABLE, CHILD_DATA));
        struct locker_page *p = (struct locker_page *)lockers[i].page;
        must("give a locker the shared memory",
             child_map(&self, &lockers[i], shared.made, RIGHT_R | RIGHT_W, &region));
        must("give a locker the lock's notification",
             child_give(&self, &lockers[i], note, RIGHT_R | RIGHT_W, &p->lock.note));
        p->lock.word = lock.word;
        p->count = (uint32_t)(uintptr_t)&l->count;
        p->named = named;
    }
    for (uint32_t i = 0; named && i < LOCKERS; i++) {
        struct locker_page *p = (struct locker_page *)lockers[i].page;
        p->name = (struct named_lock){ lock.word, p->lock.note, i + 1u, { 0 } };
        for (uint32_t j = 0; j < LOCKERS; j++) {
            if (j != i) {
                must("give a locker another's thread",
                     child_give(&self, &lockers[i], lockers[j].thread, RIGHT_X, &p->name.lend[j + 1u]));
            }
        }
    }
    for (uint32_t i = 0; i < LOCKERS; i++) {
        must("start a locker", child_start(&self, &lockers[i], locker_main, CHILD_UNITS));
    }
    for (uint32_t i = 0; i < LOCKERS; i++) {
        while (lockers[i].told != LOCKER_DONE) {
            step();
        }
        waited += ((struct locker_page *)lockers[i].page)->waited;
    }
    check(l->count == LOCKERS * LOCK_ROUNDS, "no locker's round was lost to another holding the lock too");
    check(waited > 0, "a locker found the lock held, so the takes that wait were tried");
    check(lock_try(&lock), "the lock is free once every locker is done");
    lock_give(&lock);
    say(&out, "libtest: %u lockers, %u rounds each under a lock%s, found held %u times: ok\n", LOCKERS, LOCK_ROUNDS,
        named ? " that names its holder" : "", waited);
    for (uint32_t i = 0; i < LOCKERS; i++) {
        must("take a locker down", child_free(&self, &lockers[i]));
    }
    must("the shared memory back", slot_free(&self, shared.made));
    room_give(&self, &shared);
    must("the lock's notification back", slot_free(&self, note));
    must("give the room back", self_room_free(&self));
    check(same(tally(), unroomed), "the lockers, their lock and the room gave back what they were given");
}

/* The snapshot's writer, a thread of the root task's, and its stack. */
static uint8_t snap_stack[1024] __attribute__((aligned(16)));

/* Version after version for good, every word the version, saying in the reader's page while it writes one. */
static void snap_writer(uint32_t page)
{
    struct snap_page *r = (struct snap_page *)(uintptr_t)page;
    uint32_t data[SNAP_WORDS];
    for (uint32_t version = 1;; version++) {
        for (uint32_t i = 0; i < SNAP_WORDS; i++) {
            data[i] = version;
        }
        r->writing = 1;
        seqlock_write(&r->lock, data);
        r->writing = 0;
    }
}

/*
 * A snapshot in room for children's data, given its reader read only, and its writer,
 * bound to the second core's units, or where there is none to some of the first's, taking turns with the reader;
 * the writer stopped by taking its units, and the snapshot read by the root task, as it stays for good.
 */
static void snap_round(void)
{
    _Static_assert(SEQLOCK_BYTES(SNAP_WORDS * 4u) <= SELF_ROOM_UNIT, "the snapshot fits a unit of room");
    struct self_tally unroomed = tally();
    struct block shared;
    uint32_t region, writer, time, first = 0, units = TIME_UNITS;
    must("room for children's data", self_room(&self, ROOM_SIZE));
    must("memory for a snapshot", room_take(&self, SELF_ROOM_UNIT, &shared));
    struct seqlock lock = seqlock_shape(shared.base, SNAP_WORDS * 4u);
    seqlock_init(&lock);
    must("build the reader", child_new(&self, &snap_reader, "reader", CHILD_TABLE, CHILD_DATA));
    must("give it the snapshot, read only", child_map(&self, &snap_reader, shared.made, RIGHT_R, &region));
    struct snap_page *r = (struct snap_page *)snap_reader.page;
    r->lock = lock;
    must("a slot for the writer", slot_new(&self, &writer));
    must("the writer", rv_pool_alloc(self.pool, CAP_THREAD, writer, BOOT_CAP_PROCESS));
    must("configure the writer", rv_thread_configure(writer, (uint32_t)(uintptr_t)snap_writer,
                                                     (uint32_t)(uintptr_t)(snap_stack + sizeof(snap_stack)),
                                                     (uint32_t)(uintptr_t)r));
    must("a slot for the writer's units", slot_new(&self, &time));
    int second_core = rv_time_carve(BOOT_CAP_TIME, TIME_UNITS, TIME_UNITS, time) == KERR_OK;
    if (!second_core) {
        units = CHILD_UNITS;
        must("units for the writer", units_take(&self, units, &first));
        must("carve the writer's units", rv_time_carve(self.time, first, units, time));
    }
    must("bind the writer", rv_time_bind(time, writer, 0, units));
    must("start the writer", rv_thread_resume(writer));
    must("start the reader", child_start(&self, &snap_reader, snap_reader_main, CHILD_UNITS));
    pause_for(SNAP_US);
    must("take the writer's units", rv_cap_revoke(self.table, time));
    pause_for(SNAP_STOP_US);
    uint32_t data[SNAP_WORDS], version;
    check(seqlock_read(&lock, data, &version), "a snapshot whose writer stopped reads at the first try");
    for (uint32_t i = 0; i < SNAP_WORDS; i++) {
        check(data[i] == version, "a snapshot whose writer stopped while it wrote is whole");
    }
    r->stop = 1;
    while (snap_reader.told != SNAP_READ) {
        step();
    }
    check(r->reads > 0 && version > 0, "the writer published and the reader read");
    check(r->halfway + r->again > 0, "the reader read while the writer was in the middle of a write");
    say(&out, "libtest: a snapshot read whole %u times as it was written on %s core, %u in the middle of a write, "
              "%u tries again: ok\n",
        r->reads, second_core ? "the second" : "the same", r->halfway, r->again);
    must("take the reader down", child_free(&self, &snap_reader));
    must("the writer's units back", slot_free(&self, time));
    if (!second_core) {
        units_give(&self, first, units);
    }
    must("the writer back", slot_free(&self, writer));
    must("the snapshot back", slot_free(&self, shared.made));
    room_give(&self, &shared);
    must("give the room back", self_room_free(&self));
    check(same(tally(), unroomed), "the snapshot, its reader and its writer gave back what they were given");
}

/* Two passers in room for children's data, and the buffer carved from it, which the root task sees through the room. */
static void pass_round(void)
{
    struct self_tally unroomed = tally();
    struct block buffer;
    uint32_t region = 0;
    must("room for children's data", self_room(&self, ROOM_SIZE));
    must("a buffer", room_take(&self, SELF_ROOM_UNIT, &buffer));
    for (uint32_t i = 0; i < 2; i++) {
        must("build a passer", child_new(&self, &passers[i], "passer", CHILD_TABLE, CHILD_DATA));
        ((struct passer_page *)passers[i].page)->buffer = buffer.base;
        must("start a passer", child_start(&self, &passers[i], passer_main, CHILD_UNITS));
    }
    for (uint32_t n = 0; n < PASSES; n++) {
        struct child *to = &passers[n % 2u];
        if (n > 0) {
            must("take the buffer from a passer", child_unmap(&self, &passers[(n + 1u) % 2u], region));
        }
        must("give the buffer to a passer", child_map(&self, to, buffer.made, RIGHT_R | RIGHT_W, &region));
        ((struct passer_page *)to->page)->pass = n;
        must("tell a passer it holds the buffer", child_tell(to));
        while (to->told != PASSER_HELD + n) {
            step();
        }
    }
    struct child *last = &passers[(PASSES - 1u) % 2u];
    must("take the buffer from the last passer", child_unmap(&self, last, region));
    ((struct passer_page *)last->page)->pass = PASS_GONE;
    may_fault = last;
    faulted = 0;
    must("tell the last passer it holds the buffer no longer", child_tell(last));
    while (!(faulted & last->bit_fault) && last->told != PASSER_BREACHED) {
        step();
    }
    may_fault = 0;
    check(last->told != PASSER_BREACHED, "a store to a buffer taken from a passer faulted");
    check(*(volatile uint32_t *)(uintptr_t)buffer.base == PASSES - 1u, "the buffer holds what its last pass wrote");
    say(&out, "libtest: a buffer handed between two children %u times, and taken from the last: ok\n", PASSES);
    for (uint32_t i = 0; i < 2; i++) {
        must("take a passer down", child_free(&self, &passers[i]));
    }
    must("the buffer back", slot_free(&self, buffer.made));
    room_give(&self, &buffer);
    must("give the room back", self_room_free(&self));
    check(same(tally(), unroomed), "the passers, their buffer and the room gave back what they were given");
}

/*
 * A builder, given a pool, timer lines, units and slots of its own, builds a thread from them,
 * whose fault is heard as the builder's; taken down, it gives back all of it,
 * and built again from what it gave back, leaves the root task as the first did.
 */
static void own_round(void)
{
    struct self_tally before = tally();
    for (uint32_t round = 0; round < 2; round++) {
        must("build a builder", child_new(&self, &builder, "builder", BUILDER_TABLE, CHILD_DATA));
        struct builder_page *p = (struct builder_page *)builder.page;
        must("let a child build of its own",
             child_give_own(&self, &builder, OWN_POOL, OWN_LINES, OWN_UNITS, OWN_SLOTS, &p->own));
        may_fault = &builder;
        faulted = 0;
        must("start a builder", child_start(&self, &builder, builder_main, CHILD_UNITS));
        while (!(faulted & builder.bit_fault) || builder.told != BUILDER_BUILT) {
            step();
        }
        may_fault = 0;
        check(p->worked, "a thread a child built of its own ran on its units and woke on its timer");
        must("take a builder down", child_free(&self, &builder));
        check(same(tally(), before), "a child that built of its own, taken down, gives back what it was given");
        check(round == 0 || as_at(&after), "a builder built again from what the last gave back leaves the same behind");
        after = self;
    }
    say(&out, "libtest: a child that builds a thread of its own, whose fault is the child's, taken down twice: ok\n");
}

int main(void)
{
    uint32_t base, size, region, deadline;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));
    must("a bit for the log", bit_new(&self, &log_bit));
    must("a bit for the deadline", bit_new(&self, &deadline_bit));
    must("a timer", timer_new(&self, &deadline));
    must("arm the deadline", rv_timer_set(deadline, deadline_bit, RUN_US));
    must("a bit for a pause", bit_new(&self, &pause_bit));
    must("a timer for a pause", timer_new(&self, &pause_timer));
    struct self_tally start = tally();
    say(&out, "libtest: up, %u bytes free and %u slots unused\n", start.bytes, slots_unused(&self));

    copy_round();
    read_back();

    /* Two peers, connected before either starts. */
    struct chan link;
    must("build a peer", child_new(&self, &a, "a", CHILD_TABLE, CHILD_DATA));
    must("build a peer", child_new(&self, &b, "b", CHILD_TABLE, CHILD_DATA));
    struct peer_page *pa = (struct peer_page *)a.page, *pb = (struct peer_page *)b.page;
    pa->sends_first = 1;
    must("a channel", chan_new(&self, &link, LINK_SIZE, PACKET_MAX));
    check(link.a.slots < PACKETS, "the rings hold fewer packets than are sent, so that they fill");
    must("connect the peers", chan_connect(&self, &link, &a, &pa->link, &b, &pb->link));
    must("start a peer", child_start(&self, &a, peer_main, CHILD_UNITS));
    must("start a peer", child_start(&self, &b, peer_main, CHILD_UNITS));
    while (a.told != PEER_DONE || b.told != PEER_DONE) {
        step();
    }
    say(&out, "libtest: %u packets each way through rings of %u slots: ok\n", PACKETS, link.a.slots);

    /*
     * A child that faults, heard and taken down, and nothing of it left; then the same again,
     * from the blocks the first gave back, which must leave the root task as the first did,
     * so that a child restarted for good costs nothing more.
     */
    struct self_tally before = tally();
    uint32_t root_data, root_size;
    must("the root's data", rv_frame_info(BOOT_CAP_DATA, &root_data, &root_size));
    for (uint32_t round = 0; round < 2; round++) {
        must("build the faulting child", child_new(&self, &faulty, "fault", CHILD_TABLE, CHILD_DATA));
        may_fault = &faulty;
        struct fault_page *pf = (struct fault_page *)faulty.page;
        check(pf->address == 0, "a child's data starts zeroed, whatever its block held");
        pf->address = root_data;
        must("start the faulting child", child_start(&self, &faulty, fault_main, CHILD_UNITS));
        faulted = 0;
        while (!(faulted & faulty.bit_fault)) {
            step();
        }
        uint32_t cause, pc, addr, status;
        must("what stopped it", rv_thread_fault(faulty.thread, &cause, &pc, &addr, &status));
        check(faulty.told != FAULT_BREACHED, "a store with no region faulted");
        say(&out, "libtest: the faulting child stopped, cause %x at %x\n", cause, pc);
        drain();
        must("take the faulting child down", child_free(&self, &faulty));
        check(same(tally(), before), "taking a child down gives back what it was given");
        check(round == 0 || as_at(&after), "a child built again from what the last gave back leaves the same behind");
        after = self;
    }
    say(&out, "libtest: a child taken down, and again: ok\n");

    drain();
    must("take a peer down", child_free(&self, &a));
    must("take a peer down", child_free(&self, &b));
    must("free the channel", chan_free(&self, &link));
    check_round();
    hub_round();
    lock_round(0);
    lock_round(1);
    snap_round();
    pass_round();
    own_round();
    check(same(tally(), start), "everything handed out came back");
    say(&out, "libtest: down, %u bytes free and %u slots unused\n", mem_unused(&self), slots_unused(&self));
    say(&out, "libtest: ok\n");
    halt(0);
}
