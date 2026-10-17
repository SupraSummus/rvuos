/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "irq.h"
#include "kernel.h"
#include "object.h"
#include "route.h"
#include "sched.h"
#include "timer.h"
#include "trap.h"

struct core cores[CORES] = {
    [0 ... CORES - 1] = { .nearest_release = NEAREST_NONE, .nearest_deadline = NEAREST_NONE },
};
uint32_t sched_ticks;
struct watchdog watchdog;

paddr_t unit_place[MACHINE_UNITS];
uint32_t armed_sources;

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

/* This core's timer wakes for a deadline: one nearer than its nearest brings its next interrupt forward to it. */
static void wake_for(uint32_t deadline)
{
    struct core *core = core_self();
    if (tick_before(deadline, core->nearest_deadline)) {
        core->nearest_deadline = deadline;
        core->wake_stale = true;
    }
}

void irq_set_bits(struct irq *irq, uint32_t bits)
{
    if (irq->line != LOG_IRQ_LINE) {
        count_armed(irq_armed(irq), bits != 0);
    }
    /* A timer line wakes the core that armed it, and only that one. */
    if (line_is_timer(irq->line) && bits != 0) {
        irq->core = (uint8_t)core_index();
        wake_for(irq->deadline);
    }
    irq->bits = bits;
}

/* The watchdog wakes the core that fed it last, as a timer line does the core that armed it. */
void sched_watchdog(uint32_t deadline)
{
    watchdog = (struct watchdog){ .armed = true, .core = (uint8_t)core_index(), .deadline = deadline };
    wake_for(deadline);
}

/*
 * A ring is a list of objects, each linked to the next and the one before by their physical addresses,
 * through two words of each that lie side by side, headed by the oldest, so the newest is that one's previous.
 * A notification's waiters are a ring of threads, through their queue_next and queue_prev, while they wait,
 * a borrower's lenders one through their lend_next and lend_prev,
 * and each of a core's queues a ring of places, through theirs, while their threads are ready and their turn is nowhere.
 */
struct links {
    paddr_t next;
    paddr_t prev;
};
#define WAITERS offsetof(struct thread, queue_next)
#define LENDS offsetof(struct thread, lend_next)
#define QUEUED offsetof(struct place, queue_next)
_Static_assert(offsetof(struct thread, queue_prev) == WAITERS + sizeof(paddr_t) &&
                   offsetof(struct thread, lend_prev) == LENDS + sizeof(paddr_t) &&
                   offsetof(struct place, queue_prev) == QUEUED + sizeof(paddr_t),
               "a ring's two links lie side by side");

static struct links *links_at(paddr_t at, size_t off)
{
    return (struct links *)((char *)p2v(at) + off);
}

static void ring_push(paddr_t *head, paddr_t self, size_t off)
{
    struct links *s = links_at(self, off);
    if (*head == 0) {
        s->next = s->prev = self;
        *head = self;
    } else {
        struct links *oldest = links_at(*head, off);
        s->next = *head;
        s->prev = oldest->prev;
        links_at(oldest->prev, off)->next = self;
        oldest->prev = self;
    }
}

static void ring_remove(paddr_t *head, paddr_t self, size_t off)
{
    struct links *s = links_at(self, off);
    if (s->next == self) {
        *head = 0;
    } else {
        links_at(s->prev, off)->next = s->next;
        links_at(s->next, off)->prev = s->prev;
        if (*head == self) {
            *head = s->next;
        }
    }
    s->next = s->prev = 0;
}

void sched_wait(struct thread *t, struct notification *ntfn)
{
    ring_push(&ntfn->waiters, v2p(t), WAITERS);
    t->waiting_on = v2p(&ntfn->hdr);
    t->state = THREAD_WAITING;
}

static void unlend(struct thread *t);

/* Take a waiting thread off its notification's ring; it lends nothing any more. */
static void unwait(struct thread *t)
{
    ring_remove(&((struct notification *)p2v(t->waiting_on))->waiters, v2p(t), WAITERS);
    t->waiting_on = 0;
    if (t->lend_to != 0) {
        unlend(t);
    }
}

/*
 * A place of a ready thread that may run there and whose turn it is nowhere waits on one of three queues of its core,
 * the run queue while it has a tick in its account, else the spare queue or the spent queue by its binding.
 * Taking from the front and joining at the back, of each, is the whole scheduling policy.
 */
static paddr_t *queue_head(struct core *core, uint8_t queue)
{
    return queue == QUEUE_RUN ? &core->run_queue : queue == QUEUE_SPARE ? &core->spare_queue : &core->spent_queue;
}

