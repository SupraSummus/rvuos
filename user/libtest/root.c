/*
 * The library's test: a root task that does with user/lib/ what a program of several processes does,
 * and checks that everything it hands out comes back.
 *
 * It reads bytes left in free memory, as a loader leaves a program's files there;
 * connects two peers with a channel whose rings hold a few packets, and has each send the other more than that;
 * builds a child that stores where it has no region, hears it fault and takes it down, twice;
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

static struct self self, after;
static struct kernel_log klog;
static uint32_t log_bit, deadline_bit;

static struct child a, b, faulty;
static struct child *const children[] = { &a, &b, &faulty };
static uint32_t faulted; /* the bits of the faults the root task heard */

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
static void step(void)
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
        if ((bits & c->bit_fault) && c != &faulty) {
            say(&out, "libtest: FAIL: %s faulted\n", c->name);
            halt(1);
        }
        faulted |= bits & c->bit_fault;
    }
    check(!(bits & deadline_bit), "done before the deadline");
}

/*
 * What the root task holds, as the library counts it: free bytes, and slots, regions, bits and units in use.
 * The slots of free blocks are left out, since halving memory adds blocks that are never joined again.
 */
struct tally {
    uint32_t bytes, slots, regions, bits, units[TIME_UNITS / 32];
};

static struct tally tally(void)
{
    struct tally t = { mem_unused(&self), self.slot_end - self.slot_first - slots_unused(&self), self.regions, self.bits,
                       { 0 } };
    for (uint32_t i = 0; i < self.free_count; i++) {
        t.slots -= self.free[i].untyped >= self.slot_first && self.free[i].untyped < self.slot_end;
    }
    memcpy(t.units, self.units, sizeof(t.units));
    return t;
}

static int same(struct tally x, struct tally y)
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
    struct tally start = tally();
    say(&out, "libtest: up, %u bytes free and %u slots unused\n", start.bytes, slots_unused(&self));

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
    struct tally before = tally();
    uint32_t root_data, root_size;
    must("the root's data", rv_frame_info(BOOT_CAP_DATA, &root_data, &root_size));
    for (uint32_t round = 0; round < 2; round++) {
        must("build the faulting child", child_new(&self, &faulty, "fault", CHILD_TABLE, CHILD_DATA));
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
    check(same(tally(), start), "everything handed out came back");
    say(&out, "libtest: down, %u bytes free and %u slots unused\n", mem_unused(&self), slots_unused(&self));
    say(&out, "libtest: ok\n");
    halt(0);
}
