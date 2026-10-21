#ifndef RVUOS_SCHED_H
#define RVUOS_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "object.h"
#include "timer.h"

/*
 * Threads' turns and the cores they take them on: the units, the virtual times and the queues,
 * what each core has of its own, and the calls of sched.c and core.c.
 * See DESIGN.md, "Scheduling" and "Cores".
 */

/* sched.c */

/*
 * Every core's units, TIME_UNITS of each, numbered core by core:
 * unit u is a part of core u / TIME_UNITS, and a thread's place holds the time of the core its units lie on.
 */
#define MACHINE_UNITS (CORES * TIME_UNITS)
_Static_assert(MACHINE_UNITS <= UINT16_MAX, "a Time capability names its first unit and its count in two bytes each");

static inline uint32_t unit_core(uint32_t unit)
{
    return unit / TIME_UNITS;
}

/*
 * The place of a thread that earns each unit of time, 0 for none.
 * The units are a fixed few, as the timer lines are, so the kernel finds every place with units
 * by looking at each of them and at nothing else; see DESIGN.md, "Scheduling".
 */
extern paddr_t unit_place[MACHINE_UNITS];

/*
 * The virtual time each unit nobody earns was left at by the place that earned it last, 0 for none:
 * a bind takes the latest of its units', so that no thread sheds what it ran ahead of its units
 * by handing them to another.
 */
extern uint64_t unit_vt[MACHINE_UNITS];

/* A count no clock reaches: the time of an event that never comes. */
#define COUNT_NEVER UINT64_MAX

/* The units a place earns, none while it is empty or holds none. */
static inline uint32_t place_units(const struct place *p)
{
    const struct cap *b = place_time(p);
    return b->type == CAP_TIME ? range_count(b) : 0;
}

/* Whether a place may run its thread on spare time: bound through a Time capability with RIGHT_X. */
static inline bool place_spare(const struct place *p)
{
    const struct cap *b = place_time(p);
    return b->type == CAP_TIME && (b->rights & RIGHT_X) != 0;
}

/*
 * How far past its virtual time a place's request is due, in counts: the span its binding or its thread's last wait gave,
 * or else the default span, the longest power of two of counts within a millisecond.
 * It is also how long a turn on spare time lasts while another thread waits for one.
 * See sched.c, "Requests".
 */
#define PLACE_SPAN 0x3fu
static inline uint32_t place_span(const struct place *p)
{
    uint32_t code = p->span & PLACE_SPAN;
    return code != 0 ? 1u << (code - 1u) : timer_default_span();
}

/*
 * The span byte's top bit says whether the place's thread has a request there, see sched.c, "Requests":
 * one that waits gives up the rest of its request, and one bound starts afresh, so the bit is cleared as each does.
 * The bit below it says the request was made on a lender's units, not the place's own,
 * so that a request is open only on the units it was made on, though its deadline lies within the other's span too.
 */
#define PLACE_REQUEST 0x80u
#define PLACE_LENT 0x40u

/* Whether p's thread has a request open on payer's units, p's own or its lender's: made on them, and not yet over. */
static inline bool request_open(const struct place *payer, const struct place *p)
{
    uint32_t made = PLACE_REQUEST | (payer != p ? PLACE_LENT : 0);
    return (p->span & (PLACE_REQUEST | PLACE_LENT)) == made && p->deadline - (uint32_t)payer->vt - 1 < place_span(payer);
}

/* What a place keeps of a span OP_TIME_BIND or a wait takes, a power of two up to 2^SPAN_MAX or 0; see place_span. */
static inline uint8_t span_code(uint32_t span)
{
    return span != 0 ? (uint8_t)(__builtin_ctz(span) + 1u) : 0;
}

/* Whether a call may give a span: 0, or a power of two up to 2^SPAN_MAX. */
static inline bool span_valid(uint32_t span)
{
    return span <= 1u << SPAN_MAX && (span & (span - 1u)) == 0;
}

/*
 * The place of a thread that holds the time of a core, NULL for none: a look at each of its places,
 * which are CORES at most and hold one core's time each, so it is the only one.
 */