/* Take a place off the queue it waits on, which its thread's turn, or the next, takes it from. */
static void place_unqueue(struct place *p)
{
    ring_remove(queue_head(&cores[p->core], p->queue), v2p(p), QUEUED);
    p->queue = QUEUE_NONE;
}

/*
 * Whether it is the thread's turn, or its account pays for the turn of a thread that runs on lent time,
 * on the core it names: a thread has either on one core at most, and while it has, none of its places waits on a queue.
 */
static bool is_turn(const struct thread *t)
{
    const struct core *core = &cores[t->core];
    return core->turn == t || core->payer == t;
}

/* The thread a core's turn is charged to: the one that lends the turn's thread its time, or the turn's own. */
static struct thread *turn_payer(const struct core *core)
{
    return core->payer != NULL ? core->payer : core->turn;
}

/* The place a core's turn is charged to: the payer's place on the core, NULL with no turn, or no such place. */
static struct place *turn_place(const struct core *core)
{
    const struct thread *payer = turn_payer(core);
    return payer != NULL ? thread_place_on(payer, (uint32_t)(core - cores)) : NULL;
}

/* The core whose time a place holds, which it waits on the queues of; its last while it is empty. */
static uint32_t place_core(const struct place *p)
{
    const struct cap *b = place_time(p);
    return b->type == CAP_TIME ? unit_core(range_first(b)) : p->core;
}

/*
 * The count a place's account is counted to: its core's while it holds the time of the core whose turn its thread has,
 * or pays for, which the core charges it for, and the machine's otherwise, an empty place's among them.
 */
static uint32_t count_of(const struct place *p)
{
    const struct thread *t = place_thread(p);
    const struct cap *b = place_time(p);
    return is_turn(t) && b->type == CAP_TIME && unit_core(range_first(b)) == t->core ? cores[t->core].ticks : sched_ticks;
}

/* A place's account at the count: it gains its tick's parts every tick since its stamp, up to its cap. */
static uint32_t account_now(const struct place *p)
{
    uint64_t balance = p->balance + (uint64_t)(count_of(p) - p->stamp) * tick_gain(p);
    uint32_t cap = account_cap(p);
    return balance > cap ? cap : (uint32_t)balance;
}

/* Bring a place's account up to the count; inline, since every switch calls it and clang stopped inlining it alone. */
static inline void account_refill(struct place *p)
{
    p->balance = account_now(p);
    p->stamp = count_of(p);
}

/* Whether an account holds a turn's worth, a tick: then the thread has time, and takes turns with those that have. */
static bool has_time(uint32_t balance)
{
    return balance >= tick_parts();
}

/* The ticks an account at balance, below a tick, takes to reach one, for a place with units. */
static uint32_t account_fill(const struct place *p, uint32_t balance)
{
    uint32_t gain = tick_gain(p);
    return (tick_parts() - balance + gain - 1) / gain;
}

/*
 * A place's account after whole ticks of its turn, at least one, as if each tick had been taken on its own:
 * a tick that starts with a tick in the account costs a tick less the tick's gain, and any other earns the gain.
 * Either way the balance moves by the gain modulo a tick.
 * It falls while it holds a tick, and once below one it stays in [gain, tick + gain),
 * a stretch a tick wide, so the one value there in the right residue is where it ends.
 * The cap never cuts in: that stretch lies within the cap of a place with units.
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
 * What the turn's place, p, owes for the turn up to offset counts into the tick:
 * a count's parts for each count from turn_from, if it had time as the turn began, and nothing on spare time.
 * A turn with time began with a tick in the account and owes at most the rest of the tick, so the account covers it.
 */
static uint32_t turn_owes(const struct place *p, uint32_t offset)
{
    uint32_t from = core_self()->turn_from;
    return offset > from && p != NULL && has_time(p->balance) ? COUNT_PARTS * (offset - from) : 0;
}

/* The account of the turn's place, p, at the next tick: it pays for the rest of this one and earns the gain. */
static uint32_t turn_next(const struct place *p)
{
    uint32_t next = p->balance - turn_owes(p, timer_tick_counts()) + tick_gain(p);
    return next < account_cap(p) ? next : account_cap(p);
}

/*
 * The ticks from the count to the one that drains the account of the turn's place, p, which has time:
 * the first to leave it less than a tick; NEAREST_NONE for none, with the whole processor.
 * After the next tick every tick costs the account a tick less the gain.
 */
