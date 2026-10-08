/*
 * The OS functions Espressif's Wi-Fi libraries call, on rvuos: struct osi_funcs of esp.h.
 *
 * The libraries expect a FreeRTOS: tasks, semaphores, mutexes, queues, event groups and timers,
 * an interrupt that preempts the tasks, and critical sections that hold it off.
 * Here every task is a thread of the driver's process, with a stack from the heap,
 * a notification of its own it waits on, and a timer line that signals it there,
 * all built from the driver's own account, lib/child.h's child_self;
 * the driver's first thread, which the root task started, joins them, osi_adopt;
 * each finds its own record in tp, which nothing else the driver runs uses, the libraries and the ROM included;
 * the interrupt is a thread that waits for the MAC's line and calls the handler the libraries set,
 * and a critical section holds a lock that thread takes too.
 * A watcher, a thread of the adapter's, tells the faults of the others; the root task watches the watcher.
 * The objects lie in the heap, under one lock, and a thread that waits on one leaves it a link in the object's list;
 * whoever changes the object wakes the first, or every, thread on it, which looks again.
 * A tick is a millisecond.
 */

#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "osi.h"
#include "rvuos.h"
#include "tracer/watch.h"

/* The bits of a thread's notification. */
#define BIT_WAKE  0x1u /* something it waits on changed */
#define BIT_TIMER 0x2u /* its timer line */
#define BIT_IRQ   0x4u /* the interrupt thread's: a line it binds, from here up, one bit a CPU interrupt */
#define BIT_FAULT 0x100u /* the watcher's: from here up, one bit for each thread of osi.threads */
#define STACK_WORDS 16   /* of a faulted thread's stack, which the watcher tells */
#define STACK_PAINT 0xa5u /* what a new thread's stack is filled with, so that osi_stack_used finds how deep it went */

#define THREADS 8
#define INTRS   8 /* CPU interrupts the libraries route sources to, numbered below 32 */

/*
 * trace=1: the driver's steps and the calls the libraries make into the adapter, in the request order the device
 * accesses come in, so the window's two halves -- the accesses the root serves, and what the libraries asked the
 * adapter for between them -- read as one sequence. The buffer is taken from the heap by a traced run's start
 * (osi_trace_init, from trace_run, in both trace modes so the heap they see is alike), so an ordinary run pays
 * nothing; no lock on the path (a faulting thread may call it); printed after the window. Purely synchronising
 * calls (mutexes, semaphores, queues) are left out.
 */
static struct { uint32_t asked, thread, a0, a1; const char *what; } *osi_trace_buf;
static uint32_t osi_trace_max, osi_trace_n, osi_trace_lost;

struct thread {
    uint32_t cap, note, timer;
    uintptr_t stack_lo, stack_hi;
    void (*f)(void *);
    void *arg;
    void *semphr;
    struct thread *next; /* on an object's list of waiters */
    int woken;
    const char *name;
};

struct waiters {
    struct thread *first;
};

struct semphr {
    struct waiters w;
    uint32_t count, max;
};

struct mutex {
    struct waiters w;
    struct thread *owner;
    uint32_t depth;
    int recursive;
};

struct queue {
    struct waiters senders, receivers;
    uint32_t len, item_size, head, count;
    uint8_t items[];
};

struct event_group {
    struct waiters w;
    uint32_t bits;
};

/* ETSTimer's layout, which the libraries allocate: the driver keeps its armed timers listed through next. */
struct ets_timer {
    struct ets_timer *next;
    uint32_t expire;
    uint32_t period;
    void (*f)(void *);
    void *arg;
};

static struct {
    struct drv *d;
    struct self *s; /* what the driver builds its threads from */
    struct lock big, crit, heap;
    uint32_t big_word, crit_word, heap_word;
    struct thread *crit_owner;
    uint32_t crit_depth;
    struct thread threads[THREADS];
    uint32_t thread_count;
    struct thread *isr, *timers, *watcher;
    struct {
        uint32_t source;
        uint32_t irq; /* the slot of the Irq bound to the source's line, 0 until first enabled */
        void (*f)(void *);
        void *arg;
    } intr[INTRS];
    uint32_t intr_on;
    struct ets_timer *armed;
    uint64_t rng_last; /* when drv_random last took a byte */
} osi;

/* The traced run's counters, for the step that holds it against the same window with no trace; see osi.h. */
static uint32_t fault_count;
static uint32_t isr_count;

uint32_t osi_faults_served(void)
{
    return fault_count;
}

uint32_t osi_interrupts(void)
{
    return isr_count;
}

/* --- Time. --- */

uint64_t osi_now_us(void)
{
    uint64_t now;
    uint32_t hz, counter;
    rv_clock_read(osi.d->clock, &now, &hz, &counter);
    return now / (hz / 1000000u);
}

/* --- Threads. --- */

/* The thread's own record, or 0 in a thread the adapter has not adopted yet, whose tp the kernel zeroed. */
static struct thread *self(void)
{
    struct thread *t;
    __asm__ volatile("mv %0, tp" : "=r"(t));
    return t;
}

/* --- The adapter calls, the start's other half, for trace=1; the buffer is declared at the top. --- */

void osi_trace_init(uint32_t entries)
{
    osi_trace_buf = osi_malloc(entries * sizeof(*osi_trace_buf));
    osi_trace_max = osi_trace_buf ? entries : 0;
    osi_trace_n = 0;
    osi_trace_lost = 0;
}

void osi_trace(const char *what, uint32_t a0, uint32_t a1)
{
    if (drv_self->trace != 1 || osi_trace_buf == 0) {
        return;
    }
    uint32_t i = __atomic_fetch_add(&osi_trace_n, 1, __ATOMIC_RELAXED);
    if (i >= osi_trace_max) {
        __atomic_fetch_add(&osi_trace_lost, 1, __ATOMIC_RELAXED);
        return;
    }
    osi_trace_buf[i].asked = drv_self->share.asked;
    osi_trace_buf[i].thread = (uint32_t)(self() - osi.threads);
    osi_trace_buf[i].what = what;
    osi_trace_buf[i].a0 = a0;
    osi_trace_buf[i].a1 = a1;
}

