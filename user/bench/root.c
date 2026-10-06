/*
 * The benchmark: what a system call and a switch cost, each measure over many rounds by the clock,
 * which tests/bench.py turns into the core's cycles, or under QEMU into instructions.
 *
 * call: one system call, a signal nobody waits for.
 * signal-wait: a signal and a wait that finds its bit set, two calls and no switch.
 * threads: a round trip between two threads of one process, four calls and two switches.
 * processes: the same between two processes, whose switches load the other's regions too.
 * channel: 64 bytes there and back through a channel of lib/chan.h between two children.
 * lock: a lock of lib/lock.h that names its holder, taken and given back, nobody else wanting it, no call.
 * lock-wait: the same lock another thread holds, waited for, eleven calls and four switches,
 * the holder waiting for a signal while it holds it, so that each round waits once,
 * and for another before it takes it again, so that a tick between the two cannot leave both waiting.
 * lock-lend: the same with the waiter lending the holder its time, which hands the holder the rest of its turn
 * and has the turn back with the give's signal, rather than taking the next turn and joining the queue.
 * seqlock-write: a snapshot of 32 bytes published through lib/seqlock.h, nobody reading, no call.
 * seqlock-read: the same snapshot copied out whole, nobody writing, no call.
 * contended-worst: the worst round of processes beside a third process that always wants the core,
 * over a tenth of a second; it waits for that process's turns, so the tick sets it.
 *
 * Each measure is a line of its rounds, the clock's counts over them and the clock's rate,
 * and the run ends with "bench: done" and halt code 0.
 */

#include "bench.h"
#include "console.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "lib/log.h"
#include "lib/say.h"
#include "lib/seqlock.h"

#define ROUNDS      1000u
#define WARM_UP     10u
#define WINDOWS     10u /* contended-worst's window: a second over this */
#define ROOT_UNITS  16u
#define UNITS       8u  /* each other thread's */
#define CHILD_TABLE 16u
#define CHILD_DATA  0x1000u
#define ROOM_SIZE   0x8000u /* the children's data, from one room, since Hazard3 gives the root task seven regions */
#define POOL_SIZE   0x1000u
#define SNAPSHOT    32u /* bytes, as a little state a server publishes */

static struct self self;
static struct kernel_log klog;
static const struct out out = { log_write, (void *)BOOT_CAP_DEBUG };
static uint8_t pong_stack[1024] __attribute__((aligned(16)));
static uint8_t holder_stack[1024] __attribute__((aligned(16)));
static uint32_t note, ping_note, pong_note, held_note, hold_note, done_note, lock_note;
/* The lock's word, and how the root thread, named 1, and the holder, named 2, take it; see lock-wait. */
static uint32_t lock_word;
static struct named_lock waiter_lock, holder_lock;
static struct child pong;
static struct seqlock snap;
static uint32_t snap_memory[SEQLOCK_BYTES(SNAPSHOT) / 4u], snap_data[SNAPSHOT / 4u];

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
        say(&out, "bench: %s failed", what);
        if (self.what != 0) {
            say(&out, " at %s", self.what);
        }
        say(&out, ": %x\n", status);
        halt(2);
    }
}

static uint64_t now(uint32_t *hz)
{
    uint64_t t;
    uint32_t counter;
    must("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &t, hz, &counter));
    return t;
}

static void report(const char *name, uint32_t rounds, uint32_t counts, uint32_t hz)
{
    say(&out, "bench: %s rounds=%u counts=%u hz=%u\n", name, rounds, counts, hz);
    drain();
}

static void round_call(void)
{
    rv_signal(note, 1);
}

static void round_signal_wait(void)
{
    uint32_t bits;
    rv_signal(note, 1);
    rv_wait(note, &bits);
}

/* The other end of threads: a second thread of the root task's process. */
static void pong_thread(uint32_t arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits;
        rv_wait(pong_note, &bits);
        rv_signal(ping_note, 1);
    }
}

static void round_thread(void)
{
    uint32_t bits;
    rv_signal(pong_note, 1);
    rv_wait(ping_note, &bits);
}

static void round_lock(void)
{
    named_lock_take(&waiter_lock);
    named_lock_give(&waiter_lock);
}

/*
 * The other end of lock-wait and lock-lend: it holds the lock until the waiter signals,
 * and takes it again once the waiter is done with it.
 */
static void holder_thread(uint32_t arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits;
        named_lock_take(&holder_lock);
        rv_signal(held_note, 1);
        rv_wait(hold_note, &bits);
        named_lock_give(&holder_lock);
        rv_wait(done_note, &bits);
    }
}

/* Once the holder holds the lock, the waiter wakes it and waits for the lock, which it gives back at once. */
static void round_lock_wait(void)
{
    uint32_t bits;
    rv_wait(held_note, &bits);
    rv_signal(hold_note, 1);
    named_lock_take(&waiter_lock);
    named_lock_give(&waiter_lock);
    rv_signal(done_note, 1);
}

static void round_seqlock_write(void)
{
    seqlock_write(&snap, snap_data);
}

static void round_seqlock_read(void)
{
    uint32_t version;
    seqlock_read(&snap, snap_data, &version);
}

static void round_process(void)
{
    uint32_t bits;
    rv_signal(pong.inbox, CHILD_BIT_PARENT);
    rv_wait(self.inbox, &bits);
}

static void measure(const char *name, void (*round)(void))
{
    uint32_t hz;
    for (uint32_t i = 0; i < WARM_UP; i++) {
        round();
    }
    uint64_t start = now(&hz);
    for (uint32_t i = 0; i < ROUNDS; i++) {
        round();
    }
    report(name, ROUNDS, (uint32_t)(now(&hz) - start), hz);
}

