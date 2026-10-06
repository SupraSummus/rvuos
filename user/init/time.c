/*
 * Time: a timer line the root task sleeps on, timed on the clock, and a period kept on it.
 * The child waits for the root task and the logger waits on the log,
 * which nothing but a running thread can feed,
 * so while this thread sleeps nothing is runnable
 * and the kernel has to wait for the tick rather than stop.
 */

#include <stdint.h>

#include "init.h"
#include "rvuos.h"

/*
 * A sleep longer than one reload of ARM's SysTick reaches at RP2350's 150 MHz, 112 ms,
 * so that an idle kernel there sets it again on its way to the deadline.
 * QEMU's boards run SysTick slower, mps2-an521's reaching 0.84 s,
 * and an idle second costs QEMU about a minute there, so they do not reach the second reload.
 */
#define LONG_SLEEP_US 130000u

/* A period, kept long enough that arming each from the wake would fall a period behind. */
#define PERIOD_US 5000u
#define PERIODS 20u

uint32_t timer_wait(void)
{
    uint32_t bits;
    uint32_t status = rv_wait(SLOT_TIMER_NTFN, &bits);
    if (status != KERR_OK) {
        return status;
    }
    return bits == BIT_TIMER ? KERR_OK : KERR_INVALID_ARG;
}

/*
 * What sleeping is on rvuos: arm a timer line and wait on the notification it signals.
 * The same notification can carry a device's bit next to the timer's,
 * which makes a wait with a timeout the same two calls.
 */
uint32_t sleep_us(uint32_t us)
{
    uint32_t status = rv_timer_set(SLOT_TIMER, BIT_TIMER, us);
    return status == KERR_OK ? timer_wait() : status;
}

static uint32_t period_wait(uint32_t us)
{
    uint32_t skipped;
    uint32_t status = rv_timer_period(SLOT_TIMER, BIT_TIMER, us, &skipped);
    return status == KERR_OK ? timer_wait() : status;
}

void time_demo(uint32_t *hz_out, uint32_t *counter_out)
{
    expect("allocate the timer's notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_NOTIFICATION, SLOT_TIMER_NTFN, 0));
    expect("carve a timer line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, SLOT_TIMER_LINE));
    expect("bind the timer line",
           rv_invoke(OP_IRQ_BIND, SLOT_TIMER_LINE, SLOT_NEW_POOL, SLOT_TIMER_NTFN, SLOT_TIMER));

    /*
     * The clock times the two sleeps, through a call and through the counter's frame.
     * A timer line fires no earlier than its delay, so either must show at least both;
     * how much more is the board's, so the transcript prints neither.
     */
    uint64_t called, called_after;
    uint32_t hz, counter;
    expect("read the clock", rv_clock_read(BOOT_CAP_CLOCK, &called, &hz, &counter));
    expect("derive the counter's frame",
           rv_invoke(OP_CLOCK_FRAME, BOOT_CAP_CLOCK, SLOT_COUNTER, 0, 0));
    expect("map the counter",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_COUNTER_SLOT, SLOT_COUNTER, RIGHT_R));
    uint32_t start = rv_counter_low(counter);
    expect("sleep", sleep_us(SLEEP_US));
    expect("sleep again", sleep_us(SLEEP_US));
    uint64_t elapsed = (uint32_t)(rv_counter_low(counter) - start);
    expect("read the clock again", rv_clock_read(BOOT_CAP_CLOCK, &called_after, &hz, &counter));
    puts("root: timer ok\n");
    expect("the clock saw both sleeps",
           elapsed * 1000000u >= (uint64_t)hz * (2 * SLEEP_US)
                   && (called_after - called) * 1000000u >= (uint64_t)hz * (2 * SLEEP_US)
               ? KERR_OK : KERR_INVALID_ARG);
    puts("root: clock ok\n");
    expect("sleep past a reload of the compare", sleep_us(LONG_SLEEP_US));
    puts("root: long sleep ok\n");

    /*
     * A period counts from the deadline before, not from the wake,
     * and the tick keeps pace with the counter,
     * so after the first wake sets the phase, PERIODS more take PERIODS periods on the clock,
     * give or take a wake's lateness.
     * A loop of sleeps would take a tick more per period.
     */
    expect("set the phase", period_wait(PERIOD_US));
    start = rv_counter_low(counter);
    uint32_t status = KERR_OK;
    for (uint32_t i = 0; i < PERIODS && status == KERR_OK; i++) {
        status = period_wait(PERIOD_US);
    }
    expect("wait the periods", status);
    elapsed = (uint64_t)(uint32_t)(rv_counter_low(counter) - start) * 1000000u;
    expect("the periods kept their pace",
           elapsed >= (uint64_t)hz * ((PERIODS - 1) * PERIOD_US)
                   && elapsed < (uint64_t)hz * ((PERIODS + 1) * PERIOD_US)
               ? KERR_OK : KERR_INVALID_ARG);
    puts("root: period ok\n");

    *hz_out = hz;
    *counter_out = counter;
}
