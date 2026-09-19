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
    uint8_t pad[3];
    paddr_t pool;
};

/*
 * A capability slot.
 * For CAP_REGION the slot is self-contained:
 * a is the base address and b the size, and there is no object.
 * For every other type a is the object's physical address.
 */
struct cap {
    uint8_t type;
    uint8_t rights;
    uint8_t pad[2];
    uint32_t a;
    uint32_t b;
};

#define CAPTABLE_MAX_SLOTS 1024

struct captable {
    struct obj_header hdr;
    uint32_t nslots;
    struct cap slots[];
};

/*
 * A pool's descriptor is the first object in its own memory,
 * so the kernel keeps no state about pools outside them
 * except the list head.
 * Objects are bump-allocated and walked in order;
 * obj_size() recovers each object's length from its type.
 *
 * Pools form a tree: a pool's parent is the pool its creator lived in,
 * and a destroy takes every pool below; see DESIGN.md, "Kernel pools and revocation".
 */
struct pool {
    struct obj_header hdr;
    paddr_t base;
    uint32_t size;
    uint32_t used;      /* bytes handed out, header included */
    paddr_t next;       /* global list of pools, 0 at the end */
    paddr_t parent;     /* the pool the creating thread lived in; 0 for the boot pool */
    uint8_t rights;     /* what the region carried; returned on destroy */
};

/* A slot with rights 0 is empty. */
struct region_slot {
    uint32_t base;
    uint32_t size;
    uint8_t rights;
};

struct pmp_image {
    unsigned count;
    uint32_t addr[PMP_MAX_ENTRIES];
    uint8_t cfg[PMP_MAX_ENTRIES];
};

struct process {
    struct obj_header hdr;
    paddr_t ctable;
    struct region_slot slots[PROCESS_REGION_SLOTS];
    struct pmp_image pmp;
};

/*
 * Thread states.
 * The kernel keeps no run queue:
 * a thread's own state says whether it may run,
 * and the kernel finds a runnable thread by walking the pools,
 * as the overlap checks do, continuing from the running thread.
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
    struct trap_frame frame;
};

/*
 * A notification is a word of sticky bits, and the only way
 * a thread can stop running and be started again.
 * Data does not travel through it:
 * two processes that share a region move bytes through the region
 * and use the notification to say when.
 *
 * The kernel keeps no list of waiters.
 * A waiting thread records the notification it waits on,
 * and a signal walks the pools to find one,
 * as the overlap checks do.
 */
struct notification {
    struct obj_header hdr;
    uint32_t bits;
};

/*
 * A timer signals a notification when a delay has passed.
 * It is how time reaches userspace: as a signal like any other,
 * so one wait can cover a device and a timeout, one bit each.
 * The notification lies in the timer's own pool and dies with it,
 * as a process's table does; the link is not checked on use.
 * bits is what the timer will signal, zero while it is disarmed,
 * and deadline is the tick count it fires at;
 * see DESIGN.md, "Time".
 */
struct timer {
    struct obj_header hdr;
    paddr_t ntfn;
    uint32_t bits;
    uint32_t deadline;
};


_Static_assert(sizeof(struct obj_header) == 8, "object layout");
_Static_assert(sizeof(struct cap) == 12, "object layout");
_Static_assert(sizeof(struct captable) == 12, "object layout");
_Static_assert(sizeof(struct pool) == 32, "object layout");
_Static_assert(sizeof(struct pmp_image) == 4 + 5 * PMP_MAX_ENTRIES, "object layout");
_Static_assert(sizeof(struct process) == 12 + 12 * PROCESS_REGION_SLOTS + sizeof(struct pmp_image),
               "object layout");
_Static_assert(sizeof(struct thread) == 20 + sizeof(struct trap_frame), "object layout");
_Static_assert(sizeof(struct notification) == 12, "object layout");
_Static_assert(sizeof(struct timer) == 20, "object layout");

static inline struct pool *obj_pool(const struct obj_header *o) { return p2v(o->pool); }
static inline struct pool *pool_next_pool(const struct pool *p) { return p->next ? p2v(p->next) : NULL; }
static inline struct pool *pool_parent(const struct pool *p) { return p->parent ? p2v(p->parent) : NULL; }
static inline struct captable *process_table(const struct process *p) { return p2v(p->ctable); }
static inline struct process *thread_process(const struct thread *t) { return p2v(t->proc); }
static inline struct captable *thread_table(const struct thread *t) { return process_table(thread_process(t)); }
static inline struct notification *timer_notification(const struct timer *t) { return p2v(t->ntfn); }

/* True if the timer will fire; it holds the bits it will signal. */
static inline bool timer_armed(const struct timer *t) { return t->bits != 0; }

