/*
 * The cores: the lock around the kernel, what each tells the others without it,
 * and how one makes another trap.
 * See DESIGN.md, "Cores".
 */

#include "kernel.h"
#include "object.h"

#if CORES > 1
_Static_assert(CORES <= 32, "the cores owed an interrupt fit a word");

/*
 * A ticket lock, so that the cores take it in the order they asked for it,
 * and a core waits for it at most as long as each core ahead holds it:
 * one step of a walk at most, since a walk stops for a core waiting as for an interrupt.
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

void core_thread_stopped(const struct thread *t)
{
#if CORES > 1
    for (uint32_t i = 0; i < CORES; i++) {
        LOOP_BOUND(CORES);
        if (cores[i].current == t) {
            core_shoot(&cores[i]);
        }
    }
#else
    (void)t;
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
        if (c->turn == t) {
            c->turn = NULL;
        }
    }
}

struct thread *core_trapped(void)
{
    struct thread *t = core_self()->current;
    if (t != NULL && t->state == THREAD_READY) {
        return t;
    }
    sched_run_next();
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
     * A core in the kernel sets its timer again before it leaves, and one whose timer comes within a tick then;
     * an idle one waits for an interrupt, and so does one that runs on with its timer set further.
     */
    if (c != core_self() &&
        (c->turn == NULL || (__atomic_load_n(&c->in_user, __ATOMIC_ACQUIRE) && c->wake_tick - c->ticks > 1))) {
        c->interrupt_owed = true;
    }
#else
    (void)c;
#endif
}