static inline struct place *thread_place_on(const struct thread *t, uint32_t core)
{
    if (CORES == 1) {
        return t->time[0].type == CAP_TIME ? thread_place(t, 0) : NULL;
    }
    for (uint32_t i = 0; i < t->places; i++) {
        LOOP_BOUND(CORES);
        const struct cap *b = &t->time[i];
        if (b->type == CAP_TIME && unit_core(range_first(b)) == core) {
            return thread_place(t, i);
        }
    }
    return NULL;
}

/*
 * Deadlines are compared modulo 2^32, so each lies within 2^31 counts of the others on its run queue:
 * a request lies a span past the virtual time it starts at, which lies at most a span past the count,
 * and one past due lies behind the count by less than a span, which the order meets.
 */
_Static_assert(3ull << SPAN_MAX < 1ull << 31, "a deadline lies within 2^31 counts of the others");
/*
 * How many lenders lent time goes through: a borrower's first lender with no time for it lends what it borrows,
 * so a client's units pay for its server, for what that one waits on, and on down a chain of this many.
 * Every lender of a chain waits, so only the thread at its end may wait for a turn,
 * and what settles a chain settles that end alone.
 */
#define LEND_DEPTH 64u

/*
 * The ready threads whose turn it is nowhere wait on three queues of each core they hold the time of, by their place there,
 * each a ring of places through queue_next and queue_prev, oldest first, 0 when empty:
 * the run queue those with time, a request open on their units or their lender's,
 * the spare queue those without that may run on spare time,
 * and the spent queue those with units, and no spare time, that wait for their virtual time to come.
 * The one due first on the run queue has the next turn, or when it is empty the oldest on the spare queue,
 * and a thread that becomes ready joins behind the newest of its queue on each of its cores.
 * A place keeps the index of the one it waits on, QUEUE_NONE for none, and the core.
 */
enum { QUEUE_NONE, QUEUE_RUN, QUEUE_SPARE, QUEUE_SPENT, QUEUES };

/*
 * How many Irqs are armed on a device's line or a timer line,
 * the sources that can make a thread runnable while none is; see sched_run_next.
 * Every write of an Irq's bits goes through irq_set_bits, which keeps it.
 */
extern uint32_t armed_sources;

/*
 * What each core has of its own: its thread, its turn, its queues, its release, its nearest deadline, its timer,
 * the call it is in, what it tells the others, and what another core's stop left its trap to drop.
 * The units, the clock and the lines are the machine's.
 * Every core's is the others' to read and change under the kernel's lock, in_user aside; see DESIGN.md, "Cores".
 */
struct core {
    /*
     * The running thread, or the last one to run while none does;
     * NULL before the core first runs one and once another core destroyed it.
     */
    struct thread *current;

    /* The thread whose turn it is, the running one; NULL while none runs, and the core idles. */
    struct thread *turn;

    /*
     * The thread whose units pay for the turn while the turn's thread runs on lent time, NULL while it pays itself:
     * a thread with a place on this core that lends the turn's thread its time while it waits; see OP_NOTIFY_LEND.
     * It is charged as the turn's thread would be, and pays until the turn ends, or takes the turn over as the turn's thread wakes it.
     */
    struct thread *payer;

    /* The count the turn has been charged to, from which its payer owes what it runs; see turn_close in sched.c. */
    uint64_t turn_from;

    /* The count the turn began at, which a turn on spare time lasts a span from while another waits for one. */
    uint64_t turn_began;

    /* The heads of the three queues, each the oldest place on it; see QUEUE_RUN. */
    paddr_t run_queue;
    paddr_t spare_queue;
    paddr_t spent_queue;

    /*
     * The count the kernel next looks at the places with units of the core that wait without time,
     * to give those whose virtual time has come within a span of the clock time again:
     * the earliest such count, or COUNT_NEVER while nobody waits on the spare and spent queues.
     * A thread brings it forward as it waits without time, and the look makes it exact again,
     * so it may lie before the earliest such count but never after one, and always ahead of the count.
     */
    uint64_t nearest_release;

