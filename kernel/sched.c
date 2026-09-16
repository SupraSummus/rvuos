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
    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        for (struct obj_header *o = pool_first(p); o != NULL; o = pool_next(p, o)) {
            if (o->type != CAP_THREAD) {
                continue;
            }
            struct thread *t = (struct thread *)o;
            if (t != current && t->state == state && t->waiting_on == waiting_on) {
                return t;
            }
        }
    }
    return NULL;
}

struct thread *sched_waiter(paddr_t notification)
{
    return find_thread(THREAD_WAITING, notification);
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
