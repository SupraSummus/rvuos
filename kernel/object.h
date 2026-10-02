#ifndef RVUOS_OBJECT_H
#define RVUOS_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rvuos/abi.h"
#include "kernel.h"
#include "paddr.h"
#include "pmp.h"
#include "timer.h"
#include "trap.h"

/*
 * Kernel objects.
 *
 * Every object starts with obj_header and lives inside a pool.
 * Objects are never freed individually;
 * a pool is destroyed as a whole, and the destroy clears every capability
 * that named an object in it, so nothing survives to dangle.
 * See DESIGN.md, "Kernel pools and revocation".
 *
 * Objects link to each other by physical address, never by pointer,
 * so their layout is the same in the host build as on the target
 * and the two allocate identically.
 * The static asserts below pin the sizes.
 */
struct obj_header {
    uint8_t type;       /* CAP_* */
    uint8_t pad;
    uint16_t back;      /* how far the object before lies, in OBJ_ALIGN units; 0 for a descriptor */
    paddr_t pool;
};

/*
 * A capability slot, and a node of the derivation tree; see DESIGN.md, "The derivation tree".
 * For CAP_FRAME and CAP_IRQ_LINE the slot is self-contained:
 * a is the base address or first line and b the size or count,
 * and there is no object.
 * For CAP_UNTYPED a is the block, its base and size in one word as NAPOT encodes them,
 * see untyped_base, and b is 0; there is no object either, and whether it is free the tree says.
 * For every other type a is the object's physical address.
 *
 * child is the first slot derived from this one, by physical address, 0 if none.
 * next is the next sibling, or at the last sibling the parent with LINK_UP set,
 * which the four-byte alignment of slots leaves free;
 * a root's next is LINK_UP alone, an empty slot's is 0.
 * prev is the previous sibling, and at the first sibling the last one,
 * so that a node leaves its ring without a walk;
 * an only child's is itself, and a root's and an empty slot's is 0.
 * A process's region slots are slots of this type too, CAP_INSTALLED, and leaves of the tree,
 * and so is its table slot, which holds a CAP_CAPTABLE capability,
 * and a thread's process, CAP_HOSTED, its units, CAP_BOUND, and its watch, CAP_WATCHED,
 * and an Irq's notification, CAP_SIGNALLED,
 * and so is a pool's own node, CAP_RETYPED, in the pool's descriptor.
 * For CAP_TIME a is the first unit and b the count, as for CAP_IRQ_LINE.
 */
struct cap {
    uint8_t type;
    uint8_t rights;
    uint8_t index;      /* an installed region's slot in its process; 0 elsewhere */
    uint8_t pad;
    uint32_t a;
    uint32_t b;
    uint32_t child;
    uint32_t next;
    uint32_t prev;
};

#define LINK_UP 0x1u

/* A region installed in a process's region slot. Kernel internal: never in a table. */
#define CAP_INSTALLED 0x80
/*
 * A pool's own node, in its descriptor: what the retype made, below the Untyped.
 * Every capability to the pool and to its objects lies below it.
 * a is the pool's address, as for a capability to an object. Kernel internal: never in a table.
 */
#define CAP_RETYPED 0x81
/*
 * The units of time a thread earns, in the thread: a is the first and b how many, none at all among them,
 * and the rights, spare time with RIGHT_X, are those of the Time capability the thread was bound through,
 * below which the node hangs. Kernel internal: never in a table.
 */
#define CAP_BOUND 0x82
/*
 * A thread's process, in the thread: a is the process, and the node hangs below
 * the Process capability the thread was made with. Kernel internal: never in a table.
 */
#define CAP_HOSTED 0x83
/*
 * An Irq's notification, in the Irq: a is the notification, and the node hangs below
 * the Notification capability the Irq was bound with. Kernel internal: never in a table.
 */
#define CAP_SIGNALLED 0x84
/*
 * A thread's watch, in the thread: a is the notification its faults signal and b the bits, never none,
 * and the node hangs below the Notification capability the watch was set with. Kernel internal: never in a table.
 */
#define CAP_WATCHED 0x85

#define CAPTABLE_MAX_SLOTS 1024

struct captable {
    struct obj_header hdr;
    uint32_t nslots;
    struct cap slots[];
};

/*
 * A pool's descriptor is the first object in its own memory,
 * so the kernel keeps no state about pools outside them
 * except the list head, which only the self-check and the host walk.
 * Objects are bump-allocated behind it;
 * obj_size() recovers each object's length from its type, and its back field the one before,
 * so the destroy takes them newest first and the used mark shrinks as it goes:
 * a pool half destroyed is a pool with fewer objects.
 * See DESIGN.md, "Kernel pools and revocation".
 */