    /*
     * The count the core's timer has to interrupt at for the timer lines armed on it, see struct irq,
     * and for the watchdog if it fed it last:
     * the nearest deadline of these, or COUNT_NEVER with none.
     * An arm brings it forward, and the timer's look at every line makes every core's exact again,
     * so it may lie before the nearest such deadline but never after one, and always ahead of the count.
     */
    uint64_t nearest_deadline;

    /*
     * The count the timer is set for, sched_wake's, and whether a change since may call for another.
     * Settling a thread, binding or unbinding one, a switch, the timer, an arm and the stall mark it stale.
     */
    uint64_t wake_at;
    bool wake_stale;

    /* Whether the timer found the turn's request over, or its span on spare time run, which ends the turn as the trap returns. */
    bool turn_due;

    /* Whether a walk stopped where OP_TEST_PREEMPT armed it, which the call's dispatch turns into a record's step of time. */
    bool preempt_stopped;

    /*
     * Whether the running thread stands at a call that stopped for an interrupt, which it makes again
     * before a turn on spare time gives the processor up: set as the call stops,
     * and cleared as a call ends without stopping, or another thread runs; see spare_end in sched.c.
     */
    bool recall;

    /*
     * Whether a place joined the run queue, or the thread whose turn it is or pays for changed,
     * since the core last looked for a deadline before the turn's; see spare_end.
     */
    bool run_stale;

    /* The bits the last wake handed over, zero if none; for the trace. */
    uint32_t trace_wake_bits;

    /*
     * Whether the core runs user mode, which it tells the others without the lock:
     * cleared as a trap begins, once the thread's registers are in its frame,
     * and set under the lock as the core leaves for user mode; see core_shoot.
     */
    bool in_user;

    /* Whether the protection unit holds regions the running thread's process no longer has, to load again before it runs. */
    bool pmp_stale;

    /* The entries of the protection unit the last image loaded here turned on; those past them are off. */
    uint8_t pmp_loaded;

    /* Whether a core told this one of a change it must trap for, which it interrupts as it gives the lock up. */
    bool interrupt_owed;

    /*
     * Whether another core stopped the thread whose turn this one has, while this core ran it or had trapped for it:
     * the trap that next takes the lock here drops what it was for, whatever the thread's state is by then,
     * since a call between may have resumed it; see core_trapped.
     * Only another core sets it, under the lock, and it goes with the turn it was set for.
     */
    bool drop_trap;
};

extern struct core cores[CORES];

#if CORES > 1
/* The kernel's lock, a ticket lock: the ticket the next core to ask for it takes, and the one that holds it. */
struct kernel_lock {
    uint32_t next;
    uint32_t owner;
};
extern struct kernel_lock kernel_lock;
#endif

/* The core this trap runs on. */
static inline struct core *core_self(void)
{
    return &cores[core_index()];
}

/* Whether another core waits for the kernel's lock; see core.c. */
static inline bool core_lock_waited(void)
{
#if CORES > 1
    return __atomic_load_n(&kernel_lock.next, __ATOMIC_RELAXED) != kernel_lock.owner + 1;
#else
    return false;
#endif
}

/* core.c */

/*
 * The first and the last thing the kernel does on a core, on every way in from user mode and out to it:
 * enter tells the others the core runs the kernel, its thread's registers saved, takes the kernel's lock,
 * and loads the regions again if another core took some from the process;
 * leave tells the others the core runs user mode, gives the lock up,
 * and then interrupts the cores told of a change meanwhile, see core_notify. With one core neither does anything.
 * Every way out passes trap_return, which leaves once the frame it returns into is whole:
 * start.S does on RISC-V, and frame_give on ARM, which first writes half of the frame to the thread's stack.
 * A core that idles and wakes takes the lock as its stall ends, and switch_to loads the regions.
 */
void core_enter(void);
void core_leave(void);

/* The stall gives the lock up while the core waits for an interrupt, and takes it again after. */
void core_stall_begin(void);
void core_stall_end(void);

/*
 * Make another core trap, if it runs user mode, and wait until its thread's registers are in its frame,
 * so that the thread reaches nothing more until the core has the lock again.
 */
void core_shoot(struct core *c);

