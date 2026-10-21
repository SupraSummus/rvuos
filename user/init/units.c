/* Units of the processor, spare time and charging; see DESIGN.md, "Scheduling". */

#include <stdint.h>

#include "init.h"
#include "rvuos.h"

#define UNITS_US 40000u
volatile uint32_t spins[SPINNERS];

/* A thread that does a round of work and waits briefly between, counting what it did. */
#define DODGE_US 80000u
#define DODGE_ROUND 1000u
static volatile uint32_t dodged;
static uint32_t dodge_counter;

/* The spinners: each counts in the word its argument names and never waits. */
static void spinner_main(uint32_t i)
{
    for (;;) {
        spins[i]++;
    }
}

/*
 * The dodger: it sleeps for no time, then counts DODGE_ROUND steps as a spinner does.
 * A timer line set for no delay fires at once, so the dodger waits only for as long as the kernel takes to wake it.
 */
static void dodger_main(void)
{
    for (;;) {
        uint32_t bits;
        if (rv_timer_set(SLOT_DODGER_TIMER, BIT_TIMER, 0) != KERR_OK ||
            rv_wait(SLOT_DODGER_NTFN, &bits) != KERR_OK || bits != BIT_TIMER) {
            puts("dodger: cannot sleep\n");
            rv_halt(BOOT_CAP_HALT, 1);
        }
        for (uint32_t i = 0; i < DODGE_ROUND; i++) {
            dodged++;
        }
    }
}

/* The probe: it notes the count it first ran at, tells the root task, and spins. */
#define BIT_PROBE 0x1u
#define BIT_WAKE  0x2u
#define CATCH_US 10000u /* longer than a virtual time lies ahead of the clock, a span at most */
static volatile uint32_t probe_at, probe_span;
static void probe_main(void)
{
    probe_at = *(const volatile uint32_t *)dodge_counter;
    rv_signal(SLOT_DODGER_NTFN, BIT_PROBE);
    for (;;) {
    }
}

/*
 * The probe again, as a driver: it waits for its wake within probe_span, then does as probe_main does.
 * A timer bit the requests left pending comes first, and it waits again.
 */
static void waiting_probe_main(void)
{
    uint32_t bits = 0;
    while (!(bits & BIT_WAKE)) {
        if (rv_wait_within(SLOT_DODGER_NTFN, probe_span, &bits) != KERR_OK) {
            puts("probe: cannot wait\n");
            rv_halt(BOOT_CAP_HALT, 1);
        }
    }
    probe_main();
}

/*
 * The driver: each wake it works for driver_work counts of the counter, as a driver serving its device would,
 * counts the wake, and sleeps a period. A bit the probe left pending wakes it once more, which changes nothing.
 */
#define DRIVER_PERIOD_US 2000u
#define DRIVER_US 40000u
static volatile uint32_t driver_work, driver_wakes;
static void driver_main(void)
{
    const volatile uint32_t *low = (const volatile uint32_t *)dodge_counter;
    for (;;) {
        uint32_t start = *low, bits;
        while (*low - start < driver_work) {
        }
        driver_wakes++;
        if (rv_timer_set(SLOT_DODGER_TIMER, BIT_TIMER, DRIVER_PERIOD_US) != KERR_OK ||
            rv_wait(SLOT_DODGER_NTFN, &bits) != KERR_OK) {
            puts("driver: cannot sleep\n");
            rv_halt(BOOT_CAP_HALT, 1);
        }
    }
}

