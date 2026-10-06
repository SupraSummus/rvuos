#ifndef RVUOS_SCHED_H
#define RVUOS_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel.h"
#include "object.h"
#include "timer.h"

/*
 * Threads' turns and the cores they take them on: the units, the accounts and the queues,
 * what each core has of its own, and the calls of sched.c and core.c.
 * See DESIGN.md, "Scheduling" and "Cores".
 */

/* sched.c */

/*
 * A count of the counter in the parts an account counts:
 * a unit earns one every count, a tick's counts of them every tick,
 * and a count of a turn on the thread's time costs this many.
 */
#define COUNT_PARTS TIME_UNITS

/*
 * Every core's units, TIME_UNITS of each, numbered core by core:
 * unit u is a part of core u / TIME_UNITS, and a thread runs on the core its units lie on.
 */
#define MACHINE_UNITS (CORES * TIME_UNITS)

static inline uint32_t unit_core(uint32_t unit)
{
    return unit / TIME_UNITS;
}

/*
 * The thread that earns each unit of time, 0 for none.
 * The units are a fixed few, as the timer lines are, so the kernel finds every thread with units
 * by looking at each of them and at nothing else; see DESIGN.md, "Scheduling".
 */
extern paddr_t unit_thread[MACHINE_UNITS];

/* The units a thread earns, none while it is bound to none. */
static inline uint32_t thread_units(const struct thread *t)
{
    return t->time.type == CAP_BOUND ? t->time.b : 0;
}

/* Whether a thread may run on spare time: bound through a Time capability with RIGHT_X. */
static inline bool thread_spare(const struct thread *t)
{
    return t->time.type == CAP_BOUND && (t->time.rights & RIGHT_X) != 0;
}

/* A tick in parts: what a tick of a turn with time costs, and what an account holds while its thread has time. */
static inline uint32_t tick_parts(void)
{
    return COUNT_PARTS * timer_tick_counts();
}

/* The parts a thread's account gains every tick, a tick's counts for each unit it earns. */
static inline uint32_t tick_gain(const struct thread *t)
{
    return thread_units(t) * timer_tick_counts();
}

/* The most a thread's account holds: a tenth of a second's worth of its units, none without. */
static inline uint32_t account_cap(const struct thread *t)
{
    _Static_assert((uint64_t)TIME_UNITS * (ACCOUNT_TICKS + 1) * TICK_COUNTS_MAX <= UINT32_MAX,
                   "the cap and a tick's gain besides fit a word");
    return tick_gain(t) * ACCOUNT_TICKS;
}

/*
 * The ready threads but the one whose turn it is wait on three queues,
 * each a ring through queue_next and queue_prev, oldest first, 0 when empty:
 * the run queue those with a tick in their account,
 * the spare queue those without that may run on spare time,
 * and the spent queue those with units, and no spare time, that wait for their account to reach a tick.
 * The oldest on the run queue has the next turn, or when it is empty the oldest on the spare queue,
 * and a thread that becomes ready joins behind the newest of its queue.
 * A thread keeps the index of the one it waits on, QUEUE_NONE for none.
 */
enum { QUEUE_NONE, QUEUE_RUN, QUEUE_SPARE, QUEUE_SPENT, QUEUES };

/*
 * How many Irqs are armed on a device's line or a timer line,
 * the sources that can make a thread runnable while none is; see sched_run_next.
 * Every write of an Irq's bits goes through irq_set_bits, which keeps it.
 */
extern uint32_t armed_sources;

/*
 * How far ahead of the count a core's nearest deadline or release lies while it has none,
 * as far as a signed difference reaches.
 */
#define NEAREST_NONE 0x7fffffffu