void osi_trace_dump(void)
{
    /*
     * Each thread's role first, a line of its own: a reader can tell the interrupt's and the timers' accesses, whose
     * place in time is the hardware's, from the synchronous ones'; tools/mac-trace.py's diff reads these lines.
     */
    for (uint32_t i = 0; i < osi.thread_count; i++) {
        const char *role = &osi.threads[i] == osi.isr      ? "isr"
                           : &osi.threads[i] == osi.timers ? "timers"
                           : &osi.threads[i] == osi.watcher ? "watcher"
                                                            : "other";
        drv_say("trace: thread %u %s\n", i, role);
    }
    uint32_t n = osi_trace_n < osi_trace_max ? osi_trace_n : osi_trace_max;
    for (uint32_t i = 0; i < n; i++) {
        drv_say("trace: req %u t%u %s %u %u\n", osi_trace_buf[i].asked, osi_trace_buf[i].thread,
                osi_trace_buf[i].what, osi_trace_buf[i].a0, osi_trace_buf[i].a1);
    }
    if (osi_trace_lost) {
        drv_say("trace: %u more past the buffer\n", osi_trace_lost);
    }
}

/* Every thread starts here, with a0 at its own record, which it keeps in tp. */
static void start(struct thread *t)
{
    __asm__ volatile("mv tp, %0" : : "r"(t));
    t->f(t->arg);
    /* A task that returns, or deletes itself, waits for good. */
    for (;;) {
        uint32_t bits;
        rv_wait(t->note, &bits);
    }
}

/* A thread's record, and what it waits on: a notification of its own, and a timer line that signals it. */
static uint32_t thread_new(struct self *s, struct thread *t, const char *name, uintptr_t stack_lo, uintptr_t stack_hi)
{
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->stack_lo = stack_lo;
    t->stack_hi = stack_hi;
    s->what = 0;
    uint32_t status;
    if ((status = slot_new(s, &t->note)) != KERR_OK ||
        (status = rv_pool_alloc(s->pool, CAP_NOTIFICATION, t->note, 0)) != KERR_OK) {
        return status;
    }
    return timer_bind(s, s->pool, t->note, &t->timer);
}

/*
 * A thread more, watched by the watcher, or the watcher itself, watched through the root task's fault bit,
 * so that its fault is the driver's.
 */
static uint32_t start_thread(struct self *s, struct thread *t, void (*f)(void *), void *arg, const char *name,
                             uint8_t *mem, uint32_t stack)
{
    uint32_t unit, time, status;
    if ((status = thread_new(s, t, name, (uintptr_t)mem, (uintptr_t)mem + stack)) != KERR_OK) {
        return status;
    }
    t->f = f;
    t->arg = arg;
    uint32_t watch = osi.watcher ? osi.watcher->note : osi.d->own.watch;
    uint32_t bits = osi.watcher ? BIT_FAULT << (t - osi.threads) : NOTIFY_ALL_BITS;
    if ((status = slot_new(s, &t->cap)) != KERR_OK ||
        (status = rv_pool_alloc(s->pool, CAP_THREAD, t->cap, s->process)) != KERR_OK ||
        (status = rv_thread_watch(t->cap, watch, bits)) != KERR_OK ||
        (status = rv_thread_configure(t->cap, (uint32_t)(uintptr_t)start, t->stack_hi, (uint32_t)(uintptr_t)t)) !=
            KERR_OK ||
        (status = units_take(s, 1, &unit)) != KERR_OK || (status = slot_new(s, &time)) != KERR_OK) {
        return status;
    }
    status = rv_time_carve(s->time, unit, 1, time);
    if (status == KERR_OK) {
        status = rv_time_bind(time, t->cap, 0, 1);
    }
    slot_free(s, time);
    return status;
}

/*
 * A thread more, which starts at f with arg on a stack of its own from the heap;
 * the process's account of its slots and units is the big lock's too, since threads share it.
 */
struct thread *osi_thread(void (*f)(void *), void *arg, const char *name, uint32_t stack)
{
    struct self *s = osi.s;
    stack = (stack + 15u) & ~15u;
    uint8_t *mem = osi_malloc(stack);
    if (mem == 0) {
        drv_failed("a thread's stack", KERR_NO_MEMORY);
        return 0;
    }
    memset(mem, STACK_PAINT, stack);
    lock_take(&osi.big);
    uint32_t i = osi.thread_count;
    struct thread *t = i < THREADS ? &osi.threads[i] : 0;
    uint32_t status = t ? start_thread(s, t, f, arg, name, mem, stack) : KERR_LIMIT;
    if (status == KERR_OK) {
        osi.thread_count = i + 1;
        status = rv_thread_resume(t->cap);
    }
    lock_give(&osi.big);
    if (status != KERR_OK) {
        osi_free(mem);
        drv_failed(s->what ? s->what : "start a thread", status);
        return 0;
    }
    return t;
}

/* How deep a thread went, by the paint it left: to size a stack by, as no stack has a guard to catch an overflow. */
uint32_t osi_stack_used(const struct thread *t)
{
    const uint8_t *p = (const uint8_t *)t->stack_lo, *hi = (const uint8_t *)t->stack_hi;
    while (p < hi && *p == STACK_PAINT) {
        p++;
    }
    return (uint32_t)(hi - p);
}

struct thread *osi_adopt(const char *name, uintptr_t stack_lo, uintptr_t stack_hi)
{
    lock_take(&osi.big);
    uint32_t i = osi.thread_count;
    struct thread *t = i < THREADS ? &osi.threads[i] : 0;
    uint32_t status = t ? thread_new(osi.s, t, name, stack_lo, stack_hi) : KERR_LIMIT;
    if (status == KERR_OK) {
        osi.thread_count = i + 1;
    }
    lock_give(&osi.big);
    if (status != KERR_OK) {
        drv_failed(osi.s->what ? osi.s->what : "adopt a thread", status);
        return 0;
    }
    if (osi.d->trace == 1) {
        /*
         * Trace mode: the driver's first thread is watched by the watcher too,
         * so every fault of the driver's, the libraries' bring-up's among them, takes the one served path.
         * The root task gave the driver a capability to that thread, drv.h's own_thread.
         */
        t->cap = osi.d->own_thread;
        status = rv_thread_watch(t->cap, osi.watcher->note, BIT_FAULT << i);
        if (status != KERR_OK) {
            drv_failed("watch the driver's first thread", status);
            return 0;
        }
    }
    __asm__ volatile("mv tp, %0" : : "r"(t));
    return t;
}