/* Run the driver on units of the half without spare time for DRIVER_US; say its wakes and overruns meanwhile. */
static void drive(uint32_t units, uint32_t *wakes, uint32_t *overruns)
{
    uint32_t before, after, w0 = driver_wakes;
    expect("read the driver's overruns", rv_thread_overruns(SLOT_DODGER, &before));
    expect("derive the half without spare time for it",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    expect("bind the driver", rv_time_bind(SLOT_CAPPED_TIME, SLOT_DODGER, 0, units, 0));
    expect("let it drive", sleep_us(DRIVER_US));
    expect("take its units back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("read them again", rv_thread_overruns(SLOT_DODGER, &after));
    *wakes = driver_wakes - w0;
    *overruns = after - before;
    puts("root: driver on units ");
    rv_put_hex(BOOT_CAP_PRINT, units);
    puts(": wakes ");
    rv_put_hex(BOOT_CAP_PRINT, *wakes);
    puts(" overruns ");
    rv_put_hex(BOOT_CAP_PRINT, *overruns);
    puts("\n");
}

/*
 * Wait on the notification until the probe's bit comes, or counts of the counter have passed since start:
 * a timer bit the dodger left pending may come first, which a wait takes at once. Returns the bits taken.
 */
static uint32_t wait_probe(uint32_t ntfn, uint32_t counter, uint32_t start, uint32_t counts)
{
    const volatile uint32_t *low = (const volatile uint32_t *)counter;
    uint32_t bits = 0, got;
    do {
        expect("wait", rv_wait(ntfn, &got));
        bits |= got;
    } while (!(bits & BIT_PROBE) && *low - start < counts);
    return bits;
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
     * The clock tells its rate with no right, and the default span, the longest power of two of counts within a millisecond,
     * which a request is due within when no wait gave another.
     */
    uint32_t type, rights, clock[3];
    expect("describe the clock", rv_cap_info(BOOT_CAP_CLOCK, &type, &rights, clock));
    uint32_t ms_counts = hz / 1000u, default_span = clock[1];
    expect("it tells the rate a read does, and the default span",
           type == CAP_CLOCK && clock[0] == hz && (default_span & (default_span - 1)) == 0 && default_span <= ms_counts &&
                   2 * default_span > ms_counts
               ? KERR_OK : KERR_INVALID_ARG);

    /*
     * The child is done, and its half goes to three spinners: the first earns a quarter of the processor
     * without spare time, and the other two earn nothing and run on spare time alone.
     * While this thread sleeps, the first runs about a quarter of each span, however many threads run on the rest,
     * and no more, so the other two spin about three times as far together.
     * The derived capability lies below the half, so revoking below the half stops all three.
     */
    expect("take the child's half back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("derive the half without spare time",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    for (uint32_t i = 0; i < SPINNERS; i++) {
        expect("allocate a spinner",
               rv_thread_alloc(BOOT_CAP_POOL, SLOT_SPINNER + i, BOOT_CAP_PROCESS, 1));
        expect("configure it",
               rv_invoke(OP_THREAD_CONFIGURE, SLOT_SPINNER + i, (uint32_t)&spinner_main,
                         data_base + data_size / 4 - i * SPINNER_STACK, i));
        expect("bind it",
               i == 0 ? rv_time_bind(SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 4, 0)
                      : rv_time_bind(SLOT_HALF_TIME, SLOT_SPINNER + i, 0, 0, 0));
        expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_SPINNER + i, 0, 0, 0));
    }
    expect("let them spin", sleep_us(UNITS_US));
    /* Stopped before they are counted, since they would have more turns meanwhile. */
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
           rv_time_bind(SLOT_HALF_TIME, SLOT_SPINNER, 0, 0, 0));
    expect("sleep while it spins", sleep_us(SLEEP_US));
    expect("take its units back again",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("it spun, and only it",
           spins[0] > first_spun && spun() - spins[0] == root_spun - first_spun ? KERR_OK : KERR_INVALID_ARG);
    puts("root: rebind ok\n");

    /*
     * Spare time; see DESIGN.md, "Scheduling".
     * Without spare time a thread runs no more than its units earn, however idle the processor is otherwise.
     * With an eighth the spinner runs an eighth of each span of the sleep, waiting the rest for its virtual time,
     * and the processor idles meanwhile;
     * with spare time it runs all of the next sleep, half as long, since nothing else wants the processor.
     */
    expect("derive the half without spare time again",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    first_spun = spins[0];
    expect("bind the spinner to an eighth without spare time",
           rv_time_bind(SLOT_CAPPED_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8, 0));
    expect("sleep while it earns its time", sleep_us(CAPPED_US));
    uint32_t capped = spins[0] - first_spun;
    expect("give it spare time",
           rv_time_bind(SLOT_HALF_TIME, SLOT_SPINNER, 0, TIME_UNITS / 8, 0));
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
     * A turn costs the counts it ran, and a wait forgives none of them, so a thread cannot dodge the charge by waiting:
     * a request starts only once its virtual time has come to the clock.
     * The dodger earns an eighth without spare time, as the spinner did, and waits briefly between rounds of counting:
     * it counts no faster than the spinner did, less what its waits cost it,
     * where a wait that brought its virtual time back to the clock would let it count about eight times as fast.
     */
    dodge_counter = counter;
    expect("carve a second timer line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 1, 1, SLOT_DODGER_TIMER));
    expect("bind it for the dodger",
           rv_invoke(OP_IRQ_BIND, SLOT_DODGER_TIMER, SLOT_NEW_POOL, SLOT_DODGER_NTFN, SLOT_DODGER_TIMER));
    expect("allocate the dodger",
           rv_thread_alloc(BOOT_CAP_POOL, SLOT_DODGER, BOOT_CAP_PROCESS, 1));
    expect("configure it",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_DODGER, (uint32_t)&dodger_main,
                     data_base + data_size / 4 - SPINNERS * SPINNER_STACK, 0));
    expect("derive the half without spare time once more",
           rv_invoke(OP_CAP_DERIVE, BOOT_CAP_CAPTABLE, SLOT_CAPPED_TIME, SLOT_HALF_TIME, RIGHT_W));
    expect("bind the dodger to an eighth without spare time",
           rv_time_bind(SLOT_CAPPED_TIME, SLOT_DODGER, 0, TIME_UNITS / 8, 0));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_DODGER, 0, 0, 0));
    expect("sleep while it dodges the charge", sleep_us(DODGE_US));
    /* Stopped before its rounds are counted, and its timer line cancelled, so that nothing wakes it again. */
    expect("take its units back",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("cancel its timer", rv_timer_set(SLOT_DODGER_TIMER, 0, 0));
    expect("it paid for the time it ran, though it waited between",
           dodged != 0 && (uint64_t)dodged * CAPPED_US < 2ull * capped * DODGE_US ? KERR_OK : KERR_INVALID_ARG);
    puts("root: charge ok\n");

    /*
     * No tick; see DESIGN.md, "Scheduling".
     * The timer interrupts only at a count that could change what runs,
     * so a thread that spins while every other thread waits runs without a trap,
     * since it may go on with spare time whatever its requests come to,
     * where a timer at the end of each would end its turn only to hand it back.
     * It lets the logger carry everything out first, and prints nothing as it spins.
     */
    uint32_t status = logger_finish();
    uint32_t traps = spin_traps(counter, hz, SPIN_US / 1000u);
    expect("let the logger finish", status);
    expect("the only thread to run spun with hardly a trap",
           4 * traps < SPIN_US / 1000u ? KERR_OK : KERR_INVALID_ARG);
    puts("root: tickless ok\n");

    /*
     * Requests; see DESIGN.md, "Scheduling".
     * A spinner and the probe start together, their virtual times come to the clock, with nearly equal units.
     * The spinner's requests are due within half a millisecond or less, each an eighth of its span's work at its 8 units,
     * so its first is due first and it runs;
     * the probe's are due within twice as long, and it starts first, so its first is due before the spinner's second:
     * it runs as the spinner's first request ends, which the timer marks.
     * This thread says nothing until it waits, so that no print spends its own request, due before the probe's.
     */
    uint32_t span = rv_span(ms_counts / 2), was;
    expect("stop the spinner", rv_thread_stop(SLOT_SPINNER, &was));
    expect("stop the dodger", rv_thread_stop(SLOT_DODGER, &was));
    expect("make it the probe",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_DODGER, (uint32_t)&probe_main,
                     data_base + data_size / 4 - SPINNERS * SPINNER_STACK, 0));
    expect("bind the spinner to 8 units in requests due within half a millisecond",
           rv_time_bind(SLOT_HALF_TIME, SLOT_SPINNER, 0, 8, span));
    expect("bind the probe to 9 in requests due within twice that", rv_time_bind(SLOT_HALF_TIME, SLOT_DODGER, 8, 9, 2 * span));
    uint32_t caught = sleep_us(CATCH_US);
    const volatile uint32_t *low = (const volatile uint32_t *)counter;
    uint32_t start = *low;
    uint32_t armed = rv_timer_set(SLOT_DODGER_TIMER, BIT_TIMER, 1000u);
    uint32_t probe_started = rv_invoke(OP_THREAD_RESUME, SLOT_DODGER, 0, 0, 0);
    uint32_t spinner_started = rv_invoke(OP_THREAD_RESUME, SLOT_SPINNER, 0, 0, 0);
    uint32_t bits = wait_probe(SLOT_DODGER_NTFN, counter, start, ms_counts);
    expect("let their virtual times come to the clock", caught);
    expect("wait for the probe, a millisecond at most", armed);
    expect("start the probe", probe_started);
    expect("start the spinner", spinner_started);
    expect("cancel the timer", rv_timer_set(SLOT_DODGER_TIMER, 0, 0));
    expect("take their units back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("the probe ran as the spinner's first request ended, within half a millisecond",
           (bits & BIT_PROBE) != 0 && probe_at - start < ms_counts / 2 ? KERR_OK : KERR_INVALID_ARG);
    puts("root: requests ok\n");

    /*
     * Spans at the wait.
     * The probe, bound to the half with the default span, waits
     * and says its wake is due within a quarter of a millisecond, a request of half that, longer than the switch to it.
     * This thread wakes it and spins on for half a millisecond, its own request due within the default span,
     * so the probe runs at once, taking the processor from it in that call,
     * where with the binding's span it would be due after this thread's request and wait for it to end.
     */
    probe_span = rv_span(ms_counts / 4);
    probe_at = 0;
    expect("stop the probe", rv_thread_stop(SLOT_DODGER, &was));
    expect("make it a waiting probe",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_DODGER, (uint32_t)&waiting_probe_main,
                     data_base + data_size / 4 - SPINNERS * SPINNER_STACK, 0));
    expect("bind the probe to the half with the default span",
           rv_time_bind(SLOT_HALF_TIME, SLOT_DODGER, 0, HALF_UNITS, 0));
    expect("start it, to wait", rv_invoke(OP_THREAD_RESUME, SLOT_DODGER, 0, 0, 0));
    expect("let its virtual time come to the clock", sleep_us(CATCH_US));
    start = *low;
    expect("wake the probe", rv_signal(SLOT_DODGER_NTFN, BIT_WAKE));
    while (*low - start < ms_counts / 2) {
    }
    expect("wait for it, a millisecond at most", rv_timer_set(SLOT_DODGER_TIMER, BIT_TIMER, 1000u));
    bits = wait_probe(SLOT_DODGER_NTFN, counter, start, ms_counts);
    expect("cancel the timer", rv_timer_set(SLOT_DODGER_TIMER, 0, 0));
    expect("take its units back", rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_HALF_TIME, 0, 0));
    expect("the probe ran within its span of the wake, before the spin ended",
           (bits & BIT_PROBE) != 0 && probe_at - start < probe_span ? KERR_OK : KERR_INVALID_ARG);
    puts("root: spans ok\n");

    /*
     * Overruns; see OP_THREAD_OVERRUNS.
     * The driver works an eighth of the default span at each wake, its requests due within that span, and sleeps.
     * On the half its request is half the span, which the work and the switch to it fit in, so it runs none out;
     * on four units its request is a sixteenth, which the work runs out at every wake, however idle the processor.
     */
    driver_work = default_span / 8u;
    expect("stop the probe", rv_thread_stop(SLOT_DODGER, &was));
    expect("make it the driver",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_DODGER, (uint32_t)&driver_main,
                     data_base + data_size / 4 - SPINNERS * SPINNER_STACK, 0));
    expect("start it", rv_invoke(OP_THREAD_RESUME, SLOT_DODGER, 0, 0, 0));
    uint32_t wakes, overruns;
    drive(HALF_UNITS, &wakes, &overruns);
    expect("on enough units the driver ran no request out", wakes != 0 && overruns == 0 ? KERR_OK : KERR_INVALID_ARG);
    drive(4, &wakes, &overruns);
    expect("on too few it ran one out at each wake",
           wakes != 0 && overruns >= wakes ? KERR_OK : KERR_INVALID_ARG);
    expect("stop the driver", rv_thread_stop(SLOT_DODGER, &was));
    expect("cancel its timer", rv_timer_set(SLOT_DODGER_TIMER, 0, 0));
    puts("root: overruns ok\n");
}
