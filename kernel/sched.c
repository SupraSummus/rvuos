/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "kernel.h"
#include "object.h"
#include "timer.h"

struct thread *current;
uint32_t sched_ticks;
uint32_t trace_wake_bits;

/*
 * The kernel keeps no queues,
 * so every question it asks about threads is answered
 * by walking every object, as the overlap checks are.
 */
struct thread *sched_waiter(paddr_t notification)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_THREAD) {
            continue;
        }
        struct thread *t = (struct thread *)o;
        if (t->state == THREAD_WAITING && t->waiting_on == notification) {
            return t;
        }
    }
    return NULL;
}

/*
 * The first runnable thread after `from` in the walk, wrapping around,
 * so that `from` itself comes last.
 * This is the whole scheduling policy: round-robin in allocation order,
 * with no state beyond the position of the running thread.
 * NULL when no thread at all is runnable.
 */
static struct thread *runnable_after(struct thread *from)
{
    struct obj_header *o = object_next(&from->hdr);
    for (;;) {
        if (o == NULL) {
            o = object_first();
        }
        if (o->type == CAP_THREAD && ((struct thread *)o)->state == THREAD_READY) {
            return (struct thread *)o;
        }
        if (o == &from->hdr) {
            return NULL;
        }
        o = object_next(o);
    }
}

/*
 * Hand a waiting thread the bits it waited for.
 * It resumes after its ecall with the status and the bits in place.
 */
static void wake(struct thread *t, uint32_t bits)
{
    t->frame.regs[REG_A0] = KERR_OK;
    t->frame.regs[REG_A1] = bits;
    t->waiting_on = 0;
    t->state = THREAD_READY;
    trace_wake_bits = bits;
}

void sched_signal(struct notification *ntfn, uint32_t bits)
{
    ntfn->bits |= bits;
    struct thread *waiter = sched_waiter(v2p(&ntfn->hdr));
    if (waiter != NULL) {
        wake(waiter, ntfn->bits);
        ntfn->bits = 0;
    }
}

/*
 * One tick of time: count it and fire every timer that is due.
 * Timers are found the way waiters are, by walking the pools,
 * and a timer that fires disarms itself,
 * so that after the tick no armed timer is due.
 */
static void tick_advance(void)
{
    sched_ticks++;
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_TIMER) {
            continue;
        }
        struct timer *t = (struct timer *)o;
        if (!timer_armed(t) || !timer_due(t, sched_ticks)) {
            continue;
        }
        uint32_t bits = t->bits;
        t->bits = 0;
        sched_signal(timer_notification(t), bits);
    }
}

/* True if some timer will fire, so that waiting for the tick can change what is runnable. */
static bool timer_pending(void)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_TIMER && timer_armed((struct timer *)o)) {
            return true;
        }
    }
    return false;
}

void sched_unblock_range(uint32_t base, uint32_t size)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_THREAD) {
            continue;
        }
        struct thread *t = (struct thread *)o;
        if (t->state != THREAD_WAITING || !range_contains(base, size, t->waiting_on)) {
            continue;
        }
        /*
         * The notification is gone, so the wait cannot be answered.
         * The thread learns that the way every other call learns it.
         */
        t->frame.regs[REG_A0] = KERR_INVALID_CAP;
        t->frame.regs[REG_A1] = 0;
        t->waiting_on = 0;
        t->state = THREAD_READY;
    }
}

static void switch_to(struct thread *next)
{
    if (debug_trace && (next->flags & THREAD_UNTRACED)) {
        /*
         * The host build has no instruction fetch and cannot follow
         * a thread whose program counter it did not see set.
         * Stopping here keeps the two builds' transcripts identical;
         * see DESIGN.md, "Verification".
         */
        kputs("untraced thread\n");
        khalt(6);
    }
    if (thread_process(next) != thread_process(current)) {
        process_activate(thread_process(next));
    }
    current = next;
}

void sched_run_next(void)
{
    struct thread *next = runnable_after(current);
    while (next == NULL) {
        /*
         * Only a running thread or a firing timer can make another one runnable,
         * and device interrupts do not exist yet.
         * With no timer armed nothing can change this, so say so and stop.
         * While tracing is on, time moves only through OP_DEBUG_TICK,
         * which nobody is left to perform, so the same holds
         * and the host build, which cannot wait for a clock, agrees.
         */
        if (debug_trace || !timer_pending()) {
            kputs("no runnable thread\n");
            khalt(5);
        }
        timer_wait();
        tick_advance();
        next = runnable_after(current);
    }
    switch_to(next);
}

void sched_tick(void)
{
    tick_advance();
    struct thread *next = runnable_after(current);
    /* The running thread is ready, so there is always at least itself. */
    if (next != current) {
        switch_to(next);
    }
}