/*
 * What another core runs lost something it may reach, before the call goes on:
 * a region of the process, after which the core loads the regions again before its thread runs on,
 * or the thread stopped, by a stop or by the loss of its process, after which its frame is the one a watcher reads.
 * A core whose turn the stopped thread has is marked to drop its trap, see drop_trap in struct core,
 * and the result says whether one was: that core has the thread's registers from a trap of its own by then.
 */
void core_regions_changed(const struct process *proc);
bool core_thread_stopped(const struct thread *t);

/* The thread is destroyed: no core runs it any more, nor has a turn for it, nor a trap to drop for it. */
void core_thread_gone(const struct thread *t);

/*
 * The thread a trap from user mode is for, once the trap has the lock and has looked at the timer:
 * the core's running thread, or NULL if another core stopped or destroyed it while the trap waited for the lock,
 * when what it trapped for is dropped and the turn ends; see DESIGN.md, "Cores".
 * The stopping core marked the trap, and the mark decides, not the state:
 * another call may have resumed the thread in between, and it then joins its queue as the turn ends.
 */
struct thread *core_trapped(void);

/*
 * A thread became ready on a core, or brought its release forward:
 * the core sets its timer again, and is interrupted for it if it runs user mode or idles,
 * once this core gives the lock up, so that it does not wait for the lock as it wakes.
 */
void core_notify(struct core *c);

/*
 * Interrupt another core once this one gives the lock up, whatever it runs,
 * for a change it finds as it takes the lock: a line its controller is to forward, or no longer, see nvic.c.
 */
void core_interrupt_later(struct core *c);

/*
 * Whether this trap made a change only its end makes known, which a walk stops for:
 * a thread with time ready here, whose request may be due before the turn's,
 * or another core owed an interrupt, see core_notify.
 * The trap's end sets the timer and sends the interrupts, so the call made again does not stop for the same change.
 * Unlike core_notify's, a turn on spare time is no reason: a stopped call is made again first, see recall in struct core.
 */
bool core_notice_owed(void);

/*
 * The board's, only with more than one core: let the other cores interrupt this one, in user mode and in wfi,
 * interrupt another core, and take this core's interrupt back.
 */
void ipi_enable(void);
void ipi_send(uint32_t core);
void ipi_clear(void);

#ifdef RVUOS_HOST
/* The core core_shoot waits for traps, its registers saved, and waits for the lock; see host/shim.c. */
void host_core_traps(uint32_t core);
#endif

/* Arm with bits, or disarm with zero; the deadline, or the line's mask, is the caller's. */
void irq_set_bits(struct irq *irq, uint32_t bits);

/* The line fired: disarm the Irq and signal its bits. The mask, if the line has one, is the caller's. */
void irq_signal(struct irq *irq);

/*
 * Clear an Irq's notification the tree has already let go of:
 * the Irq is disarmed and its line masked, and it stays so, bound to the line, until its pool goes.
 */
void irq_drop(struct cap *signalled);

/* Make a stopped or woken thread ready: it joins the back of the queue its virtual time puts it on, if it may run. */
void sched_ready(struct thread *t);

/*
 * The place of a thread a bind of the time of a core fills: the one that holds it already,
 * else an empty one, else NULL, when the thread holds the time of as many cores as it has places.
 */
struct place *sched_place_for(const struct thread *t, uint32_t core);

/*
 * Bind a thread's place, sched_place_for's, to count units from first, with rights, as a node below parent;
 * it leaves the units it earned, which keep its virtual time, and takes the latest of its own and the new units'.
 * A place bound to none without spare time holds the core's time all the same: its thread runs there on lent time alone.
 * No other thread earns any of the units; the caller has made sure.
 * A ready thread whose turn it is nowhere joins the back of the queue its virtual time puts it on, on each of its cores.
 * Its requests are due span counts past its virtual time, a power of two, or the default span for 0, and the first starts now.
 */
void sched_bind(struct place *p, uint32_t first, uint32_t count, uint8_t rights, struct cap *parent, uint32_t span);

/*
 * A thread that waits gives the span its requests are due within on each of its cores, from the one its wake begins on,
 * a power of two up to 2^SPAN_MAX.
 * See sched.c, "Requests".
 */