/*
 * What each core has of its own: its thread, its turn, its queues, its release, its nearest deadline, its timer,
 * the call it is in, and what it tells the others.
 * The units, the tick count and the lines are the machine's.
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
     * The thread whose account pays for the turn while the turn's thread runs on lent time, NULL while it pays itself:
     * a thread of this core that lends the turn's thread its time while it waits; see OP_NOTIFY_LEND.
     * It is charged as the turn's thread would be, and counted to the core's count while it pays,
     * and it pays until the turn ends, or takes the turn over as the turn's thread wakes it.
     */
    struct thread *payer;

    /*
     * The machine's tick the core has counted to, and charged its turn to:
     * a trap on the core counts up to the machine's count as it begins,
     * so it lags while the core runs on without trapping, and another core counted the ticks since.
     */
    uint32_t ticks;

    /*
     * How far into the tick the turn has been charged, in counts, at most a tick's:
     * the turn's thread owes the counts from here to where the counter is.
     * Under tracing the clock is the tick count alone, so it is always zero.
     */
    uint32_t turn_from;

    /* The heads of the three queues; see QUEUE_RUN. */
    paddr_t run_queue;
    paddr_t spare_queue;
    paddr_t spent_queue;

    /*
     * The tick the kernel next looks at the threads with units that wait without time,
     * to give those with a tick time again:
     * the nearest tick one of their accounts reaches a tick, or NEAREST_NONE ticks ahead of the count with none.
     * A thread brings it forward as it waits without time, and the look makes it exact again,
     * so it may lie before the nearest such tick but never after one, and always ahead of the count.
     */
    uint32_t nearest_release;

    /*
     * The tick the core's timer has to interrupt at for the timer lines armed on it, see struct irq,
     * and for the watchdog if it fed it last:
     * the nearest deadline of these, or NEAREST_NONE ticks ahead of the count with none.
     * An arm brings it forward, and a tick, which looks at every line, makes every core's exact again,
     * so it may lie before the nearest such deadline but never after one, and always ahead of the count.
     */
    uint32_t nearest_deadline;

    /*
     * The tick the timer is set for, whether a change since may call for another,
     * and whether a trap's count has reached it, which ends the turn as that trap returns.
     * Settling a thread, binding or unbinding one, a switch, a tick, an arm and the stall mark it stale.
     * The marks of a switch and a bind only let the timer be set further sooner, so no check misses them.
     */
    uint32_t wake_tick;
    bool wake_stale;
    bool turn_due;

    /* Whether a walk stopped where OP_DEBUG_PREEMPT armed it, which the call's dispatch turns into a tick. */
    bool preempt_stopped;

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

    /* Whether a core told this one of a change it must trap for, which it interrupts as it gives the lock up. */
    bool interrupt_owed;
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
 * or the thread its process, after which its frame is the one a watcher reads.
 */
void core_regions_changed(const struct process *proc);
void core_thread_stopped(const struct thread *t);

/* The thread is destroyed: no core runs it any more, nor has a turn for it. */
void core_thread_gone(const struct thread *t);

/*
 * The thread a trap from user mode is for, once the trap has the lock and has counted its ticks:
 * the core's running thread, or NULL if another core stopped or destroyed it while the trap waited for the lock,
 * when what it trapped for is dropped and the core hands the processor on; see DESIGN.md, "Cores".
 * Another core stops a thread only by taking its process, which it never gets back, so its state tells.
 */
struct thread *core_trapped(void);

/*
 * A thread became ready on a core, or brought its release forward:
 * the core sets its timer again, and is interrupted for it unless it does so within a tick,
 * once this core gives the lock up, so that it does not wait for the lock as it wakes.
 */
void core_notify(struct core *c);

/*
 * Interrupt another core once this one gives the lock up, whatever it runs,
 * for a change it finds as it takes the lock: a line its controller is to forward, or no longer, see nvic.c.
 */
void core_interrupt_later(struct core *c);

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

/* Make a stopped or woken thread ready: it joins the back of the queue its account puts it on, if it may run. */
void sched_ready(struct thread *t);

/*
 * Bind a thread to count units from first, with rights, as a node below parent;
 * it leaves the units it earned, and keeps what its account held, up to what the new units hold at most.
 * No other thread earns any of the units; the caller has made sure.
 * A ready thread that is not running joins the back of the queue its account puts it on.
 */
void sched_bind(struct thread *t, uint32_t first, uint32_t count, uint8_t rights, struct cap *parent);

/*
 * Clear a thread's units the tree has already let go of:
 * nobody earns them any more, its account is emptied, and a ready thread that is not running leaves its queue.
 */
