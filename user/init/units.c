/* Units of the processor, spare time and charging; see DESIGN.md, "Scheduling". */

#include <stdint.h>

#include "init.h"
#include "rvuos.h"

#define UNITS_US 40000u
volatile uint32_t spins[SPINNERS];

/* A thread that spins through most of each tick and sleeps across its end, counting its rounds. */
#define DODGE_US 80000u
static uint32_t dodge_counter, dodge_counts;
static volatile uint32_t dodges;

/* The spinners: each counts in the word its argument names and never waits. */
static void spinner_main(uint32_t i)
{
    for (;;) {
        spins[i]++;
    }
}

/*
 * The dodger: it sleeps until the next tick, spins for dodge_counts of the counter, and counts a round.
 * A timer line set for no delay fires at the next tick, so the dodger is never running when a tick comes.
 */
static void dodger_main(void)
{
    const volatile uint32_t *low = (const volatile uint32_t *)dodge_counter;
    for (;;) {
        uint32_t bits;
        if (rv_timer_set(SLOT_DODGER_TIMER, BIT_TIMER, 0) != KERR_OK ||
            rv_wait(SLOT_DODGER_NTFN, &bits) != KERR_OK || bits != BIT_TIMER) {
            puts("dodger: cannot sleep\n");
            rv_halt(BOOT_CAP_DEBUG, 1);
        }
        uint32_t start = *low;
        while (*low - start < dodge_counts) {
        }
        dodges++;
    }
}

static uint32_t spun(void)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SPINNERS; i++) {
        sum += spins[i];
    }
    return sum;
}

/*
 * Read the counter's low word until it has moved on by counts,
 * and count the gaps between two reads longer than gap; *reads receives how many reads it took.
 */
static uint32_t spin_reads(uint32_t counter, uint32_t counts, uint32_t gap, uint32_t *reads)
{
    const volatile uint32_t *low = (const volatile uint32_t *)counter;
    uint32_t start = *low, last = start, now, gaps = 0, n = 0;
    do {
        now = *low;
        gaps += now - last > gap;
        last = now;
        n++;
    } while (now - start < counts);
    *reads = n;
    return gaps;
}

/*
 * Spin for ms milliseconds and count the traps that came meanwhile:
 * the gaps between two reads of the counter longer than eight reads take on average, plus a step of the counter.
 * A trap costs far more than a read on every board, while the counter's rate against the processor is the board's,
 * so the average comes from a first spin a tenth as long, which a trap would barely move.
 */
static uint32_t spin_traps(uint32_t counter, uint32_t hz, uint32_t ms)
{
    uint32_t counts = hz / 1000u * ms, reads;
    spin_reads(counter, counts / 10u, UINT32_MAX, &reads);
    return spin_reads(counter, counts, 8u * (counts / 10u) / reads + 2u, &reads);
}

