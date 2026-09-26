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

struct share shares[SHARES];
struct share *run_queue;
struct share *turn;
uint32_t armed_sources;

static void count_armed(bool was, bool is)
{
    if (is && !was) {
        armed_sources++;
    } else if (was && !is) {
        armed_sources--;
    }
}

void irq_set_bits(struct irq *irq, uint32_t bits)
{
    if (irq->line != LOG_IRQ_LINE) {
        count_armed(irq_armed(irq), bits != 0);
    }
    irq->bits = bits;
}

/*
 * A thread is on at most one ring, through queue_next and queue_prev:
 * its notification's waiters while it waits,
 * or its share's while it is ready, bound, and another thread runs.
 * A ring's head names the oldest, so the newest is that one's queue_prev.
 */
static void ring_push(paddr_t *head, struct thread *t)
{
    paddr_t self = v2p(t);
    if (*head == 0) {
        t->queue_next = t->queue_prev = self;
        *head = self;
    } else {
        struct thread *oldest = p2v(*head);
        struct thread *newest = p2v(oldest->queue_prev);
        t->queue_next = *head;
        t->queue_prev = oldest->queue_prev;
        newest->queue_next = self;
        oldest->queue_prev = self;
    }
}

static void ring_remove(paddr_t *head, struct thread *t)
{
    if (t->queue_next == v2p(t)) {
        *head = 0;
    } else {
        ((struct thread *)p2v(t->queue_prev))->queue_next = t->queue_next;
        ((struct thread *)p2v(t->queue_next))->queue_prev = t->queue_prev;
        if (*head == v2p(t)) {
            *head = t->queue_next;
        }
    }
    t->queue_next = t->queue_prev = 0;
}

void sched_wait(struct thread *t, struct notification *ntfn)
{
    ring_push(&ntfn->waiters, t);
    t->waiting_on = v2p(&ntfn->hdr);
    t->state = THREAD_WAITING;
}

/* Take a waiting thread off its notification's ring. */
static void unwait(struct thread *t)
{
    ring_remove(&((struct notification *)p2v(t->waiting_on))->waiters, t);
    t->waiting_on = 0;
}

/*
 * The run queue is a ring of shares as a share's ring is one of threads, oldest first,
 * and a share is on it exactly while it has a ready thread and it is not its turn.
 * Taking from the front and joining at the back, of both, is the whole scheduling policy.
 */
static void share_push(struct share *s)
{
    if (run_queue == NULL) {
        s->next = s->prev = s;
        run_queue = s;
    } else {
        s->next = run_queue;
        s->prev = run_queue->prev;
        run_queue->prev->next = s;
        run_queue->prev = s;
    }
}

static void share_remove(struct share *s)
{
    if (s->next == s) {
        run_queue = NULL;
    } else {
        s->prev->next = s->next;
        s->next->prev = s->prev;
        if (run_queue == s) {
            run_queue = s->next;
        }
    }
    s->next = s->prev = NULL;
}

/* Put a share on the run queue or take it off, whichever its threads and the turn now say. */
static void share_settle(struct share *s)
{
    bool owed = s->threads != 0 && s != turn;
    if (owed && s->next == NULL) {
        share_push(s);
    } else if (!owed && s->next != NULL) {
        share_remove(s);
    }
}

/* A ready thread that is not running joins the back of its share's ring; one without a share waits for one. */
static void enqueue(struct thread *t)
{
    struct share *s = thread_share(t);
    if (s != NULL) {
        ring_push(&s->threads, t);
        share_settle(s);
    }
}

/* A ready thread that is not running leaves its share's ring. */
static void dequeue(struct thread *t)
{
    struct share *s = thread_share(t);
    if (s != NULL) {
        ring_remove(&s->threads, t);
        share_settle(s);
    }
}

/* The oldest ready thread of a share that has one, taken off its ring. */
static struct thread *share_take(struct share *s)
{
    struct thread *t = p2v(s->threads);
    ring_remove(&s->threads, t);
    return t;
}

/* The oldest share on the run queue, taken off it: its turn begins. NULL when the queue is empty. */
static struct share *next_turn(void)
{
    struct share *s = run_queue;
    if (s != NULL) {
        share_remove(s);
    }
    return s;
}

void sched_ready(struct thread *t)
{
    t->state = THREAD_READY;
    enqueue(t);
}

void sched_bind(struct thread *t, uint32_t share, struct cap *parent)
{
    if (t->share.type != CAP_NONE) {
        cap_delete(&t->share, false);
    }
    t->share = (struct cap){ .type = CAP_BOUND, .rights = RIGHT_W, .a = share };
    cap_attach(parent, &t->share);
    if (t->state == THREAD_READY && t != current) {
        enqueue(t);
    }
}

void sched_unbind(struct cap *bound)
{
    struct thread *t = bound_thread(bound);
    if (t->state == THREAD_READY && t != current) {
        dequeue(t);
    }
    *bound = (struct cap){ 0 };
}

