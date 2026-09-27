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

struct share shares[SHARES];
struct share *run_queue;
struct share *spare_queue;
struct share *spent_queue;
struct share *turn;
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
 * A queue is a ring of shares as a share's ring is one of threads, oldest first,
 * and a share is on one exactly while it has a ready thread and it is not its turn:
 * the run queue while it has time, else the spare queue or the spent queue by its flags.
 * Taking from the front and joining at the back, of each, is the whole scheduling policy.
 */
static void share_push(struct share **queue, struct share *s)
{
    if (*queue == NULL) {
        s->next = s->prev = s;
        *queue = s;
    } else {
        s->next = *queue;
        s->prev = (*queue)->prev;
        (*queue)->prev->next = s;
        (*queue)->prev = s;
    }
    s->queue = queue;
}

static void share_remove(struct share *s)
{
    struct share **queue = s->queue;
    if (s->next == s) {
        *queue = NULL;
    } else {
        s->prev->next = s->next;
        s->next->prev = s->prev;
        if (*queue == s) {
            *queue = s->next;
        }
    }
    s->next = s->prev = NULL;
    s->queue = NULL;
}

/* The most a share's account holds: its budget's part of ACCOUNT_TICKS ticks, rounded up to whole ticks. */
static uint32_t share_cap(const struct share *s)
{
    _Static_assert((uint64_t)BUDGET_WHOLE * ACCOUNT_TICKS + TICK_PARTS <= UINT32_MAX, "the cap does not wrap");
    return (s->budget * ACCOUNT_TICKS + TICK_PARTS - 1) / TICK_PARTS * TICK_PARTS;
}

/*
 * A share's account at the count: it gains its budget every tick since its stamp, up to its cap.
 * *drained says whether it has no time then: less than a tick, or drained before and not full again.
 */
static uint32_t share_balance(const struct share *s, bool *drained)
{
    uint32_t cap = share_cap(s);
    uint64_t balance = s->balance + (uint64_t)(sched_ticks - s->stamp) * s->budget;
    if (balance > cap) {
        balance = cap;
    }
    *drained = balance < TICK_PARTS || (s->drained && balance < cap);
    return (uint32_t)balance;
}

/* Bring a share's account up to the count. */
static void share_refill(struct share *s)
{
    bool drained;
    s->balance = share_balance(s, &drained);
    s->drained = drained;
    s->stamp = sched_ticks;
}

/* The ticks an account at balance takes to fill, for a share with a budget. */
static uint32_t share_fill(const struct share *s, uint32_t balance)
{
    return (share_cap(s) - balance + s->budget - 1) / s->budget;
}

/* The queue a share waits on, as its threads, the turn, its account and its flags now say; NULL for none. */
static struct share **share_queue(const struct share *s)
{
    if (s->threads == 0 || s == turn) {
        return NULL;
    }
    if (!s->drained) {
        return &run_queue;
    }
    return (s->flags & SHARE_SPARE) ? &spare_queue : &spent_queue;
}

/*
 * Bring a share's account up to the count and move the share to the queue it now waits on,
 * or take it off the one it was on.
 * A drained share that waits brings the release forward to the tick its account is full.
 */
static void share_settle(struct share *s)
{
    wake_stale = true;
    share_refill(s);
    struct share **queue = share_queue(s);
    if (queue != s->queue) {
        if (s->queue != NULL) {
            share_remove(s);
        }
        if (queue != NULL) {
            share_push(queue, s);
        }
    }
    if (s->drained && s->queue != NULL && s->budget != 0 &&
        tick_before(sched_ticks + share_fill(s, s->balance), nearest_release)) {
        nearest_release = sched_ticks + share_fill(s, s->balance);
    }
}

/*
 * Charge ticks of its turn to a share: while it has time each costs it a tick and gains it its budget,
 * and the tick that leaves it less than a tick drains it.
 * The ticks after that are spare time, which costs nothing, and gain it its budget from the stamp as any share's do.
 */
static void share_spend(struct share *s, uint32_t ticks)
{
    share_refill(s);
    if (s->drained) {
        return;
    }
    uint32_t loss = TICK_PARTS - s->budget;
    uint32_t spent = ticks;
    if (loss != 0 && spent > (s->balance - TICK_PARTS) / loss) {
        spent = (s->balance - TICK_PARTS) / loss + 1;
        s->drained = true;
    }
    s->balance -= spent * loss;
    s->stamp += spent;
}

/*
 * The ticks from the count to the one at which the account of the share whose turn it is changes what it may do,
 * brought up to the count: the tick that drains it while it has time, or the tick it fills while drained;
 * NEAREST_NONE for neither, with the whole processor or no budget.
 */
