/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "timer.h"
#include "trap.h"

struct thread *current;
uint32_t sched_ticks;
uint32_t trace_wake_bits;

paddr_t unit_thread[TIME_UNITS];
paddr_t run_queue;
paddr_t spare_queue;
paddr_t spent_queue;
struct thread *turn;
uint32_t armed_sources;
uint32_t nearest_deadline = NEAREST_NONE;
uint32_t nearest_release = NEAREST_NONE;
uint32_t wake_tick;
bool wake_stale;
bool turn_due;

static void count_armed(bool was, bool is)
{
    if (is && !was) {
        armed_sources++;
    } else if (was && !is) {
        armed_sources--;
    }
}

/* True if tick a comes before tick b; the count wraps, so as a signed difference. */
static bool tick_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

void irq_set_bits(struct irq *irq, uint32_t bits)
{
    if (irq->line != LOG_IRQ_LINE) {
        count_armed(irq_armed(irq), bits != 0);
    }
    /* A timer line armed for a nearer deadline brings the timer's next interrupt forward to it. */
    if (line_is_timer(irq->line) && bits != 0 && tick_before(irq->deadline, nearest_deadline)) {
        nearest_deadline = irq->deadline;
        wake_stale = true;
    }
    irq->bits = bits;
}

/*
 * A thread is on at most one ring, through queue_next and queue_prev:
 * its notification's waiters while it waits,
 * or one of the scheduler's queues while it is ready, may run, and another thread runs.
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
 * A ready thread that may run and whose turn it is not waits on one of three queues,
 * the run queue while it has a tick in its account, else the spare queue or the spent queue by its binding.
 * Taking from the front and joining at the back, of each, is the whole scheduling policy.
 */
static paddr_t *queue_head(uint8_t queue)
{
    return queue == QUEUE_RUN ? &run_queue : queue == QUEUE_SPARE ? &spare_queue : &spent_queue;
}

/* The most a thread's account holds: a tenth of a second's worth of its units, none without. */
static uint32_t account_cap(const struct thread *t)
{
    _Static_assert((uint64_t)TIME_UNITS * ACCOUNT_TICKS <= UINT32_MAX, "the cap does not wrap");
    _Static_assert(ACCOUNT_TICKS >= TICK_PARTS + 1, "one unit's cap holds account_after's stretch");
    return thread_units(t) * ACCOUNT_TICKS;
}

/* A thread's account at the count: it gains a part for each unit every tick since its stamp, up to its cap. */
static uint32_t account_now(const struct thread *t)
{
    uint64_t balance = t->balance + (uint64_t)(sched_ticks - t->stamp) * thread_units(t);
    uint32_t cap = account_cap(t);
    return balance > cap ? cap : (uint32_t)balance;
}

/* Bring a thread's account up to the count. */
static void account_refill(struct thread *t)
{
    t->balance = account_now(t);
    t->stamp = sched_ticks;
}

/* Whether an account holds a turn's worth, a tick: then the thread has time, and takes turns with those that have. */
static bool has_time(uint32_t balance)
{
    return balance >= TICK_PARTS;
}

/* The ticks an account at balance, below a tick, takes to reach one, for a thread with units. */
static uint32_t account_fill(const struct thread *t, uint32_t balance)
{
    uint32_t units = thread_units(t);
    return (TICK_PARTS - balance + units - 1) / units;
}

/*
 * The ticks from the count to the one that drains an account with time, the first to leave it less than a tick,
 * for the thread whose turn it is; NEAREST_NONE for none, with the whole processor.
 */
static uint32_t account_drain(const struct thread *t, uint32_t balance)
{
    uint32_t loss = TICK_PARTS - thread_units(t);
    return loss != 0 ? (balance - TICK_PARTS) / loss + 1 : NEAREST_NONE;
}