struct thread *osi_self(void)
{
    return self();
}

/*
 * Waits, with the big lock held and given up meanwhile, on w until woken or until deadline, in microseconds;
 * 1 if woken.
 */
static int wait_on(struct waiters *w, uint64_t deadline)
{
    struct thread *t = self();
    t->woken = 0;
    t->next = 0;
    struct thread **p = &w->first;
    while (*p) {
        p = &(*p)->next;
    }
    *p = t;
    for (;;) {
        uint64_t now = osi_now_us();
        if (t->woken || now >= deadline) {
            break;
        }
        if (deadline != UINT64_MAX) {
            uint64_t left = deadline - now;
            rv_timer_set(t->timer, BIT_TIMER, left > 0x7fffffffu ? 0x7fffffffu : (uint32_t)left);
        }
        lock_give(&osi.big);
        uint32_t bits;
        rv_wait(t->note, &bits);
        lock_take(&osi.big);
    }
    if (deadline != UINT64_MAX) {
        rv_timer_set(t->timer, 0, 0);
    }
    if (!t->woken) {
        for (p = &w->first; *p; p = &(*p)->next) {
            if (*p == t) {
                *p = t->next;
                break;
            }
        }
    }
    return t->woken;
}

static void wake(struct waiters *w, int all)
{
    while (w->first) {
        struct thread *t = w->first;
        w->first = t->next;
        t->woken = 1;
        rv_signal(t->note, BIT_WAKE);
        if (!all) {
            return;
        }
    }
}

static uint64_t deadline_of(uint32_t ticks)
{
    return ticks == OSI_FOREVER ? UINT64_MAX : osi_now_us() + (uint64_t)ticks * 1000u;
}

/* --- Interrupts, and the critical sections that hold them off. --- */

static uint32_t int_disable(void *mux)
{
    (void)mux;
    struct thread *t = self();
    if (osi.crit_owner == t && t != 0) {
        osi.crit_depth++;
        return 0;
    }
    lock_take(&osi.crit);
    osi.crit_owner = t;
    osi.crit_depth = 1;
    return 0;
}

static void int_restore(void *mux, uint32_t tmp)
{
    (void)mux;
    (void)tmp;
    if (--osi.crit_depth == 0) {
        osi.crit_owner = 0;
        lock_give(&osi.crit);
    }
}

/*
 * One served access in trace mode: decode the fault, hand the request to the root task, wait for its answer,
 * and leave it in the thread; no lock is taken and nothing goes on the heap,
 * since the faulted thread may hold one of the adapter's, which the fault keeps with it.
 * The answer comes on its own notification, so the watcher's fault bits are not taken with it.
 */
static void trace_cannot(struct thread *t);