static uint32_t turn_drain(const struct place *p)
{
    uint32_t next = turn_next(p);
    if (!has_time(next)) {
        return 1;
    }
    uint32_t loss = tick_parts() - tick_gain(p);
    return loss != 0 ? (next - tick_parts()) / loss + 2 : NEAREST_NONE;
}

/*
 * The place whose time a borrower's place runs on while it has none of its own:
 * that of the thread that has lent the borrower time longest, on the place's core, if that one holds the core's time,
 * else NULL. One step: the lender lends its own account, not what it may borrow itself.
 */
static struct place *place_lender(const struct place *p)
{
    const struct thread *t = place_thread(p);
    const struct cap *b = place_time(p);
    if (t->lenders == 0 || b->type != CAP_TIME) {
        return NULL;
    }
    return thread_place_on(p2v(t->lenders), unit_core(range_first(b)));
}

/* The borrower's place a lender's place lends its time to, if it is that one's: the inverse of place_lender, or NULL. */
static struct place *place_borrower(const struct place *l)
{
    const struct thread *lt = place_thread(l);
    const struct cap *b = place_time(l);
    if (lt->lend_to == 0 || b->type != CAP_TIME) {
        return NULL;
    }
    const struct thread *h = p2v(lt->lend_to);
    return h->lenders == v2p(lt) ? thread_place_on(h, unit_core(range_first(b))) : NULL;
}

/*
 * The queue a place waits on, as its thread's state and turn, its account at balance, its lender's and its binding now say:
 * a place with time of its own or lent has the run queue,
 * and one whose own units or its lender's will give it time again the spent queue, unless it may run on spare time.
 * The lender is place_lender's, which the caller looked up only if the place has no time of its own.
 */
static uint8_t place_queue(const struct place *p, uint32_t balance, const struct place *l)
{
    const struct thread *t = place_thread(p);
    if (t->state != THREAD_READY || is_turn(t) || place_time(p)->type != CAP_TIME) {
        return QUEUE_NONE;
    }
    if (has_time(balance) || (l != NULL && has_time(account_now(l)))) {
        return QUEUE_RUN;
    }
    if (place_spare(p)) {
        return QUEUE_SPARE;
    }
    return place_units(p) != 0 || (l != NULL && place_units(l) != 0) ? QUEUE_SPENT : QUEUE_NONE;
}

/*
 * Bring a place's account up to the count and move the place to the queue it now waits on,
 * or take it off the one it was on.
 * A place waits on the queues of the core whose time it holds, so one emptied leaves its queue.
 * A place that waits without time, with units or a lender with units,
 * brings its core's release forward to the tick its account, or its lender's, reaches a tick.
 * Another core is told of a place that joins one of its queues, and of a release it brought forward.
 */
static void place_settle(struct place *p)
{
    core_self()->wake_stale = true;
    account_refill(p);
    struct core *was = &cores[p->core];
    struct core *core = &cores[place_core(p)];
    const struct place *l = has_time(p->balance) ? NULL : place_lender(p);
    uint8_t queue = place_queue(p, p->balance, l);
    bool joined = queue != QUEUE_NONE && (queue != p->queue || core != was);
#if SCHED_EDF
    core->run_joined |= joined && queue == QUEUE_RUN;
#endif
    if (queue != p->queue || core != was) {
        if (p->queue != QUEUE_NONE) {
            place_unqueue(p);
        }
        p->core = (uint8_t)(core - cores);
        if (queue != QUEUE_NONE) {
            ring_push(queue_head(core, queue), v2p(p), QUEUED);
        }
        p->queue = queue;
    }
    /* Neither account holds a tick there, or the place would have the run queue. */
    uint32_t fill = NEAREST_NONE;
    if (queue == QUEUE_SPARE || queue == QUEUE_SPENT) {
        if (place_units(p) != 0) {
            fill = account_fill(p, p->balance);
        }
        if (l != NULL && place_units(l) != 0 && account_fill(l, account_now(l)) < fill) {
            fill = account_fill(l, account_now(l));
        }
    }
    bool sooner = fill != NEAREST_NONE && tick_before(sched_ticks + fill, core->nearest_release);
    if (sooner) {
        core->nearest_release = sched_ticks + fill;
    }
    if (joined || sooner) {
        core_notify(core);
    }
}

/*
 * Settle each of a thread's places, a step for each, CORES at most.
 * The core whose turn it has is told of any change to it, which may end the turn sooner.
 */
static void thread_settle(struct thread *t)
{
    for (uint32_t i = 0; i < thread_places(t); i++) {
        LOOP_BOUND(CORES);
        place_settle(thread_place(t, i));
    }
    if (is_turn(t)) {
        core_notify(&cores[t->core]);
    }
}

