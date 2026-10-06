/*
 * Cores; see DESIGN.md, "Cores".
 * BOOT_CAP_TIME holds the units of every core, TIME_UNITS of each, numbered core by core,
 * so the units of core n can be carved out of it only where there is a core n.
 * A thread runs on the core its units are of, and only there.
 */

#include <stdint.h>

#include "console.h"
#include "init.h"
#include "rvuos.h"

/*
 * The second core's threads: the ponger answers PINGS pings, each within WAKE_US,
 * the reader reads the word at read_at, and the lost thread counts until it is destroyed,
 * having first added ADDS to added an atomic add at a time, as this thread does beside it.
 * Their stacks lie below the dodger's.
 */
#define PINGS 8u
#define WAKE_US 500000u
#define ADDS 20000u
#define CORE_STACK(i) (SPINNERS + 1u + (i))
static volatile uint32_t pongs, read_count, read_at, lost_spins, added;

static void ponger_main(void)
{
    for (;;) {
        uint32_t bits;
        if (rv_wait(SLOT_PING, &bits) != KERR_OK || bits != BIT_PING) {
            puts("ponger: cannot wait\n");
            rv_halt(BOOT_CAP_DEBUG, 1);
        }
        pongs++;
        rv_signal(SLOT_TIMER_NTFN, BIT_PONG);
    }
}

static void reader_main(void)
{
    for (;;) {
        (void)*(volatile uint32_t *)read_at;
        read_count++;
    }
}

static void add_atomically(void)
{
    for (uint32_t i = 0; i < ADDS; i++) {
        __atomic_fetch_add(&added, 1u, __ATOMIC_RELAXED);
    }
}

static void lost_main(void)
{
    add_atomically();
    for (;;) {
        lost_spins++;
    }
}

/*
 * Spin, making no call, until *word moves on from was or counts of the counter pass; whether it moved.
 * The timer is set for the next tick first, and its bit taken after,
 * so that a machine that runs its cores in turns, as QEMU does, turns to the other core by then:
 * QEMU runs one core until the next deadline of any timer, or until it waits.
 * The counter is read before the word, so a word that has not moved had not by then either,
 * however long a trap or the other core's turn came between the two.
 */
static int moved_while_spinning(const volatile uint32_t *word, uint32_t was, uint32_t counter, uint32_t counts)
{
    const volatile uint32_t *low = (const volatile uint32_t *)counter;
    expect("set the timer for the next tick", rv_timer_set(SLOT_TIMER, BIT_TIMER, 0));
    uint32_t start = *low;
    uint32_t passed;
    int moved;
    do {
        passed = *low - start;
        moved = *word != was;
    } while (!moved && passed < counts);
    expect("take the timer's bit", timer_wait());
    return moved;
}

static uint32_t count_cores(void)
{
    uint32_t n = 1;
    while (n < 256u && rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, n * TIME_UNITS, TIME_UNITS, SLOT_CORE_TIME) == KERR_OK) {
        expect("let the core's units go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_CORE_TIME, 0, 0));
        n++;
    }
    return n;
}