static int trace_serve(struct thread *t, int batch)
{
    struct drv *d = osi.d;
    struct trace_req req;
    struct trace_pending p;
    if (trace_fault(t->cap, &req, &p) != 0) {
        trace_cannot(t);
        return -1;
    }
    req.thread = (uint32_t)(t - osi.threads);
    if (batch) {
        req.flags |= TRACE_FLAG_BATCH;
    }
    d->share.req = req;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->share.asked++;
    rv_signal(CHILD_PARENT, NOTIFY_ALL_BITS);
    /* The answer's own bit may be one left from an earlier request, so wait until the root has answered this one. */
    uint32_t bits = 0;
    do {
        rv_wait(d->trace_note, &bits);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (d->share.answered != d->share.asked);
    req.value = d->share.req.value;
    if (d->share.req.status != TRACE_DONE) {
        return -1; /* the root task refused it, and logged why */
    }
    if (trace_done(t->cap, &req, &p) != 0) {
        trace_cannot(t);
        return -1;
    }
    return 0;
}

/* Why a fault could not even become a request, into the page, so the root task tells it before it halts. */
static void trace_cannot(struct thread *t)
{
    struct drv *d = osi.d;
    uint32_t cause = 0, pc = 0, addr = 0, status = 0;
    rv_thread_fault(t->cap, &cause, &pc, &addr, &status);
    d->share.giveup.cause = cause;
    d->share.giveup.pc = pc;
    d->share.giveup.address = addr;
    d->share.giveup.thread = (uint32_t)(t - osi.threads);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->share.cannot = 1;
}

/* A fault the tracer cannot carry out, told without a lock; the root task halts the machine on it. */
static void trace_fail(void)
{
    struct drv *d = osi.d;
    static const char what[] = "the tracer cannot serve it";
    memcpy(d->failed, what, sizeof(what));
    child_report(&d->c, CHILD_FAILED);
}

/*
 * The faults of the adapter's threads, each told with its thread's name, cause, pc, the address it touched,
 * return address, stack pointer and the stack's top words, which may hold the return addresses of its callers;
 * the first fails the driver, which the root task halts the machine for.
 * In trace mode each is handed to the root task and served instead, so the libraries' bring-up runs with no device mapped.
 */
static void watcher_main(void *arg)
{
    (void)arg;
    struct thread *me = self();
    for (;;) {
        uint32_t bits;
        rv_wait(me->note, &bits);
        /* Whether one wake brought faults of more than one thread, whose order among them is not known. */
        uint32_t waiting = 0;
        for (uint32_t i = 0; i < THREADS; i++) {
            waiting += (bits & (BIT_FAULT << i)) != 0;
        }
        for (uint32_t i = 0; i < THREADS; i++) {
            struct thread *t = &osi.threads[i];
            uint32_t cause, pc, addr, status, ra = 0, sp = 0;
            if (!(bits & (BIT_FAULT << i)) || t->cap == 0) {
                continue;
            }
            if (osi.d->trace == 1) {
                if (trace_serve(t, waiting > 1) == 0) {
                    fault_count++;
                    continue;
                }
                trace_fail();
                for (;;) {
                    rv_wait(me->note, &bits);
                }
            }
            if (rv_thread_fault(t->cap, &cause, &pc, &addr, &status) != KERR_OK) {
                continue;
            }
            rv_thread_read_reg(t->cap, 1, &ra);
            rv_thread_read_reg(t->cap, 2, &sp);
            drv_say("fault: thread %s, cause %x, pc %08x, address %08x, ra %08x, sp %08x\n", t->name, (unsigned)cause,
                    (unsigned)pc, (unsigned)addr, (unsigned)ra, (unsigned)sp);
            for (uint32_t w = 0; w < STACK_WORDS && sp >= t->stack_lo && sp + 4 * (w + 4) <= t->stack_hi; w += 4) {
                const uint32_t *word = (const uint32_t *)(uintptr_t)sp + w;
                drv_say("fault: stack %08x: %08x %08x %08x %08x\n", (unsigned)(sp + 4 * w), (unsigned)word[0],
                        (unsigned)word[1], (unsigned)word[2], (unsigned)word[3]);
            }
            drv_failed("a thread faulted", cause);
            child_report(&osi.d->c, CHILD_FAILED);
        }
    }
}

/*
 * The adapter's own threads find their records through self, as they may run before osi_thread returns them.
 */
static void isr_main(void *arg)
{
    (void)arg;
    struct thread *me = self();
    for (;;) {
        uint32_t bits;
        rv_wait(me->note, &bits);
        for (uint32_t n = 0; n < INTRS; n++) {
            if (!(bits & (BIT_IRQ << n))) {
                continue;
            }
            /* Counted even with no handler, so a run shows the lines firing and not only the ones the driver took. */
            isr_count++;
            if (!osi.intr[n].f) {
                continue;
            }
            int_disable(0);
            osi.intr[n].f(osi.intr[n].arg);
            int_restore(0, 0);
            /* The interrupt masked the line; unmask it again for the next, if the libraries still want it. */
            if (osi.intr_on & (1u << n)) {
                rv_irq_set(osi.intr[n].irq, BIT_IRQ << n);
            }
        }
    }
}

static void set_intr(int32_t cpu, uint32_t source, uint32_t n, int32_t prio)
{
    (void)cpu;
    (void)prio;
    osi_trace("set_intr", source, n);
    drv_say("osi: source %u to interrupt %u\n", (unsigned)source, (unsigned)n);
    if (n < INTRS) {
        osi.intr[n].source = source;
    }
}

static void clear_intr(uint32_t source, uint32_t n)
{
    osi_trace("clear_intr", source, n);
}

static void set_isr(int32_t n, void *f, void *arg)
{
    osi_trace("set_isr", (uint32_t)n, 0);
    if ((uint32_t)n < INTRS) {
        osi.intr[n].f = (void (*)(void *))f;
        osi.intr[n].arg = arg;
    }
}

/*
 * The handler of the CPU interrupt the libraries routed source to, exchanged with isr in the critical section,
 * which holds the interrupt's thread off; 0 if they routed nothing from source.
 */
int osi_isr_swap(uint32_t source, struct osi_isr *isr)
{
    for (uint32_t n = 0; n < INTRS; n++) {
        if (osi.intr[n].f != 0 && osi.intr[n].source == source) {
            int_disable(0);
            struct osi_isr was = { osi.intr[n].f, osi.intr[n].arg };
            osi.intr[n].f = isr->f;
            osi.intr[n].arg = isr->arg;
            int_restore(0, 0);
            *isr = was;
            return 1;
        }
    }
    return 0;
}

/*
 * The line of a source is the source plus one, see kernel/board/esp32c6/irq.c,
 * carved from the modem's lines the root task gave the driver, which start at DRV_LINE_FIRST.
 */
static void ints_on(uint32_t mask)
{
    osi_trace("ints_on", mask, 0);
    struct self *s = osi.s;
    for (uint32_t n = 0; n < INTRS; n++) {
        if (!(mask & (1u << n))) {
            continue;
        }
        if (osi.intr[n].irq == 0) {
            uint32_t slot, status, line = osi.intr[n].source + 1u - DRV_LINE_FIRST;
            lock_take(&osi.big);
            if (line >= DRV_LINES) {
                status = KERR_INVALID_ARG;
            } else if ((status = slot_new(s, &slot)) == KERR_OK &&
                       (status = rv_irq_carve(osi.d->lines, line, 1, slot)) == KERR_OK) {
                status = rv_irq_bind(slot, s->pool, osi.isr->note, slot);
            }
            lock_give(&osi.big);
            if (status != KERR_OK) {
                drv_failed("bind an interrupt's line", status);
                continue;
            }
            osi.intr[n].irq = slot;
        }
        osi.intr_on |= 1u << n;
        rv_irq_set(osi.intr[n].irq, BIT_IRQ << n);
    }
}

static void ints_off(uint32_t mask)
{
    osi_trace("ints_off", mask, 0);
    for (uint32_t n = 0; n < INTRS; n++) {
        if ((mask & (1u << n)) && osi.intr[n].irq) {
            osi.intr_on &= ~(1u << n);
            rv_irq_set(osi.intr[n].irq, 0);
        }
    }
}

static bool is_from_isr(void)
{
    return self() == osi.isr;
}

static void *spin_lock_create(void)
{
    return &osi.crit;
}

static void spin_lock_delete(void *lock)
{
    (void)lock;
}

static void task_yield_from_isr(void)
{
}

/* --- Semaphores and mutexes. --- */

static void *semphr_create(uint32_t max, uint32_t init)
{
    struct semphr *m = osi_calloc(1, sizeof(*m));
    if (m) {
        m->max = max;
        m->count = init;
    }
    return m;
}

static void semphr_delete(void *semphr)
{
    osi_free(semphr);
}

static int32_t semphr_take(void *semphr, uint32_t ticks)
{
    struct semphr *m = semphr;
    uint64_t deadline = deadline_of(ticks);
    lock_take(&osi.big);
    while (m->count == 0 && wait_on(&m->w, deadline)) {
    }
    int ok = m->count > 0;
    if (ok) {
        m->count--;
    }
    lock_give(&osi.big);
    return ok;
}

static int32_t semphr_give(void *semphr)
{
    struct semphr *m = semphr;
    lock_take(&osi.big);
    int ok = m->count < m->max;
    if (ok) {
        m->count++;
        wake(&m->w, 0);
    }
    lock_give(&osi.big);
    return ok;
}

static void *wifi_thread_semphr_get(void)
{
    struct thread *t = self();
    if (t->semphr == 0) {
        t->semphr = semphr_create(1, 0);
    }
    return t->semphr;
}

static void *mutex_new(int recursive)
{
    struct mutex *m = osi_calloc(1, sizeof(*m));
    if (m) {
        m->recursive = recursive;
    }
    return m;
}

static void *mutex_create(void)
{
    return mutex_new(0);
}

static void *recursive_mutex_create(void)
{
    return mutex_new(1);
}

static int32_t mutex_lock(void *mutex)
{
    struct mutex *m = mutex;
    struct thread *t = self();
    lock_take(&osi.big);
    while (m->owner != 0 && m->owner != t) {
        wait_on(&m->w, UINT64_MAX);
    }
    m->owner = t;
    m->depth++;
    lock_give(&osi.big);
    return 1;
}

static int32_t mutex_unlock(void *mutex)
{
    struct mutex *m = mutex;
    lock_take(&osi.big);
    if (m->depth > 0 && --m->depth == 0) {
        m->owner = 0;
        wake(&m->w, 0);
    }
    lock_give(&osi.big);
    return 1;
}

struct mutex *osi_mutex_new(void)
{
    return mutex_new(0);
}

void osi_mutex_take(struct mutex *m)
{
    mutex_lock(m);
}

void osi_mutex_give(struct mutex *m)
{
    mutex_unlock(m);
}

/* --- Queues. --- */

static void *queue_create(uint32_t len, uint32_t item_size)
{
    struct queue *q = osi_calloc(1, sizeof(*q) + len * item_size);
    if (q) {
        q->len = len;
        q->item_size = item_size;
    }
    return q;
}

/* ESP-IDF's wifi_static_queue_t: the libraries find the queue in its first word. */
struct static_queue {
    void *handle;
    void *storage;
};

static void *wifi_create_queue(int len, int item_size)
{
    struct static_queue *q = osi_calloc(1, sizeof(*q));
    if (q && (q->handle = queue_create((uint32_t)len, (uint32_t)item_size)) == 0) {
        osi_free(q);
        q = 0;
    }
    return q;
}

static void wifi_delete_queue(void *queue)
{
    struct static_queue *q = queue;
    if (q) {
        osi_free(q->handle);
        osi_free(q);
    }
}

static void queue_delete(void *queue)
{
    osi_free(queue);
}

static int32_t queue_put(void *queue, const void *item, uint32_t ticks, int front)
{
    struct queue *q = queue;
    uint64_t deadline = deadline_of(ticks);
    lock_take(&osi.big);
    while (q->count == q->len && wait_on(&q->senders, deadline)) {
    }
    int ok = q->count < q->len;
    if (ok) {
        uint32_t at;
        if (front) {
            q->head = (q->head + q->len - 1) % q->len;
            at = q->head;
        } else {
            at = (q->head + q->count) % q->len;
        }
        memcpy(&q->items[at * q->item_size], item, q->item_size);
        q->count++;
        wake(&q->receivers, 0);
    }
    lock_give(&osi.big);
    return ok;
}

static int32_t queue_send(void *queue, void *item, uint32_t ticks)
{
    return queue_put(queue, item, ticks, 0);
}

static int32_t queue_send_from_isr(void *queue, void *item, void *hptw)
{
    (void)hptw;
    return queue_put(queue, item, 0, 0);
}

static int32_t queue_send_to_front(void *queue, void *item, uint32_t ticks)
{
    return queue_put(queue, item, ticks, 1);
}

static int32_t queue_recv(void *queue, void *item, uint32_t ticks)
{
    struct queue *q = queue;
    uint64_t deadline = deadline_of(ticks);
    lock_take(&osi.big);
    while (q->count == 0 && wait_on(&q->receivers, deadline)) {
    }
    int ok = q->count > 0;
    if (ok) {
        memcpy(item, &q->items[q->head * q->item_size], q->item_size);
        q->head = (q->head + 1) % q->len;
        q->count--;
        wake(&q->senders, 0);
    }
    lock_give(&osi.big);
    return ok;
}

static uint32_t queue_msg_waiting(void *queue)
{
    return ((struct queue *)queue)->count;
}

struct queue *osi_queue_new(uint32_t len, uint32_t item_size)
{
    return queue_create(len, item_size);
}

int osi_queue_put(struct queue *q, const void *item, uint32_t ms)
{
    return queue_put(q, item, ms, 0);
}

int osi_queue_get(struct queue *q, void *item, uint32_t ms)
{
    return queue_recv(q, item, ms);
}

/* --- Event groups. --- */

static void *event_group_create(void)
{
    return osi_calloc(1, sizeof(struct event_group));
}

static void event_group_delete(void *event)
{
    osi_free(event);
}

static uint32_t event_group_set_bits(void *event, uint32_t bits)
{
    struct event_group *e = event;
    lock_take(&osi.big);
    e->bits |= bits;
    uint32_t now = e->bits;
    wake(&e->w, 1);
    lock_give(&osi.big);
    return now;
}

static uint32_t event_group_clear_bits(void *event, uint32_t bits)
{
    struct event_group *e = event;
    lock_take(&osi.big);
    uint32_t was = e->bits;
    e->bits &= ~bits;
    lock_give(&osi.big);
    return was;
}

static uint32_t event_group_wait_bits(void *event, uint32_t bits, int clear, int all, uint32_t ticks)
{
    struct event_group *e = event;
    uint64_t deadline = deadline_of(ticks);
    lock_take(&osi.big);
    for (;;) {
        uint32_t have = e->bits & bits;
        if (all ? have == bits : have != 0) {
            break;
        }
        if (!wait_on(&e->w, deadline)) {
            break;
        }
    }
    uint32_t now = e->bits;
    uint32_t have = now & bits;
    if (clear && (all ? have == bits : have != 0)) {
        e->bits &= ~bits;
    }
    lock_give(&osi.big);
    return now;
}

/* --- Tasks. --- */

static int32_t task_create(void *f, const char *name, uint32_t stack, void *arg, uint32_t prio, void *handle)
{
    (void)prio;
    osi_trace("task", stack, 0); /* the name is in the log line below */
    drv_say("osi: task %s, %u bytes of stack\n", name, (unsigned)stack);
    struct thread *t = osi_thread((void (*)(void *))f, arg, name, stack);
    if (handle) {
        *(void **)handle = t;
    }
    return t != 0;
}

static int32_t task_create_pinned_to_core(void *f, const char *name, uint32_t stack, void *arg, uint32_t prio,
                                          void *handle, uint32_t core)
{
    (void)core;
    return task_create(f, name, stack, arg, prio, handle);
}

static void task_delete(void *handle)
{
    struct thread *t = handle ? handle : self();
    if (t == self()) {
        for (;;) {
            uint32_t bits;
            rv_wait(t->note, &bits);
        }
    }
    drv_say("osi: a task deleted another, %s, which goes on\n", t->name);
}

static void task_delay(uint32_t ticks)
{
    struct waiters nobody = { 0 };
    lock_take(&osi.big);
    wait_on(&nobody, deadline_of(ticks));
    lock_give(&osi.big);
}

void osi_delay_ms(uint32_t ms)
{
    task_delay(ms);
}

static int32_t task_ms_to_tick(uint32_t ms)
{
    return (int32_t)ms;
}

static void *task_get_current_task(void)
{
    return self();
}

static int32_t task_get_max_priority(void)
{
    return 25;
}

/* --- Timers, run by a thread of their own as ESP-IDF's esp_timer task runs them. --- */

static void timer_unlink(struct ets_timer *t)
{
    for (struct ets_timer **p = &osi.armed; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            return;
        }
    }
}