void sched_span(struct thread *t, uint32_t span);

/*
 * Clear a place's units the tree has already let go of:
 * nobody earns them any more, they keep the place's virtual time, and the place leaves the queue it waited on.
 */
void sched_unbind(struct cap *bound);

/*
 * Clear a thread's process the tree has already let go of: the thread stops, as sched_stop has it,
 * leaving its queue, or its notification's waiters to stand at the call it waited in,
 * and the running one gives the processor up as its call returns.
 */
void sched_unhost(struct cap *hosted);

/*
 * OP_THREAD_STOP: stop a thread, and return the state it was in.
 * A ready one leaves its queue, and another core that runs it traps first and drops that trap;
 * a waiting one leaves its notification's waiters and its borrower's ring of lenders,
 * and stands at the call it waited in, every register as the call found it, so that a resume makes the call again.
 * The running thread gives the processor up as its call returns.
 * A stopped thread is left as it is.
 */
uint32_t sched_stop(struct thread *t);

/*
 * Bring every virtual time to the count, the places' and the units' nobody earns,
 * as if every thread had waited a while, and put each place on the queue that puts it on.
 * The boot calls it once the root thread is bound, and OP_TEST_TRACE does.
 */
void sched_vtimes_reset(void);

/* Start running a thread with nothing else run before it; the boot's. */
void sched_start(struct thread *t);

/*
 * The clock under tracing, where time moves only by record: OP_TEST_TICK moves it a step, timer_step_counts.
 * See timer_now.
 */
extern uint64_t sched_trace_clock;

/*
 * The machine's watchdog, which OP_CLOCK_WATCHDOG arms and feeds:
 * once armed, the clock reaching its deadline halts the machine.
 * The core that fed it last wakes for its deadline, as for a timer line it armed; see struct core.
 * See DESIGN.md, "The watchdog".
 */
struct watchdog {
    bool armed;
    uint8_t core;
    uint64_t deadline;
};
extern struct watchdog watchdog;

/* Arm the watchdog, or move its deadline, to a count ahead of the clock, and have this core wake for it. */
void sched_watchdog(uint64_t deadline);

/*
 * Set bits on a notification and wake a thread waiting on it, if any:
 * the thread that pays for this core's turn first, if it waits there, true then, else the oldest.
 * Never blocks; OP_NOTIFY_SIGNAL and a firing Irq are both this.
 */
bool sched_signal(struct notification *ntfn, uint32_t bits);

/*
 * The running thread's signal woke the thread whose time pays for its turn, as sched_signal said:
 * that one takes the rest of the turn back, and the running thread goes to the back of its queue; see OP_NOTIFY_SIGNAL.
 */
void sched_turn_back(void);

/*
 * The running thread waits, or stopped, by a fault, a stop of itself or the loss of its process.
 * Charge it for the counts its turn ran,
 * and hand the processor to the next ready thread, or leave the core to idle once the trap is over,
 * or stop the machine.
 */
void sched_run_next(void);

/*
 * The end of a call whose thread may not run on, or after which a thread waits on the run queue:
 * the processor goes to the next thread as sched_run_next has it,
 * or the turn ends if it runs on spare time and a thread with time waits; see spare_end in sched.c.
 * Out of line, and the one call at the end of a call, so that the call's frame stays as it was
 * on the stack's deepest path; see DESIGN.md, "Bounded stack".
 */
void sched_call_end(void);

/*
 * The core has no thread to run: wait for an interrupt until one has its turn, then hand the processor to it,
 * or stop the machine once nothing can ever run again.
 * The architecture's core_idle calls it as a trap ends with nobody's turn, and as a core starts.
 * Each wait is the two halves below, which the host calls for a core of several on their own:
 * sleep stops the machine if nothing can ever run again, else returns the count to wait until, COUNT_NEVER for none;
 * wake takes the interrupts the wait found, and gives a thread its turn, if one may have it.
 */
void sched_idle(void);
uint64_t sched_idle_sleep(void);
bool sched_idle_wake(bool device);