int main(void)
{
    uint32_t base, size, region;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));

    struct block pool;
    must("a pool", mem_take(&self, POOL_SIZE, &pool));
    must("make the pool", mem_make(&self, &pool, CAP_POOL));
    must("a slot", slot_new(&self, &note));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, note, 0));
    measure("call", round_call);
    measure("signal-wait", round_signal_wait);

    uint32_t thread, time, first;
    must("a slot", slot_new(&self, &ping_note));
    must("a slot", slot_new(&self, &pong_note));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, ping_note, 0));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, pong_note, 0));
    must("a slot", slot_new(&self, &thread));
    must("a thread", rv_pool_alloc(pool.made, CAP_THREAD, thread, BOOT_CAP_PROCESS));
    must("configure the thread", rv_thread_configure(thread, (uint32_t)(uintptr_t)pong_thread,
                                                     (uint32_t)(uintptr_t)(pong_stack + sizeof(pong_stack)), 0));
    must("units", units_take(&self, UNITS, &first));
    must("a slot", slot_new(&self, &time));
    must("carve the units", rv_time_carve(self.time, first, UNITS, time));
    must("bind the units", rv_time_bind(time, thread, 0, UNITS));
    must("start the thread", rv_thread_resume(thread));
    measure("threads", round_thread);

    must("a slot", slot_new(&self, &held_note));
    must("a slot", slot_new(&self, &hold_note));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, held_note, 0));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, hold_note, 0));
    must("a slot", slot_new(&self, &done_note));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, done_note, 0));
    must("a slot", slot_new(&self, &lock_note));
    must("a notification", rv_pool_alloc(pool.made, CAP_NOTIFICATION, lock_note, 0));
    waiter_lock = (struct named_lock){ (uint32_t)(uintptr_t)&lock_word, lock_note, 1, { 0 } };
    holder_lock = (struct named_lock){ (uint32_t)(uintptr_t)&lock_word, lock_note, 2, { 0 } };
    named_lock_init(&waiter_lock);
    measure("lock", round_lock);
    uint32_t holder;
    must("a slot", slot_new(&self, &holder));
    must("a thread", rv_pool_alloc(pool.made, CAP_THREAD, holder, BOOT_CAP_PROCESS));
    must("configure the thread", rv_thread_configure(holder, (uint32_t)(uintptr_t)holder_thread,
                                                     (uint32_t)(uintptr_t)(holder_stack + sizeof(holder_stack)), 0));
    must("units", units_take(&self, UNITS, &first));
    must("a slot", slot_new(&self, &time));
    must("carve the units", rv_time_carve(self.time, first, UNITS, time));
    must("bind the units", rv_time_bind(time, holder, 0, UNITS));
    must("start the thread", rv_thread_resume(holder));
    measure("lock-wait", round_lock_wait);
    /* The root task's table holds the holder's thread with every right, RIGHT_X among them. */
    waiter_lock.lend[2] = holder;
    measure("lock-lend", round_lock_wait);

    snap = seqlock_shape((uint32_t)(uintptr_t)snap_memory, SNAPSHOT);
    seqlock_init(&snap);
    measure("seqlock-write", round_seqlock_write);
    measure("seqlock-read", round_seqlock_read);

    must("room for the children's data", self_room(&self, ROOM_SIZE));
    must("build pong", child_new(&self, &pong, "pong", CHILD_TABLE, CHILD_DATA));
    must("start pong", child_start(&self, &pong, pong_main, UNITS));
    measure("processes", round_process);

    /* channel: the ping times its own rounds, and the page says how long they took. */
    struct child ping, echo;
    struct chan link;
    must("build ping", child_new(&self, &ping, "ping", CHILD_TABLE, CHILD_DATA));
    must("build echo", child_new(&self, &echo, "echo", CHILD_TABLE, CHILD_DATA));
    struct ping_page *pp = (struct ping_page *)ping.page, *ep = (struct ping_page *)echo.page;
    pp->rounds = ROUNDS;
    must("give ping the clock", child_give(&self, &ping, BOOT_CAP_CLOCK, RIGHT_R, &pp->clock));
    must("a channel", chan_new(&self, &link, LINK_SIZE, LINK_SLOT));
    must("connect ping and echo", chan_connect(&self, &link, &ping, &pp->link, &echo, &ep->link));
    must("start echo", child_start(&self, &echo, echo_main, UNITS));
    must("start ping", child_start(&self, &ping, ping_main, UNITS));
    while (ping.told != PING_DONE) {
        uint32_t bits, state;
        must("wait for ping", rv_wait(self.inbox, &bits));
        if ((child_poll(&ping, &state) && state == CHILD_FAILED) || (bits & (ping.bit_fault | echo.bit_fault))) {
            say(&out, "bench: the channel's children failed\n");
            halt(1);
        }
    }
    report("channel", ROUNDS, pp->counts, pp->hz);
    must("the channel back", chan_free(&self, &link));
    must("ping back", child_free(&self, &ping));
    must("echo back", child_free(&self, &echo));

    struct child spin;
    must("build spin", child_new(&self, &spin, "spin", CHILD_TABLE, CHILD_DATA));
    must("start spin", child_start(&self, &spin, spin_main, UNITS));
    uint32_t hz, worst = 0;
    uint64_t begun = now(&hz), last = begun, t;
    do {
        round_process();
        t = now(&hz);
        worst = (uint32_t)(t - last) > worst ? (uint32_t)(t - last) : worst;
        last = t;
    } while (t - begun < hz / WINDOWS);
    report("contended-worst", 1, worst, hz);

    say(&out, "bench: done\n");
    halt(0);
}