static void timer_link(struct ets_timer *t)
{
    struct ets_timer **p = &osi.armed;
    while (*p && (int32_t)((*p)->expire - t->expire) <= 0) {
        p = &(*p)->next;
    }
    t->next = *p;
    *p = t;
}

static void timer_disarm(void *timer)
{
    osi_trace("timer_disarm", 0, 0);
    lock_take(&osi.big);
    timer_unlink(timer);
    lock_give(&osi.big);
}

static void timer_setfn(void *timer, void *f, void *arg)
{
    struct ets_timer *t = timer;
    lock_take(&osi.big);
    timer_unlink(t);
    t->f = (void (*)(void *))f;
    t->arg = arg;
    t->period = 0;
    lock_give(&osi.big);
}

static void timer_arm_us(void *timer, uint32_t us, bool repeat)
{
    osi_trace("timer_arm_us", us, repeat);
    struct ets_timer *t = timer;
    lock_take(&osi.big);
    timer_unlink(t);
    t->expire = (uint32_t)osi_now_us() + us;
    t->period = repeat ? us : 0;
    timer_link(t);
    if (osi.timers) {
        rv_signal(osi.timers->note, BIT_WAKE);
    }
    lock_give(&osi.big);
}

static void timer_arm(void *timer, uint32_t ms, bool repeat)
{
    timer_arm_us(timer, ms * 1000u, repeat);
}