/*
 * OP_TEST_TICK, under tracing: the clock moves a step, timer_step_counts, the timer does what is due by then,
 * see sched_time, and the running thread's turn ends: it goes to the back of the queue its virtual time puts it on,
 * and the next thread has its turn.
 */
void sched_tick(void);

/*
 * The turn ends: the thread whose turn it was is charged for it
 * and goes to the back of the queue its virtual time puts it on, if it may run, and the next thread has its turn.
 * One that lost its units during its turn goes nowhere, and without it the queues may be empty.
 * The timer ends a turn so, and so does a trap another core marked to be dropped, see core_trapped.
 */
void sched_turn_end(void);

/*
 * The timer, at the count now: fire every timer line that is due, halt the machine if the watchdog is,
 * give time again to the places of every core whose virtual time came,
 * and end the turn if its request is over, or if it ran a span on spare time while another waits for spare time.
 * A trap from user mode calls it as it begins, once the clock reached what the timer was set for, untraced,
 * and so does the stall, and OP_TEST_TICK.
 */
void sched_time(uint64_t now);

/*
 * The trap is over: end a turn that may not run on, and the turn on spare time a thread with time waits for,
 * and say whether the timer is to be set again, to *at, since a change marked it stale.
 * Every trap calls it on its way back, and the host after every call, so that the self-check can hold it.
 */
bool sched_wake(uint64_t *at);

/*
 * Whether a core's turn runs on spare time: nobody pays for it,
 * the units of its payer, its own or a lender's, having no request of its thread open on them, or there being none,
 * as the payer's virtual time stood when it was last charged.
 */
static inline bool sched_spare_turn(const struct core *c)
{
    if (c->turn == NULL) {
        return false;
    }
    uint32_t core = (uint32_t)(c - cores);
    const struct place *own = thread_place_on(c->turn, core);
    const struct place *p = c->payer != NULL ? thread_place_on(c->payer, core) : own;
    return p == NULL || own == NULL || place_units(p) == 0 || !request_open(p, own);
}

/*
 * The count the timer is to interrupt at, the first that could change what runs, COUNT_NEVER for none:
 * the nearest deadline and release of the core,
 * the end of the turn's request unless its thread may go on alone on spare time,
 * and a span into a turn on spare time while another waits for spare time.
 * See DESIGN.md, "Scheduling".
 */
uint64_t sched_next_wake(void);

/*
 * A device interrupt on a line: the Irq armed on it masks the line,
 * disarms and signals its bits.
 * False, and nothing done, when no Irq is armed on the line.
 * The interrupt calls it through sched_claim_interrupts, and OP_TEST_IRQ does on request.
 */
bool sched_interrupt(uint32_t line);

/* Take every interrupt the controller holds: claim, deliver, complete. */
void sched_claim_interrupts(void);

/* Block a thread on a notification, behind the threads already waiting there. */
void sched_wait(struct thread *t, struct notification *ntfn);

/*
 * The running thread waits on a notification, as sched_wait has it,
 * and lends another thread its time meanwhile, behind the threads that lend it already.
 * If its own time pays for its turn, it is now the borrower's first lender, and the borrower waits for a turn on its core,
 * it hands the borrower the rest of the turn; see OP_NOTIFY_LEND.
 */
void sched_lend(struct thread *t, struct notification *ntfn, struct thread *borrower);

/*
 * Before an object of a pool that goes is zeroed, undo what it left in the rest of the kernel:
 * a notification wakes every thread waiting on it
 * with KERR_INVALID_CAP and no bits, because the object is gone,
 * a thread wakes every thread that lends it its time the same way, since the call each made names what is gone,
 * and an Irq leaves its line, which is free again.
 * The nodes an object holds were cleared first:
 * a thread stopped as they were, and an Irq was disarmed and its line masked.
 * The waiters wake one step at a time, and with preempt the walk may stop between two:
 * false then, and the next call goes on with the ones still waiting.
 */
bool sched_forget(struct obj_header *o, bool preempt);

/* The Irq bound to each line, 0 for none; LINES long, the timer lines included. */
extern paddr_t line_irq[];

/* The Irq bound to a line, or NULL; there is at most one. */
struct irq *line_binding(uint32_t line);

#endif