/*
 * Settle a thread whose units or account changed other than by a turn, and the borrower it lends its time to,
 * whose queues that account decides too.
 * A lender waits, so its borrower's queues rest on the lender's own accounts and not on what it borrows,
 * and settling it is one step for each of its places.
 * A thread that becomes ready, or whose turn ends, lends nothing, and its borrower's turn its caller settles.
 */
static void lender_settle(struct thread *t)
{
    thread_settle(t);
    struct thread *h = t->lend_to != 0 ? p2v(t->lend_to) : NULL;
    if (h != NULL && h->lenders == v2p(t)) {
        thread_settle(h);
    }
}

/* How far into the tick the counter is; nowhere under tracing, where the clock is the tick count alone. */
static uint32_t clock_offset(void)
{
    return debug_trace ? 0 : timer_offset();
}

/* The turn ends where the counter is: charge its place, if any, and return how far into the tick that is. */
static uint32_t turn_close(void)
{
    uint32_t offset = clock_offset();
    struct place *p = turn_place(core_self());
    if (p != NULL) {
        p->balance -= turn_owes(p, offset);
    }
    return offset;
}

/* The turn's payer pays no more, the turn being over: it goes to the queues it now waits on, if it is ready. */
static void payer_drop(void)
{
    struct core *core = core_self();
    struct thread *payer = core->payer;
    core->payer = NULL;
    if (payer != NULL) {
        thread_settle(payer);
    }
}

/*
 * A thread's turn begins on this core: it names the core, and its places on the others leave their queues,
 * to join them again as the turn ends, so that no other core takes it meanwhile.
 * A step for each of its places, CORES at most.
 */
static void turn_take(struct thread *t)
{
    t->core = (uint8_t)core_index();
#if CORES > 1
    for (uint32_t i = 0; i < thread_places(t); i++) {
        LOOP_BOUND(CORES);
        struct place *q = thread_place(t, i);
        if (q->queue != QUEUE_NONE) {
            place_unqueue(q);
        }
    }
#endif
}

#if SCHED_EDF
/*
 * An experiment of the order open decision 9 weighs, built with make SCHED=edf:
 * the run queue is taken earliest deadline first rather than oldest first,
 * a place's deadline the tick its account would be full again if its thread did not run, and its window after that.
 * A thread that waits keeps its deadline, as its account gains the same every tick, until the account is full,
 * and one that runs moves it later by COUNT_PARTS / units ticks for each tick it pays for:
 * the tick its account would be full is its virtual time, which runs at the inverse of its share as it runs.
 * So a thread that ran less than its units' part has the nearer deadline, and one that wants the core all along the furthest,
 * and work of c made ready while the account is full ends c / its share and its window after, whatever the others do,
 * since the units of a core add up to its time, which is all earliest deadline first needs.
 * The window is the place's to choose: a longer one only puts its own work later.
 * The search walks the run queue, TIME_UNITS places at most, since each earns a unit or borrows from a lender that does.
 */

/* A place's window, in ticks. */
static uint32_t place_window(const struct place *p)
{
    return p->pad != 0 ? 1u << (p->pad - 1u) : 0;
}

/* The deadline of a place whose account at balance was counted to the tick at; NEAREST_NONE ahead without units. */
static uint32_t deadline_of(const struct place *p, uint32_t balance, uint32_t at)
{
    uint32_t gain = tick_gain(p);
    if (gain == 0) {
        return sched_ticks + NEAREST_NONE;
    }
    uint32_t cap = account_cap(p);
    return at + (cap > balance ? (cap - balance + gain - 1) / gain : 0) + place_window(p);
}

/* A queued place's deadline: its own account's while it has time, else its lender's. */
static uint32_t place_deadline(const struct place *p)
{
    uint32_t balance = account_now(p);
    if (has_time(balance)) {
        return deadline_of(p, balance, sched_ticks);
    }
    const struct place *l = place_lender(p);
    return l != NULL ? deadline_of(l, account_now(l), sched_ticks) : sched_ticks + NEAREST_NONE;
}

/* The place on a core's run queue with the earliest deadline, the oldest of those tied, and that deadline. */
static struct place *run_earliest(const struct core *core, uint32_t *deadline)
{
    paddr_t at = core->run_queue;
    struct place *best = NULL;
    uint32_t best_at = 0;
    for (uint32_t i = 0; i < TIME_UNITS; i++) {
        LOOP_BOUND(TIME_UNITS);
        struct place *p = p2v(at);
        uint32_t d = place_deadline(p);
        if (best == NULL || tick_before(d, best_at)) {
            best = p;
            best_at = d;
        }
        at = p->queue_next;
        if (at == core->run_queue) {
            break;
        }
    }
    *deadline = best_at;
    return best;
}
#endif