static void timer_done(void *timer)
{
    timer_disarm(timer);
}

/* A timer of the driver's own, which the timer thread runs as it runs the libraries'. */
struct ets_timer *osi_timer_new(void (*f)(void *), void *arg)
{
    struct ets_timer *t = osi_calloc(1, sizeof(*t));
    if (t) {
        timer_setfn(t, (void *)f, arg);
    }
    return t;
}

void osi_timer_arm_us(struct ets_timer *t, uint32_t us)
{
    timer_arm_us(t, us, false);
}

void osi_timer_disarm(struct ets_timer *t)
{
    timer_disarm(t);
}

static void timers_main(void *arg)
{
    (void)arg;
    struct thread *t = self();
    for (;;) {
        lock_take(&osi.big);
        struct ets_timer *due = osi.armed;
        uint32_t now = (uint32_t)osi_now_us();
        if (due && (int32_t)(due->expire - now) <= 0) {
            osi.armed = due->next;
            if (due->period) {
                due->expire += due->period;
                if ((int32_t)(due->expire - now) <= 0) {
                    due->expire = now + due->period;
                }
                timer_link(due);
            }
            void (*f)(void *) = due->f;
            void *farg = due->arg;
            lock_give(&osi.big);
            if (f) {
                f(farg);
            }
            continue;
        }
        if (due) {
            rv_timer_set(t->timer, BIT_TIMER, due->expire - now);
        }
        lock_give(&osi.big);
        uint32_t bits;
        rv_wait(t->note, &bits);
    }
}

/* --- Memory, see heap.c. --- */

static uint32_t get_free_heap_size(void)
{
    return osi_heap_free();
}

static void *zalloc(size_t size)
{
    return osi_calloc(1, size);
}

/* --- The rest. --- */

static bool env_is_chip(void)
{
    return true;
}

static int32_t event_post(const char *base, int32_t id, void *data, size_t size, uint32_t ticks)
{
    (void)ticks;
    osi_trace("event_post", (uint32_t)id, size);
    drv_event(base, id, data, size);
    return 0;
}

/*
 * The chip's random number generator, LPPERI's RNG_DATA register, which the root task installs read only.
 * It takes two bits of entropy from the radio each cycle of the 40 MHz APB while the radio runs,
 * so, as ESP-IDF's esp_random does on the ESP32-C6, a byte is taken 2560 of those cycles after the last,
 * every read meanwhile folded into it.
 * Threads that ask at once may each wait less; each byte still folds in at least one read of its own.
 */
#define RNG_BYTE_US 64u

void drv_random(uint8_t *buf, size_t len)
{
    volatile const uint32_t *rng = (volatile const uint32_t *)RNG_DATA;
    for (size_t i = 0; i < len; i++) {
        uint64_t until = osi.rng_last + RNG_BYTE_US, now;
        uint32_t x = 0;
        do {
            x ^= *rng;
            now = osi_now_us();
        } while (now < until);
        osi.rng_last = now;
        buf[i] = (uint8_t)(x ^ x >> 8 ^ x >> 16 ^ x >> 24);
    }
}