/*
 * A thread's account after ticks of its turn, as if each tick had been taken on its own:
 * a tick that starts with a tick in the account costs TICK_PARTS - units, and any other earns units.
 * Either way the balance moves by units modulo TICK_PARTS.
 * It falls while it holds a tick, and once below one it stays in [units, TICK_PARTS + units),
 * a stretch TICK_PARTS wide, so the one value there in the right residue is where it ends.
 * The cap never cuts in: that stretch lies within the cap of a thread with units.
 */
static uint32_t account_after(uint32_t balance, uint32_t units, uint32_t ticks)
{
    uint64_t spent = (uint64_t)ticks * (TICK_PARTS - units);
    uint32_t earned = (uint32_t)((balance + (uint64_t)(ticks % TICK_PARTS) * units) % TICK_PARTS);
    uint32_t hover = earned >= units ? earned : earned + TICK_PARTS;
    return spent < balance && balance - spent > hover ? (uint32_t)(balance - spent) : hover;
}

/* The queue a thread waits on, as its state, the turn, its account at balance and its binding now say. */
static uint8_t thread_queue(const struct thread *t, uint32_t balance)
{
    if (t->state != THREAD_READY || t == turn || t->time.type != CAP_BOUND) {
        return QUEUE_NONE;
    }
    if (has_time(balance)) {
        return QUEUE_RUN;
    }
    if (thread_spare(t)) {
        return QUEUE_SPARE;
    }
    return thread_units(t) != 0 ? QUEUE_SPENT : QUEUE_NONE;
}

/*
 * Bring a thread's account up to the count and move the thread to the queue it now waits on,
 * or take it off the one it was on.
 * A thread with units that waits without time brings the release forward to the tick its account reaches a tick.
 */
static void thread_settle(struct thread *t)
{
    wake_stale = true;
    account_refill(t);
    uint8_t queue = thread_queue(t, t->balance);
    if (queue != t->queue) {
        if (t->queue != QUEUE_NONE) {
            ring_remove(queue_head(t->queue), t);
        }
        if (queue != QUEUE_NONE) {
            ring_push(queue_head(queue), t);
        }
        t->queue = queue;
    }
    if ((queue == QUEUE_SPARE || queue == QUEUE_SPENT) && thread_units(t) != 0 &&
        tick_before(sched_ticks + account_fill(t, t->balance), nearest_release)) {
        nearest_release = sched_ticks + account_fill(t, t->balance);
    }
}

/*
 * The oldest thread with time, or with none the oldest that may run on spare time,
 * taken off its queue with its account brought up to the count: its turn begins,
 * and every tick of it is charged to it from here. NULL when neither queue has one.
 */
static struct thread *next_turn(void)
{
    paddr_t *head = run_queue != 0 ? &run_queue : &spare_queue;
    if (*head == 0) {
        return NULL;
    }
    struct thread *t = p2v(*head);
    ring_remove(head, t);
    t->queue = QUEUE_NONE;
    account_refill(t);
    return t;
}

void sched_ready(struct thread *t)
{
    t->state = THREAD_READY;
    thread_settle(t);
}

void sched_bind(struct thread *t, uint32_t first, uint32_t count, uint8_t rights, struct cap *parent)
{
    account_refill(t);
    uint32_t kept = t->balance;
    if (t->time.type != CAP_NONE) {
        cap_delete(&t->time, false);
    }
    t->time = (struct cap){ .type = CAP_BOUND, .rights = rights, .a = first, .b = count };
    cap_attach(parent, &t->time);
    for (uint32_t i = 0; i < count; i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_thread[first + i] = v2p(t);
    }
    /* What the account held stays, and settling holds it to what the new units hold at most. */
    t->balance = kept;
    thread_settle(t);
}

void sched_unbind(struct cap *bound)
{
    struct thread *t = bound_thread(bound);
    for (uint32_t i = 0; i < bound->b; i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_thread[bound->a + i] = 0;
    }
    *bound = (struct cap){ 0 };
    /* A thread without units holds nothing, so settling empties its account as it takes the thread off its queue. */
    thread_settle(t);
}

