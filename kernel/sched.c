/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "kernel.h"
#include "object.h"

struct thread *current;

/*
 * The kernel keeps no queues,
 * so every question it asks about threads is answered
 * by walking every object, as the overlap checks are.
 */
struct thread *sched_waiter(paddr_t notification)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_THREAD) {
            continue;
        }
        struct thread *t = (struct thread *)o;
        if (t->state == THREAD_WAITING && t->waiting_on == notification) {
            return t;
        }
    }
    return NULL;
}

/*
 * The first runnable thread after `from` in the walk, wrapping around,
 * so that `from` itself comes last.
 * This is the whole scheduling policy: round-robin in allocation order,
 * with no state beyond the position of the running thread.
 * NULL when no thread at all is runnable.
 */
static struct thread *runnable_after(struct thread *from)
{
    struct obj_header *o = object_next(&from->hdr);
    for (;;) {
        if (o == NULL) {
            o = object_first();
        }
        if (o->type == CAP_THREAD && ((struct thread *)o)->state == THREAD_READY) {
            return (struct thread *)o;
        }
        if (o == &from->hdr) {
            return NULL;
        }
        o = object_next(o);
    }
}

void sched_unblock_range(uint32_t base, uint32_t size)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_THREAD) {
            continue;
        }
        struct thread *t = (struct thread *)o;
        if (t->state != THREAD_WAITING || !range_contains(base, size, t->waiting_on)) {
            continue;
        }
        /*
         * The notification is gone, so the wait cannot be answered.
         * The thread learns that the way every other call learns it.
         */
        t->frame.regs[REG_A0] = KERR_INVALID_CAP;
        t->frame.regs[REG_A1] = 0;
        t->waiting_on = 0;
        t->state = THREAD_READY;
    }
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
}

void sched_run_next(void)
{
    struct thread *next = runnable_after(current);
    if (next == NULL) {
        /*
         * Only a running thread can make another one runnable:
         * the tick wakes nobody and device interrupts do not exist yet,
         * so nothing can change this. Say so and stop.
         */
        kputs("no runnable thread\n");
        khalt(5);
    }
    switch_to(next);
}

void sched_tick(void)
{
    /* The host build cannot reproduce where a preemption lands; see DESIGN.md, "Verification". */
    if (debug_trace) {
        return;
    }
    struct thread *next = runnable_after(current);
    /* The running thread is ready, so there is always at least itself. */
    if (next != current) {
        switch_to(next);
    }
}
