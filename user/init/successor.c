/* The handover, and what the successor does after it: a thread that faults, and the watchdog, whose halt ends the demo. */

#include <stdint.h>

#include "csrs.h"
#include "init.h"
#include "rvuos.h"

/* What the watchdog waits for, which a sleep of half as long and a feed after each keeps from halting. */
#define WATCHDOG_US 20000u
#define WATCHDOG_FEEDS 3u

/*
 * How long a thread sleeps for its account to fill before it counts on having time ahead of spare time:
 * ten ticks' worth with the whole processor, over four with the root task's part.
 */
#define FILL_US 10000u

/*
 * The prober: a thread of the successor's, in its process, that loads the word at probe_at.
 * The region is not installed the first time, so the load faults,
 * which stops the prober alone, at the load, and signals its watch.
 * Resumed once the region is back, it makes the same load again, which goes through,
 * and its breakpoint after that is a fault its watch hears too.
 * Its watcher then sets its argument and program counter to run prober_again,
 * as one that emulates an instruction steps a thread past it.
 */
static volatile uint32_t probe_at, probed;

static __attribute__((noreturn)) void prober_main(void)
{
    probed = *(volatile uint32_t *)probe_at;
    for (;;) {
        rv_breakpoint();
    }
}

static __attribute__((noreturn)) void prober_again(uint32_t word)
{
    probed = word;
    for (;;) {
        rv_breakpoint();
    }
}

/* What woke the successor on the prober's watch, and whether it was the prober's fault. */
static uint32_t fault_wait(void)
{
    uint32_t bits;
    uint32_t status = rv_wait(SLOT_PROBE_NTFN, &bits);
    if (status != KERR_OK) {
        return status;
    }
    return bits == BIT_FAULT ? KERR_OK : KERR_INVALID_ARG;
}

/*
 * What OP_THREAD_FAULT says of the prober's fault, into *pc and the log,
 * where tests/run.sh holds it to the kernel's own report of the fault, with each architecture's and board's causes.
 */
static uint32_t fault_tell(uint32_t *pc)
{
    uint32_t cause, addr, status;
    uint32_t err = rv_thread_fault(SLOT_PROBER, &cause, pc, &addr, &status);
    if (err != KERR_OK) {
        return err;
    }
    puts("the prober's fault: cause=");
    rv_put_hex(BOOT_CAP_DEBUG, cause);
    puts(" pc=");
    rv_put_hex(BOOT_CAP_DEBUG, *pc);
    puts(" addr=");
    rv_put_hex(BOOT_CAP_DEBUG, addr);
    puts(" status=");
    rv_put_hex(BOOT_CAP_DEBUG, status);
    puts("\n");
    return KERR_OK;
}

/*
 * The successor, in a process and a table of its own,
 * into which the root task moved every capability it held, each to the same slot,
 * so every slot means here what it meant there; see DESIGN.md, "The root task is its capabilities".
 */