static void second_core(uint32_t data_base, uint32_t data_size, uint32_t shared_base, uint32_t counter, uint32_t hz)
{
    uint32_t base, bits, status, was, cause, pc, addr, fault_status;
    uint32_t counts = hz / 1000u * (SPIN_US / 1000u);
    uint32_t stack = data_base + data_size / 4;

#ifdef CORES_IRQ
    /* Where each core's controller is its own, the line the cores interrupt each other on is the kernel's. */
    expect("carve the cores' line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, CORES_IRQ, 1, SLOT_SPARE_LINE));
    expect("the cores' line is the kernel's",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE, SLOT_POOL, SLOT_TIMER_NTFN, SLOT_SPARE_IRQ) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);
    expect("let it go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_SPARE_LINE, 0, 0));
#endif

    /* The spinner earns the whole second core and nothing of this one, so it spins there while this thread does too. */
    expect("carve the second core's units",
           rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS, TIME_UNITS, SLOT_CORE_TIME));
    was = spins[0];
    expect("bind a spinner to the whole of the second core",
           rv_invoke(OP_TIME_BIND, SLOT_CORE_TIME, SLOT_SPINNER, 0, TIME_UNITS));
    expect("it spins while this thread spins, making no call",
           moved_while_spinning(&spins[0], was, counter, counts) ? KERR_OK : KERR_INVALID_ARG);
    puts("root: second core ok\n");

    /*
     * Alone on the second core it has the whole of it while this thread sleeps.
     * Bound to an eighth of this core without spare time while it runs there, it finishes its turn there
     * and comes here, where once its account has drained it runs an eighth of the time.
     */
    was = spins[0];
    expect("sleep while it spins there", sleep_us(CAPPED_US));
    uint32_t there = spins[0] - was;
    expect("derive the half without spare time to move it",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    expect("move the spinner to an eighth of this core",
           rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8));
    expect("let its account drain", sleep_us(CAPPED_US));
    was = spins[0];
    expect("sleep while it spins here", sleep_us(CAPPED_US));
    uint32_t here = spins[0] - was;
    expect("take its units back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it ran an eighth of this core, where it had the whole of the other",
           here != 0 && 3 * here < there ? KERR_OK : KERR_INVALID_ARG);
    puts("root: move ok\n");

    /*
     * The ponger runs on spare time on the second core and waits for each ping,
     * so that core idles in between, and a signal from this one has to wake it.
     * An idle core's timer wakes only for the timer lines armed there, and this thread's is armed here,
     * so a ping that did not interrupt it would not be answered before the timer fires.
     * How much sooner is not checked:
     * under icount QEMU runs the harts in turns on one host thread,
     * and a hart that waits for the kernel's lock keeps it for the rest of its slice, tens of milliseconds.
     */
    expect("allocate what the ponger waits on",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_PING, 0));
    expect("allocate the ponger", rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_PONGER, BOOT_CAP_PROCESS));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_PONGER, (uint32_t)&ponger_main,
                     stack - CORE_STACK(0) * SPINNER_STACK, 0));
    expect("bind it to spare time on the second core", rv_invoke(OP_TIME_BIND, SLOT_CORE_TIME, SLOT_PONGER, 0, 0));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_PONGER, 0, 0, 0));
    status = KERR_OK;
    for (uint32_t i = 0; i < PINGS && status == KERR_OK; i++) {
        status = rv_timer_set(SLOT_TIMER, BIT_TIMER, WAKE_US);
        if (status == KERR_OK) {
            status = rv_signal(SLOT_PING, BIT_PING);
        }
        if (status == KERR_OK) {
            status = rv_wait(SLOT_TIMER_NTFN, &bits);
        }
        if (status == KERR_OK && (bits & (BIT_PONG | BIT_TIMER)) != BIT_PONG) {
            status = KERR_INVALID_ARG;
        }
    }
    expect("cancel the timer", rv_timer_set(SLOT_TIMER, 0, 0));
    expect("every ping was answered from the second core",
           status == KERR_OK && pongs == PINGS ? KERR_OK : KERR_INVALID_ARG);
    puts("root: wake across cores ok\n");

    /*
     * The reader reads the shared region on the second core, which holds the region in its protection unit
     * until it traps: the uninstall makes it trap before it returns, so the reader's next read faults,
     * and it counts at most the one read it was in when its core trapped.
     * It reads the second word, so that its fault does not pass for the prober's, which reads the first.
     */
    read_at = shared_base + 4;
    expect("allocate the reader", rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_READER, BOOT_CAP_PROCESS));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_READER, (uint32_t)&reader_main,
                     stack - CORE_STACK(1) * SPINNER_STACK, 0));
    expect("watch it on the timer's notification",
           rv_invoke(OP_THREAD_WATCH, SLOT_READER, SLOT_TIMER_NTFN, BIT_FAULT, 0));
    expect("bind it to the whole of the second core",
           rv_invoke(OP_TIME_BIND, SLOT_CORE_TIME, SLOT_READER, 0, TIME_UNITS));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_READER, 0, 0, 0));
    expect("it reads while this thread spins",
           moved_while_spinning(&read_count, 0, counter, counts) ? KERR_OK : KERR_INVALID_ARG);
    expect("arm the timer", rv_timer_set(SLOT_TIMER, BIT_TIMER, WAKE_US));
    expect("unmap the shared region under the reader",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, 0, 0));
    was = read_count;
    expect("hear the reader's fault",
           rv_wait(SLOT_TIMER_NTFN, &bits) == KERR_OK && (bits & BIT_FAULT) != 0 ? KERR_OK : KERR_INVALID_ARG);
    expect("cancel the timer", rv_timer_set(SLOT_TIMER, 0, 0));
    expect("it faulted reading the region",
           rv_thread_fault(SLOT_READER, &cause, &pc, &addr, &fault_status) == KERR_OK &&
                   (addr == read_at || addr == 0)
               ? KERR_OK : KERR_INVALID_ARG);
    expect("it read nothing once the unmap returned", read_count - was <= 1 ? KERR_OK : KERR_INVALID_ARG);
    puts("root: shootdown ok\n");

    /*
     * A thread destroyed while it runs on the second core: that core traps before the thread's memory goes,
     * so the thread counts no more, and the core runs the next thread it is given.
     * Before it counts, it adds to one word as this thread does, and no add of either core may be lost:
     * on the Cortex-M33 an exclusive pair reaches the other core only where the MPU marks the memory Shareable.
     */
    expect("take the second core's units back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_CORE_TIME, 0, 0));
    expect("make a pool to destroy", rv_retype(SLOT_DOOMED_MEMORY, CAP_POOL, SLOT_DOOMED_POOL, &base));
    expect("allocate a thread in it",
           rv_invoke(OP_POOL_ALLOC, SLOT_DOOMED_POOL, CAP_THREAD, SLOT_LOST, BOOT_CAP_PROCESS));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_LOST, (uint32_t)&lost_main, stack - CORE_STACK(2) * SPINNER_STACK, 0));
    expect("bind it to the whole of the second core",
           rv_invoke(OP_TIME_BIND, SLOT_CORE_TIME, SLOT_LOST, 0, TIME_UNITS));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_LOST, 0, 0, 0));
    add_atomically();
    expect("it counts while this thread spins",
           moved_while_spinning(&lost_spins, 0, counter, counts) ? KERR_OK : KERR_INVALID_ARG);
    expect("no add of either core was lost", added == 2 * ADDS ? KERR_OK : KERR_INVALID_ARG);
    expect("destroy its pool while it runs",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_DOOMED_MEMORY, 0, 0));
    expect("it is gone",
           rv_invoke(OP_THREAD_RESUME, SLOT_LOST, 0, 0, 0) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    expect("it counts no more while this thread spins",
           !moved_while_spinning(&lost_spins, lost_spins, counter, counts) ? KERR_OK : KERR_INVALID_ARG);
    was = spins[0];
    expect("give the second core to a spinner",
           rv_invoke(OP_TIME_BIND, SLOT_CORE_TIME, SLOT_SPINNER, 0, TIME_UNITS));
    expect("the core runs it", moved_while_spinning(&spins[0], was, counter, counts) ? KERR_OK : KERR_INVALID_ARG);
    expect("take the second core's units back again",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_CORE_TIME, 0, 0));
    puts("root: taken from its core ok\n");
}

void cores_demo(uint32_t data_base, uint32_t data_size, uint32_t shared_base, uint32_t counter, uint32_t hz)
{
    uint32_t cores = count_cores();
    puts("root: cores ");
    rv_put_hex(BOOT_CAP_DEBUG, cores);
    puts("\n");
    if (cores > 1) {
        second_core(data_base, data_size, shared_base, counter, hz);
    }
}