struct pool {
    struct obj_header hdr;
    struct cap node;    /* CAP_RETYPED: below the Untyped the pool was retyped from */
    uint32_t size;
    uint32_t used;      /* bytes handed out, descriptor included */
    paddr_t last;       /* the newest object, the descriptor itself when there is none */
    paddr_t next;       /* list of pools, newest first, 0 at the end */
    paddr_t prev;       /* 0 at the head */
    uint16_t sweep;     /* while dying: the next slot of the newest object the destroy clears */
    uint8_t dying;      /* set by the first step of a destroy; nothing is allocated from then on */
    uint8_t pad;
};

static inline paddr_t pool_base(const struct pool *p) { return p->hdr.pool; }

struct pmp_image {
    unsigned count;
    uint32_t addr[PMP_MAX_ENTRIES];
    uint8_t cfg[PMP_MAX_ENTRIES];
};

/*
 * A process holds its table by a capability, not a pointer:
 * a node of the derivation tree below the capability the process was made with,
 * which a revoke or the destroy of the table's pool clears.
 * See DESIGN.md, "A process's table".
 */
struct process {
    struct obj_header hdr;
    struct cap table;   /* CAP_CAPTABLE, or CAP_NONE once the table is taken */
    struct cap slots[PROCESS_REGION_SLOTS]; /* CAP_INSTALLED, or CAP_NONE when empty */
    struct pmp_image pmp;
};

/*
 * Thread states.
 * A ready thread other than the running one is on the queue its account puts it on, if it may run,
 * and a waiting one on its notification's waiters,
 * so the kernel finds the next thread to run without a walk;
 * see DESIGN.md, "Scheduling".
 * A thread bound to no units keeps its state and does not run.
 * A thread without a process is stopped, and stays so.
 * A thread that faults stops, and its watch hears it; see DESIGN.md, "Faults".
 */
enum {
    THREAD_STOPPED = 0, /* created, configured and not started, or stopped where it faulted */
    THREAD_READY,       /* running, or waiting for the processor */
    THREAD_WAITING,     /* blocked on the notification in waiting_on */
};

/*
 * A thread whose code the host build cannot follow.
 * The host has no instruction fetch,
 * so a differential replay can only cross threads
 * whose program counter was fixed before tracing began;
 * see DESIGN.md, "Verification".
 */
#define THREAD_UNTRACED 0x1
/*
 * A thread stopped where it faulted, whose frame holds what the fault left, until a resume or a configure;
 * see OP_THREAD_FAULT.
 * A new thread's frame is zero, which is a cause too, so only this tells the two apart.
 */
#define THREAD_FAULTED 0x2

/* A thread holds its process as a process holds its table, so the two may lie in different pools. */
struct thread {
    struct obj_header hdr;
    uint8_t state;
    uint8_t flags;
    uint8_t queue;      /* the scheduler's queue it waits on, one of QUEUE_*, while READY and not running */
    uint8_t core;       /* the core it runs and waits on, that of its units; see thread_settle */
    paddr_t waiting_on; /* the notification, while WAITING; 0 otherwise */
    /* The notification's waiters while WAITING, the scheduler's queue while it waits on one. */
    paddr_t queue_next;
    paddr_t queue_prev;
    struct cap proc;    /* CAP_HOSTED, or CAP_NONE once the process is taken */
    struct cap time;    /* CAP_BOUND, or CAP_NONE while the thread is bound to no units */
    struct cap watch;   /* CAP_WATCHED, or CAP_NONE while nothing hears the thread's faults */
    /*
     * Its account at the tick stamp, in COUNT_PARTS parts of a count,
     * at most ACCOUNT_TICKS ticks' counts of parts for each unit it earns, and so none while it earns none.
     * It is brought up to the count only when the kernel looks at it,
     * and the charges of a turn in the tick it was counted to are taken off it as that turn ends.
     */
    uint32_t balance;
    uint32_t stamp;
    struct trap_frame frame;
};

/*
 * A notification is a word of sticky bits, and the only way
 * a thread can stop running and be started again.
 * Data does not travel through it:
 * two processes that share a region move bytes through the region
 * and use the notification to say when.
 *
 * Its waiters are a ring through their threads, oldest first,
 * so a signal finds one without a walk; see DESIGN.md, "Bounded work".
 */