/* Settling takes a stopped thread off its queue; syscall_dispatch hands the processor on from a running one. */
void sched_unhost(struct cap *hosted)
{
    struct thread *t = hosted_thread(hosted);
    if (t->state == THREAD_WAITING) {
        unwait(t);
    }
    t->state = THREAD_STOPPED;
    *hosted = (struct cap){ 0 };
    thread_settle(t);
}

/* The thread that earns a unit first of those it earns, else NULL, so that a look at each unit meets it once. */
static struct thread *unit_first(uint32_t unit)
{
    struct thread *t = unit_thread[unit] != 0 ? p2v(unit_thread[unit]) : NULL;
    return t != NULL && t->time.a == unit ? t : NULL;
}

/*
 * A thread with units earns them, so a look at each unit finds every one,
 * and the units are a fixed few, as the timer lines are; see DESIGN.md, "Bounded work".
 * A thread with none has an empty account already.
 */
void sched_accounts_fill(void)
{
    nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < TIME_UNITS; i++) {
        LOOP_BOUND(TIME_UNITS);
        struct thread *t = unit_first(i);
        if (t != NULL) {
            t->balance = account_cap(t);
            t->stamp = sched_ticks;
            thread_settle(t);
        }
    }
}

/*
 * Look at every thread with units that waits without time: one whose account reached a tick joins the run queue,
 * and the others bring the release forward to the tick theirs does.
 * It looks at each unit, a fixed few, as the tick looks at the timer lines,
 * and only at the tick nearest_release said an account would reach a tick.
 */
static void release(void)
{
    nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < TIME_UNITS; i++) {
        LOOP_BOUND(TIME_UNITS);
        struct thread *t = unit_first(i);
        if (t != NULL && (t->queue == QUEUE_SPARE || t->queue == QUEUE_SPENT)) {
            thread_settle(t);
        }
    }
}

void sched_start(struct thread *t)
{
    current = turn = t;
    /* The boot sets the timer for the first tick. */
    wake_tick = sched_ticks + 1;
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

/* Only an armed Irq signals, and an Irq without a notification is never armed. */
void irq_signal(struct irq *irq)
{
    uint32_t bits = irq->bits;
    irq_set_bits(irq, 0);
    sched_signal(irq_notification(irq), bits);
}

void irq_drop(struct cap *signalled)
{
    struct irq *irq = signalled_irq(signalled);
    irq_set_bits(irq, 0);
    /* Only the controller's lines have a mask. */
    if (line_on_controller(irq->line)) {
        irq_enable(irq->line, false);
    }
    *signalled = (struct cap){ 0 };
}

/*
 * Ticks of time, one or the few a trap counts at once:
 * charge them to the thread whose turn it is, or to nobody while none runs,
 * count them, fire every timer line that is due,
 * and give time again to the threads whose account reached a tick.
 * The timer lines are a fixed few, so the tick looks at each of them
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the tick no armed one is due,
 * and the nearest deadline of those still armed is exact again.
 */
static void tick_advance(uint32_t ticks)
{
    wake_stale = true;
    if (turn != NULL) {
        account_refill(turn);
        turn->balance = account_after(turn->balance, thread_units(turn), ticks);
        turn->stamp += ticks;
    }
    sched_ticks += ticks;
    uint32_t nearest = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < TIMER_LINES; i++) {
        LOOP_BOUND(TIMER_LINES);
        struct irq *irq = line_binding(IRQ_LINES + i);
        if (irq == NULL || !irq_armed(irq)) {
            continue;
        }
        if (irq_due(irq, sched_ticks)) {
            irq_signal(irq);
        } else if (tick_before(irq->deadline, nearest)) {
            nearest = irq->deadline;
        }
    }
    nearest_deadline = nearest;
    if (!tick_before(sched_ticks, nearest_release)) {
        release();
    }
}

