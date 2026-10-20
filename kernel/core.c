/*
 * The cores: the lock around the kernel, what each tells the others without it,
 * and how one makes another trap.
 * See DESIGN.md, "Cores".
 */

#include "kernel.h"
#include "object.h"
#include "sched.h"

#if CORES > 1
_Static_assert(CORES <= 32, "the cores owed an interrupt fit a word");

/*
 * A ticket lock, so that the cores take it in the order they asked for it,
 * and a core waits for it at most as long as each core ahead holds it:
 * one step of a walk at most, since a walk stops for a core waiting as for an interrupt.
 * It is taken with an acquire and given up with a release, which programs rely on too:
 * a wait that returns a signal's bits sees what the signaller stored before it; MANUAL.md, section 5.7.
 */
struct kernel_lock kernel_lock;

static void core_lock(void)
{
    uint32_t ticket = arch_ticket_take(&kernel_lock.next);
#ifdef RVUOS_HOST
    /* The host runs one core at a time, so a lock it finds taken would never be given up. */
    if (__atomic_load_n(&kernel_lock.owner, __ATOMIC_RELAXED) != ticket) {
        kpanic("the kernel's lock is taken twice");
    }
#endif
    while (__atomic_load_n(&kernel_lock.owner, __ATOMIC_ACQUIRE) != ticket) {
        LOOP_CORE("the cores that asked first, each of which holds the lock for a step of a walk at most");
        arch_core_wait();
    }
}

static void core_unlock(void)
{
    __atomic_store_n(&kernel_lock.owner, kernel_lock.owner + 1, __ATOMIC_RELEASE);
    arch_core_wake();
}

/* The cores owed an interrupt, taken under the lock, to interrupt once it is given up. */
static uint32_t core_owed(void)
{
    uint32_t owed = 0;
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        if (cores[i].interrupt_owed) {
            cores[i].interrupt_owed = false;
            owed |= 1u << i;
        }
    }
    return owed;
}

static void core_interrupt(uint32_t owed)
{
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        if (owed & (1u << i)) {
            ipi_send(i);
        }
    }
}
#endif

void core_enter(void)
{
#if CORES > 1
    struct core *core = core_self();
    /* The registers are in the frame, so a core waiting for them to be may go on. */
    __atomic_store_n(&core->in_user, false, __ATOMIC_RELEASE);
    arch_core_wake();
    core_lock();
    /*
     * Regions another core took from the process are loaded again before the trap reads anything,
     * unless the thread lost its process too, or went, and the next switch loads another process's.
     */
    const struct thread *t = core->current;
    if (core->pmp_stale && t != NULL && thread_process(t) != NULL) {
        core->pmp_stale = false;
        process_activate(thread_process(t));
    }
#endif
}

void core_leave(void)
{
#if CORES > 1
    uint32_t owed = core_owed();
    core_self()->in_user = true;
    core_unlock();
    core_interrupt(owed);
#endif
}

void core_stall_begin(void)
{
#if CORES > 1
    uint32_t owed = core_owed();
    core_unlock();
    core_interrupt(owed);
#endif
}

void core_stall_end(void)
{
#if CORES > 1
    core_lock();
#endif
}

void core_shoot(struct core *c)
{
#if CORES > 1
    if (c == core_self() || !__atomic_load_n(&c->in_user, __ATOMIC_ACQUIRE)) {
        return;
    }
    ipi_send((uint32_t)(c - cores));
    while (__atomic_load_n(&c->in_user, __ATOMIC_ACQUIRE)) {
        LOOP_CORE("the core to trap, which user mode does at once, and save its thread's registers");
        arch_core_wait();
#ifdef RVUOS_HOST
        /* The host runs one core at a time, so the core this one waits for traps here. */
        host_core_traps((uint32_t)(c - cores));
#endif
    }
#else
    (void)c;
#endif
}

void core_regions_changed(const struct process *proc)
{
#if CORES > 1
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        struct core *c = &cores[i];
        if (c != core_self() && c->current != NULL && thread_process(c->current) == proc) {
            c->pmp_stale = true;
            core_shoot(c);
        }
    }
#else
    (void)proc;
#endif
}

bool core_thread_stopped(const struct thread *t)
{
#if CORES > 1
    bool marked = false;
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        struct core *c = &cores[i];
        /* A call between this one and the core's trap may resume the thread, so the mark, not its state, says it stopped. */
        if (c != core_self() && c->turn == t) {
            c->drop_trap = true;
            marked = true;
        }
        if (c->current == t) {
            core_shoot(c);
        }
    }
    return marked;
#else
    (void)t;
    return false;
#endif
}

void core_thread_gone(const struct thread *t)
{
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        struct core *c = &cores[i];
        if (c->current == t) {
            c->current = NULL;
        }
        /* The trap finds no thread, and drops what it was for all the same. */
        if (c->turn == t) {
            c->turn = NULL;
            c->drop_trap = false;
        }
        /* The turn it paid for may be left on spare time, which a thread with time waiting there takes. */
        if (c->payer == t) {
            c->payer = NULL;
            core_notify(c);
        }
    }
}

struct thread *core_trapped(void)
{
    struct core *core = core_self();
    struct thread *t = core->current;
#if CORES > 1
    /* A trap another core marked is not its thread's any more, whatever the thread is by then. */
    if (core->drop_trap) {
        core->drop_trap = false;
        t = NULL;
    }
#endif
    if (t != NULL && t->state == THREAD_READY) {
        return t;
    }
    /*
     * The turn is over, as the stall has it: an end the trap's count found due is moot,
     * and the thread stands at no call it is to make again first, whatever a watcher did to it meanwhile.
     * One resumed since goes to the back of its queue, as at a tick.
     */
    core->turn_due = false;
    core->recall = false;
    sched_turn_end();
    return NULL;
}

void core_interrupt_later(struct core *c)
{
#if CORES > 1
    if (c != core_self()) {
        c->interrupt_owed = true;
    }
#else
    (void)c;
#endif
}

void core_notify(struct core *c)
{
#if CORES > 1
    c->wake_stale = true;
    /*
     * A core in the kernel looks at its timer and its turn before it leaves, and one whose timer comes within a tick then;
     * an idle one waits for an interrupt, and so does one that runs on with its timer set further,
     * or whose run queue or turn changed while a thread waits there, which takes the core at once
     * if the turn is on spare time or its request is due before the turn's.
     */
    if (c != core_self() &&
        (c->turn == NULL || (__atomic_load_n(&c->in_user, __ATOMIC_ACQUIRE) &&
                             (c->wake_tick - c->ticks > 1 || (c->run_queue != 0 && c->run_stale))))) {
        c->interrupt_owed = true;
    }
#else
    (void)c;
#endif
}

bool core_notice_owed(void)
{
    const struct core *core = core_self();
    if (core->run_queue != 0 && core->wake_tick - core->ticks > 1) {
        return true;
    }
#if CORES > 1
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        if (cores[i].interrupt_owed) {
            return true;
        }
    }
#endif
    return false;
}
