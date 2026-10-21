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
    [0 ... CORES - 1] = { .nearest_release = COUNT_NEVER, .nearest_deadline = COUNT_NEVER, .wake_at = COUNT_NEVER },
};
uint64_t sched_trace_clock;
struct watchdog watchdog;

paddr_t unit_place[MACHINE_UNITS];
uint64_t unit_vt[MACHINE_UNITS];
uint32_t armed_sources;

static void count_armed(bool was, bool is)
{
    if (is && !was) {
        armed_sources++;
    } else if (was && !is) {
        armed_sources--;
    }
}

/* True if deadline a comes before deadline b; deadlines are kept modulo 2^32, so as a signed difference. */
static bool due_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

/* This core's timer wakes for a deadline: one nearer than its nearest brings its next interrupt forward to it. */
static void wake_for(uint64_t deadline)
{
    struct core *core = core_self();
    if (deadline < core->nearest_deadline) {
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
void sched_watchdog(uint64_t deadline)
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
 * the run queue while it has time, else the spare queue or the spent queue by its binding.
 * Taking the earliest deadline from the run queue and the oldest from the spare queue,
 * and joining at the back of each, is the whole scheduling policy.
 */
static paddr_t *queue_head(struct core *core, uint8_t queue)
{
    return queue == QUEUE_RUN ? &core->run_queue : queue == QUEUE_SPARE ? &core->spare_queue : &core->spent_queue;
}

/*
 * Take a place off the queue it waits on, which its thread's turn, or the next, takes it from.
 * A core with nobody left on its spare and spent queues owes no release, so the timer does not wake for one.
 */
static void place_unqueue(struct place *p)
{
    struct core *core = &cores[p->core];
    ring_remove(queue_head(core, p->queue), v2p(p), QUEUED);
    p->queue = QUEUE_NONE;
    if (core->spare_queue == 0 && core->spent_queue == 0) {
        core->nearest_release = COUNT_NEVER;
    }
}

/*
 * Whether it is the thread's turn, or its units pay for the turn of a thread that runs on lent time,
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

/* The turn's own place on its core, which holds its request: on lent time too, as its payer's is another. */
static struct place *turn_own(const struct core *core)
{
    return core->turn != NULL ? thread_place_on(core->turn, (uint32_t)(core - cores)) : NULL;
}

/* The core whose time a place holds, which it waits on the queues of; its last while it is empty. */
static uint32_t place_core(const struct place *p)
{
    const struct cap *b = place_time(p);
    return b->type == CAP_TIME ? unit_core(range_first(b)) : p->core;
}

/*
 * Time.
 * A place's virtual time is the count of the clock its units have paid for its thread's running up to:
 * each count the thread runs on them moves it on TIME_UNITS / units counts, rounded up,
 * so a thread that runs its units' part of the core keeps it at the count, and one that runs more puts it ahead.
 * A place has time while its thread's request is open on its units,
 * or while its virtual time lies at most a span ahead of the count, so that the next may start; see "Requests".
 * A request starts no earlier than the count, so a place runs at most a span and a request ahead of what its units paid for,
 * and a thread that waited comes back with nothing saved.
 * A thread that wants the core all along so runs about a span ahead, and its requests are due after those of a thread
 * that waits and wakes with a span no longer than its own: the span it chose is how far it runs ahead.
 * There is no constant in this, but the units, the span the thread gives, and the clock.
 */

/*
 * Whether payer's units give p's thread time at the count now: its request is open on them, or the next may start,
 * the virtual time lying at most a span of payer's ahead of the clock.
 */
static bool has_time(const struct place *payer, const struct place *p, uint64_t now)
{
    return place_units(payer) != 0 && (request_open(payer, p) || payer->vt <= now + place_span(payer));
}

/* The count a place's units give it time again at, with no request open: its virtual time less its span. */
static uint64_t place_release(const struct place *p)
{
    return p->vt - place_span(p);
}

/*
 * The place whose time a borrower's place runs on while it has none of its own:
 * that of the thread that has lent the borrower time longest, on the place's core, if that one holds the core's time,
 * or, while that one has no time for it, of its own first lender, and on, LEND_DEPTH lenders up at most; else NULL.
 */
static struct place *place_lender(const struct place *p, uint64_t now)
{
    const struct thread *t = place_thread(p);
    const struct cap *b = place_time(p);
    if (t->lenders == 0 || b->type != CAP_TIME) {
        return NULL;
    }
    uint32_t core = unit_core(range_first(b));
    struct place *l = thread_place_on(p2v(t->lenders), core);
    for (uint32_t i = 1; i < LEND_DEPTH && l != NULL && !has_time(l, p, now); i++) {
        LOOP_BOUND(LEND_DEPTH);
        const struct thread *lt = place_thread(l);
        if (lt->lenders == 0) {
            break;
        }
        struct place *next = thread_place_on(p2v(lt->lenders), core);
        if (next == NULL) {
            break;
        }
        l = next;
    }
    return l;
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
 * The queue a place waits on, as its thread's state and turn, its virtual time, its lender's and its binding now say:
 * a place with time of its own or lent has the run queue,
 * and one whose own units or its lender's will give it time again the spent queue, unless it may run on spare time.
 * The lender is place_lender's.
 */
static uint8_t place_queue(const struct place *p, const struct place *l, uint64_t now)
{
    const struct thread *t = place_thread(p);
    if (t->state != THREAD_READY || is_turn(t) || place_time(p)->type != CAP_TIME) {
        return QUEUE_NONE;
    }
    if (has_time(p, p, now) || (l != NULL && has_time(l, p, now))) {
        return QUEUE_RUN;
    }
    if (place_spare(p)) {
        return QUEUE_SPARE;
    }
    return place_units(p) != 0 || (l != NULL && place_units(l) != 0) ? QUEUE_SPENT : QUEUE_NONE;
}

/*
 * Requests.
 * A thread runs in requests, and the run queue is taken earliest deadline first.
 * A request is due its payer's span past the virtual time it starts at,
 * the span its thread's last wait gave, or its binding, or the default span,
 * and it is over once the thread has run units / TIME_UNITS counts for each count of the span,
 * its payer's virtual time having reached its deadline.
 * So a short span brings a request nearer and makes it shorter, and each request puts the next further.
 * A request starts as its thread is made ready or bound, and the next as one is over, at the latest of the virtual time and the count,
 * once the virtual time lies within a span of the count: a thread that ran further ahead of its units waits for them.
 * A thread that waits gives up the rest of its request.
 * Its deadline stands for the whole request, so the order changes only as a thread is made ready,
 * which takes the core from a turn whose request is due later, and as the turn's request is over, which the timer marks.
 * A wake while the virtual time is at or behind the count is answered within its span, with units / TIME_UNITS of it,
 * whatever the others do, since the units of a core add up to its time, which is all earliest deadline first needs;
 * and whatever span a thread gives, its requests take that part of their spans and no more, so no span costs another thread.
 * A place on lent time runs its lender's requests, at the lender's units and the lender's span.
 */

/*
 * The place whose units a place's thread runs on there at the count now, l being its lender's place or NULL:
 * those its request in progress was made on, while they are units, else its own while they give it time, else its lender's, or NULL.
 */
static struct place *request_payer(struct place *p, struct place *l, uint64_t now)
{
    if (l != NULL && place_units(l) != 0 && request_open(l, p)) {
        return l;
    }
    if (has_time(p, p, now)) {
        return p;
    }
    return l != NULL && has_time(l, p, now) ? l : NULL;
}

/*
 * p's thread goes on with its request on payer's units, which give it time, or starts the next if none is open there:
 * at the latest of payer's virtual time and the count, now, due a span of payer's past that.
 */
static void request_renew(struct place *p, struct place *payer, uint64_t now)
{
    if (request_open(payer, p)) {
        return;
    }
    if (payer->vt < now) {
        payer->vt = now;
    }
    p->deadline = (uint32_t)payer->vt + place_span(payer);
    p->span = (uint8_t)((p->span & PLACE_SPAN) | PLACE_REQUEST | (payer != p ? PLACE_LENT : 0));
}

/*
 * n / units, units at most TIME_UNITS, in words, since the kernel divides no wider:
 * a word of n at a time, then the rest by halves, each dividend below 2^32 since a remainder is below TIME_UNITS.
 */
static uint64_t units_divide(uint64_t n, uint32_t units)
{
    _Static_assert(TIME_UNITS < 1u << 16, "a remainder and a half word fit a word");
    uint32_t high = (uint32_t)(n >> 32), low = (uint32_t)n;
    uint32_t q_high = high / units, rest = high % units;
    uint32_t mid = rest << 16 | low >> 16;
    uint32_t q_mid = mid / units;
    uint32_t bottom = (mid % units) << 16 | (low & 0xffffu);
    return (uint64_t)q_high << 32 | (uint64_t)q_mid << 16 | bottom / units;
}

/*
 * The virtual time of a core's turn's payer, its place p, charged for the turn from turn_from up to the count now:
 * TIME_UNITS / units counts for each count, rounded up, up to the end of the request open on it,
 * and past that, or with none open, for a turn on its own units' place, on spare time, up to a span past the count:
 * a thread that runs on spare time falls no further behind the threads that wait,
 * and runs at most a span ahead of the count for it, which its next request starts from.
 */
static uint64_t turn_vtime(const struct core *core, const struct place *p, uint64_t now)
{
    const struct place *own = turn_own(core);
    uint32_t units = place_units(p);
    if (own == NULL || units == 0 || now <= core->turn_from) {
        return p->vt;
    }
    uint64_t bound = p == own ? now + place_span(p) : p->vt;
    if (request_open(p, own)) {
        uint64_t end = p->vt + (uint32_t)(own->deadline - (uint32_t)p->vt);
        bound = end > bound ? end : bound;
    }
    if (p->vt >= bound) {
        return p->vt;
    }
    uint64_t cost = units_divide((now - core->turn_from) * TIME_UNITS + units - 1, units);
    return cost < bound - p->vt ? p->vt + cost : bound;
}

/* The count the turn's request is over at, as its payer's units pay for the rest of it; COUNT_NEVER with none open. */
static uint64_t turn_request_end(const struct core *core)
{
    const struct place *p = turn_place(core), *own = turn_own(core);
    if (p == NULL || own == NULL || place_units(p) == 0 || !request_open(p, own)) {
        return COUNT_NEVER;
    }
    uint32_t room = own->deadline - (uint32_t)p->vt;
    return core->turn_from + ((uint64_t)room * place_units(p) + TIME_UNITS - 1) / TIME_UNITS;
}

/*
 * Move the place to the queue it now waits on, or take it off the one it was on.
 * A place waits on the queues of the core whose time it holds, so one emptied leaves its queue.
 * A place that waits without time, with units or a lender with units,
 * brings its core's release forward to the count its virtual time, or its lender's, comes.
 * Another core is told of a place that joins one of its queues, and of a release it brought forward.
 */
static void place_settle(struct place *p)
{
    core_self()->wake_stale = true;
    uint64_t now = timer_now();
    struct core *was = &cores[p->core];
    struct core *core = &cores[place_core(p)];
    struct place *l = place_lender(p, now);
    uint8_t queue = place_queue(p, l, now);
    bool joined = queue != QUEUE_NONE && (queue != p->queue || core != was);
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
    /*
     * A place on the run queue goes on with its request on the units it was made on while it is open there,
     * or starts the next on whichever units pay for it now, as a place that joined would.
     */
    if (queue == QUEUE_RUN) {
        request_renew(p, request_payer(p, l, now), now);
        core->run_stale = true;
        joined = true;
    }
    /* Neither virtual time lies within a span of the clock, or the place would have the run queue. */
    uint64_t release = COUNT_NEVER;
    if (queue == QUEUE_SPARE || queue == QUEUE_SPENT) {
        if (place_units(p) != 0) {
            release = place_release(p);
        }
        if (l != NULL && place_units(l) != 0 && place_release(l) < release) {
            release = place_release(l);
        }
    }
    bool sooner = release < core->nearest_release;
    if (sooner) {
        core->nearest_release = release;
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
    /* What the turn runs on, and the request it runs, may have changed, so its core looks at its run queue again. */
    if (is_turn(t)) {
        cores[t->core].run_stale = true;
        core_notify(&cores[t->core]);
    }
}

/*
 * Settle the borrower a thread's units give lent time to: the end of the chain of borrowers it lends to first and on,
 * LEND_DEPTH of them at most, since every one between lends, and so waits on no queue.
 */
static void borrowers_settle(struct thread *t)
{
    struct thread *end = NULL;
    for (uint32_t i = 0; i < LEND_DEPTH; i++) {
        LOOP_BOUND(LEND_DEPTH);
        struct thread *h = t->lend_to != 0 ? p2v(t->lend_to) : NULL;
        if (h == NULL || h->lenders != v2p(t)) {
            break;
        }
        end = t = h;
    }
    if (end != NULL) {
        thread_settle(end);
    }
}

/* Settle a thread whose units or virtual time changed other than by a turn, and the borrower its time reaches. */
static void lender_settle(struct thread *t)
{
    thread_settle(t);
    borrowers_settle(t);
}

/* A core's turn is charged up to the count: its payer's virtual time moves on for what it ran. Returns the count. */
static uint64_t turn_charge(struct core *core)
{
    uint64_t now = timer_now();
    struct place *p = turn_place(core);
    if (p != NULL) {
        p->vt = turn_vtime(core, p, now);
    }
    core->turn_from = now;
    return now;
}

static uint64_t turn_close(void)
{
    return turn_charge(core_self());
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

/* The place on a core's run queue, which has one, with the earliest deadline, the oldest of those tied. */
static struct place *run_earliest(const struct core *core)
{
    struct place *best = p2v(core->run_queue);
    for (paddr_t at = best->queue_next; at != core->run_queue;) {
        LOOP_BOUND(TIME_UNITS);
        struct place *p = p2v(at);
        if (due_before(p->deadline, best->deadline)) {
            best = p;
        }
        at = p->queue_next;
    }
    return best;
}

/*
 * The thread of the place with the earliest deadline on the run queue,
 * or of the oldest on the spare queue when none has time,
 * taken off its queues: its turn begins at the count now, and is charged to that place from there,
 * or to its lender's place while it has no time and its lender has, which pays for the turn from then on.
 * NULL when neither queue has one.
 */
static struct thread *next_turn(uint64_t now)
{
    struct core *core = core_self();
    core->turn_from = core->turn_began = now;
    core->run_stale = false;
    struct place *p;
    if (core->run_queue != 0) {
        p = run_earliest(core);
    } else if (core->spare_queue != 0) {
        p = p2v(core->spare_queue);
    } else {
        return NULL;
    }
    struct thread *t = place_thread(p);
    place_unqueue(p);
    turn_take(t);
    struct place *payer = request_payer(p, place_lender(p, now), now);
    if (payer != NULL) {
        request_renew(p, payer, now);
        if (payer != p) {
            struct thread *lt = place_thread(payer);
            core->payer = lt;
            lt->core = (uint8_t)core_index();
        }
    }
    return t;
}

/* A thread made ready gives up the rest of its request, and starts the next on each of its cores as it joins the run queue. */
void sched_ready(struct thread *t)
{
    t->state = THREAD_READY;
    for (uint32_t i = 0; i < thread_places(t); i++) {
        LOOP_BOUND(CORES);
        thread_place(t, i)->span &= PLACE_SPAN;
    }
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

/*
 * A place's units are about to change: a turn they pay for, on any core, or that runs on them as its own place,
 * on lent time too, is charged up to the count first, at the units it ran on, and goes on from there,
 * since a turn whose thread has no place on its core is charged nothing.
 */
static void place_charge(struct place *p)
{
    const struct thread *t = place_thread(p);
    struct core *core = &cores[t->core];
    if (is_turn(t) && (turn_place(core) == p || turn_own(core) == p)) {
        turn_charge(core);
    }
}

/*
 * A bind starts afresh, its request closed, and its virtual time is the latest of its own and the new units':
 * a place keeps what it ran ahead of its units, and so do units it leaves, so no bind sheds it.
 */
void sched_bind(struct place *p, uint32_t first, uint32_t count, uint8_t rights, struct cap *parent, uint32_t span)
{
    place_charge(p);
    p->span = span_code(span);
    struct cap *b = place_time(p);
    if (b->type != CAP_NONE) {
        cap_delete(b, false);
    }
    *b = cap_to_time(first, count, rights);
    b->b = held_word(v2p(place_thread(p)), 0);
    cap_attach(parent, b);
    for (uint32_t i = 0; i < count; i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_place[first + i] = v2p(p);
        if (unit_vt[first + i] > p->vt) {
            p->vt = unit_vt[first + i];
        }
    }
    lender_settle(place_thread(p));
}

/*
 * A wait's span; the request in progress stays open only if it lies within the new span,
 * and a wait that returns at once has its core look at the turn and the timer again, as the span sets both,
 * which starts the next request if the one in progress does not stay open.
 * The caller's turn is charged first, at the span it ran with.
 */
void sched_span(struct thread *t, uint32_t span)
{
    bool turn = is_turn(t);
    if (turn) {
        turn_close();
    }
    for (uint32_t i = 0; i < thread_places(t); i++) {
        LOOP_BOUND(CORES);
        struct place *p = thread_place(t, i);
        p->span = (uint8_t)((p->span & ~PLACE_SPAN) | span_code(span));
    }
    if (turn) {
        cores[t->core].run_stale = true;
        cores[t->core].wake_stale = true;
    }
}

void sched_unbind(struct cap *bound)
{
    struct thread *t = p2v(cap_holder(bound));
    struct place *p = thread_place(t, (uint32_t)(bound - t->time));
    place_charge(p);
    for (uint32_t i = 0; i < range_count(bound); i++) {
        LOOP_BOUND(TIME_UNITS);
        unit_place[range_first(bound) + i] = 0;
        unit_vt[range_first(bound) + i] = p->vt;
    }
    *bound = (struct cap){ 0 };
    /* An empty place holds no time, so settling takes it off its queue. */
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
 */
void sched_vtimes_reset(void)
{
    uint64_t now = timer_now();
    /* What every core's turn owed goes too. */
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        cores[c].turn_from = cores[c].turn_began = now;
        cores[c].nearest_release = COUNT_NEVER;
    }
    for (uint32_t i = 0; i < MACHINE_UNITS; i++) {
        LOOP_BOUND(MACHINE_UNITS);
        unit_vt[i] = 0;
        struct place *p = unit_first(i);
        if (p != NULL) {
            p->vt = now;
            lender_settle(place_thread(p));
        }
    }
}

/*
 * Look at every place with units of a core that waits without time: one whose virtual time came joins the run queue,
 * and the others bring the release forward to the count theirs comes.
 * It looks at each unit of the core, a fixed few, as the timer looks at the timer lines,
 * and only once the clock reached the count nearest_release said a virtual time would come.
 * A place's units are a run of one core's, each naming it, so the look steps over the rest of a place's at once,
 * and takes a step for each place and each unit nobody earns.
 */
static void release(uint32_t c)
{
    uint32_t first = c * TIME_UNITS;
    cores[c].nearest_release = COUNT_NEVER;
    for (uint32_t i = 0; i < TIME_UNITS;) {
        LOOP_BOUND(TIME_UNITS);
        struct place *p = unit_first(first + i);
        i += p != NULL ? place_units(p) : 1;
        if (p != NULL && (p->queue == QUEUE_SPARE || p->queue == QUEUE_SPENT)) {
            place_settle(p);
        }
        /*
         * A lender waits on no queue, and its units may give time to the end of the chain of borrowers it lends to,
         * which alone of the chain may wait for a turn, every one between lending on.
         */
        struct place *h = p != NULL ? place_borrower(p) : NULL;
        for (uint32_t d = 1; d < LEND_DEPTH && h != NULL; d++) {
            LOOP_BOUND(LEND_DEPTH);
            struct place *next = place_borrower(h);
            if (next == NULL) {
                break;
            }
            h = next;
        }
        if (h != NULL && (h->queue == QUEUE_SPARE || h->queue == QUEUE_SPENT)) {
            place_settle(h);
        }
    }
}

void sched_start(struct thread *t)
{
    struct core *core = core_self();
    core->current = core->turn = t;
    core->turn_from = core->turn_began = timer_now();
    /* The boot sets the timer as its trap leaves. */
    core->wake_stale = true;
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
 * the thread that lent the core's turn its time first, so that its borrower's signal hands the turn back, true then.
 * Out of line, so that a signal nobody waits for saves no registers.
 */
static __attribute__((noinline)) bool signal_wake(struct notification *ntfn)
{
    struct core *core = core_self();
    struct thread *lender = core->payer;
    for (uint32_t i = 1;
         i < LEND_DEPTH && lender != NULL && lender->state == THREAD_WAITING && lender->waiting_on != v2p(&ntfn->hdr);
         i++) {
        LOOP_BOUND(LEND_DEPTH);
        struct thread *next = lender->lend_to != 0 ? p2v(lender->lend_to) : NULL;
        if (next == NULL || next->lenders != v2p(lender)) {
            break;
        }
        lender = next;
    }
    bool paying = lender != NULL && lender->waiting_on == v2p(&ntfn->hdr);
    wake(paying ? lender : p2v(ntfn->waiters), ntfn->bits);
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
 * The thread that lent the turn its time was woken, and may run now: it has the rest of the turn,
 * charged first for what the borrower ran on the lent request, and the thread whose turn it was goes to the back of its queue,
 * as at the end of a turn.
 */
void sched_turn_back(void)
{
    struct core *core = core_self();
    turn_close();
    struct thread *was = core->turn;
    struct thread *lender = core->payer;
    for (uint32_t i = 1; i < LEND_DEPTH && lender->lend_to != 0; i++) {
        LOOP_BOUND(LEND_DEPTH);
        lender = p2v(lender->lend_to);
    }
    if (lender == core->payer) {
        core->payer = NULL;
    }
    core->turn = lender;
    lender->core = (uint8_t)core_index();
    thread_settle(was);
    thread_settle(lender);
    switch_to(lender);
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
        borrowers_settle(h);
    }
}

void sched_lend(struct thread *t, struct notification *ntfn, struct thread *borrower)
{
    /*
     * The turn is charged first: the borrower may start a request on the units that pay for it,
     * which brings their virtual time up to the clock, and counts it ran before that are its to pay as they stood.
     */
    uint64_t now = turn_close();
    sched_wait(t, ntfn);
    lend_push(borrower, t);
    if (borrower->lenders == v2p(t)) {
        thread_settle(borrower);
        borrowers_settle(borrower);
    }
    /*
     * A lender whose turn its own time or its payer's pays for, and that now lends first to a borrower waiting for a turn
     * on its core, hands it the rest of the turn and of its request, which the payer goes on paying for:
     * the turn does not end here.
     */
    struct core *core = core_self();
    struct thread *payer = turn_payer(core);
    struct place *tp = payer != NULL ? thread_place_on(payer, core_index()) : NULL;
    struct place *bp = thread_place_on(borrower, core_index());
    struct place *own = turn_own(core);
    if (tp == NULL || bp == NULL || own == NULL || bp->queue == QUEUE_NONE) {
        return;
    }
    /* The borrower takes the turn's request, if the units paying for it are the ones it would run on. */
    uint32_t deadline = bp->deadline;
    uint8_t span = bp->span;
    bp->deadline = own->deadline;
    bp->span = (uint8_t)((span & PLACE_SPAN) | (own->span & PLACE_REQUEST) | PLACE_LENT);
    if (request_open(tp, bp) && place_lender(bp, now) == tp) {
        place_unqueue(bp);
        core->turn = borrower;
        core->payer = payer;
        turn_take(borrower);
        switch_to(borrower);
    } else {
        bp->deadline = deadline;
        bp->span = span;
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
 * The timer lines are a fixed few, so the timer looks at each of them and at the watchdog
 * and at nothing else; see DESIGN.md, "Time".
 * A line that fires disarms its Irq, so that after the look no armed one is due,
 * and each core's nearest deadline of those still armed on it, and of the watchdog it fed, is exact again,
 * which only puts it later, so no other core is told.
 * The releases of every core come next, those due by now, so that under tracing, where a record moves time,
 * one core's look does what each core's timer would.
 */
void sched_time(uint64_t now)
{
    struct core *core = core_self();
    core->wake_stale = true;
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        cores[c].nearest_deadline = COUNT_NEVER;
    }
    for (uint32_t i = 0; i < TIMER_LINES; i++) {
        LOOP_BOUND(TIMER_LINES);
        struct irq *irq = line_binding(IRQ_LINES + i);
        if (irq == NULL || !irq_armed(irq)) {
            continue;
        }
        struct core *armed = &cores[irq->core];
        if (irq_due(irq, now)) {
            irq_signal(irq);
        } else if (irq->deadline < armed->nearest_deadline) {
            armed->nearest_deadline = irq->deadline;
        }
    }
    if (watchdog.armed) {
        if (now >= watchdog.deadline) {
            kputs("watchdog: not fed in time\n");
            khalt(7);
        }
        struct core *fed = &cores[watchdog.core];
        if (watchdog.deadline < fed->nearest_deadline) {
            fed->nearest_deadline = watchdog.deadline;
        }
    }
    for (uint32_t c = 0; c < CORES; c++) {
        LOOP_BOUND(CORES);
        if (now >= cores[c].nearest_release) {
            release(c);
        }
    }
    /*
     * A turn whose request is over goes on with the next if its own units give it one, as the run queue is looked at again;
     * a turn on lent time, or whose units give it no more, ends, and so does a span on spare time while another waits for one.
     */
    struct place *p = turn_place(core), *own = turn_own(core);
    if (core->turn == NULL || own == NULL) {
        return;
    }
    if (p != NULL && place_units(p) != 0 && request_open(p, own)) {
        if (now >= turn_request_end(core)) {
            turn_close();
            if (core->payer == NULL && has_time(p, own, now)) {
                request_renew(own, p, now);
                core->run_stale = true;
            } else {
                core->turn_due = true;
            }
        }
    } else if (core->spare_queue != 0 && now - core->turn_began >= place_span(own)) {
        core->turn_due = true;
    }
}

uint64_t sched_next_wake(void)
{
    struct core *core = core_self();
    uint64_t at = core->nearest_deadline < core->nearest_release ? core->nearest_deadline : core->nearest_release;
    struct place *own = turn_own(core);
    if (core->turn == NULL || own == NULL) {
        return at;
    }
    uint64_t end = turn_request_end(core);
    if (end != COUNT_NEVER) {
        /* Alone on its own time with spare time to go on with, the turn's request changes nothing that runs as it ends. */
        bool alone = core->payer == NULL && place_spare(own) && core->run_queue == 0 && core->spare_queue == 0;
        if (!alone && end < at) {
            at = end;
        }
    } else if (core->spare_queue != 0 && core->turn_began + place_span(own) < at) {
        at = core->turn_began + place_span(own);
    }
    return at;
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
 * so that an interrupt or the timer finds its Irq without a walk; see DESIGN.md, "Bounded work".
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
 * Only a running thread, a timer line, a device interrupt or a virtual time that comes
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

uint64_t sched_idle_sleep(void)
{
    struct core *core = core_self();
    if (machine_stuck()) {
        kputs("no runnable thread\n");
        khalt(5);
    }
    /* No thread runs, so no turn needs ending: an end the timer found due is moot, since the turn is over. */
    core->turn_due = false;
    core->wake_stale = true;
    core->wake_at = sched_next_wake();
    return core->wake_at;
}

bool sched_idle_wake(bool device)
{
    struct core *core = core_self();
    /* Under tracing time moves only by record, so the stall moves none of it. */
    if (!debug_trace) {
        uint64_t now = timer_now();
        if (now >= core->wake_at) {
            sched_time(now);
        }
    }
    if (device) {
        sched_claim_interrupts();
    }
    core->turn_due = false;
    core->turn = next_turn(turn_close());
    if (core->turn == NULL) {
        return false;
    }
    switch_to(core->turn);
    return true;
}

void sched_idle(void)
{
    bool device;
    do {
        LOOP_WAIT("an interrupt, in intr_wait");
        device = intr_wait(sched_idle_sleep());
    } while (!sched_idle_wake(device));
}

void sched_run_next(void)
{
    uint64_t now = turn_close();
    payer_drop();
    core_self()->turn_due = false;
    core_self()->turn = next_turn(now);
    run_turn();
}

void sched_turn_end(void)
{
    struct core *core = core_self();
    uint64_t now = turn_close();
    struct thread *was = core->turn;
    core->turn = NULL;
    core->turn_due = false;
    payer_drop();
    if (was != NULL) {
        thread_settle(was);
    }
    core->turn = next_turn(now);
    run_turn();
}

void sched_tick(void)
{
    sched_trace_clock += timer_step_counts();
    sched_time(timer_now());
    sched_turn_end();
}

/*
 * Whether a thread waits on the run queue with a deadline before the turn's request,
 * once a place joined it, or the turn's thread or its request changed, which marked the core:
 * then it takes the processor, as from a turn on spare time.
 * A request over, or closed by the change, gives way to the next, if the turn's own units give it one,
 * which a join looks for too, since no timer marks a request's end while nobody waits;
 * a turn on lent time, or one that has no time left, takes the processor from nobody who waits.
 * The turn is charged first, unless nobody waits and its request is open, where nothing would change.
 * Out of line, so that a look at a turn nothing marked saves no registers for it.
 */
static __attribute__((noinline)) bool deadline_due(struct core *core)
{
    struct place *p = turn_place(core), *own = turn_own(core);
    if (p == NULL || own == NULL || place_units(p) == 0 || (core->run_queue == 0 && request_open(p, own))) {
        return false;
    }
    uint64_t now = turn_close();
    if (!request_open(p, own)) {
        if (core->payer != NULL || !has_time(p, own, now)) {
            return core->run_queue != 0;
        }
        request_renew(own, p, now);
        /* Its end is the timer's to mark. */
        core->wake_stale = true;
    }
    return core->run_queue != 0 && due_before(run_earliest(core)->deadline, own->deadline);
}

/*
 * Whether a turn may go on: it has time, its request open on whoever pays for it, or it may run on spare time.
 * A turn left with neither, by a bind or an unbind, or as its request ended, has its thread wait for its units.
 */
static bool turn_may_run(const struct core *core)
{
    const struct place *own = turn_own(core);
    return !sched_spare_turn(core) || (own != NULL && place_spare(own));
}

/*
 * A turn on spare time is one no thread with time wants, so it ends as soon as one waits on the core's run queue,
 * unless the running thread stands at a call that stopped for an interrupt, which it makes again first;
 * so does a turn with time that a thread with an earlier deadline joined the run queue during,
 * and a turn that may not go on once a change to its thread marked it.
 * The end of a call and of every trap ask it, so a wake by a call, an interrupt or another core takes the processor at once;
 * see DESIGN.md, "Scheduling".
 */
static __attribute__((noinline)) void spare_look(struct core *core)
{
    bool marked = core->run_stale;
    core->run_stale = false;
    if ((marked && deadline_due(core)) || (core->run_queue != 0 && sched_spare_turn(core)) ||
        (marked && !turn_may_run(core))) {
        sched_turn_end();
    }
}

/* Out of line past the common case, a turn nothing marked with nobody waiting, so that saves no registers. */
static void spare_end(struct core *core)
{
    if (!core->recall && core->turn != NULL && (core->run_stale || core->run_queue != 0)) {
        spare_look(core);
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

bool sched_wake(uint64_t *at)
{
    struct core *core = core_self();
    /* Under tracing only a record ends a turn. */
    if (core->turn_due) {
        core->turn_due = false;
        if (!debug_trace) {
            sched_turn_end();
        }
    }
    /* An interrupt, or another core, may have made a thread with time ready while the turn runs on spare time. */
    spare_end(core);
    if (!core->wake_stale) {
        return false;
    }
    core->wake_stale = false;
    *at = core->wake_at = sched_next_wake();
    return true;
}