/*
 * The next tick hands the processor back to the thread whose turn it is
 * when it would have the next turn again: nobody is on the run queue,
 * and it has time, or may run on spare time with nobody on the spare queue.
 * Its account changes what runs only at the tick that drains it,
 * and only while it has no spare time to go on with, or shares that with others.
 */
uint32_t sched_wake_ticks(void)
{
    uint32_t wake = nearest_deadline - sched_ticks;
    if (tick_before(nearest_release, nearest_deadline)) {
        wake = nearest_release - sched_ticks;
    }
    if (turn == NULL) {
        return wake;
    }
    if (run_queue != 0) {
        return 1;
    }
    uint32_t balance = account_now(turn);
    bool alone = thread_spare(turn) && spare_queue == 0;
    if (!has_time(balance)) {
        return alone ? wake : 1;
    }
    if (alone) {
        return wake;
    }
    uint32_t drain = account_drain(turn, balance);
    return drain < wake ? drain : wake;
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
    case CAP_IRQ:
        /* It was disarmed and its line masked as its notification was cleared; the line is free again. */
        line_irq[((struct irq *)o)->line] = 0;
        break;
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
    /* The thread leaving may have lost its process during its call; the one coming has one, since it is ready. */
    if (thread_process(next) != thread_process(current)) {
        process_activate(thread_process(next));
    }
    current = next;
    wake_stale = true;
}

/* Hand the processor to the thread whose turn it is, waiting for one to become ready while it is nobody's turn. */
static void run_turn(void)
{
    while (turn == NULL) {
        LOOP_WAIT("an interrupt, in intr_wait");
        /*
         * Only a running thread, a timer line, a device interrupt or an account that reaches a tick
         * can make another one runnable, the last for a thread that spent its time.
         * With no Irq armed on either kind of line and no such thread nothing can change this,
         * so say so and stop.
         * The log's line is not a source: only the kernel raises it,
         * and the kernel runs only when a thread or one of these does.
         * While tracing is on, time moves and lines fire only through OP_DEBUG_TICK
         * and OP_DEBUG_IRQ, which nobody is left to perform, so the same holds
         * and the host build, which has no clock and no devices, agrees.
         */
        if (debug_trace || (armed_sources == 0 && spent_queue == 0)) {
            kputs("no runnable thread\n");
            khalt(5);
        }
        /*
         * No thread runs, so no turn needs ending: the stall takes no tick before the nearest deadline,
         * and an end of a turn the trap's count found due is moot, since the turn is over.
         */
        uint32_t ticks;
        turn_due = false;
        wake_stale = true;
        bool device = intr_wait(sched_wake_ticks(), &ticks);
        if (ticks != 0) {
            tick_advance(ticks);
        }
        if (device) {
            sched_claim_interrupts();
        }
        turn = next_turn();
    }
    switch_to(turn);
}

void sched_run_next(void)
{
    turn = next_turn();
    run_turn();
}

/*
 * The turn ends: the thread whose turn it was goes to the back of the queue its account puts it on,
 * if it may run, and the next thread has its turn.
 * One that lost its units during its turn goes nowhere, and without it the queues may be empty.
 */
static void turn_end(void)
{
    struct thread *was = turn;
    turn = NULL;
    if (was != NULL) {
        thread_settle(was);
    }
    turn = next_turn();
    run_turn();
}

void sched_tick(uint32_t ticks)
{
    tick_advance(ticks);
    turn_end();
}

void sched_count(uint32_t ticks)
{
    if (ticks != 0) {
        tick_advance(ticks);
        turn_due |= !tick_before(sched_ticks, wake_tick);
    }
}

uint32_t sched_wake(void)
{
    /* Under tracing only a record ends a turn, so a count before tracing began ends none. */
    if (turn_due) {
        turn_due = false;
        if (!debug_trace) {
            turn_end();
        }
    }
    if (!wake_stale) {
        return 0;
    }
    wake_stale = false;
    uint32_t ticks = sched_wake_ticks();
    wake_tick = sched_ticks + ticks;
    return ticks;
}
