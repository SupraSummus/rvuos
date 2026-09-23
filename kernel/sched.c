/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "trap.h"

struct thread *current;
uint32_t sched_ticks;
uint32_t trace_wake_bits;

/*
 * A notification's waiters are a ring through wait_next and wait_prev,
 * and its waiters field names the oldest, so the newest is that one's wait_prev.
 */
void sched_wait(struct thread *t, struct notification *ntfn)
{
    paddr_t self = v2p(t);
    if (ntfn->waiters == 0) {
        t->wait_next = t->wait_prev = self;
        ntfn->waiters = self;
    } else {
        struct thread *oldest = p2v(ntfn->waiters);
        struct thread *newest = p2v(oldest->wait_prev);
        t->wait_next = ntfn->waiters;
        t->wait_prev = oldest->wait_prev;
        newest->wait_next = self;
        oldest->wait_prev = self;
    }
    t->waiting_on = v2p(&ntfn->hdr);
    t->state = THREAD_WAITING;
}

/* Take a waiting thread off its notification's ring. */
static void unwait(struct thread *t)
{
    struct notification *ntfn = p2v(t->waiting_on);
    if (t->wait_next == v2p(t)) {
        ntfn->waiters = 0;
    } else {
        ((struct thread *)p2v(t->wait_prev))->wait_next = t->wait_next;
        ((struct thread *)p2v(t->wait_next))->wait_prev = t->wait_prev;
        if (ntfn->waiters == v2p(t)) {
            ntfn->waiters = t->wait_next;
        }
    }
    t->wait_next = t->wait_prev = 0;
    t->waiting_on = 0;
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
    unwait(t);
    t->frame.regs[REG_A0] = KERR_OK;
    t->frame.regs[REG_A1] = bits;
    t->state = THREAD_READY;
    trace_wake_bits = bits;
}

void sched_signal(struct notification *ntfn, uint32_t bits)
{
    ntfn->bits |= bits;
    if (ntfn->waiters != 0) {
        wake(p2v(ntfn->waiters), ntfn->bits);
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

bool sched_interrupt(uint32_t line)
{
    struct irq *irq = line_binding(line);
    if (irq == NULL || !irq_armed(irq)) {
        return false;
    }
    /*
     * Masked before it signals and disarmed as it does,
     * so the driver hears once and the level may stay high until it has looked.
     */
    irq_enable(line, false);
    uint32_t bits = irq->bits;
    irq->bits = 0;
    sched_signal(irq_notification(irq), bits);
    return true;
}

void sched_claim_interrupts(void)
{
    for (uint32_t line = irq_claim(); line != 0; line = irq_claim()) {
        /* The controller forwards a line only while an Irq is armed on it. */
        if (!sched_interrupt(line)) {
            kpanic("interrupt on a line nothing is armed on");
        }
        irq_complete(line);
    }
}

/*
 * True if some timer or device Irq will fire,
 * so that waiting for an interrupt can change what is runnable.
 * The log's line is not a source: only the kernel raises it,
 * and the kernel runs only when a thread or one of these does.
 */
static bool source_armed(void)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type == CAP_TIMER && timer_armed((struct timer *)o)) {
            return true;
        }
        if (o->type == CAP_IRQ && irq_armed((struct irq *)o) &&
            ((struct irq *)o)->line != LOG_IRQ_LINE) {
            return true;
        }
    }
    return false;
}

/*
 * The Irq bound to each line, 0 for none,
 * so that an interrupt finds its Irq without a walk; see DESIGN.md, "Bounded work".
 */
paddr_t line_irq[IRQ_LINES];

struct irq *line_binding(uint32_t line)
{
    return line_irq[line] != 0 ? p2v(line_irq[line]) : NULL;
}

void sched_forget_pool(struct pool *pool)
{
    for (struct obj_header *o = pool_first(pool); o != NULL; o = pool_next(pool, o)) {
        switch (o->type) {
        case CAP_NOTIFICATION: {
            struct notification *ntfn = (struct notification *)o;
            /*
             * The notification is gone, so the wait cannot be answered.
             * The thread learns that the way every other call learns it.
             */
            while (ntfn->waiters != 0) {
                struct thread *t = p2v(ntfn->waiters);
                unwait(t);
                t->frame.regs[REG_A0] = KERR_INVALID_CAP;
                t->frame.regs[REG_A1] = 0;
                t->state = THREAD_READY;
            }
            break;
        }
        case CAP_THREAD:
            /* The thread is gone, and the ring it is on may live on in another pool. */
            if (((struct thread *)o)->state == THREAD_WAITING) {
                unwait((struct thread *)o);
            }
            break;
        case CAP_IRQ: {
            /* The object that would receive the interrupt is gone; the log's line has no controller. */
            uint32_t line = ((struct irq *)o)->line;
            if (line != LOG_IRQ_LINE) {
                irq_enable(line, false);
            }
            line_irq[line] = 0;
            break;
        }
        default:
            break;
        }
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
         * Only a running thread, a firing timer or a device interrupt
         * can make another one runnable.
         * With no timer and no device Irq armed nothing can change this, so say so and stop.
         * While tracing is on, time moves and lines fire only through OP_DEBUG_TICK
         * and OP_DEBUG_IRQ, which nobody is left to perform, so the same holds
         * and the host build, which has no clock and no devices, agrees.
         */
        if (debug_trace || !source_armed()) {
            kputs("no runnable thread\n");
            khalt(5);
        }
        unsigned pending = intr_wait();
        if (pending & INTR_TICK) {
            tick_advance();
        }
        if (pending & INTR_DEVICE) {
            sched_claim_interrupts();
        }
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
