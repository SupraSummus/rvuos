/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "timer.h"
#include "trap.h"

struct core cores[CORES] = { [0 ... CORES - 1] = { .nearest_release = NEAREST_NONE } };
uint32_t sched_ticks;

paddr_t unit_thread[MACHINE_UNITS];
uint32_t armed_sources;
uint32_t nearest_deadline = NEAREST_NONE;

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
    /*
     * A timer line armed for a nearer deadline brings the timer's next interrupt forward to it.
     * Every core wakes for the nearest deadline, so this one sets its timer for it as the trap ends,
     * and each of the others, which need not, does so once it traps.
     */
    if (line_is_timer(irq->line) && bits != 0 && tick_before(irq->deadline, nearest_deadline)) {
        nearest_deadline = irq->deadline;
        for (uint32_t c = 0; c < CORES; c++) {
            LOOP_BOUND(CORES);
            cores[c].wake_stale = true;
        }
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
 * A ready thread that may run and whose turn it is not waits on one of three queues of the core it runs on,
 * the run queue while it has a tick in its account, else the spare queue or the spent queue by its binding.
 * Taking from the front and joining at the back, of each, is the whole scheduling policy.
 */
static paddr_t *queue_head(struct core *core, uint8_t queue)
{
    return queue == QUEUE_RUN ? &core->run_queue : queue == QUEUE_SPARE ? &core->spare_queue : &core->spent_queue;
}

/* Whether it is the thread's turn: a thread has one only on the core it runs on. */
static bool is_turn(const struct thread *t)
{
    return cores[t->core].turn == t;
}

/*
 * The count a thread's account is counted to: its core's while it is that core's turn, which the core charges it for,
 * and the machine's otherwise.
 */
static uint32_t count_of(const struct thread *t)
{
    return is_turn(t) ? cores[t->core].ticks : sched_ticks;
}

/* A thread's account at the count: it gains its tick's parts every tick since its stamp, up to its cap. */
static uint32_t account_now(const struct thread *t)
{
    uint64_t balance = t->balance + (uint64_t)(count_of(t) - t->stamp) * tick_gain(t);
    uint32_t cap = account_cap(t);
    return balance > cap ? cap : (uint32_t)balance;
}

/* Bring a thread's account up to the count. */
static void account_refill(struct thread *t)
{
    t->balance = account_now(t);
    t->stamp = count_of(t);
}

/* Whether an account holds a turn's worth, a tick: then the thread has time, and takes turns with those that have. */
static bool has_time(uint32_t balance)
{
    return balance >= tick_parts();
}

/* The ticks an account at balance, below a tick, takes to reach one, for a thread with units. */
static uint32_t account_fill(const struct thread *t, uint32_t balance)
{
    uint32_t gain = tick_gain(t);
    return (tick_parts() - balance + gain - 1) / gain;
}

/*
 * A thread's account after whole ticks of its turn, at least one, as if each tick had been taken on its own:
 * a tick that starts with a tick in the account costs a tick less the tick's gain, and any other earns the gain.
 * Either way the balance moves by the gain modulo a tick.
 * It falls while it holds a tick, and once below one it stays in [gain, tick + gain),
 * a stretch a tick wide, so the one value there in the right residue is where it ends.
 * The cap never cuts in: that stretch lies within the cap of a thread with units.
 */
static uint32_t account_after(uint32_t balance, uint32_t units, uint32_t ticks)
{
    _Static_assert(ACCOUNT_TICKS >= COUNT_PARTS + 1, "one unit's cap holds the stretch");
    uint32_t counts = timer_tick_counts(), tick = tick_parts(), gain = units * counts;
    uint64_t spent = (uint64_t)ticks * (tick - gain);
    uint32_t earned = (balance % tick + ((ticks % COUNT_PARTS) * units % COUNT_PARTS) * counts) % tick;
    uint32_t hover = earned >= gain ? earned : earned + tick;
    return spent < balance && balance - spent > hover ? (uint32_t)(balance - spent) : hover;
}

/*
 * What the thread whose turn it is owes for its turn up to offset counts into the tick:
 * a count's parts for each count from turn_from, if it had time as the turn began, and nothing on spare time.
 * A turn with time began with a tick in the account and owes at most the rest of the tick, so the account covers it.
 */
static uint32_t turn_owes(uint32_t offset)
{
    const struct core *core = core_self();
    uint32_t from = core->turn_from;
    return offset > from && has_time(core->turn->balance) ? COUNT_PARTS * (offset - from) : 0;
}

/* The account of the thread whose turn it is at the next tick: it pays for the rest of this one and earns the gain. */
static uint32_t turn_next(void)
{
    struct thread *turn = core_self()->turn;
    uint32_t next = turn->balance - turn_owes(timer_tick_counts()) + tick_gain(turn);
    return next < account_cap(turn) ? next : account_cap(turn);
}

/*
 * The ticks from the count to the one that drains the account of the thread whose turn it is, which has time:
 * the first to leave it less than a tick; NEAREST_NONE for none, with the whole processor.
 * After the next tick every tick costs the account a tick less the gain.
 */
static uint32_t turn_drain(void)
{
    uint32_t next = turn_next();
    if (!has_time(next)) {
        return 1;
    }
    uint32_t loss = tick_parts() - tick_gain(core_self()->turn);
    return loss != 0 ? (next - tick_parts()) / loss + 2 : NEAREST_NONE;
}

/* The queue a thread waits on, as its state, its turn, its account at balance and its binding now say. */
static uint8_t thread_queue(const struct thread *t, uint32_t balance)
{
    if (t->state != THREAD_READY || is_turn(t) || t->time.type != CAP_BOUND) {
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
 * A thread that is no core's turn runs on the core its units lie on, from its next turn,
 * so one bound to units of another core during its turn moves there as the turn ends.
 * A thread with units that waits without time brings its core's release forward to the tick its account reaches a tick.
 * Another core is told of a thread that joins one of its queues, of a release it brought forward,
 * and of any change to the thread whose turn it has, which may end the turn sooner.
 */
static void thread_settle(struct thread *t)
{
    core_self()->wake_stale = true;
    account_refill(t);
    struct core *was = &cores[t->core];
    if (t->time.type == CAP_BOUND && !is_turn(t)) {
        t->core = (uint8_t)unit_core(t->time.a);
    }
    struct core *core = &cores[t->core];
    uint8_t queue = thread_queue(t, t->balance);
    bool joined = queue != QUEUE_NONE && (queue != t->queue || core != was);
    if (queue != t->queue || core != was) {
        if (t->queue != QUEUE_NONE) {
            ring_remove(queue_head(was, t->queue), t);
        }
        if (queue != QUEUE_NONE) {
            ring_push(queue_head(core, queue), t);
        }
        t->queue = queue;
    }
    bool sooner = (queue == QUEUE_SPARE || queue == QUEUE_SPENT) && thread_units(t) != 0 &&
                  tick_before(sched_ticks + account_fill(t, t->balance), core->nearest_release);
    if (sooner) {
        core->nearest_release = sched_ticks + account_fill(t, t->balance);
    }
    if (joined || sooner || is_turn(t)) {
        core_notify(core);
    }
}

/* How far into the tick the counter is; nowhere under tracing, where the clock is the tick count alone. */
static uint32_t clock_offset(void)
{
    return debug_trace ? 0 : timer_offset();
}

/* The turn ends where the counter is: charge its thread, if any, and return how far into the tick that is. */
static uint32_t turn_close(void)
{
    uint32_t offset = clock_offset();
    struct thread *turn = core_self()->turn;
    if (turn != NULL) {
        turn->balance -= turn_owes(offset);
    }
    return offset;
}

/*
 * The oldest thread with time, or with none the oldest that may run on spare time,
 * taken off its queue with its account brought up to the count: its turn begins offset counts into the tick,
 * and every count of it is charged to it from there. NULL when neither queue has one.
 */
static struct thread *next_turn(uint32_t offset)
{
    struct core *core = core_self();
    core->turn_from = offset;
    paddr_t *head = core->run_queue != 0 ? &core->run_queue : &core->spare_queue;
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

/*
 * Settling takes a stopped thread off its queue; syscall_dispatch hands the processor on from a running one,
 * and another core that runs it traps first, and hands it on as it has the lock.
 */
void sched_unhost(struct cap *hosted)
{
    struct thread *t = hosted_thread(hosted);
    if (t->state == THREAD_WAITING) {
        unwait(t);
    }
    t->state = THREAD_STOPPED;
    core_thread_stopped(t);
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
    /* The fill clears what every core's turn owed, too. */
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        cores[c].turn_from = clock_offset();
        cores[c].nearest_release = sched_ticks + NEAREST_NONE;
    }
    for (uint32_t i = 0; i < MACHINE_UNITS; i++) {
        LOOP_BOUND(MACHINE_UNITS);
        struct thread *t = unit_first(i);
        if (t != NULL) {
            t->balance = account_cap(t);
            t->stamp = count_of(t);
            thread_settle(t);
        }
    }
}

/*
 * Look at every thread with units of a core that waits without time: one whose account reached a tick joins the run queue,
 * and the others bring the release forward to the tick theirs does.
 * It looks at each unit of the core, a fixed few, as the tick looks at the timer lines,
 * and only at the tick nearest_release said an account would reach a tick.
 * Accounts count the machine's ticks, so the core that counts the tick looks, whichever core's release it is.
 */
static void release(uint32_t c)
{
    uint32_t first = c * TIME_UNITS;
    cores[c].nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < TIME_UNITS; i++) {
        LOOP_BOUND(TIME_UNITS);
        struct thread *t = unit_first(first + i);
        if (t != NULL && (t->queue == QUEUE_SPARE || t->queue == QUEUE_SPENT)) {
            thread_settle(t);
        }
    }
}

void sched_start(struct thread *t)
{
    struct core *core = core_self();
    core->current = core->turn = t;
    core->turn_from = clock_offset();
    /* The boot sets the timer for the first tick. */
    core->wake_tick = sched_ticks + 1;
}

/*
 * Hand a waiting thread the bits it waited for.
 * It resumes after its call instruction with the status and the bits in place.
 */
static void wake(struct thread *t, uint32_t bits)
{
    unwait(t);
    t->frame.regs[REG_A0] = KERR_OK;
    t->frame.regs[REG_A1] = bits;
    sched_ready(t);
    core_self()->trace_wake_bits = bits;
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
 * Ticks of time, none, one or the few a trap counts at once, and the ticks other cores counted since this one last did:
 * charge them to the thread whose turn it is, the first from turn_from on, or to nobody while none runs,
 * count them, fire every timer line that is due,
 * and give time again to the threads of every core whose account reached a tick.
 * The timer lines are a fixed few, so the tick looks at each of them
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the tick no armed one is due,
 * and the nearest deadline of those still armed is exact again.
 */
static void tick_advance(uint32_t ticks)
{
    struct core *core = core_self();
    struct thread *turn = core->turn;
    uint32_t owed = sched_ticks + ticks - core->ticks;
    core->wake_stale = true;
    if (owed != 0) {
        if (turn != NULL) {
            account_refill(turn);
            turn->balance = turn_next();
            if (owed > 1) {
                turn->balance = account_after(turn->balance, thread_units(turn), owed - 1);
            }
            turn->stamp += owed;
        }
        core->turn_from = 0;
        core->ticks += owed;
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
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        if (!tick_before(sched_ticks, cores[c].nearest_release)) {
            release(c);
        }
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
    struct core *core = core_self();
    struct thread *turn = core->turn;
    uint32_t wake = nearest_deadline - sched_ticks;
    if (tick_before(core->nearest_release, nearest_deadline)) {
        wake = core->nearest_release - sched_ticks;
    }
    if (turn == NULL) {
        return wake;
    }
    /* A thread bound to another core's units during its turn here goes there as the next tick ends the turn. */
    if (core->run_queue != 0 || (turn->time.type == CAP_BOUND && unit_core(turn->time.a) != core_index())) {
        return 1;
    }
    uint32_t balance = account_now(turn);
    bool alone = thread_spare(turn) && core->spare_queue == 0;
    if (!has_time(balance)) {
        return alone ? wake : 1;
    }
    if (alone) {
        return wake;
    }
    uint32_t drain = turn_drain();
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
    case CAP_THREAD:
        /* It lost its process before, and so stopped; a core that ran it may not have looked since. */
        core_thread_gone((struct thread *)o);
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
    struct core *core = core_self();
    /*
     * The thread leaving may have lost its process during its call, or another core destroyed it;
     * the one coming has one, since it is ready.
     */
    if (core->current == NULL || thread_process(next) != thread_process(core->current)) {
        process_activate(thread_process(next));
        board_user_csrs_reset();
        core->pmp_stale = false;
    } else if (core->pmp_stale) {
        process_activate(thread_process(next));
        core->pmp_stale = false;
    }
    core->current = next;
    core->wake_stale = true;
}

/*
 * Whether nothing can ever run again.
 * Only a running thread, a timer line, a device interrupt or an account that reaches a tick
 * can make another one runnable, the last for a thread that spent its time.
 * With no core running a thread or holding one that waits for its turn,
 * no Irq armed on either kind of line and no such thread nothing can change this.
 * The log's line is not a source: only the kernel raises it,
 * and the kernel runs only when a thread or one of these does.
 * While tracing is on, time moves and lines fire only through OP_DEBUG_TICK
 * and OP_DEBUG_IRQ, which nobody is left to perform, so the same holds
 * and the host build, which has no clock and no devices, agrees.
 */
static bool machine_stuck(void)
{
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        const struct core *c = &cores[i];
        if (c->turn != NULL || c->run_queue != 0 || c->spare_queue != 0 || (!debug_trace && c->spent_queue != 0)) {
            return false;
        }
    }
    return debug_trace || armed_sources == 0;
}

/*
 * Hand the processor to the thread whose turn it is.
 * While it is nobody's turn the core idles once the trap is over, see sched_idle,
 * unless nothing can ever run again, which it says before it stops the machine.
 */
static void run_turn(void)
{
    struct core *core = core_self();
    if (core->turn != NULL) {
        switch_to(core->turn);
    } else if (machine_stuck()) {
        kputs("no runnable thread\n");
        khalt(5);
    }
}

uint32_t sched_idle_sleep(void)
{
    struct core *core = core_self();
    if (machine_stuck()) {
        kputs("no runnable thread\n");
        khalt(5);
    }
    /*
     * No thread runs, so no turn needs ending: the stall takes no tick before the nearest deadline,
     * and an end of a turn the trap's count found due is moot, since the turn is over.
     */
    core->turn_due = false;
    core->wake_stale = true;
    uint32_t wake = sched_wake_ticks();
    core->wake_tick = sched_ticks + wake;
    return wake;
}

bool sched_idle_wake(uint32_t ticks, bool device)
{
    struct core *core = core_self();
    /* Under tracing time moves only by record, so the stall counts none of it. */
    if (debug_trace) {
        ticks = 0;
    }
    if (ticks != 0 || core->ticks != sched_ticks) {
        tick_advance(ticks);
    }
    if (device) {
        sched_claim_interrupts();
    }
    core->turn = next_turn(turn_close());
    if (core->turn == NULL) {
        return false;
    }
    switch_to(core->turn);
    return true;
}

void sched_idle(void)
{
    uint32_t ticks;
    bool device;
    do {
        LOOP_WAIT("an interrupt, in intr_wait");
        device = intr_wait(sched_idle_sleep(), &ticks);
    } while (!sched_idle_wake(ticks, device));
}

void sched_run_next(void)
{
    core_self()->turn = next_turn(turn_close());
    run_turn();
}

/*
 * The turn ends: the thread whose turn it was is charged for it
 * and goes to the back of the queue its account puts it on, if it may run, and the next thread has its turn.
 * One that lost its units during its turn goes nowhere, and without it the queues may be empty.
 */
static void turn_end(void)
{
    struct core *core = core_self();
    uint32_t offset = turn_close();
    struct thread *was = core->turn;
    core->turn = NULL;
    if (was != NULL) {
        thread_settle(was);
    }
    core->turn = next_turn(offset);
    run_turn();
}

void sched_tick(uint32_t ticks)
{
    tick_advance(ticks);
    turn_end();
}

void sched_count(uint32_t ticks)
{
    struct core *core = core_self();
    if (ticks != 0 || core->ticks != sched_ticks) {
        tick_advance(ticks);
        core->turn_due |= !tick_before(sched_ticks, core->wake_tick);
    }
}

uint32_t sched_wake(void)
{
    struct core *core = core_self();
    /* Under tracing only a record ends a turn, so a count before tracing began ends none. */
    if (core->turn_due) {
        core->turn_due = false;
        if (!debug_trace) {
            turn_end();
        }
    }
    if (!core->wake_stale) {
        return 0;
    }
    core->wake_stale = false;
    uint32_t ticks = sched_wake_ticks();
    core->wake_tick = sched_ticks + ticks;
    return ticks;
}