/* True if the timer's deadline lies at or before the tick count now. */
static inline bool timer_due(const struct timer *t, uint32_t now)
{
    return (int32_t)(t->deadline - now) <= 0;
}

#define OBJ_ALIGN 8

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
 * Turn a zeroed range into a pool below parent, NULL for the boot pool.
 * The range must be OBJ_ALIGN aligned and large enough for the descriptor.
 * The caller has already checked for overlaps.
 */
struct pool *pool_create(paddr_t base, uint32_t size, uint8_t rights, struct pool *parent);

/* True if pool is ancestor or lies below it in the tree. */
bool pool_under(const struct pool *pool, const struct pool *ancestor);

/*
 * Destroy a pool and every pool below it:
 * clear every capability naming an object in them,
 * wake every thread waiting on a notification in them with an error,
 * and zero the memory.
 * The caller has checked that the running thread does not live in any of them.
 */
void pool_destroy(struct pool *pool);

/* Allocate a zeroed object of the given type and size. NULL if exhausted. */
void *pool_alloc(struct pool *pool, uint8_t type, size_t size);

/* Byte length of an object, from its header. */
size_t obj_size(const struct obj_header *obj);

/* Walk one pool's objects, the descriptor excluded. Returns NULL at the end. */
struct obj_header *pool_first(struct pool *pool);
struct obj_header *pool_next(struct pool *pool, struct obj_header *obj);

/*
 * Walk every object in every pool as one sequence,
 * pool descriptors included, since a descriptor is an object like any other.
 * The kernel keeps no indexes, so every question it asks about objects
 * is this walk with a filter;
 * see DESIGN.md, "Kernel pools and revocation" and "Scheduling".
 * An object's pool is its own header, so the walk needs no cursor.
 */
struct obj_header *object_first(void);
struct obj_header *object_next(struct obj_header *obj);

/* The live object at a physical address, or NULL. */
struct obj_header *object_find(paddr_t p);

/* True if [base, base + size) intersects any pool. */
bool pool_overlaps(uint32_t base, uint32_t size);

/* True if [base, base + size) intersects a region installed in any process. */
bool installed_overlaps(uint32_t base, uint32_t size);

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

/* Store into an empty slot; fails as cap_slot_free does. */
int cap_store(struct captable *table, uint32_t slot, const struct cap *cap);

/* Clear a slot. KERR_INVALID_CAP if out of range. */
int cap_clear(struct captable *table, uint32_t slot);

/*
 * Clear every capability, in every table, naming an object
 * in [base, base + size).
 * This is what a pool destroy revokes with,
 * and it is exact because every capability lives in a CapTable,
 * every CapTable is an object in a pool, and every pool is on the list.
 * Region capabilities name no object and are left alone;
 * see DESIGN.md, open decision 7.
 */
void cap_revoke_range(uint32_t base, uint32_t size);

/* Build a capability to an object. */
struct cap cap_to_object(struct obj_header *obj, uint8_t rights);

/* Build a region capability. */
struct cap cap_to_region(uint32_t base, uint32_t size, uint8_t rights);

/* The object behind a non-region capability. */
static inline struct obj_header *cap_object(const struct cap *cap)
{
    return p2v(cap->a);
}

/* sched.c */

/* The running thread. */
extern struct thread *current;

/*
 * Ticks so far.
 * Only the tick and OP_DEBUG_TICK move it,
 * and Timer deadlines are counted in it.
 */
extern uint32_t sched_ticks;

/*
 * Set bits on a notification and wake a thread waiting on it, if any.
 * Never blocks; OP_NOTIFY_SIGNAL and a firing Timer are both this.
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
 * The timer tick: count it, fire every Timer that is due,
 * and hand the processor to the next runnable thread in the round,
 * if there is one besides the running thread.
 * The interrupt calls it, and OP_DEBUG_TICK does on request.
 */
void sched_tick(void);

/* The first thread blocked on a notification, or NULL. */
struct thread *sched_waiter(paddr_t notification);

/*
 * Wake every thread waiting on a notification in [base, base + size)
 * with KERR_INVALID_CAP and no bits, because the object is gone.
 */
void sched_unblock_range(uint32_t base, uint32_t size);

/* process.c */

int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights);
int process_uninstall(struct process *proc, unsigned slot);

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
#define GRANTED_RANGES 4
extern struct granted_range boot_granted[GRANTED_RANGES];
extern struct pool *boot_pool;

/*
 * Build the root task.
 * The boot pool range holds the root task's kernel objects;
 * the free range becomes its BOOT_CAP_FREE_RAM region.
 */
struct thread *boot_create_root(paddr_t boot_pool_base, uint32_t boot_pool_size,
                                paddr_t free_base, uint32_t free_size);

#endif