/*
 * The thread of the oldest place with time, or with none of the oldest that may run on spare time,
 * taken off its queues with the place's account brought up to the count: its turn begins offset counts into the tick,
 * and every count of it is charged to that place from there,
 * or to its lender's place while it has no time and its lender has, which pays for the turn from then on.
 * NULL when neither queue has one.
 */
static struct thread *next_turn(uint32_t offset)
{
    struct core *core = core_self();
    core->turn_from = offset;
    paddr_t *head = core->run_queue != 0 ? &core->run_queue : &core->spare_queue;
    if (*head == 0) {
        return NULL;
    }
    struct place *p = p2v(*head);
#if SCHED_EDF
    core->run_joined = false;
    if (core->run_queue != 0) {
        uint32_t deadline;
        p = run_earliest(core, &deadline);
    }
#endif
    struct thread *t = place_thread(p);
    place_unqueue(p);
    turn_take(t);
    account_refill(p);
    struct place *l = has_time(p->balance) ? NULL : place_lender(p);
    if (l != NULL && has_time(account_now(l))) {
        struct thread *lt = place_thread(l);
        core->payer = lt;
        lt->core = (uint8_t)core_index();
        account_refill(l);
    }
    return t;
}

void sched_ready(struct thread *t)
{
    t->state = THREAD_READY;
    thread_settle(t);
}

struct place *sched_place_for(const struct thread *t, uint32_t core)
{
    struct place *empty = NULL;
    for (uint32_t i = 0; i < thread_places(t); i++) {
        LOOP_BOUND(CORES);
        const struct cap *b = &t->time[i];
        if (b->type == CAP_TIME && unit_core(range_first(b)) == core) {
            return thread_place(t, i);
        }
        if (b->type == CAP_NONE && empty == NULL) {
            empty = thread_place(t, i);
        }
    }
    return empty;
}

void sched_bind(struct place *p, uint32_t first, uint32_t count, uint8_t rights, struct cap *parent, uint32_t window_exp)
{
#if SCHED_EDF
    p->pad = (uint8_t)window_exp;
#else
    (void)window_exp;
#endif
    struct cap *b = place_time(p);
    account_refill(p);
    uint32_t kept = p->balance;
    if (b->type != CAP_NONE) {
        cap_delete(b, false);
    }
    *b = cap_to_time(first, count, rights);
    b->b = held_word(v2p(place_thread(p)), 0);
    cap_attach(parent, b);
    for (uint32_t i = 0; i < count; i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_place[first + i] = v2p(p);
    }
    /*
     * What the account held stays, and settling holds it to what the new units hold at most.
     * It counts from now, on the count the binding puts it on: an empty place's was the machine's.
     */
    p->balance = kept;
    p->stamp = count_of(p);
    lender_settle(place_thread(p));
}

void sched_unbind(struct cap *bound)
{
    struct thread *t = p2v(cap_holder(bound));
    for (uint32_t i = 0; i < range_count(bound); i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_place[range_first(bound) + i] = 0;
    }
    *bound = (struct cap){ 0 };
    /* An empty place holds nothing, so settling empties its account as it takes the place off its queue. */
    lender_settle(t);
}

/*
 * Stop a thread that is not stopped, the stop's and the loss of a process's alike, which its caller settles.
 * A waiting one leaves its notification's waiters and lends nothing any more,
 * and goes back to the call it waited in, which wrote no register, so a resume makes the call again.
 * Another core that runs it traps first, marked to drop the trap; whether one has, see core_thread_stopped.
 */
static bool thread_stop(struct thread *t)
{
    if (t->state == THREAD_WAITING) {
        unwait(t);
        t->frame.pc -= CALL_SIZE;
    }
    t->state = THREAD_STOPPED;
    return core_thread_stopped(t);
}

/*
 * Settling takes a stopped thread off its queue; syscall_dispatch hands the processor on from a running one,
 * and another core that runs it traps first, and hands it on as it has the lock.
 * A thread stopped already, by a stop or a fault, stays as it is.
 */
void sched_unhost(struct cap *hosted)
{
    struct thread *t = p2v(cap_holder(hosted));
    /* switch_to would find the running thread with no process to save the CSRs it keeps into, so they go now. */
    if (core_self()->current == t) {
        board_user_csrs_switch(thread_process(t), NULL);
    }
    if (t->state != THREAD_STOPPED) {
        thread_stop(t);
    }
    *hosted = (struct cap){ 0 };
    thread_settle(t);
}

