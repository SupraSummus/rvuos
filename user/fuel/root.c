/*
 * The fuel prototype's test, for a kernel built with make FUEL=1; see FUEL in kernel/sched.c.
 *
 *   power   a child with 8 units and no spare time, its fuel 20 ms of the clock's counts, stops once it ran them,
 *           eight times as long after it started, since its units are an eighth of the core
 *   energy  a child with spare time and the same fuel stops once it ran them, on the idle core about as long after
 *   refill  given 10 ms more and resumed, the first stops again once it ran those, sooner than eight times their length,
 *           as the account it saved while stopped runs first
 *   lent    a client with 10 ms of fuel lends its time to a server with none of its own, which spins on it:
 *           the client's fuel pays, its lend ends with KERR_LIMIT as it runs dry, and the client stops as it runs
 *
 * Each says what it measured, in microseconds and in the children's rounds of work, and the run ends with "fuel: done".
 */

#include "console.h"
#include "fuel.h"
#include "lib/libc.h"
#include "lib/log.h"
#include "lib/say.h"

#define ROOT_UNITS  8u
#define UNITS       8u
#define CHILD_TABLE 16u
#define CHILD_DATA  0x1000u
#define ROOM_SIZE   0x8000u
#define TIMEOUT_US  2000000u

static struct self self;
static struct kernel_log klog;
static const struct out out = { log_write, (void *)BOOT_CAP_PRINT };
static uint32_t timer, timer_bit, hz;

static void console_byte(char c)
{
    console_put_polled(c);
}

static void drain(void)
{
    kernel_log_take(&klog, console_byte);
    console_flush();
}

static __attribute__((noreturn)) void halt(uint32_t code)
{
    drain();
    rv_halt(BOOT_CAP_HALT, code);
}

static void must(const char *what, uint32_t status)
{
    if (status != KERR_OK) {
        say(&out, "fuel: %s failed", what);
        if (self.what != 0) {
            say(&out, " at %s", self.what);
        }
        say(&out, ": %x\n", status);
        halt(2);
    }
}

/* The clock's low word in microseconds, enough for the differences here; 64-bit division is no library's. */
static uint32_t now_us(void)
{
    uint64_t t;
    uint32_t counter;
    must("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &t, &hz, &counter));
    return (uint32_t)t / (hz / 1000000u);
}

static uint32_t fuel_of(struct child *c, uint32_t mode, uint32_t amount)
{
    uint32_t left = 0;
    must("fuel", rv_process_fuel(c->process, mode, amount, &left));
    return left;
}

static struct fuel_page *page_of(struct child *c)
{
    return (struct fuel_page *)c->page;
}

/* Waits for a child's fault bit, which a dry fuel sets as a fault does, and halts at the timeout. */
static void wait_stop(struct child *c)
{
    must("set the timer", rv_timer_set(timer, timer_bit, TIMEOUT_US));
    for (;;) {
        uint32_t bits;
        must("wait", rv_wait(self.inbox, &bits));
        if (bits & c->bit_fault) {
            return;
        }
        if (bits & timer_bit) {
            uint32_t left = 0;
            rv_process_fuel(c->process, FUEL_READ, 0, &left);
            say(&out, "fuel: %s did not stop, left=%u spins=%u state=%u\n", c->name, left, page_of(c)->spins,
                c->page->state);
            halt(1);
        }
    }
}

/* Binds a started child to its units again, through a capability without RIGHT_X, so that it has no spare time. */
static void no_spare(struct child *c)
{
    uint32_t carved, capped;
    must("a slot", slot_new(&self, &carved));
    must("a slot", slot_new(&self, &capped));
    must("carve the units", rv_time_carve(self.time, c->unit, c->units, carved));
    must("derive them without spare time", rv_cap_derive(self.table, capped, carved, RIGHT_W));
    must("bind them", rv_time_bind(capped, c->thread, 0, c->units));
    must("slot back", slot_free(&self, carved));
    must("slot back", slot_free(&self, capped));
}

/* Binds a started child to no units through a capability without RIGHT_X, so that it runs on lent time alone. */
static void lent_alone(struct child *c)
{
    uint32_t unit, carved, capped;
    must("a unit of the core", core_unit(&self, 0, &unit));
    must("a slot", slot_new(&self, &carved));
    must("a slot", slot_new(&self, &capped));
    must("carve a unit", rv_time_carve(self.time, unit, 1, carved));
    must("derive it without spare time", rv_cap_derive(self.table, capped, carved, RIGHT_W));
    must("bind none of it", rv_time_bind(capped, c->thread, 0, 0));
    must("slot back", slot_free(&self, carved));
    must("slot back", slot_free(&self, capped));
}

/* A child that runs fuel_us of the clock's counts, how long it took, and how many rounds it did. */
static void run_dry(const char *what, struct child *c, uint32_t fuel_us, int spare)
{
    fuel_of(c, FUEL_GIVE, fuel_us * (hz / 1000000u));
    uint32_t t0 = now_us();
    if (c->page->state == CHILD_STARTING) {
        must("start", child_start(&self, c, spin_main, 0, UNITS));
        if (!spare) {
            no_spare(c);
        }
    } else {
        must("resume", rv_thread_resume(c->thread));
    }
    wait_stop(c);
    uint32_t took = now_us() - t0;
    say(&out, "fuel: %s %s fuel=%u us took=%u us left=%u spins=%u\n", what, c->name, fuel_us, took,
        fuel_of(c, FUEL_READ, 0), page_of(c)->spins);
}

int main(void)
{
    uint32_t base, size, region;
    must("the root's own", self_root(&self, ROOT_UNITS));
    must("the console", rv_frame_info(BOOT_CAP_UART, &base, &size));
    must("install the console", region_install(&self, BOOT_CAP_UART, RIGHT_R | RIGHT_W, &region));
    console_init(base);
    must("the kernel's log", kernel_log_open(&self, &klog));
    must("room", self_room(&self, ROOM_SIZE));
    must("the root's timer", timer_new(&self, &timer));
    must("the root's timer bit", bit_new(&self, &timer_bit));
    now_us();

    struct child capped, free_running, client, server;
    must("build", child_new(&self, &capped, "capped", CHILD_TABLE, CHILD_DATA));
    must("build", child_new(&self, &free_running, "spare", CHILD_TABLE, CHILD_DATA));
    run_dry("power", &capped, 20000u, 0);
    run_dry("energy", &free_running, 20000u, 1);
    run_dry("refill", &capped, 10000u, 0);

    /* The server has no units and no spare time: it runs on what the client lends it alone. */
    must("build", child_new(&self, &client, "client", CHILD_TABLE, CHILD_DATA));
    must("build", child_new(&self, &server, "server", CHILD_TABLE, CHILD_DATA));
    must("give the client the server's thread",
         child_give(&self, &client, server.thread, RIGHT_X, &page_of(&client)->lend_to));
    must("start the server", child_start(&self, &server, spin_main, 0, 0));
    lent_alone(&server);
    fuel_of(&client, FUEL_GIVE, 10000u * (hz / 1000000u));
    uint32_t t0 = now_us();
    must("start the client", child_start(&self, &client, client_main, 0, UNITS));
    wait_stop(&client);
    uint32_t took = now_us() - t0;
    say(&out, "fuel: lent client fuel=10000 us took=%u us left=%u lend=%x server-spins=%u client-spins=%u\n",
        took, fuel_of(&client, FUEL_READ, 0), page_of(&client)->lent & 0xffu, page_of(&server)->spins,
        page_of(&client)->spins);
    say(&out, "fuel: done\n");
    halt(0);
}
