/*
 * What runs next.
 * See DESIGN.md, "Scheduling".
 */

#include "kernel.h"
#include "object.h"

struct thread *current;

/*
 * The kernel keeps no queues,
 * so both questions it can ask about threads are answered
 * by walking every object, as the overlap checks are.
 * A thread that is not waiting waits on nothing,
 * so asking for THREAD_READY and 0 asks for a runnable thread.
 * The order is the order objects were allocated in,
 * which is not a scheduling policy and is not meant to be one;
 * priorities arrive with the timer in roadmap step 5.
 */
static struct thread *find_thread(uint8_t state, paddr_t waiting_on)
{
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        if (o->type != CAP_THREAD) {
            continue;
        }
        struct thread *t = (struct thread *)o;
        if (t != current && t->state == state && t->waiting_on == waiting_on) {
            return t;
        }
    }
    return NULL;
}

struct thread *sched_waiter(paddr_t notification)
{
    return find_thread(THREAD_WAITING, notification);
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

void sched_run_next(void)
{
    struct thread *next = find_thread(THREAD_READY, 0);
    if (next == NULL) {
        /*
         * Only a running thread can make another one runnable,
         * and interrupts do not exist yet,
         * so nothing can change this. Say so and stop.
         */
        kputs("no runnable thread\n");
        khalt(5);
    }
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