static uint32_t share_change(const struct share *s, uint32_t balance, bool drained)
{
    if (!drained) {
        uint32_t loss = TICK_PARTS - s->budget;
        return loss != 0 ? (balance - TICK_PARTS) / loss + 1 : NEAREST_NONE;
    }
    return s->budget != 0 ? share_fill(s, balance) : NEAREST_NONE;
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

/*
 * The oldest share with time, or with none the oldest that may run on spare time,
 * taken off its queue with its account brought up to the count: its turn begins,
 * and every tick of it is charged to the share from here. NULL when neither queue has one.
 */
static struct share *next_turn(void)
{
    struct share *s = run_queue != NULL ? run_queue : spare_queue;
    if (s != NULL) {
        share_remove(s);
        share_refill(s);
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
    wake_stale = true;
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
    wake_stale = true;
    struct thread *t = bound_thread(bound);
    if (t->state == THREAD_READY && t != current) {
        dequeue(t);
    }
    *bound = (struct cap){ 0 };
}

void sched_share_changed(struct share *s)
{
    share_settle(s);
}

/*
 * The account moves with the budget it was saved at, so a share lent budget can run at once,
 * and what one share gives the other takes, so the budgets still add up to the whole.
 * Settling holds each account to its new cap.
 */
void sched_share_move(struct share *from, struct share *to, uint32_t budget)
{
    share_refill(from);
    share_refill(to);
    uint32_t moved = budget * ACCOUNT_TICKS;
    if (moved > from->balance) {
        moved = from->balance;
    }
    from->budget -= budget;
    from->balance -= moved;
    to->budget += budget;
    to->balance += moved;
    share_settle(from);
    share_settle(to);
}

/*
 * The shares are a fixed few, so filling them looks at each of them and at nothing else,
 * as the tick does at the timer lines; see DESIGN.md, "Bounded work".
 */
void sched_accounts_fill(void)
{
    nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < SHARES; i++) {
        LOOP_BOUND(SHARES);
        struct share *s = &shares[i];
        s->balance = share_cap(s);
        s->stamp = sched_ticks;
        s->drained = false;
        share_settle(s);
    }
}

/*
 * Look at every drained share that waits: one whose account is full has time again and joins the run queue,
 * and the others bring the release forward to the tick theirs fills.
 * The shares are a fixed few, so it looks at each of them, as the tick looks at the timer lines,
 * and only at the tick nearest_release said an account would be full.
 */
static void release(void)
{
    nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < SHARES; i++) {
        LOOP_BOUND(SHARES);
        if (shares[i].drained && shares[i].queue != NULL) {
            share_settle(&shares[i]);
        }
    }
}

void sched_start(struct thread *t)
{
    current = t;
    turn = thread_share(t);
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

void irq_signal(struct irq *irq)
{
    uint32_t bits = irq->bits;
    irq_set_bits(irq, 0);
    sched_signal(irq_notification(irq), bits);
}

/*
 * Ticks of time, one or the few a trap counts at once:
 * charge them to the share whose turn it is, or to nobody while none runs,
 * count them, fire every timer line that is due,
 * and give time again to the drained shares whose account is full.
 * The timer lines are a fixed few, so the tick looks at each of them
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the tick no armed one is due,
 * and the nearest deadline of those still armed is exact again.
 */
static void tick_advance(uint32_t ticks)
{
    wake_stale = true;
    if (turn != NULL) {
        share_spend(turn, ticks);
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
 * The next tick hands the processor back to the running thread
 * when it is on the share whose turn it is, which has no other thread ready
 * and would have the next turn again: with time and nobody on the run queue,
 * or drained, with SHARE_SPARE and nobody on either queue.
 */
uint32_t sched_wake_ticks(void)
{
    if (turn != NULL && (thread_share(current) != turn || turn->threads != 0 || run_queue != NULL)) {
        return 1;
    }
    uint32_t wake = nearest_deadline - sched_ticks;
    if (tick_before(nearest_release, nearest_deadline)) {
        wake = nearest_release - sched_ticks;
    }
    if (turn == NULL) {
        return wake;
    }
    bool drained;
    uint32_t balance = share_balance(turn, &drained);
    if (drained && (!(turn->flags & SHARE_SPARE) || spare_queue != NULL)) {
        return 1;
    }
    uint32_t change = share_change(turn, balance, drained);
    return change < wake ? change : wake;
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
    wake_stale = true;
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
         * Only a running thread, a timer line, a device interrupt or an account that fills
         * can make another one runnable, the last for a share that spent its time.
         * With no Irq armed on either kind of line and no such share nothing can change this,
         * so say so and stop.
         * The log's line is not a source: only the kernel raises it,
         * and the kernel runs only when a thread or one of these does.
         * While tracing is on, time moves and lines fire only through OP_DEBUG_TICK
         * and OP_DEBUG_IRQ, which nobody is left to perform, so the same holds
         * and the host build, which has no clock and no devices, agrees.
         */
        if (debug_trace || (armed_sources == 0 && spent_queue == NULL)) {
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

/*
 * The turn ends: the share whose turn it was goes to the back of the queue its budget puts it on
 * if it has a thread ready, and the running thread to the back of its share's ring,
 * which puts its share behind that one if it was moved to another.
 * One that lost its share during its turn goes nowhere, and without it the queues may be empty.
 */
static void turn_end(void)
{
    struct share *was = turn;
    turn = NULL;
    if (was != NULL) {
        share_settle(was);
    }
    enqueue(current);
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