static uint32_t osi_rand(void)
{
    uint32_t v;
    drv_random((uint8_t *)&v, sizeof(v));
    return v;
}

static unsigned long osi_random(void)
{
    return osi_rand();
}

static int get_random(uint8_t *buf, size_t len)
{
    drv_random(buf, len);
    return 0;
}

static int get_time(void *t)
{
    uint64_t us = osi_now_us();
    uint32_t *tv = t; /* struct timeval of 64-bit seconds and 32-bit microseconds, as newlib lays it out */
    tv[0] = (uint32_t)(us / 1000000u);
    tv[1] = 0;
    tv[2] = (uint32_t)(us % 1000000u);
    return 0;
}

static int64_t timer_get_time(void)
{
    return (int64_t)osi_now_us();
}

/* The slow clock's period in microseconds, as Q13.19: about 136 kHz, the RC oscillator's; not measured. */
static uint32_t slowclk_cal_get(void)
{
    uint32_t hz = 3855000u;
    osi_trace("slowclk", hz, 0);
    return hz;
}

static int read_mac(uint8_t *mac, unsigned int type)
{
    osi_trace("read_mac", type, 0);
    memcpy(mac, osi.d->mac, 6);
    mac[5] += (uint8_t)type; /* ESP-IDF derives the soft-AP's and the rest from the station's this way */
    return 0;
}

/* The libraries' lines that reach the log, at ESP_LOG_INFO and below unless the configuration asks for more. */
static unsigned int log_level = 3;

void osi_log_level(unsigned int level)
{
    log_level = level;
}

/* The libraries write a line in pieces, the first of which carries ESP-IDF's level, time and tag. */
static void esp_log_writev(unsigned int level, const char *tag, const char *fmt, va_list args)
{
    (void)tag;
    if (level <= log_level) {
        drv_vsay(0, fmt, args);
    }
}

static void esp_log_write(unsigned int level, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    esp_log_writev(level, tag, fmt, args);
    va_end(args);
}

static uint32_t log_timestamp(void)
{
    return (uint32_t)(osi_now_us() / 1000u);
}

static void nothing(void)
{
}

static int zero(void)
{
    return 0;
}

static int coex_log_init(void)
{
    osi_trace("coex_init", 0, 0);
    return 0;
}

static int coex_log_enable(void)
{
    osi_trace("coex_enable", 0, 0);
    return 0;
}

static int nvs_none(void)
{
    return 0x1102; /* ESP_ERR_NVS_NOT_FOUND */
}

/*
 * The two coex stubs that have an output. ESP-IDF's adapter without coexistence returns success and writes
 * nothing, so the libraries program the byte or word as they find it -- uninitialized stack, which shifts with
 * the image's layout. These instead write what the coexistence library would, so the register is the same
 * whatever the image. The PTI is the value the chip expects: esp-coex-lib's coex_pti_tab
 * (espressif/esp-coex-lib, c758e7b56e0fa22177a0539796e1df59978dc322, esp32c6/libcoexist.a), which the ROM's
 * coex_core_pti_get reads as tab[event]; a wrong PTI there costs the reception. esp-coex-lib is Apache-2.0, as
 * the rest of the libraries. The cast the other stubs use would hide a missing write, so these are typed.
 */
static const uint8_t coex_pti_tab[49] = {
    0x0a, 0x05, 0x07, 0x07, 0x0a, 0x01, 0x01, 0x01, 0x01, 0x07, 0x03, 0x02, 0x01, 0x01, 0x01, 0x01,
    0x04, 0x09, 0x04, 0x04, 0x09, 0x04, 0x09, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04,
    0x04, 0x02, 0x02, 0x02, 0x0f, 0x0a, 0x04, 0x0e, 0x00, 0x0c, 0x08, 0x03, 0x01, 0x0a, 0x0a, 0x0f,
    0x0f,
};

static int coex_pti_get(uint32_t event, uint8_t *pti)
{
    *pti = event < sizeof(coex_pti_tab) ? coex_pti_tab[event] : 0;
    return 0;
}

static int coex_event_duration_get(uint32_t event, uint32_t *duration)
{
    /* esp-coex-lib's coex_core_event_duration_get: a duration in microseconds per event, and -1 for the rest. */
    switch (event) {
    case 4:    *duration = 25000; return 0;
    case 7:    *duration = 20000; return 0;
    case 9:    *duration = 5000;  return 0;
    case 0x2d: *duration = 25000; return 0;
    case 0x2e: *duration = 50000; return 0;
    default:   *duration = 0;     return -1;
    }
}

static void *null(void)
{
    return 0;
}

static bool no(void)
{
    return false;
}

/* A stub of another type than its field's, which takes and returns no more than the field's type does. */
#define STUB(field, f) .field = (__typeof__(((struct osi_funcs *)0)->field))(void (*)(void))(f)