struct notification {
    struct obj_header hdr;
    uint32_t bits;
    paddr_t waiters;    /* the oldest waiting thread, 0 for none */
};

/*
 * An Irq signals a notification when its interrupt line fires:
 * bound at creation to a notification in any pool,
 * armed with the bits it will signal, and disarmed by signalling.
 * It holds the notification as a thread holds its process, so the two may lie in different pools,
 * and one whose notification is taken is disarmed and stays so, its line masked and still bound.
 * On a line of the controller, disarmed means masked there:
 * the line stays quiet until the driver arms the Irq again,
 * which it does once it has serviced the device.
 * One Irq per line; see DESIGN.md, "Interrupts".
 * Line LOG_IRQ_LINE is the kernel's log, which klog.c raises and masks
 * with no controller behind it; see DESIGN.md, "The kernel log".
 * The timer lines have no controller either: the tick raises one
 * once the tick count reaches its deadline, which the set that armed it wrote,
 * so time reaches userspace as an interrupt like any other; see DESIGN.md, "Time".
 */
struct irq {
    struct obj_header hdr;
    struct cap ntfn;    /* CAP_SIGNALLED, or CAP_NONE once the notification is taken */
    uint32_t bits;
    uint16_t line;
    uint8_t core;       /* on a timer line, the core that armed it, whose timer wakes for it */
    uint8_t pad;
    uint32_t deadline;  /* on a timer line, the tick count it fires at */
};

_Static_assert(sizeof(struct obj_header) == 8, "object layout");
_Static_assert(sizeof(struct cap) == 24, "object layout");
_Static_assert(sizeof(struct captable) == 12, "object layout");
_Static_assert(sizeof(struct pool) == 56, "object layout");
_Static_assert(sizeof(struct pmp_image) == 4 + 5 * PMP_MAX_ENTRIES, "object layout");
_Static_assert(sizeof(struct process) == 8 + 24 * (1 + PROCESS_REGION_SLOTS) + sizeof(struct pmp_image),
               "object layout");
_Static_assert(sizeof(struct thread) == 32 + 3 * sizeof(struct cap) + sizeof(struct trap_frame), "object layout");
_Static_assert(sizeof(struct notification) == 16, "object layout");
_Static_assert(sizeof(struct irq) == 20 + sizeof(struct cap), "object layout");

/*
 * True if a capability of this type names a kernel object.
 * Frames, Untypeds, interrupt lines, units of time and the clock name hardware, and the debug capability nothing,
 * so they carry no address to check.
 */
static inline bool cap_has_object(uint8_t type)
{
    return type != CAP_NONE && type != CAP_FRAME && type != CAP_UNTYPED && type != CAP_IRQ_LINE &&
           type != CAP_DEBUG && type != CAP_CLOCK && type != CAP_INSTALLED && type != CAP_TIME &&
           type != CAP_BOUND;
}

/*
 * An Untyped's block in one word, as pmpaddr encodes NAPOT but in bytes:
 * the base with the bits below half the size set.
 * Every block is at least eight bytes, so the word always ends in a one.
 */
static inline uint32_t untyped_block(uint32_t base, uint32_t size) { return base | (size / 2 - 1); }
static inline uint32_t untyped_size(const struct cap *c) { return (c->a ^ (c->a + 1)) + 1; }
static inline uint32_t untyped_base(const struct cap *c) { return c->a & ~(untyped_size(c) - 1); }
/* Whether an Untyped makes something: only while nothing made of it is left below it. */
static inline bool untyped_free(const struct cap *c) { return c->child == 0; }

static inline struct pool *obj_pool(const struct obj_header *o) { return p2v(o->pool); }

_Static_assert(offsetof(struct process, slots) == offsetof(struct process, table) + sizeof(struct cap),
               "a process's table slot and region slots are one array of nodes");
_Static_assert(offsetof(struct thread, time) == offsetof(struct thread, proc) + sizeof(struct cap) &&
                   offsetof(struct thread, watch) == offsetof(struct thread, time) + sizeof(struct cap),
               "a thread's process, units and watch are one array of nodes");

/*
 * The nodes of the derivation tree an object holds, as one array, and how many:
 * a table's slots, a process's table slot and region slots, a thread's process, units and watch,
 * an Irq's notification, a pool's own node.
 * Other objects hold none.
 */