void units_demo(uint32_t data_base, uint32_t data_size, uint32_t counter, uint32_t hz)
{
    /*
     * The child is done, and its half goes to three spinners: the first earns a quarter of the processor
     * without spare time, and the other two earn nothing and run on spare time alone.
     * While this thread sleeps, the first runs about one tick in four, however many threads run on the rest,
     * and no more, so the other two spin about three times as far together.
     * The derived capability lies below the half, so revoking below the half stops all three.
     */
    expect("take the child's half back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("derive the half without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    for (uint32_t i = 0; i < SPINNERS; i++) {
        expect("allocate a spinner",
               rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_THREAD, SLOT_SPINNER + i, BOOT_CAP_PROCESS));
        expect("configure it",
               rv_invoke(OP_THREAD_CONFIGURE, SLOT_SPINNER + i, (uint32_t)&spinner_main,
                         data_base + data_size / 4 - i * SPINNER_STACK, i));
        expect("bind it",
               i == 0 ? rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 4)
                      : rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER + i, 0, 0));
        expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_SPINNER + i, 0, 0, 0));
    }
    expect("let them spin", sleep_us(UNITS_US));
    /* Stopped before they are counted, since the tick would give them more turns meanwhile. */
    expect("take the spinners' units back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    uint32_t root_spun = spun(), earned = spins[0], others = spins[1] + spins[2];
    for (uint32_t i = 0; i < SPINNERS; i++) {
        expect("every spinner had a turn", spins[i] != 0 ? KERR_OK : KERR_INVALID_ARG);
    }
    expect("the quarter ran a quarter of the time",
           2 * others > 3 * earned && others < 6 * earned ? KERR_OK : KERR_INVALID_ARG);
    puts("root: units ok\n");

    /* A thread bound to no units keeps its state and does not run, whoever else does. */
    expect("sleep while they have no units", sleep_us(SLEEP_US));
    expect("no thread without units ran", spun() == root_spun ? KERR_OK : KERR_INVALID_ARG);
    puts("root: unbind ok\n");

    /* Bound again, a thread goes on where it stopped: the first spinner, on spare time alone. */
    uint32_t first_spun = spins[0];
    expect("bind a spinner again",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER, 0, 0));
    expect("sleep while it spins", sleep_us(SLEEP_US));
    expect("take its units back again",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it spun, and only it",
           spins[0] > first_spun && spun() - spins[0] == root_spun - first_spun ? KERR_OK : KERR_INVALID_ARG);
    puts("root: rebind ok\n");

    /*
     * Spare time; see DESIGN.md, "Scheduling".
     * Without spare time a thread runs no more than its units earn, however idle the processor is otherwise.
     * With an eighth and an account that starts empty, the spinner runs about 10 of the 81 ticks of the sleep,
     * one in eight, and the processor idles the rest;
     * with spare time it runs all of the next 41, since nothing else wants the processor.
     */
    expect("derive the half without spare time again",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    first_spun = spins[0];
    expect("bind the spinner to an eighth without spare time",
           rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8));
    expect("sleep while it earns its time", sleep_us(CAPPED_US));
    uint32_t capped = spins[0] - first_spun;
    expect("give it spare time",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8));
    first_spun = spins[0];
    expect("sleep while it runs on spare time", sleep_us(CAPPED_US / 2));
    uint32_t spare = spins[0] - first_spun;
    expect("take its units back for good",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it ran an eighth of the time, and all of the spare time",
           2 * capped < spare && 8 * capped > spare ? KERR_OK : KERR_INVALID_ARG);
    puts("root: spare ok\n");

    /*
     * Charging; see DESIGN.md, "Scheduling".
     * A turn costs the counts it ran, so a thread cannot dodge the charge by sleeping across every tick.
     * The dodger earns an eighth without spare time and spins nine tenths of each tick it runs:
     * it runs about ten rounds in the 81 ticks of the sleep, where a charge of whoever the tick found running let it run some seventy.
     */
    dodge_counter = counter;
    /* Nine tenths of a millisecond, a tick on every board so far. */
    dodge_counts = hz / 1000u * 9u / 10u;
    expect("carve a second timer line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 1, 1, SLOT_DODGER_TIMER));
    expect("bind it for the dodger",
           rv_invoke(OP_IRQ_BIND, SLOT_DODGER_TIMER, SLOT_NEW_POOL, SLOT_DODGER_NTFN, SLOT_DODGER_TIMER));
    expect("allocate the dodger",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_THREAD, SLOT_DODGER, BOOT_CAP_PROCESS));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_DODGER, (uint32_t)&dodger_main,
                     data_base + data_size / 4 - SPINNERS * SPINNER_STACK, 0));
    expect("derive the half without spare time once more",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    expect("bind the dodger to an eighth without spare time",
           rv_invoke(OP_TIME_BIND, SLOT_CAPPED_TIME, SLOT_DODGER, 0, TIME_UNITS / 8));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_DODGER, 0, 0, 0));
    expect("sleep while it dodges the ticks", sleep_us(DODGE_US));
    /* Stopped before its rounds are counted, and its timer line cancelled, so that nothing wakes it again. */
    expect("take its units back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("cancel its timer", rv_timer_set(SLOT_DODGER_TIMER, 0, 0));
    expect("it paid for the time it ran, though no tick found it running",
           dodges != 0 && 4 * dodges < DODGE_US / 1000u ? KERR_OK : KERR_INVALID_ARG);
    puts("root: charge ok\n");

    /*
     * The tick; see DESIGN.md, "Scheduling".
     * The timer interrupts only at a tick that could change what runs,
     * so a thread that spins while every other thread waits runs without a trap,
     * since it may go on with spare time whatever its account holds,
     * where a tick at every millisecond would end its turn only to hand it back.
     * It sleeps first so that the logger carries everything out and waits, and prints nothing as it spins.
     */
    uint32_t status = sleep_us(SLEEP_US);
    uint32_t traps = spin_traps(counter, hz, SPIN_US / 1000u);
    expect("let the logger finish", status);
    expect("the only thread to run spun through the ticks",
           4 * traps < SPIN_US / 1000u ? KERR_OK : KERR_INVALID_ARG);
    puts("root: tickless ok\n");
}