void sched_unbind(struct cap *bound);

/*
 * Clear a thread's process the tree has already let go of: the thread stops,
 * leaving its queue or its notification's waiters,
 * and the running one gives the processor up as its call returns.
 */
void sched_unhost(struct cap *hosted);

/*
 * Fill the account of every thread with units, and put each on the queue that puts it on.
 * The boot calls it once the root thread is bound, and OP_DEBUG_TRACE does.
 */
void sched_accounts_fill(void);

/* Start running a thread with nothing else run before it; the boot's. */
void sched_start(struct thread *t);

/*
 * Ticks so far, on the whole machine.
 * Only a trap's count of the ticks, the stall's and OP_DEBUG_TICK move it,
 * and the deadlines of timer lines are counted in it.
 * Each core counts up to it as its traps begin; see struct core.
 */
extern uint32_t sched_ticks;

/*
 * The machine's watchdog, which OP_CLOCK_WATCHDOG arms and feeds:
 * once armed, the tick count reaching its deadline halts the machine.
 * The core that fed it last wakes for its deadline, as for a timer line it armed; see struct core.
 * See DESIGN.md, "The watchdog".
 */
struct watchdog {
    bool armed;
    uint8_t core;
    uint32_t deadline;
};
extern struct watchdog watchdog;

/* Arm the watchdog, or move its deadline, to a tick ahead of the count, and have this core wake for it. */
void sched_watchdog(uint32_t deadline);

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
 * The running thread waits.
 * Charge it for the counts its turn ran,
 * and hand the processor to the next ready thread, or leave the core to idle once the trap is over,
 * or stop the machine.
 */
void sched_run_next(void);

/*
 * The core has no thread to run: wait for an interrupt until one has its turn, then hand the processor to it,
 * or stop the machine once nothing can ever run again.
 * The architecture's core_idle calls it as a trap ends with nobody's turn, and as a core starts.
 * Each wait is the two halves below, which the host calls for a core of several on their own:
 * sleep stops the machine if nothing can ever run again, else returns the ticks to the one to wait for;
 * wake takes what the wait counted and the interrupts it found, and gives a thread its turn, if one may have it.
 */
void sched_idle(void);
uint32_t sched_idle_sleep(void);
bool sched_idle_wake(uint32_t ticks, bool device);

/*
 * The timer tick: charge the ticks that passed to the thread whose turn it is, count them,
 * fire every timer line that is due, give time again to the threads whose account reached a tick,
 * and end the running thread's turn: it is charged for the counts it ran past the tick,
 * goes to the back of the queue its account puts it on, and the next thread has its turn.
 * OP_DEBUG_TICK calls it; the interrupt does the same through sched_count and sched_wake.
 */
void sched_tick(uint32_t ticks);

/*
 * The ticks a trap found passed since the last counted one, which the timer did not interrupt for:
 * charge and count them as sched_tick does, with those other cores counted since this one last did,
 * and note whether they reached wake_tick.
 * Every trap calls it on entry, with none under tracing, so the count is current before anything reads it.
 */
void sched_count(uint32_t ticks);

/*
 * The trap is over: end the turn if its count reached wake_tick, untraced,
 * and if wake_tick is stale return the ticks from the count to the next the timer has to interrupt at,
 * one being the next, which becomes wake_tick; zero if the timer is right as it is.
 * Every trap calls it on its way back, and the host after every call, so that the self-check can hold it.
 */
uint32_t sched_wake(void);

/*
 * The ticks from the count to the first that could change what runs, at least one:
 * the next while a thread other than the running one could have the next turn,
 * else the nearest of the deadline, the release, and the tick the running thread's account drains
 * while that ends its turn.
 * See DESIGN.md, "Scheduling".
 */
uint32_t sched_wake_ticks(void);

/*
 * A device interrupt on a line: the Irq armed on it masks the line,
 * disarms and signals its bits.
 * False, and nothing done, when no Irq is armed on the line.
 * The interrupt calls it through sched_claim_interrupts, and OP_DEBUG_IRQ does on request.
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