void sched_start(struct thread *t)
{
    current = t;
    turn = thread_share(t);
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
    sched_ready(t);
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

void irq_signal(struct irq *irq)
{
    uint32_t bits = irq->bits;
    irq_set_bits(irq, 0);
    sched_signal(irq_notification(irq), bits);
}

/*
 * Ticks of time, one or the few a late interrupt covers:
 * count them and fire every timer line that is due.
 * The timer lines are a fixed few, so the tick looks at each of them
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the tick no armed one is due.
 */
static void tick_advance(uint32_t ticks)
{
    sched_ticks += ticks;
    for (uint32_t i = 0; i < TIMER_LINES; i++) {
        LOOP_BOUND(TIMER_LINES);
        struct irq *irq = line_binding(IRQ_LINES + i);
        if (irq != NULL && irq_armed(irq) && irq_due(irq, sched_ticks)) {
            irq_signal(irq);
        }
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
    irq_signal(irq);
    return true;
}

void sched_claim_interrupts(void)
{
    for (uint32_t line = irq_claim(); line != 0; line = irq_claim()) {
        /* A line is masked as it signals, so each is claimed once. */
        LOOP_BOUND(IRQ_LINES);
        /* The controller forwards a line only while an Irq is armed on it. */
        if (!sched_interrupt(line)) {
            kpanic("interrupt on a line nothing is armed on");
        }
        irq_complete(line);
    }
}

/*
 * The Irq bound to each line, 0 for none,
 * so that an interrupt or the tick finds its Irq without a walk; see DESIGN.md, "Bounded work".
 */
paddr_t line_irq[LINES];

struct irq *line_binding(uint32_t line)
{
    return line_irq[line] != 0 ? p2v(line_irq[line]) : NULL;
}

bool sched_forget(struct obj_header *o, bool preempt)
{
    switch (o->type) {
    case CAP_NOTIFICATION: {
        struct notification *ntfn = (struct notification *)o;
        /*
         * The notification is gone, so the wait cannot be answered.
         * The thread learns that the way every other call learns it.
         * Its progress is the queue: a stop leaves the rest waiting on a notification still whole.
         */
        while (ntfn->waiters != 0) {
            LOOP_PAID(sched_forget, waiter, "a thread that waited, by a call of its own");
            struct thread *t = p2v(ntfn->waiters);
            unwait(t);
            t->frame.regs[REG_A0] = KERR_INVALID_CAP;
            t->frame.regs[REG_A1] = 0;
            sched_ready(t);
            if (ntfn->waiters != 0 && cap_stop_here(preempt)) {
                return false;
            }
        }
        break;
    }
    case CAP_THREAD: {
        /*
         * The thread is gone, and the notification it waits on may live on in another pool.
         * Its share went as a node it holds, and with it its place on the share's ring.
         */
        struct thread *t = (struct thread *)o;
        if (t->state == THREAD_WAITING) {
            unwait(t);
        }
        break;
    }
    case CAP_IRQ: {
        /* It is gone and will not fire, so it is no source. */
        irq_set_bits((struct irq *)o, 0);
        /* The object that would receive the interrupt is gone; only the controller's lines have a mask. */
        uint32_t line = ((struct irq *)o)->line;
        if (line_on_controller(line)) {
            irq_enable(line, false);
        }
        line_irq[line] = 0;
        break;
    }
    default:
        break;
    }
    return true;
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

/*
 * Hand the processor to the oldest ready thread of the share whose turn it is,
 * waiting for a share to have one while it is nobody's turn.
 */
static void run_turn(void)
{
    while (turn == NULL) {
        LOOP_WAIT("an interrupt, in intr_wait");
        /*
         * Only a running thread, a timer line or a device interrupt
         * can make another one runnable.
         * With no Irq armed on either kind of line nothing can change this, so say so and stop.
         * The log's line is not a source: only the kernel raises it,
         * and the kernel runs only when a thread or one of these does.
         * While tracing is on, time moves and lines fire only through OP_DEBUG_TICK
         * and OP_DEBUG_IRQ, which nobody is left to perform, so the same holds
         * and the host build, which has no clock and no devices, agrees.
         */
        if (debug_trace || armed_sources == 0) {
            kputs("no runnable thread\n");
            khalt(5);
        }
        uint32_t ticks;
        bool device = intr_wait(&ticks);
        if (ticks != 0) {
            tick_advance(ticks);
        }
        if (device) {
            sched_claim_interrupts();
        }
        turn = next_turn();
    }
    switch_to(share_take(turn));
}

void sched_run_next(void)
{
    /* The share whose turn it is keeps it while it has another thread ready. */
    if (turn == NULL || turn->threads == 0) {
        turn = next_turn();
    }
    run_turn();
}

void sched_tick(uint32_t ticks)
{
    tick_advance(ticks);
    /*
     * The turn ends: the share whose turn it was goes to the back of the run queue
     * if it has a thread ready, and the running thread to the back of its share's ring,
     * which puts its share behind that one if it was moved to another.
     * One that lost its share during its turn goes nowhere, and without it the queue may be empty.
     */
    struct share *was = turn;
    turn = NULL;
    if (was != NULL) {
        share_settle(was);
    }
    enqueue(current);
    turn = next_turn();
    run_turn();
}