static inline struct cap *obj_nodes(struct obj_header *o, uint32_t *count)
{
    switch (o->type) {
    case CAP_CAPTABLE:
        *count = ((struct captable *)o)->nslots;
        return ((struct captable *)o)->slots;
    case CAP_PROCESS:
        *count = 1 + PROCESS_REGION_SLOTS;
        return &((struct process *)o)->table;
    case CAP_THREAD:
        *count = 3;
        return &((struct thread *)o)->proc;
    case CAP_IRQ:
        *count = 1;
        return &((struct irq *)o)->ntfn;
    case CAP_POOL:
        *count = 1;
        return &((struct pool *)o)->node;
    default:
        *count = 0;
        return NULL;
    }
}
static inline struct pool *pool_next_pool(const struct pool *p) { return p->next ? p2v(p->next) : NULL; }
/* The table a process names capabilities in, or NULL once it was taken. The type test is cap_lookup's. */
static inline struct captable *process_table(const struct process *p)
{
    if (p->table.type != CAP_CAPTABLE) {
        return NULL;
    }
    struct captable *t = p2v(p->table.a);
    return t->hdr.type == CAP_CAPTABLE ? t : NULL;
}
/* The process a thread runs in, or NULL once it was taken. */
static inline struct process *thread_process(const struct thread *t)
{
    return t->proc.type == CAP_HOSTED ? p2v(t->proc.a) : NULL;
}
/* The thread whose process this is. */
static inline struct thread *hosted_thread(struct cap *n)
{
    return (struct thread *)((char *)n - offsetof(struct thread, proc));
}
/* The thread whose units these are. */
static inline struct thread *bound_thread(struct cap *n)
{
    return (struct thread *)((char *)n - offsetof(struct thread, time));
}
/* The table a thread names capabilities in, or NULL once its process or the process's table was taken. */
static inline struct captable *thread_table(const struct thread *t)
{
    const struct process *p = thread_process(t);
    return p != NULL ? process_table(p) : NULL;
}
/* The notification a thread's faults signal, or NULL while it has no watch. */
static inline struct notification *thread_watch(const struct thread *t)
{
    return t->watch.type == CAP_WATCHED ? p2v(t->watch.a) : NULL;
}
/* The thread whose watch this is. */
static inline struct thread *watched_thread(struct cap *n)
{
    return (struct thread *)((char *)n - offsetof(struct thread, watch));
}
/* The notification an Irq signals, or NULL once it was taken. */
static inline struct notification *irq_notification(const struct irq *i)
{
    return i->ntfn.type == CAP_SIGNALLED ? p2v(i->ntfn.a) : NULL;
}
/* The Irq whose notification this is. */
static inline struct irq *signalled_irq(struct cap *n)
{
    return (struct irq *)((char *)n - offsetof(struct irq, ntfn));
}

/* True if the Irq will signal; a controller's line is unmasked exactly then. */
static inline bool irq_armed(const struct irq *i) { return i->bits != 0; }

/* True if an Irq on a timer line has its deadline at or before the tick count now. */
static inline bool irq_due(const struct irq *i, uint32_t now)
{
    return (int32_t)(i->deadline - now) <= 0;
}

#define OBJ_ALIGN 8

/* The largest object, a table of the most slots; pool_alloc refuses a larger one. */
#define OBJ_MAX_SIZE (sizeof(struct captable) + CAPTABLE_MAX_SLOTS * sizeof(struct cap))

/* Both are correct even if a range ends at the top of the address space. */
static inline bool ranges_overlap(uint32_t a, uint32_t asize, uint32_t b, uint32_t bsize)
{
    return (a - b) < bsize || (b - a) < asize;
}

static inline bool range_contains(uint32_t base, uint32_t size, uint32_t at)
{
    return at - base < size;
}

/* RIGHT_* bits to pmpcfg permission bits. */
static inline uint8_t rights_to_pmp(uint8_t rights)
{
    uint8_t cfg = 0;
    if (rights & RIGHT_R) cfg |= PMP_R;
    if (rights & RIGHT_W) cfg |= PMP_W;
    if (rights & RIGHT_X) cfg |= PMP_X;
    return cfg;
}

/* pool.c */
extern struct pool *pool_list;

/*
 * Turn a range into a pool whose node hangs below parent, NULL for a root.
 * The range must be OBJ_ALIGN aligned and large enough for the descriptor.
 */
struct pool *pool_create(paddr_t base, uint32_t size, struct cap *parent);

/* The pool whose own node this is. */
static inline struct pool *node_pool(struct cap *n)
{
    return (struct pool *)((char *)n - offsetof(struct pool, node));
}