static __attribute__((noreturn)) void successor_main(void)
{
    uint32_t base, size, data_base, data_size, pc, at_load, reg, ignored;

    puts("successor: started\n");
    expect("successor: the user-mode csrs set back", csrs_are_reset() ? KERR_OK : KERR_INVALID_ARG);
    expect("stop the root task's thread for good",
           rv_invoke(OP_TIME_BIND, SLOT_FREEZE_TIME, BOOT_CAP_THREAD, 0, 0));
    /*
     * The successor lives in a pool of its own, so nothing keeps it from destroying the boot pool,
     * by a revoke below the block it was made of, which leaves the code both run where it is.
     */
    expect("destroy the boot pool, and the root task with it",
           rv_invoke(OP_CAP_REVOKE, SLOT_SUCC_TABLE, BOOT_CAP_POOL_RAM, 0, 0));
    expect("the root task's thread is gone",
           rv_invoke(OP_THREAD_RESUME, BOOT_CAP_THREAD, 0, 0, 0) == KERR_INVALID_CAP
               ? KERR_OK : KERR_INVALID_ARG);
    /* The units the root task and the logger earned came free with them. */
    expect("earn the whole processor",
           rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_SUCC_THREAD, 0, TIME_UNITS));
    /* Its account starts empty, and earning the whole core stays near a tick while it runs; see TODO.md. */
    expect("let its account fill", sleep_us(FILL_US));
    /* The destroy emptied the root task's own slots; the successor's objects take them. */
    expect("take the root task's table slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_CAPTABLE, SLOT_SUCC_TABLE, RIGHT_ALL));
    expect("take the root task's process slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_PROCESS, SLOT_SUCC_PROCESS, RIGHT_ALL));
    expect("take the root task's thread slot",
           rv_invoke(OP_CAP_COPY, SLOT_SUCC_TABLE, BOOT_CAP_THREAD, SLOT_SUCC_THREAD, RIGHT_ALL));
    puts("root: handover ok\n");

    expect("shared region info", rv_frame_info(SLOT_SHARED, &base, &size));
    expect("map the shared region",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    expect("unmap the shared region",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, 0, 0));

    /*
     * Faults; see DESIGN.md, "Faults".
     * A fault stops the thread that makes it and signals its watch, and the machine goes on.
     * The prober runs in this process on spare time, on the logger's old stack,
     * and its watch is a notification this thread waits on.
     */
    expect("data info", rv_frame_info(BOOT_CAP_DATA, &data_base, &data_size));
    expect("allocate the prober's watch",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_NOTIFICATION, SLOT_PROBE_NTFN, 0));
    expect("allocate the prober",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_THREAD, SLOT_PROBER, BOOT_CAP_PROCESS));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_PROBER, (uint32_t)&prober_main, data_base + data_size / 2, 0));
    expect("watch it", rv_invoke(OP_THREAD_WATCH, SLOT_PROBER, SLOT_PROBE_NTFN, BIT_FAULT, 0));
    expect("bind it to spare time", rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_PROBER, 0, 0));
    /* A new thread's frame is zero, which a cause would be too. */
    expect("a thread that never ran has no fault to tell",
           rv_thread_fault(SLOT_PROBER, &ignored, &ignored, &ignored, &ignored) == KERR_STATE
               ? KERR_OK : KERR_INVALID_ARG);
    probe_at = base;
    probed = 0;

    /*
     * The fault report lands in the log after the logger's last look at it,
     * so the halt writes it out; tests/run.sh reads it there.
     */
    puts("reading the removed region at ");
    rv_put_hex(BOOT_CAP_DEBUG, base);
    puts(", expecting a fault\n");
    expect("start the prober", rv_invoke(OP_THREAD_RESUME, SLOT_PROBER, 0, 0, 0));
    expect("hear its fault", fault_wait());
    expect("the protection unit stopped the load", probed == 0 ? KERR_OK : KERR_INVALID_ARG);
    expect("ask what the fault was", fault_tell(&at_load));
    expect("it stopped where the fault says",
           rv_thread_read_reg(SLOT_PROBER, THREAD_REG_PC, &reg) == KERR_OK && reg == at_load
               ? KERR_OK : KERR_INVALID_ARG);
    expect("map the region again",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    /* Only a stopped thread resumes, so this is also what shows the fault stopped it. */
    expect("resume the prober where it faulted", rv_invoke(OP_THREAD_RESUME, SLOT_PROBER, 0, 0, 0));
    /* It runs on spare time, so not before this thread, which has time, waits. */
    expect("a resume leaves nothing to tell",
           rv_thread_fault(SLOT_PROBER, &ignored, &ignored, &ignored, &ignored) == KERR_STATE
               ? KERR_OK : KERR_INVALID_ARG);
    expect("hear its breakpoint", fault_wait());
    expect("the same load went through", probed == MAGIC ? KERR_OK : KERR_INVALID_ARG);
    expect("the breakpoint is a fault of its own",
           rv_thread_fault(SLOT_PROBER, &ignored, &pc, &ignored, &ignored) == KERR_OK && pc != at_load
               ? KERR_OK : KERR_INVALID_ARG);
    expect("set the prober's argument",
           rv_invoke(OP_THREAD_WRITE_REG, SLOT_PROBER, RV_REG_A0, MAGIC + 2, 0));
    expect("set its program counter",
           rv_invoke(OP_THREAD_WRITE_REG, SLOT_PROBER, THREAD_REG_PC, (uint32_t)&prober_again, 0));
    expect("resume it there", rv_invoke(OP_THREAD_RESUME, SLOT_PROBER, 0, 0, 0));
    expect("hear its breakpoint there", fault_wait());
    expect("it ran from there on what it was given", probed == MAGIC + 2 ? KERR_OK : KERR_INVALID_ARG);
    puts("root: fault ok\n");

    /*
     * The watchdog; see DESIGN.md, "The watchdog".
     * Fed after every sleep, it lets the machine run longer than it waits,
     * and once it is not, it halts the machine before this thread wakes from a sleep twice as long;
     * tests/run.sh holds the run's exit status to the watchdog's code.
     */
    expect("arm the watchdog", rv_clock_watchdog(BOOT_CAP_CLOCK, WATCHDOG_US));
    for (uint32_t i = 0; i < WATCHDOG_FEEDS; i++) {
        expect("sleep half as long", sleep_us(WATCHDOG_US / 2));
        expect("feed the watchdog", rv_clock_watchdog(BOOT_CAP_CLOCK, WATCHDOG_US));
    }
    puts("root: watchdog fed ok\n");
    expect("sleep past the watchdog", sleep_us(2 * WATCHDOG_US));
    puts("root: the watchdog let the machine run\n");
    rv_halt(BOOT_CAP_DEBUG, 0);
}