/*
 * A trap another core took for the thread before the stop dropped it, whose stacking lost the thread's registers,
 * is the thread's fault, as it would have been at that trap: it stays stopped where it faulted, unheard,
 * which the log says as for any fault; see OP_THREAD_STOP.
 */
uint32_t sched_stop(struct thread *t)
{
    uint32_t was = t->state;
    if (was != THREAD_STOPPED) {
        if (thread_stop(t) && frame_lost(&t->frame)) {
            t->flags |= THREAD_FAULTED;
            kputs("user fault\n");
            report_frame(&t->frame);
        }
        thread_settle(t);
    }
    return was;
}

/* The place that earns a unit first of those it earns, else NULL, so that a look at each unit meets it once. */
static struct place *unit_first(uint32_t unit)
{
    struct place *p = unit_place[unit] != 0 ? p2v(unit_place[unit]) : NULL;
    return p != NULL && range_first(place_time(p)) == unit ? p : NULL;
}

/*
 * A place with units earns them, so a look at each unit finds every one,
 * and the units are a fixed few, as the timer lines are; see DESIGN.md, "Bounded work".
 * A place with none has an empty account already.
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
        struct place *p = unit_first(i);
        if (p != NULL) {
            p->balance = account_cap(p);
            p->stamp = count_of(p);
            lender_settle(place_thread(p));
        }
    }
}

/*
 * Look at every place with units of a core that waits without time: one whose account reached a tick joins the run queue,
 * and the others bring the release forward to the tick theirs does.
 * It looks at each unit of the core, a fixed few, as the tick looks at the timer lines,
 * and only at the tick nearest_release said an account would reach a tick.
 * A place's units are a run of one core's, each naming it, so the look steps over the rest of a place's at once,
 * and takes a step for each place and each unit nobody earns.
 * Accounts count the machine's ticks, so the core that counts the tick looks, whichever core's release it is.
 */