/*
 * Destroy a pool, as a revoke below an Untyped does when it meets the pool's node:
 * revoke below that node,
 * then take its objects newest first, clearing the nodes they hold as cap_delete does,
 * undoing what each left in the rest of the kernel and zeroing it;
 * then clear the pool's node and zero the descriptor.
 * With preempt it may stop between two steps for a pending interrupt and return false,
 * the pool dying and the rest of the work kept in it; see DESIGN.md, "Bounded work".
 * The caller has checked that the running thread and its table do not live in the pool.
 */
bool pool_destroy(struct pool *pool, bool preempt);

/* Whether pool_alloc would find room for size bytes. */
bool pool_fits(const struct pool *pool, size_t size);

/* Allocate a zeroed object of the given type and size. NULL if exhausted, or larger than OBJ_MAX_SIZE. */
void *pool_alloc(struct pool *pool, uint8_t type, size_t size);

/* Byte length of an object, from its header; inline, so the self-check's walks have a copy of their own. */
static inline size_t obj_size(const struct obj_header *obj)
{
    switch (obj->type) {
    case CAP_POOL:
        return sizeof(struct pool);
    case CAP_CAPTABLE: {
        const struct captable *t = (const struct captable *)obj;
        return sizeof(*t) + t->nslots * sizeof(struct cap);
    }
    case CAP_PROCESS:
        return sizeof(struct process);
    case CAP_THREAD:
        return sizeof(struct thread);
    case CAP_NOTIFICATION:
        return sizeof(struct notification);
    case CAP_IRQ:
        return sizeof(struct irq);
    default:
        kpanic("object of unknown type in pool");
    }
}

/* cap.c */

/*
 * Resolve a slot in a table.
 * Returns KERR_OK and fills *out on success,
 * or KERR_INVALID_CAP for an empty, out-of-range, or stale slot.
 */
int cap_lookup(struct captable *table, uint32_t slot, struct cap *out);

/* Resolve a slot and check its type and required rights. */
int cap_lookup_typed(struct captable *table, uint32_t slot,
                     uint8_t type, uint8_t rights, struct cap *out);

/* KERR_OK if the slot exists and is empty, else KERR_INVALID_CAP or KERR_SLOT_IN_USE. */
int cap_slot_free(const struct captable *table, uint32_t slot);

/*
 * Store into an empty slot as a child of parent, or as a root when parent is NULL;
 * fails as cap_slot_free does.
 * The links the caller's copy carries are ignored.
 */
int cap_store(struct captable *table, uint32_t slot, const struct cap *cap, struct cap *parent);

/* Store into an empty slot as a sibling of beside: derived from the same parent. */
int cap_store_beside(struct captable *table, uint32_t slot, const struct cap *cap, struct cap *beside);

/* Make an already filled node, an installed region, a child of parent, or a root when parent is NULL. */
void cap_attach(struct cap *parent, struct cap *node);

/*
 * Move a filled table slot into an empty one, from and to distinct:
 * the new node has the parent, the siblings and the children the old one had,
 * and the old one is left empty. Constant time.
 */
void cap_move(struct cap *from, struct cap *to);


/*
 * Clear a slot. KERR_INVALID_CAP if out of range.
 * What was derived from it is adopted by its parent; clearing an empty slot does nothing.
 * Preemptible as cap_delete is: KERR_PREEMPTED when it stopped.
 */
int cap_clear(struct captable *table, uint32_t slot);

/*
 * Clear a node; what was derived from it is adopted by its parent,
 * or, when the node is a root, each child becomes a root, a step per child.
 * With preempt it may stop between two steps for a pending interrupt and return false,
 * the tree whole and the rest of the work still in it; see DESIGN.md, "Bounded work".
 */
bool cap_delete(struct cap *node, bool preempt);

/*
 * Clear every node below one, leaving the node itself; preemptible as cap_delete is.
 * A pool's own node below it is a pool to destroy, which pool_destroy does.
 * It ends early, and true, once it took the running thread's table or process,
 * or through, the slot the call was made through.
 * The caller has checked that no pool it destroys takes any of the three.
 */
bool cap_revoke_below(struct cap *node, const struct cap *through, bool preempt);

/*
 * One step of a revoke below a pool's own node, where no other pool's node lies:
 * clear the first node below root, whose children take its place.
 * False when nothing is left.
 */
bool cap_revoke_step(struct cap *root);

/* Whether a preemptible walk that has done a step stops before its next; see intr_pending and OP_DEBUG_PREEMPT. */
bool cap_stop_here(bool preempt);