/*
 * Hand the root task's place to a successor, which runs the same code on the same data, on a stack at sp,
 * and wait for good on a notification kept back.
 * The logger names its capabilities in this table, so it stops first,
 * and the halt writes out what it leaves in the log.
 */
__attribute__((noreturn)) void hand_over(uint32_t sp)
{
    uint32_t base, bits;

    puts("root: handing over\n");
    /* A thread keeps what it runs on: the root task cannot destroy the pool it lives in. */
    expect("the root task cannot destroy its own pool",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, BOOT_CAP_POOL_RAM, 0, 0) == KERR_STATE
               ? KERR_OK : KERR_INVALID_ARG);
    expect("derive every unit without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_FREEZE_TIME, BOOT_CAP_TIME, RIGHT_W));
    expect("stop the logger", rv_invoke(OP_TIME_BIND, SLOT_FREEZE_TIME, SLOT_LOGGER, 0, 0));

    expect("make the successor's pool of what is left of the free ram",
           rv_retype(rest, CAP_POOL, SLOT_SUCC_POOL, &base));
    expect("allocate the successor's table",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_CAPTABLE, SLOT_SUCC_TABLE, ROOT_TABLE_SLOTS));
    expect("allocate the successor's process",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_PROCESS, SLOT_SUCC_PROCESS, SLOT_SUCC_TABLE));
    expect("allocate the successor's thread",
           rv_invoke(OP_POOL_ALLOC, SLOT_SUCC_POOL, CAP_THREAD, SLOT_SUCC_THREAD, SLOT_SUCC_PROCESS));
    expect("map the successor's code",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_SUCC_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X));
    expect("map the successor's data",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_SUCC_PROCESS, 1, BOOT_CAP_DATA, RIGHT_R | RIGHT_W));
    expect("configure the successor",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_SUCC_THREAD, (uint32_t)&successor_main, sp, 0));
    /*
     * On spare time, so it runs once this thread waits, as long as this thread has time until then;
     * spinning drained its account, so it sleeps for it to fill first.
     */
    expect("bind the successor", rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, SLOT_SUCC_THREAD, 0, 0));
    expect("let this thread's account fill", sleep_us(FILL_US));
    expect("start the successor", rv_invoke(OP_THREAD_RESUME, SLOT_SUCC_THREAD, 0, 0, 0));
    expect("allocate what to wait on",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_EXIT_NTFN, 0));
    expect("mark the user-mode csrs", csrs_mark() ? KERR_OK : KERR_INVALID_ARG);

    /* From here on this thread holds no debug capability and says nothing; the empty slots fail. */
    for (uint32_t slot = BOOT_CAP_NULL + 1; slot < ROOT_TABLE_SLOTS; slot++) {
        if (slot != SLOT_SUCC_TABLE && slot != SLOT_EXIT_NTFN) {
            rv_invoke(OP_CAP_MOVE, SLOT_SUCC_TABLE, slot, slot, 0);
        }
    }
    rv_invoke(OP_CAP_MOVE, SLOT_SUCC_TABLE, SLOT_SUCC_TABLE, SLOT_SUCC_TABLE, 0);
    for (;;) {
        rv_wait(SLOT_EXIT_NTFN, &bits);
    }
}