static const struct osi_funcs funcs = {
    .version = OSI_VERSION,
    .env_is_chip = env_is_chip,
    .set_intr = set_intr,
    .clear_intr = clear_intr,
    .set_isr = set_isr,
    .ints_on = ints_on,
    .ints_off = ints_off,
    .is_from_isr = is_from_isr,
    .spin_lock_create = spin_lock_create,
    .spin_lock_delete = spin_lock_delete,
    .wifi_int_disable = int_disable,
    .wifi_int_restore = int_restore,
    .task_yield_from_isr = task_yield_from_isr,
    .semphr_create = semphr_create,
    .semphr_delete = semphr_delete,
    .semphr_take = semphr_take,
    .semphr_give = semphr_give,
    .wifi_thread_semphr_get = wifi_thread_semphr_get,
    .mutex_create = mutex_create,
    .recursive_mutex_create = recursive_mutex_create,
    .mutex_delete = osi_free,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .queue_create = queue_create,
    .queue_delete = queue_delete,
    .queue_send = queue_send,
    .queue_send_from_isr = queue_send_from_isr,
    .queue_send_to_back = queue_send,
    .queue_send_to_front = queue_send_to_front,
    .queue_recv = queue_recv,
    .queue_msg_waiting = queue_msg_waiting,
    .event_group_create = event_group_create,
    .event_group_delete = event_group_delete,
    .event_group_set_bits = event_group_set_bits,
    .event_group_clear_bits = event_group_clear_bits,
    .event_group_wait_bits = event_group_wait_bits,
    .task_create_pinned_to_core = task_create_pinned_to_core,
    .task_create = task_create,
    .task_delete = task_delete,
    .task_delay = task_delay,
    .task_ms_to_tick = task_ms_to_tick,
    .task_get_current_task = task_get_current_task,
    .task_get_max_priority = task_get_max_priority,
    .malloc = osi_malloc,
    .free = osi_free,
    .event_post = event_post,
    .get_free_heap_size = get_free_heap_size,
    .rand = osi_rand,
    .dport_access_stall_other_cpu_start_wrap = nothing,
    .dport_access_stall_other_cpu_end_wrap = nothing,
    .wifi_pm_sleep_lock_acquire = nothing,
    .wifi_pm_sleep_lock_release = nothing,
    .phy_disable = drv_phy_disable,
    .phy_enable = drv_phy_enable,
    STUB(phy_update_country_info, zero),
    .read_mac = read_mac,
    .timer_arm = timer_arm,
    .timer_disarm = timer_disarm,
    .timer_done = timer_done,
    .timer_setfn = timer_setfn,
    .timer_arm_us = timer_arm_us,
    .wifi_reset_mac = drv_wifi_reset_mac,
    .wifi_clock_enable = drv_wifi_clock_enable,
    .wifi_clock_disable = nothing,
    .wifi_rtc_enable_iso = nothing,
    .wifi_rtc_disable_iso = nothing,
    .esp_timer_get_time = timer_get_time,
    STUB(nvs_set_i8, nvs_none),
    STUB(nvs_get_i8, nvs_none),
    STUB(nvs_set_u8, nvs_none),
    STUB(nvs_get_u8, nvs_none),
    STUB(nvs_set_u16, nvs_none),
    STUB(nvs_get_u16, nvs_none),
    STUB(nvs_open, nvs_none),
    STUB(nvs_close, nothing),
    STUB(nvs_commit, nvs_none),
    STUB(nvs_set_blob, nvs_none),
    STUB(nvs_get_blob, nvs_none),
    STUB(nvs_erase_key, nvs_none),
    .get_random = get_random,
    .get_time = get_time,
    .random = osi_random,
    .slowclk_cal_get = slowclk_cal_get,
    .log_write = esp_log_write,
    .log_writev = esp_log_writev,
    .log_timestamp = log_timestamp,
    .malloc_internal = osi_malloc,
    .realloc_internal = osi_realloc,
    .calloc_internal = osi_calloc,
    .zalloc_internal = zalloc,
    .wifi_malloc = osi_malloc,
    .wifi_realloc = osi_realloc,
    .wifi_calloc = osi_calloc,
    .wifi_zalloc = zalloc,
    .wifi_create_queue = wifi_create_queue,
    .wifi_delete_queue = wifi_delete_queue,
    .coex_init = coex_log_init,
    .coex_deinit = nothing,
    .coex_enable = coex_log_enable,
    .coex_disable = nothing,
    STUB(coex_status_get, zero),
    STUB(coex_condition_set, nothing),
    STUB(coex_wifi_request, zero),
    STUB(coex_wifi_release, zero),
    STUB(coex_wifi_channel_set, zero),
    .coex_event_duration_get = coex_event_duration_get,
    .coex_pti_get = coex_pti_get,
    STUB(coex_schm_status_bit_clear, nothing),
    STUB(coex_schm_status_bit_set, nothing),
    STUB(coex_schm_interval_set, zero),
    STUB(coex_schm_interval_get, zero),
    STUB(coex_schm_curr_period_get, zero),
    .coex_schm_curr_phase_get = null,
    .coex_schm_process_restart = zero,
    STUB(coex_schm_register_cb, zero),
    STUB(coex_register_start_cb, zero),
    STUB(regdma_link_set_write_wait_content, nothing),
    STUB(sleep_retention_find_link_by_id, null),
    STUB(coex_schm_flexible_period_set, zero),
    STUB(coex_schm_flexible_period_get, zero),
    STUB(coex_schm_get_phase_by_idx, null),
    .wifi_disable_ac_ax = no,
    STUB(wifi_bb_sleep_retention_attach, zero),
    STUB(wifi_bb_sleep_retention_detach, zero),
    STUB(wifi_mac_sleep_retention_attach, zero),
    STUB(wifi_mac_sleep_retention_detach, zero),
    .magic = OSI_MAGIC,
};

struct osi_funcs *osi_init(struct drv *d, struct self *s)
{
    osi.d = d;
    osi.s = s;
    uint32_t big, crit, heap, status;
    if ((status = slot_new(s, &big)) != KERR_OK || (status = rv_pool_alloc(s->pool, CAP_NOTIFICATION, big, 0)) ||
        (status = slot_new(s, &crit)) != KERR_OK || (status = rv_pool_alloc(s->pool, CAP_NOTIFICATION, crit, 0)) ||
        (status = slot_new(s, &heap)) != KERR_OK || (status = rv_pool_alloc(s->pool, CAP_NOTIFICATION, heap, 0))) {
        drv_failed("the adapter's locks", status);
        return 0;
    }
    osi.big = (struct lock){ (uint32_t)(uintptr_t)&osi.big_word, big };
    osi.crit = (struct lock){ (uint32_t)(uintptr_t)&osi.crit_word, crit };
    osi.heap = (struct lock){ (uint32_t)(uintptr_t)&osi.heap_word, heap };
    lock_init(&osi.big);
    lock_init(&osi.crit);
    lock_init(&osi.heap);
    osi_heap_init(&osi.heap);
    /* The watcher first, which the root task watches, then the threads it watches. */
    osi.watcher = osi_thread(watcher_main, 0, "watcher", 1536);
    /* 3 KiB, for mac.c's handler reads beacons on it, hostap's parsed elements taking 536 bytes. */
    osi.isr = osi_thread(isr_main, 0, "isr", 3072);
    osi.timers = osi_thread(timers_main, 0, "timers", 3072);
    return osi.watcher && osi.isr && osi.timers ? (struct osi_funcs *)&funcs : 0;
}