/*
 * OP_DEBUG_PREEMPT's count of the places a walk may stop before the one it stops at, 0 when disarmed;
 * the walk that stops there marks its core's preempt_stopped.
 */
extern uint32_t preempt_countdown;

/* Build a capability to an object. */
struct cap cap_to_object(struct obj_header *obj, uint8_t rights);

/* Build a frame capability. */
struct cap cap_to_frame(uint32_t base, uint32_t size, uint8_t rights);

/* Build an Untyped capability, over the whole of a block. */
struct cap cap_to_untyped(uint32_t base, uint32_t size, uint8_t rights);

/* Build an interrupt line capability: count lines from first. */
struct cap cap_to_lines(uint32_t first, uint32_t count, uint8_t rights);

/* Build a Time capability: count units from first. */
struct cap cap_to_time(uint32_t first, uint32_t count, uint8_t rights);

/* The object behind a capability that names one. */
static inline struct obj_header *cap_object(const struct cap *cap)
{
    return p2v(cap->a);
}

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
     * The tick the core's timer has to interrupt at for the timer lines armed on it, see struct irq:
     * the nearest deadline of one, or NEAREST_NONE ticks ahead of the count with none.
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
 * Set bits on a notification and wake a thread waiting on it, if any.
 * Never blocks; OP_NOTIFY_SIGNAL and a firing Irq are both this.
 */
void sched_signal(struct notification *ntfn, uint32_t bits);

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
 * Before an object of a pool that goes is zeroed, undo what it left in the rest of the kernel:
 * a notification wakes every thread waiting on it
 * with KERR_INVALID_CAP and no bits, because the object is gone,
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

/* process.c */

/* Install a region into a slot, as a node below parent, or a root when parent is NULL. */
int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights, struct cap *parent);
/* Clear a region slot, which leaves the derivation tree as cap_clear does. */
int process_uninstall(struct process *proc, unsigned slot);
/* Clear an installed region the tree has already let go of, and rebuild the process's PMP image. */
void process_drop(struct cap *installed);

/* Fence off, once at boot, the PMP entries the core hardwires open to user mode. */
void process_fence(void);
/* Fence the same entries off on a core other than the first, as it starts; the cores are alike. */
void process_fence_core(void);
/* Load a process's PMP image into the CSRs. */
void process_activate(struct process *proc);

/* syscall.c */
void syscall_dispatch(struct thread *t);
/*
 * The running thread faulted: say so in the log, stop it where it faulted, signal its watch if it has one,
 * and hand the processor on; see DESIGN.md, "Faults".
 */
void fault_dispatch(struct thread *t);

/* selfcheck.c */

/* Run every invariant in DESIGN.md, "Properties". Calls selfcheck_fail() on the first violation. */
void selfcheck_run(void);

/* Set by OP_DEBUG_TRACE: print each system call and run the self-check after it. */
extern bool debug_trace;

/* Walk one pool's objects, the descriptor excluded. Returns NULL at the end. */
struct obj_header *pool_first(struct pool *pool);
struct obj_header *pool_next(struct pool *pool, struct obj_header *obj);

/*
 * Walk every object in every pool as one sequence,
 * pool descriptors included, since a descriptor is an object like any other.
 * Only the self-check and the host harness walk it; no system call does.
 * An object's pool is its own header, so the walk needs no cursor.
 */
struct obj_header *object_first(void);
struct obj_header *object_next(struct obj_header *obj);

/* The live object at a physical address, or NULL. */
struct obj_header *object_find(paddr_t p);

/* boot.c */

/*
 * Memory the root task was granted at boot, with the rights it received.
 * The self-check uses it to bound what any capability may name.
 */
struct granted_range {
    paddr_t base;
    uint32_t size;
    uint8_t rights;
};
/* The counter's block, read only, which OP_CLOCK_FRAME hands out. */
#define GRANT_COUNTER 4
/* The board's DEBUG_RANGES follow, which only OP_DEBUG_FRAME hands out. */
#define GRANT_DEBUG 5
#define GRANTED_RANGES (GRANT_DEBUG + DEBUG_RANGES)
extern struct granted_range boot_granted[GRANTED_RANGES];

/*
 * Build the root task where the board's layout says:
 * its objects in the boot pool, which with its regions is made of its BOOT_CAP_ROOT_RAM,
 * and the free RAM its BOOT_CAP_FREE_RAM Untyped.
 */
struct thread *boot_create_root(void);

#endif