static void release(uint32_t c)
{
    uint32_t first = c * TIME_UNITS;
    cores[c].nearest_release = sched_ticks + NEAREST_NONE;
    for (uint32_t i = 0; i < TIME_UNITS;) {
        LOOP_BOUND(TIME_UNITS);
        struct place *p = unit_first(first + i);
        i += p != NULL ? place_units(p) : 1;
        if (p != NULL && (p->queue == QUEUE_SPARE || p->queue == QUEUE_SPENT)) {
            place_settle(p);
        }
        /* A lender waits on no queue, and its account may give the borrower it lends to time. */
        struct place *h = p != NULL ? place_borrower(p) : NULL;
        if (h != NULL && (h->queue == QUEUE_SPARE || h->queue == QUEUE_SPENT)) {
            place_settle(h);
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

/*
 * Wake a thread waiting on a notification with every bit set there:
 * the thread whose time pays for the core's turn first, so that its borrower's signal hands the turn back, true then.
 * Out of line, so that a signal nobody waits for saves no registers.
 */
static __attribute__((noinline)) bool signal_wake(struct notification *ntfn)
{
    struct thread *payer = core_self()->payer;
    bool paying = payer != NULL && payer->waiting_on == v2p(&ntfn->hdr);
    wake(paying ? payer : p2v(ntfn->waiters), ntfn->bits);
    ntfn->bits = 0;
    return paying;
}

bool sched_signal(struct notification *ntfn, uint32_t bits)
{
    ntfn->bits |= bits;
    return ntfn->waiters != 0 && signal_wake(ntfn);
}

static void switch_to(struct thread *next);

/*
 * The thread that paid for the turn was woken, and may run now: it has the rest of the turn, from where it was charged,
 * and the thread whose turn it was goes to the back of its queue, as at a tick.
 */
void sched_turn_back(void)
{
    struct core *core = core_self();
    struct thread *was = core->turn, *payer = core->payer;
    core->payer = NULL;
    core->turn = payer;
    thread_settle(was);
    thread_settle(payer);
    switch_to(payer);
}

/* A borrower's lenders are a ring of threads through their lend_next and lend_prev, headed by its lenders, oldest first. */
static void lend_push(struct thread *h, struct thread *l)
{
    l->lend_to = v2p(h);
    ring_push(&h->lenders, v2p(l), LENDS);
}

static void lend_remove(struct thread *h, struct thread *l)
{
    ring_remove(&h->lenders, v2p(l), LENDS);
    l->lend_to = 0;
}

/*
 * A thread whose wait ends lends no more, and its borrower settles if it borrowed from that one.
 * Out of line, so that a signal that wakes a thread that lends nothing, or none, costs what it did.
 */
static __attribute__((noinline)) void unlend(struct thread *t)
{
    struct thread *h = p2v(t->lend_to);
    bool first = h->lenders == v2p(t);
    lend_remove(h, t);
    if (first) {
        thread_settle(h);
    }
}

void sched_lend(struct thread *t, struct notification *ntfn, struct thread *borrower)
{
    sched_wait(t, ntfn);
    lend_push(borrower, t);
    if (borrower->lenders == v2p(t)) {
        thread_settle(borrower);
    }
    /*
     * A lender whose turn its own time pays for, and that now lends first to a borrower waiting for a turn on its core,
     * hands it the rest of the turn, which it goes on paying for: the turn neither ends nor is charged here.
     */
    struct core *core = core_self();
    struct place *tp = thread_place_on(t, core_index());
    struct place *bp = thread_place_on(borrower, core_index());
    if (core->payer == NULL && tp != NULL && has_time(tp->balance) && bp != NULL && place_lender(bp) == tp &&
        bp->queue != QUEUE_NONE) {
        place_unqueue(bp);
        core->turn = borrower;
        core->payer = t;
        turn_take(borrower);
        account_refill(bp);
        switch_to(borrower);
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
    struct irq *irq = p2v(cap_holder(signalled));
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
 * count them, fire every timer line that is due, halt the machine if the watchdog is,
 * and give time again to the threads of every core whose account reached a tick.
 * The timer lines are a fixed few, so the tick looks at each of them and at the watchdog
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the tick no armed one is due,
 * and each core's nearest deadline of those still armed on it, and of the watchdog it fed, is exact again,
 * which only puts it later, so no other core is told.
 */
static void tick_advance(uint32_t ticks)
{
    struct core *core = core_self();
    struct place *paid = turn_place(core);
    uint32_t owed = sched_ticks + ticks - core->ticks;
    core->wake_stale = true;
    if (owed != 0) {
        if (paid != NULL) {
            account_refill(paid);
            paid->balance = turn_next(paid);
            if (owed > 1) {
                paid->balance = account_after(paid->balance, place_units(paid), owed - 1);
            }
            paid->stamp += owed;
        }
        core->turn_from = 0;
        core->ticks += owed;
        /* A turn on lent time costs its thread nothing, and it earns its units as every tick earns them. */
        if (core->payer != NULL) {
            struct place *own = thread_place_on(core->turn, core_index());
            if (own != NULL) {
                account_refill(own);
            }
        }
    }
    sched_ticks += ticks;
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        cores[c].nearest_deadline = sched_ticks + NEAREST_NONE;
    }
    for (uint32_t i = 0; i < TIMER_LINES; i++) {
        LOOP_BOUND(TIMER_LINES);
        struct irq *irq = line_binding(IRQ_LINES + i);
        if (irq == NULL || !irq_armed(irq)) {
            continue;
        }
        struct core *armed = &cores[irq->core];
        if (irq_due(irq, sched_ticks)) {
            irq_signal(irq);
        } else if (tick_before(irq->deadline, armed->nearest_deadline)) {
            armed->nearest_deadline = irq->deadline;
        }
    }
    if (watchdog.armed) {
        if (!tick_before(sched_ticks, watchdog.deadline)) {
            kputs("watchdog: not fed in time\n");
            khalt(7);
        }
        struct core *fed = &cores[watchdog.core];
        if (tick_before(watchdog.deadline, fed->nearest_deadline)) {
            fed->nearest_deadline = watchdog.deadline;
        }
    }
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
    uint32_t wake = core->nearest_deadline - sched_ticks;
    if (tick_before(core->nearest_release, core->nearest_deadline)) {
        wake = core->nearest_release - sched_ticks;
    }
    if (turn == NULL) {
        return wake;
    }
    /* Each tick decides afresh who pays for a turn on lent time, and whether it has any. */
    if (core->payer != NULL) {
        return 1;
    }
    /* A thread that lost the time of this core during its turn here, its place emptied, ends the turn at the next tick. */
    const struct place *p = thread_place_on(turn, core_index());
    if (core->run_queue != 0 || p == NULL) {
        return 1;
    }
    uint32_t balance = account_now(p);
    bool alone = place_spare(p) && core->spare_queue == 0;
    if (!has_time(balance)) {
        return alone ? wake : 1;
    }
    if (alone) {
        return wake;
    }
    uint32_t drain = turn_drain(p);
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
    case CAP_ROUTE:
        /* Its ends were cleared with the object's nodes, which disarmed it; the channel is free again. */
        route_forget((struct route *)o);
        break;
    case CAP_THREAD: {
        struct thread *t = (struct thread *)o;
        /*
         * It lost its process before, and so stopped; a core that ran it may not have looked since,
         * and the turn there was its, so whoever paid for it pays no more and waits on its queue.
         */
        for (uint32_t c = 0; c < CORES; c++) {
            LOOP_BOUND(CORES);
            struct thread *payer = cores[c].payer;
            if (cores[c].turn == t && payer != NULL) {
                cores[c].payer = NULL;
                thread_settle(payer);
            }
        }
        /*
         * A thread that lends it its time named it in its call, which can no longer be answered.
         * Its progress is the ring: a stop leaves the rest lending to a thread still whole.
         */
        while (t->lenders != 0) {
            LOOP_PAID(sched_forget, waiter, "a thread that lent it its time, by a call of its own");
            struct thread *l = p2v(t->lenders);
            lend_remove(t, l);
            unwait(l);
            l->frame.regs[REG_A0] = KERR_INVALID_CAP;
            l->frame.regs[REG_A1] = 0;
            sched_ready(l);
            if (t->lenders != 0 && cap_stop_here(preempt)) {
                return false;
            }
        }
        core_thread_gone(t);
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
    struct core *core = core_self();
    /*
     * The thread leaving may have lost its process during its call, or another core destroyed it;
     * the one coming has one, since it is ready.
     */
    struct process *from = core->current != NULL ? thread_process(core->current) : NULL;
    struct process *to = thread_process(next);
    if (to != from) {
        process_activate(to);
        board_user_csrs_switch(from, to);
        core->pmp_stale = false;
    } else if (core->pmp_stale) {
        process_activate(to);
        core->pmp_stale = false;
    }
    /* A call stopped for an interrupt is the thread's, made again when it next runs. */
    if (next != core->current) {
        core->recall = false;
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
 * While tracing is on, time moves and lines fire only through OP_TEST_TICK
 * and OP_TEST_IRQ, which nobody is left to perform, so the same holds
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
    uint32_t offset = turn_close();
    payer_drop();
    core_self()->turn = next_turn(offset);
    run_turn();
}

void sched_turn_end(void)
{
    struct core *core = core_self();
    uint32_t offset = turn_close();
    struct thread *was = core->turn;
    core->turn = NULL;
    payer_drop();
    if (was != NULL) {
        thread_settle(was);
    }
    core->turn = next_turn(offset);
    run_turn();
}

void sched_tick(uint32_t ticks)
{
    tick_advance(ticks);
    sched_turn_end();
}

#if SCHED_EDF
/*
 * Whether a place that joined the run queue since the core last looked has a deadline before that of the turn's payer,
 * whose account is charged to the counter first: then it takes the processor, as from a turn on spare time.
 */
static bool deadline_due(struct core *core)
{
    struct place *p = turn_place(core);
    if (!core->run_joined || p == NULL) {
        return false;
    }
    core->run_joined = false;
    uint32_t mine = deadline_of(p, p->balance - turn_owes(p, clock_offset()), core->ticks);
    uint32_t theirs;
    run_earliest(core, &theirs);
    return tick_before(theirs, mine);
}
#else
static bool deadline_due(struct core *core)
{
    (void)core;
    return false;
}
#endif

/*
 * A turn on spare time is one no thread with time wants, so it ends as soon as one waits on the core's run queue,
 * unless the running thread stands at a call that stopped for an interrupt, which it makes again first.
 * Under SCHED_EDF so does a turn with time that a thread with an earlier deadline joined the run queue during.
 * The end of a call and of every trap ask it, so a wake by a call, an interrupt or another core takes the processor at once;
 * see DESIGN.md, "Scheduling".
 */
static void spare_end(struct core *core)
{
    if (core->run_queue != 0 && !core->recall && (sched_spare_turn(core) || deadline_due(core))) {
        sched_turn_end();
    }
}

void sched_call_end(void)
{
    struct core *core = core_self();
    if (core->current->state != THREAD_READY) {
        sched_run_next();
    } else {
        spare_end(core);
    }
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
            sched_turn_end();
        }
    }
    /* An interrupt, or another core, may have made a thread with time ready while the turn runs on spare time. */
    spare_end(core);
    if (!core->wake_stale) {
        return 0;
    }
    core->wake_stale = false;
    uint32_t ticks = sched_wake_ticks();
    core->wake_tick = sched_ticks + ticks;
    return ticks;
}
