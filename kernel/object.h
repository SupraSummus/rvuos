#ifndef RVUOS_OBJECT_H
#define RVUOS_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rvuos/abi.h"
#include "paddr.h"
#include "pmp.h"
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
 * see untyped_base, and b the watermark, an offset from the base; there is no object either.
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
 * and so is a pool's own node, CAP_RETYPED, in the pool's descriptor.
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
 * A ready thread other than the running one is on the run queue,
 * and a waiting one on its notification's waiters,
 * so the kernel finds the next thread to run without a walk;
 * see DESIGN.md, "Scheduling".
 */
enum {
    THREAD_STOPPED = 0, /* created, or configured and not started */
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

struct thread {
    struct obj_header hdr;
    paddr_t proc;
    uint8_t state;
    uint8_t flags;
    uint16_t pad;
    paddr_t waiting_on; /* the notification, while WAITING; 0 otherwise */
    /* The notification's waiters while WAITING, the run queue while READY and not running. */
    paddr_t queue_next;
    paddr_t queue_prev;
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
 * bound at creation to a notification in its own pool,
 * armed with the bits it will signal, and disarmed by signalling.
 * The notification dies with the Irq's pool, as a thread's process does;
 * the link is not checked on use.
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
    paddr_t ntfn;
    uint32_t bits;
    uint32_t line;
    uint32_t deadline;  /* on a timer line, the tick count it fires at */
};

_Static_assert(sizeof(struct obj_header) == 8, "object layout");
_Static_assert(sizeof(struct cap) == 24, "object layout");
_Static_assert(sizeof(struct captable) == 12, "object layout");
_Static_assert(sizeof(struct pool) == 56, "object layout");
_Static_assert(sizeof(struct pmp_image) == 4 + 5 * PMP_MAX_ENTRIES, "object layout");
_Static_assert(sizeof(struct process) == 8 + 24 * (1 + PROCESS_REGION_SLOTS) + sizeof(struct pmp_image),
               "object layout");
_Static_assert(sizeof(struct thread) == 28 + sizeof(struct trap_frame), "object layout");
_Static_assert(sizeof(struct notification) == 16, "object layout");
_Static_assert(sizeof(struct irq) == 24, "object layout");

/*
 * True if a capability of this type names a kernel object.
 * Frames, Untypeds, interrupt lines and the clock name hardware, and the debug capability nothing,
 * so they carry no address to check.
 */
static inline bool cap_has_object(uint8_t type)
{
    return type != CAP_NONE && type != CAP_FRAME && type != CAP_UNTYPED && type != CAP_IRQ_LINE &&
           type != CAP_DEBUG && type != CAP_CLOCK && type != CAP_INSTALLED;
}

/*
 * An Untyped's block in one word, as pmpaddr encodes NAPOT but in bytes:
 * the base with the bits below half the size set.
 * Every block is at least eight bytes, so the word always ends in a one.
 */
static inline uint32_t untyped_block(uint32_t base, uint32_t size) { return base | (size / 2 - 1); }
static inline uint32_t untyped_size(const struct cap *c) { return (c->a ^ (c->a + 1)) + 1; }
static inline uint32_t untyped_base(const struct cap *c) { return c->a & ~(untyped_size(c) - 1); }
/* Where the next retype looks from: the base again once nothing made of it is left. */
static inline uint32_t untyped_mark(const struct cap *c) { return c->child != 0 ? c->b : 0; }

static inline struct pool *obj_pool(const struct obj_header *o) { return p2v(o->pool); }

_Static_assert(offsetof(struct process, slots) == offsetof(struct process, table) + sizeof(struct cap),
               "a process's table slot and region slots are one array of nodes");

/*
 * The nodes of the derivation tree an object holds, as one array, and how many:
 * a table's slots, a process's table slot and region slots, a pool's own node.
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
static inline struct process *thread_process(const struct thread *t) { return p2v(t->proc); }
static inline struct captable *thread_table(const struct thread *t) { return process_table(thread_process(t)); }
static inline struct notification *irq_notification(const struct irq *i) { return p2v(i->ntfn); }

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
 * Destroy a pool: revoke below its node, leaving keep, a capability to the pool, for last;
 * then take its objects newest first, clearing the nodes they hold as cap_delete does,
 * undoing what each left in the rest of the kernel and zeroing it;
 * then clear keep and the pool's node and zero the descriptor.
 * keep may be NULL.
 * With preempt it may stop between two steps for a pending interrupt and return false,
 * the pool dying and the rest of the work kept in it; see DESIGN.md, "Bounded work".
 * The caller has checked that the running thread and its table do not live in the pool.
 */
bool pool_destroy(struct pool *pool, struct cap *keep, bool preempt);

/* Whether pool_alloc would find room for size bytes. */
bool pool_fits(const struct pool *pool, size_t size);

/* Allocate a zeroed object of the given type and size. NULL if exhausted, or larger than OBJ_MAX_SIZE. */
void *pool_alloc(struct pool *pool, uint8_t type, size_t size);

/* Byte length of an object, from its header. */
size_t obj_size(const struct obj_header *obj);

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
 */
bool cap_revoke_below(struct cap *node, bool preempt);

/*
 * One step of a revoke that meets no pool's own node, below a pool or a line:
 * clear the first node below root other than keep, whose children take its place.
 * False when nothing but keep is left; keep may be NULL.
 */
bool cap_revoke_step_except(struct cap *root, const struct cap *keep);
static inline bool cap_revoke_step(struct cap *root) { return cap_revoke_step_except(root, NULL); }

/* Whether a preemptible walk that has done a step stops before its next; see intr_pending. */
bool cap_stop_here(bool preempt);

/* Build a capability to an object. */
struct cap cap_to_object(struct obj_header *obj, uint8_t rights);

/* Build a frame capability. */
struct cap cap_to_frame(uint32_t base, uint32_t size, uint8_t rights);

/* Build an Untyped capability with its watermark at its base. */
struct cap cap_to_untyped(uint32_t base, uint32_t size, uint8_t rights);

/* Build an interrupt line capability: count lines from first. */
struct cap cap_to_lines(uint32_t first, uint32_t count, uint8_t rights);

/* The object behind a capability that names one. */
static inline struct obj_header *cap_object(const struct cap *cap)
{
    return p2v(cap->a);
}

/* sched.c */

/* The running thread. */
extern struct thread *current;

/*
 * The ready threads other than the running one, as a ring, oldest first;
 * 0 when there are none.
 * The oldest runs next, and a thread that becomes ready joins behind the newest.
 */
extern paddr_t run_queue;

/*
 * How many Irqs are armed on a device's line or a timer line,
 * the sources that can make a thread runnable while none is; see sched_run_next.
 * Every write of an Irq's bits goes through irq_set_bits, which keeps it.
 */
extern uint32_t armed_sources;

/* Arm with bits, or disarm with zero; the deadline, or the line's mask, is the caller's. */
void irq_set_bits(struct irq *irq, uint32_t bits);

/* The line fired: disarm the Irq and signal its bits. The mask, if the line has one, is the caller's. */
void irq_signal(struct irq *irq);

/* Make a stopped or woken thread ready: it joins the back of the round. */
void sched_ready(struct thread *t);

/*
 * Ticks so far.
 * Only the tick and OP_DEBUG_TICK move it,
 * and the deadlines of timer lines are counted in it.
 */
extern uint32_t sched_ticks;

/*
 * Set bits on a notification and wake a thread waiting on it, if any.
 * Never blocks; OP_NOTIFY_SIGNAL and a firing Irq are both this.
 */
void sched_signal(struct notification *ntfn, uint32_t bits);

/* The bits the last wake handed over, zero if none; for the trace. */
extern uint32_t trace_wake_bits;

/*
 * The running thread is no longer runnable.
 * Hand the processor to a thread that is, or stop the machine.
 */
void sched_run_next(void);

/*
 * The timer tick: count it, fire every timer line that is due,
 * and hand the processor to the thread that has waited longest for it,
 * if there is one; the running thread goes to the back of the round.
 * The interrupt calls it, and OP_DEBUG_TICK does on request.
 */
void sched_tick(void);

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
 * a thread leaves the notification it waits on or the run queue,
 * and an Irq is disarmed and its line masked and unbound.
 * A pool takes its objects newest first, so a thread goes before its process
 * and an Irq before its notification.
 */
void sched_forget(struct obj_header *o);

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

/* Load a process's PMP image into the CSRs. */
void process_activate(struct process *proc);

/* syscall.c */
void syscall_dispatch(struct thread *t);

/* selfcheck.c */

/* Run every invariant in DESIGN.md, "Properties". Calls selfcheck_fail() on the first violation. */
void selfcheck_run(void);

/* Set by OP_DEBUG_TRACE: print each system call and run the self-check after it. */
extern bool debug_trace;

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
#define GRANTED_RANGES 7
/* The counter's block, read only, which OP_CLOCK_FRAME hands out. */
#define GRANT_COUNTER 6
extern struct granted_range boot_granted[GRANTED_RANGES];
extern struct pool *boot_pool;

/*
 * Build the root task.
 * The boot pool range holds the root task's kernel objects;
 * the free range becomes its BOOT_CAP_FREE_RAM Untyped.
 */
struct thread *boot_create_root(paddr_t boot_pool_base, uint32_t boot_pool_size,
                                paddr_t free_base, uint32_t free_size);

#endif
