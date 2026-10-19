#ifndef RVUOS_OBJECT_H
#define RVUOS_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rvuos/abi.h"
#include "kernel.h"
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
 * A capability slot, and a node of the derivation tree; see DESIGN.md, "The derivation tree" and "Slot format".
 * Sixteen bytes: two words of content, then the type, the rights and the two links of the node's ring;
 * the second word holds the third link, to the first child, and LINK_UP.
 * For CAP_FRAME and CAP_UNTYPED a is the block, its base and size in one word as NAPOT encodes them,
 * see block_base; there is no object, and whether an Untyped is free the tree says, and untyped_going.
 * For CAP_IRQ_LINE, CAP_TIME and CAP_ROUTE_LINE a is the range, its first line, unit or channel and its count
 * in one word, see range_first; there is no object either.
 * For every other type a is the object's physical address, see cap_addr,
 * whose low bits hold, for CAP_NOTIFICATION, part of the bits the capability may signal; see notify_bits.
 *
 * Every node lies on NODE_ALIGN, so a link is its physical address over NODE_ALIGN in 28 bits,
 * and reaches the whole address space.
 * The first child, B_LINK of b, is the first slot derived from this one, 0 if none; see cap_child.
 * next is the next sibling, or at the last sibling the parent with B_UP set;
 * a root's next is B_UP alone, an empty slot's is 0. See cap_next.
 * prev is the previous sibling, and at the first sibling the last one,
 * so that a node leaves its ring without a walk;
 * an only child's is itself, and an empty slot's is 0.
 * A root has none, so its prev says instead whether it is an Untyped whose delete as a root has begun.
 *
 * An object holds nodes outside a table too, each a capability of the type it names:
 * a process its table slots, CAP_CAPTABLE, and its region slots, CAP_FRAME,
 * a thread its process, CAP_PROCESS, its watch, CAP_NOTIFICATION, and each place's units, CAP_TIME,
 * none at all among them, an Irq its notification, CAP_NOTIFICATION, a Route its ends, CAP_FRAME,
 * and a pool its own node, CAP_POOL.
 * All but a pool's own node are leaves, which need no first child.
 * Those whose holder hears of their clearing have B_HELD set and name the holder where the first child would lie:
 * a region slot, a thread's process and its places' units, an Irq's notification and a Route's ends; see cap_holder.
 *
 * The type takes five bits and the rights three, so prev straddles the last two words, which cap_prev joins.
 */
struct cap {
    uint32_t a;
    uint32_t b;
    uint32_t type : 5;
    uint32_t rights : 3;
    uint32_t prev_low : 24;
    uint32_t prev_high : 4;
    uint32_t next : 28;
};

/*
 * The objects ask it of the nodes they hold with NODE_ALIGNED, not the type,
 * so that a copy of a slot on ARM's stack, which lies on eight bytes, needs no realigned frame.
 */
#define NODE_ALIGN 16
#define NODE_ALIGNED __attribute__((aligned(NODE_ALIGN)))
/* The tag cap_next sets in a link up to the parent. */
#define LINK_UP 0x1u
/* The bits of b that hold the first child, or a held node's holder, over NODE_ALIGN. */
#define B_LINK 0x0fffffffu
/* A node an object holds, which names the object in B_LINK. */
#define B_HELD 0x80000000u
/* The node's next names its parent: it is the last of its ring, or a root. */
#define B_UP 0x40000000u

/* A node's links as physical addresses, LINK_UP set in a next that names the parent. */
static inline uint32_t cap_next(const struct cap *n) { return (uint32_t)n->next * NODE_ALIGN | (n->b & B_UP) / B_UP; }
static inline uint32_t cap_prev(const struct cap *n) { return ((uint32_t)n->prev_high << 24 | n->prev_low) * NODE_ALIGN; }
static inline void cap_set_next(struct cap *n, uint32_t link)
{
    n->next = link / NODE_ALIGN;
    n->b = (n->b & ~B_UP) | (link & LINK_UP) * B_UP;
}
static inline void cap_set_prev(struct cap *n, uint32_t link)
{
    n->prev_low = link / NODE_ALIGN;
    n->prev_high = link / NODE_ALIGN >> 24;
}
/* Whether an object holds the node and hears of its clearing; it is a leaf then. */
static inline bool cap_held(const struct cap *n) { return (n->b & B_HELD) != 0; }
/* The object holding a held node. */
static inline paddr_t cap_holder(const struct cap *n) { return (n->b & B_LINK) * NODE_ALIGN; }
/* The second word of a node an object holds: B_HELD and the holder, and of b only a notification's share of its code. */
static inline uint32_t held_word(paddr_t holder, uint32_t b)
{
    return (b & ~(B_LINK | B_UP)) | B_HELD | holder / NODE_ALIGN;
}
/* The first child, 0 for none, which a held node never has. */
static inline uint32_t cap_child(const struct cap *n) { return cap_held(n) ? 0 : (n->b & B_LINK) * NODE_ALIGN; }
static inline void cap_set_child(struct cap *n, uint32_t link) { n->b = (n->b & ~B_LINK) | link / NODE_ALIGN; }
/* Whether an Untyped's delete as a root has begun, see cap_delete: a root has no predecessor, so its prev says so. */
static inline bool untyped_going(const struct cap *n)
{
    return n->type == CAP_UNTYPED && cap_next(n) == LINK_UP && cap_prev(n) != 0;
}
static inline void untyped_set_going(struct cap *n) { cap_set_prev(n, NODE_ALIGN); }

#define CAPTABLE_MAX_SLOTS 1024
_Static_assert(CAPTABLE_MAX_SLOTS <= 1u << SLOT_TABLE_SHIFT, "a slot number's index reaches every slot of a table");

struct captable {
    struct obj_header hdr;
    uint32_t nslots;
    uint32_t pad;       /* the slots lie on NODE_ALIGN */
    struct cap slots[] NODE_ALIGNED;
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
    uint32_t size;
    uint32_t used;      /* bytes handed out, descriptor included */
    struct cap node NODE_ALIGNED; /* its own node, CAP_POOL naming it: below the Untyped the pool was retyped from */
    paddr_t last;       /* the newest object, the descriptor itself when there is none */
    paddr_t next;       /* list of pools, newest first, 0 at the end */
    paddr_t prev;       /* 0 at the head */
    uint16_t sweep;     /* while dying: the next slot of the newest object the destroy clears */
    uint8_t dying;      /* set by the first step of a destroy; nothing is allocated from then on */
    uint8_t pad;
};

static inline paddr_t pool_base(const struct pool *p) { return p->hdr.pool; }
/* The bytes an allocation may still take of a pool, OP_CAP_INFO's room: none while it is being destroyed. */
static inline uint32_t pool_room(const struct pool *p) { return p->dying ? 0 : p->size - p->used; }

/*
 * A process holds its tables by capabilities, not pointers:
 * nodes of the derivation tree, each below the capability the process was made with or the table mounted with,
 * which a revoke or the destroy of the table's pool clears.
 * The region slots follow the table slots, so obj_nodes hands both out as one array.
 * It has as many region slots as its creator asked for, from one to pmp_entry_count, see open decision 35,
 * and after them the PMP image the region slots make, an entry each at most:
 * the pmpaddr values, then the pmpcfg bytes, which lie on four bytes; see process_pmp_addr and process_pmp_cfg.
 * See DESIGN.md, "A process's tables" and "Region slots".
 */
struct process {
    struct obj_header hdr;
    uint8_t regions;    /* how many region slots it has, from one to pmp_entry_count */
    uint8_t pmp_count;  /* the entries its PMP image holds, one for each filled region slot */
    uint16_t pad;
#ifdef BOARD_PROCESS_CSRS
    uint32_t csrs[BOARD_PROCESS_CSRS]; /* what the board keeps of the CSRs user mode writes, see board_user_csrs_switch */
#endif
    struct cap tables[PROCESS_TABLES] NODE_ALIGNED; /* CAP_CAPTABLE, or CAP_NONE while none is mounted */
    struct cap slots[]; /* a region, CAP_FRAME held by the process, or CAP_NONE when empty; the PMP image after them */
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
 * A thread stopped while it waited stands at its call; see OP_THREAD_STOP.
 */
enum {
    THREAD_STOPPED = 0, /* created, configured and not started, stopped where it faulted, or by OP_THREAD_STOP */
    THREAD_READY,       /* running, or waiting for the processor */
    THREAD_WAITING,     /* blocked on the notification in waiting_on */
};
_Static_assert(THREAD_WAS_STOPPED == THREAD_STOPPED && THREAD_WAS_READY == THREAD_READY &&
                   THREAD_WAS_WAITING == THREAD_WAITING,
               "OP_THREAD_STOP returns the state as it is");

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

/*
 * A thread's place on a core: what it earns there, its account, and where it waits on that core's queues.
 * A thread has the places it was made with, each empty or holding the time of one core,
 * its units there, none at all among them, with spare time or without,
 * and never two the time of one core; see DESIGN.md, "Cores".
 * The binding itself is the place's node in the thread, place_time, so that a thread's nodes are one array.
 */
struct place {
    /*
     * Its account at the tick stamp, in COUNT_PARTS parts of a count,
     * at most ACCOUNT_TICKS ticks' counts of parts for each unit it earns, and so none while it earns none.
     * It is brought up to the count only when the kernel looks at it,
     * and the charges of a turn in the tick it was counted to are taken off it as that turn ends.
     */
    uint32_t balance;
    uint32_t stamp;
    /* Its core's queue while it waits on one, a ring of places. */
    paddr_t queue_next;
    paddr_t queue_prev;
    paddr_t thread;     /* the thread it is a place of */
    uint32_t deadline;  /* the count its thread's request on the core is due by; see sched.c, "Requests" */
    uint8_t queue;      /* the scheduler's queue it waits on, one of QUEUE_*, while its thread is READY and not running */
    uint8_t core;       /* the core whose queue that is, while it waits on one */
    uint8_t index;      /* its place among its thread's */
    uint8_t quantum;    /* how long a request is: 0 for a tick, else 2 to the quantum - 1 counts */
};

/* A thread holds its process as a process holds its tables, so the two may lie in different pools. */
struct thread {
    struct obj_header hdr;
    uint8_t state;
    uint8_t flags;
    /*
     * The core whose turn it has or pays for, or had or paid for last;
     * a turn there takes it off the queues of every core until the turn ends; see is_turn in sched.c.
     */
    uint8_t core;
    uint8_t places;     /* how many places it has, from one to CORES */
    paddr_t waiting_on; /* the notification, while WAITING; 0 otherwise */
    /* The notification's waiters while WAITING. */
    paddr_t queue_next;
    paddr_t queue_prev;
    /*
     * The thread it lends its time to while it waits, see OP_NOTIFY_LEND, 0 for none,
     * and its place on that thread's ring of lenders, through lend_next and lend_prev.
     */
    paddr_t lend_to;
    paddr_t lend_next;
    paddr_t lend_prev;
    /* The threads that lend it their time, a ring, oldest first, 0 for none: the oldest is the one it borrows from. */
    paddr_t lenders;
    struct trap_frame frame;
    struct cap proc NODE_ALIGNED; /* its process, CAP_PROCESS held by the thread, or CAP_NONE once the process is taken */
    struct cap watch;   /* CAP_NOTIFICATION, or CAP_NONE while nothing hears the thread's faults */
    /*
     * Each place's units, CAP_TIME held by the thread, or CAP_NONE while the place is empty, a node for each place,
     * and after them the places themselves; see place_time and thread_place.
     */
    struct cap time[];
};

/*
 * A notification is a word of sticky bits, and the only way
 * a thread can stop running until a signal starts it again;
 * OP_THREAD_STOP stops one until a resume does.
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
    uint32_t bits;
    uint16_t line;
    uint8_t core;       /* on a timer line, the core that armed it, whose timer wakes for it */
    uint8_t pad;
    /* CAP_NOTIFICATION held by the Irq, with the bits it may signal, or CAP_NONE once the notification is taken */
    struct cap ntfn NODE_ALIGNED;
    uint32_t deadline;  /* on a timer line, the tick count it fires at */
};

/* A size rounded up to NODE_ALIGN, as the nodes after it lie. */
#define NODE_ROUND(size) (((size) + NODE_ALIGN - 1) & ~(NODE_ALIGN - 1))

/*
 * A Route wires an event of one device to a task of another through a channel of the board's router,
 * with no processor in between: bound at creation to its channel, connected end by end, then armed.
 * It holds each end's frame as an Irq holds its notification, so a revoke above either disarms it,
 * and the router's channel is enabled exactly while it is armed; see DESIGN.md, "Routes".
 * One Route per channel.
 */
struct route {
    struct obj_header hdr;
    uint32_t names[2];  /* each end as the board names it within its frame, 0 while it is not connected */
    /* CAP_FRAME held by the Route, the event's and the task's, or CAP_NONE while that end is not connected */
    struct cap ends[2] NODE_ALIGNED;
    uint16_t channel;
    uint8_t armed;
    uint8_t pad;
};

_Static_assert(sizeof(struct obj_header) == 8, "object layout");
_Static_assert(sizeof(struct cap) == NODE_ALIGN, "object layout");
_Static_assert(CAP_ROUTE < 32, "a type fits its five bits");
_Static_assert(sizeof(struct captable) == 16, "object layout");
_Static_assert(sizeof(struct pool) == 48, "object layout");
#ifdef BOARD_PROCESS_CSRS
#define PROCESS_CSRS_SIZE (4 * BOARD_PROCESS_CSRS)
#if CORES > 1
#error "a process keeps one copy of the CSRs, which its threads on two cores would both change"
#endif
#else
#define PROCESS_CSRS_SIZE 0
#endif
_Static_assert(sizeof(struct process) == NODE_ROUND(12 + PROCESS_CSRS_SIZE) + 16 * PROCESS_TABLES, "object layout");
_Static_assert(sizeof(struct thread) == NODE_ROUND(40 + sizeof(struct trap_frame)) + 2 * sizeof(struct cap),
               "object layout");
_Static_assert(sizeof(struct place) == 28, "object layout");
_Static_assert(sizeof(struct notification) == 16, "object layout");
_Static_assert(sizeof(struct irq) == 48, "object layout");
_Static_assert(sizeof(struct route) == 64, "object layout");
_Static_assert(ROUTE_EVENT == 0 && ROUTE_TASK == 1, "a Route's ends are its two nodes, by end");

/*
 * True if a capability of this type names a kernel object.
 * Frames, Untypeds, interrupt lines, a router's channels, units of time and the clock name hardware,
 * a Route's ends a frame, and Print, Halt and Test nothing, so they carry no address to check.
 */
static inline bool cap_has_object(uint8_t type)
{
    return type != CAP_NONE && type != CAP_FRAME && type != CAP_UNTYPED && type != CAP_IRQ_LINE &&
           type != CAP_PRINT && type != CAP_HALT && type != CAP_TEST && type != CAP_CLOCK && type != CAP_TIME &&
           type != CAP_ROUTE_LINE;
}

/* The object a capability names, its address without what a Notification capability keeps in the bits below. */
static inline paddr_t cap_addr(const struct cap *c) { return c->a & ~(paddr_t)(NODE_ALIGN - 1); }

/*
 * A frame's or an Untyped's block in one word, as pmpaddr encodes NAPOT but in bytes:
 * the base with the bits below half the size set.
 * Every block is at least eight bytes, so the word always ends in a one.
 */
static inline uint32_t block_word(uint32_t base, uint32_t size) { return base | (size / 2 - 1); }
static inline uint32_t block_size(const struct cap *c) { return (c->a ^ (c->a + 1)) + 1; }
static inline uint32_t block_base(const struct cap *c) { return c->a & ~(block_size(c) - 1); }

/*
 * A range of interrupt lines or of units in one word: the first in the low half, the count in the high.
 * Neither the machine's lines nor its units reach 0x10000, see irq.h and sched.h.
 */
static inline uint32_t range_word(uint32_t first, uint32_t count) { return first | count << 16; }
static inline uint32_t range_first(const struct cap *c) { return c->a & 0xffffu; }
static inline uint32_t range_count(const struct cap *c) { return c->a >> 16; }

/*
 * The bits a Notification capability may signal are an aligned block of 1, 2, 4, 8, 16 or 32 bits,
 * so that six bits say which: twice its first bit, plus its count less one,
 * whose trailing ones count the block's bits as a block word's do its bytes.
 * The code's low four bits lie below the notification's address, the two above them in b, past B_LINK.
 */
/* The code of every bit, a new notification's. */
#define NOTIFY_CODE_ALL 31u
static inline uint32_t notify_code(const struct cap *c) { return (c->a & 0xfu) | (c->b >> 28 & 0x3u) << 4; }
static inline uint32_t notify_code_bits(uint32_t code)
{
    uint32_t count = ((code ^ (code + 1)) + 1) / 2;
    uint32_t first = (code / 2) & ~(count - 1);
    return (count == 32 ? 0xffffffffu : (1u << count) - 1) << first;
}
static inline uint32_t notify_bits(const struct cap *c) { return notify_code_bits(notify_code(c)); }
/* A power of two's exponent, without a loop or a library call. */
static inline uint32_t pow2_log(uint32_t x)
{
    return (uint32_t)((x & 0xaaaaaaaau) != 0) | (uint32_t)((x & 0xccccccccu) != 0) << 1 |
           (uint32_t)((x & 0xf0f0f0f0u) != 0) << 2 | (uint32_t)((x & 0xff00ff00u) != 0) << 3 |
           (uint32_t)((x & 0xffff0000u) != 0) << 4;
}
/* Whether bits are such a block, and if so its code. */
static inline bool notify_block(uint32_t bits, uint32_t *code)
{
    uint32_t low = bits & (0u - bits);
    uint32_t end = bits + low;          /* the bit past the block, 0 when the block reaches bit 31 */
    if (bits == 0 || (end & (end - 1)) != 0) {
        return false;
    }
    uint32_t first = pow2_log(low);
    uint32_t count = (end == 0 ? 32 : pow2_log(end)) - first;
    if ((count & (count - 1)) != 0 || (first & (count - 1)) != 0) {
        return false;
    }
    *code = 2 * first + count - 1;
    return true;
}
/* A Notification capability's words for an object and a block's code, the rest of b as it was. */
static inline void notify_set(struct cap *c, paddr_t obj, uint32_t code)
{
    c->a = obj | (code & 0xfu);
    c->b = (c->b & ~(0x3u << 28)) | (code >> 4) << 28;
}

/* Whether an Untyped makes something: only while nothing made of it is left, below it or gone up as a root. */
static inline bool untyped_free(const struct cap *c) { return cap_child(c) == 0 && !untyped_going(c); }
/*
 * What an Untyped made, as OP_CAP_INFO tells it: 0 while nothing lies below it, else the type of the node first there,
 * CAP_FRAME for a frame, a region installed from one or a Route's end on one, CAP_UNTYPED for a half or a derived Untyped,
 * and CAP_POOL for a pool's own node.
 * Nothing else lies below an Untyped, as the self-check's derivation rule holds.
 */
static inline uint32_t untyped_made(const struct cap *c)
{
    if (cap_child(c) == 0) {
        return 0;
    }
    return ((const struct cap *)p2v(cap_child(c)))->type;
}

static inline struct pool *obj_pool(const struct obj_header *o) { return p2v(o->pool); }

_Static_assert(offsetof(struct process, slots) == offsetof(struct process, tables) + sizeof(((struct process *)0)->tables),
               "a process's table slots and region slots are one array of nodes");
_Static_assert(offsetof(struct thread, watch) == offsetof(struct thread, proc) + sizeof(struct cap) &&
                   offsetof(struct thread, time) == offsetof(struct thread, watch) + sizeof(struct cap),
               "a thread's process, watch and places' units are one array of nodes");
_Static_assert(CORES <= UINT8_MAX, "a thread counts its places in a byte");
_Static_assert(PMP_MAX_ENTRIES <= PROCESS_REGION_SLOTS_MAX, "a process has the region slots the ABI says it may");

/*
 * The bytes a process of so many region slots takes: its nodes for them, and the PMP image after them,
 * an entry for each, its pmpcfg bytes rounded up to a word, as pmp_load reads them.
 */
static inline size_t process_size(uint32_t regions)
{
    return sizeof(struct process) + regions * (sizeof(struct cap) + sizeof(uint32_t)) + ((regions + 3) & ~3u);
}
/* A process's PMP image: the pmpaddr value of each of its entries, past its region slots. */
static inline uint32_t *process_pmp_addr(const struct process *p)
{
    return (uint32_t *)(uintptr_t)&p->slots[p->regions];
}
/* And the pmpcfg byte of each, past the pmpaddr values, on four bytes. */
static inline uint8_t *process_pmp_cfg(const struct process *p)
{
    return (uint8_t *)(process_pmp_addr(p) + p->regions);
}

/* The bytes a thread of so many places takes: its nodes for each place's units, and the places after them. */
static inline size_t thread_size(uint32_t places)
{
    return sizeof(struct thread) + places * (sizeof(struct cap) + sizeof(struct place));
}
/*
 * How many places a thread has: one on a machine of one core, which the compiler then knows,
 * so that the scheduler's every step pays nothing for places a core has no use for.
 */
static inline uint32_t thread_places(const struct thread *t)
{
    return CORES == 1 ? 1 : t->places;
}
/* A thread's place i, below its places. */
static inline struct place *thread_place(const struct thread *t, uint32_t i)
{
    return (struct place *)(uintptr_t)&t->time[thread_places(t)] + i;
}
/* The thread a place is of, and its units, the place's node: on one core the node just before it. */
static inline struct thread *place_thread(const struct place *p)
{
    if (CORES == 1) {
        return (struct thread *)((char *)(uintptr_t)p - sizeof(struct cap) - offsetof(struct thread, time));
    }
    return p2v(p->thread);
}
static inline struct cap *place_time(const struct place *p)
{
    if (CORES == 1) {
        return (struct cap *)(uintptr_t)p - 1;
    }
    return &place_thread(p)->time[p->index];
}
/* A thread pool_alloc zeroed takes its places: each knows the thread and which of its places it is. */
static inline void thread_make_places(struct thread *t, uint32_t places)
{
    t->places = (uint8_t)places;
    for (uint32_t i = 0; i < places; i++) {
        LOOP_BOUND(CORES);
        struct place *p = thread_place(t, i);
        p->thread = v2p(t);
        p->index = (uint8_t)i;
    }
}

/*
 * The nodes of the derivation tree an object holds, as one array, and how many:
 * a table's slots, a process's table slots and region slots, a thread's process, watch and places' units,
 * an Irq's notification, a Route's ends, a pool's own node.
 * Other objects hold none.
 */
static inline struct cap *obj_nodes(struct obj_header *o, uint32_t *count)
{
    switch (o->type) {
    case CAP_CAPTABLE:
        *count = ((struct captable *)o)->nslots;
        return ((struct captable *)o)->slots;
    case CAP_PROCESS:
        *count = PROCESS_TABLES + ((struct process *)o)->regions;
        return ((struct process *)o)->tables;
    case CAP_THREAD:
        *count = 2 + ((struct thread *)o)->places;
        return &((struct thread *)o)->proc;
    case CAP_IRQ:
        *count = 1;
        return &((struct irq *)o)->ntfn;
    case CAP_ROUTE:
        *count = 2;
        return ((struct route *)o)->ends;
    case CAP_POOL:
        *count = 1;
        return &((struct pool *)o)->node;
    default:
        *count = 0;
        return NULL;
    }
}
static inline struct pool *pool_next_pool(const struct pool *p) { return p->next ? p2v(p->next) : NULL; }
/* The table a process mounts in table slot i, or NULL while none is. The type test is cap_lookup's. */
static inline struct captable *process_table(const struct process *p, uint32_t i)
{
    const struct cap *c = &p->tables[i];
    if (c->type != CAP_CAPTABLE) {
        return NULL;
    }
    struct captable *t = p2v(cap_addr(c));
    return t->hdr.type == CAP_CAPTABLE ? t : NULL;
}
/* The process a thread runs in, or NULL once it was taken. */
static inline struct process *thread_process(const struct thread *t)
{
    return t->proc.type == CAP_PROCESS ? p2v(cap_addr(&t->proc)) : NULL;
}
/*
 * The table a slot number of a thread's points into, see SLOT_TABLE_SHIFT,
 * or NULL once its process was taken or while the process mounts no table there.
 */
static inline struct captable *slot_table(const struct thread *t, uint32_t slot)
{
    const struct process *p = thread_process(t);
    uint32_t i = slot >> SLOT_TABLE_SHIFT;
    return p != NULL && i < PROCESS_TABLES ? process_table(p, i) : NULL;
}
/* A slot number's index in the table it points into. */
static inline uint32_t slot_index(uint32_t slot) { return slot & ((1u << SLOT_TABLE_SHIFT) - 1); }
/* The slot a slot number of a thread's names, filled or not, or NULL when it names none. */
static inline struct cap *slot_at(const struct thread *t, uint32_t slot)
{
    struct captable *table = slot_table(t, slot);
    uint32_t i = slot_index(slot);
    return table != NULL && i < table->nslots ? &table->slots[i] : NULL;
}
/* The notification a thread's faults signal, or NULL while it has no watch. */
static inline struct notification *thread_watch(const struct thread *t)
{
    return t->watch.type == CAP_NOTIFICATION ? p2v(cap_addr(&t->watch)) : NULL;
}
/* The notification an Irq signals, or NULL once it was taken. */
static inline struct notification *irq_notification(const struct irq *i)
{
    return i->ntfn.type == CAP_NOTIFICATION ? p2v(cap_addr(&i->ntfn)) : NULL;
}


/* True if the Irq will signal; a controller's line is unmasked exactly then. */
static inline bool irq_armed(const struct irq *i) { return i->bits != 0; }

/* True if an Irq on a timer line has its deadline at or before the tick count now. */
static inline bool irq_due(const struct irq *i, uint32_t now)
{
    return (int32_t)(i->deadline - now) <= 0;
}

/* Objects lie on NODE_ALIGN, so that the nodes they hold do. */
#define OBJ_ALIGN NODE_ALIGN

/* A size as a pool counts it, rounded up to OBJ_ALIGN. */
static inline uint32_t obj_align(uint32_t size) { return (size + OBJ_ALIGN - 1) & ~(uint32_t)(OBJ_ALIGN - 1); }

/* The largest object, a table of the most slots; pool_alloc refuses a larger one. */
_Static_assert(sizeof(struct thread) + CORES * (sizeof(struct cap) + sizeof(struct place)) <=
                   sizeof(struct captable) + CAPTABLE_MAX_SLOTS * sizeof(struct cap),
               "a thread of every core is no larger than the largest table");
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

/*
 * The pool whose own node this is, or NULL for a capability to a pool:
 * a pool's own node lies in the descriptor it names, a capability in a table does not.
 */
static inline struct pool *node_pool(const struct cap *n)
{
    return n->type == CAP_POOL && v2p(n) == cap_addr(n) + offsetof(struct pool, node) ? p2v(cap_addr(n)) : NULL;
}

/*
 * Destroy a pool, as a revoke below an Untyped does when it meets the pool's node:
 * revoke below that node,
 * then take its objects newest first, clearing the nodes they hold as cap_delete does,
 * undoing what each left in the rest of the kernel and zeroing it;
 * then clear the pool's node and zero the descriptor.
 * With preempt it may stop between two steps for a pending interrupt and return false,
 * the pool dying and the rest of the work kept in it; see DESIGN.md, "Bounded work".
 * The caller has checked that the running thread and its process's tables do not live in the pool.
 */
bool pool_destroy(struct pool *pool, bool preempt);

/* Whether pool_alloc would find room for size bytes. */
bool pool_fits(const struct pool *pool, size_t size);

/* Allocate a zeroed object of the given type and size. NULL if exhausted, or larger than OBJ_MAX_SIZE. */
void *pool_alloc(struct pool *pool, uint8_t type, size_t size);

/*
 * The bytes an object of a type takes, before rounding,
 * slots its count for a table, its region slots for a process and its places for a thread:
 * what an allocation asks of a pool and what OP_POOL_INFO tells, CAP_POOL's being the descriptor.
 * obj_size below keeps a switch of its own, so the self-check's tiling and the destroy hold the two to each other.
 */
static inline size_t obj_type_size(uint8_t type, uint32_t slots)
{
    switch (type) {
    case CAP_POOL:
        return sizeof(struct pool);
    case CAP_CAPTABLE:
        return sizeof(struct captable) + slots * sizeof(struct cap);
    case CAP_PROCESS:
        return process_size(slots);
    case CAP_THREAD:
        return thread_size(slots);
    case CAP_NOTIFICATION:
        return sizeof(struct notification);
    case CAP_IRQ:
        return sizeof(struct irq);
    case CAP_ROUTE:
        return sizeof(struct route);
    default:
        kpanic("size of an unknown type");
    }
}

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
        return process_size(((const struct process *)obj)->regions);
    case CAP_THREAD:
        return thread_size(((const struct thread *)obj)->places);
    case CAP_NOTIFICATION:
        return sizeof(struct notification);
    case CAP_IRQ:
        return sizeof(struct irq);
    case CAP_ROUTE:
        return sizeof(struct route);
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

/*
 * cap_lookup, cap_lookup_typed, cap_slot_free and cap_store for a slot number of a thread's,
 * in whichever of its tables the number points into, see SLOT_TABLE_SHIFT;
 * KERR_INVALID_CAP when it points where its process mounts no table, or the thread has no process.
 */
static inline int slot_lookup(const struct thread *t, uint32_t slot, struct cap *out)
{
    struct captable *table = slot_table(t, slot);
    return table != NULL ? cap_lookup(table, slot_index(slot), out) : KERR_INVALID_CAP;
}
static inline int slot_lookup_typed(const struct thread *t, uint32_t slot, uint8_t type, uint8_t rights,
                                    struct cap *out)
{
    struct captable *table = slot_table(t, slot);
    return table != NULL ? cap_lookup_typed(table, slot_index(slot), type, rights, out) : KERR_INVALID_CAP;
}
static inline int slot_vacant(const struct thread *t, uint32_t slot)
{
    const struct captable *table = slot_table(t, slot);
    return table != NULL ? cap_slot_free(table, slot_index(slot)) : KERR_INVALID_CAP;
}
static inline int slot_store(const struct thread *t, uint32_t slot, const struct cap *cap, struct cap *parent)
{
    struct captable *table = slot_table(t, slot);
    return table != NULL ? cap_store(table, slot_index(slot), cap, parent) : KERR_INVALID_CAP;
}

/* Make an already filled node, an installed region, a child of parent, or a root when parent is NULL. */
void cap_attach(struct cap *parent, struct cap *node);

/*
 * Move a filled slot of a table into an empty one, from and to distinct:
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
 * It ends early, and true, once through, the running thread's slot number the call was made through,
 * no longer names a filled slot: the revoke took the thread's process, the table the number points into,
 * or the capability in the slot.
 * The caller has checked that no pool it destroys takes any of the three.
 */
bool cap_revoke_below(struct cap *node, uint32_t through, bool preempt);

/*
 * One step of a revoke below a pool's own node, where no other pool's node lies:
 * clear the first node below root, whose children take its place.
 * False when nothing is left.
 */
bool cap_revoke_step(struct cap *root);

/* Whether a preemptible walk that has done a step stops before its next; see intr_pending and OP_TEST_PREEMPT. */
bool cap_stop_here(bool preempt);

/*
 * OP_TEST_PREEMPT's count of the places a walk may stop before the one it stops at, 0 when disarmed;
 * the walk that stops there marks its core's preempt_stopped.
 */
extern uint32_t preempt_countdown;

/* Build a capability to an object; one to a notification may signal every bit. */
struct cap cap_to_object(struct obj_header *obj, uint8_t rights);

/* Build a frame capability. */
struct cap cap_to_frame(uint32_t base, uint32_t size, uint8_t rights);

/* Build an Untyped capability, over the whole of a block. */
struct cap cap_to_untyped(uint32_t base, uint32_t size, uint8_t rights);

/* Build an interrupt line capability: count lines from first. */
struct cap cap_to_lines(uint32_t first, uint32_t count, uint8_t rights);

/* Build a Time capability: count units from first. */
struct cap cap_to_time(uint32_t first, uint32_t count, uint8_t rights);

/* Build a capability to a router's channels: count from first. */
struct cap cap_to_routes(uint32_t first, uint32_t count, uint8_t rights);

/* The object behind a capability that names one. */
static inline struct obj_header *cap_object(const struct cap *cap)
{
    return p2v(cap_addr(cap));
}

/* process.c */

/* Install a region into a slot, as a node below parent, or a root when parent is NULL. */
int process_install(struct process *proc, unsigned slot,
                    uint32_t base, uint32_t size, uint8_t rights, struct cap *parent);
/* Clear a region slot, which leaves the derivation tree as cap_clear does. */
int process_uninstall(struct process *proc, unsigned slot);
/* Mount a table into a table slot, as a node below parent, the capability it was named by. */
int process_mount(struct process *proc, uint32_t index, const struct cap *table, struct cap *parent);
/* Clear a table slot, a leaf, so in a step. */
int process_unmount(struct process *proc, uint32_t index);
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

/*
 * Set by OP_TEST_TRACE: print each system call and run the self-check after it.
 * Without the self-check, SELFCHECK=0, the call is refused, and the tracing compiles away.
 */
#if SELFCHECK
extern bool debug_trace;
#else
static const bool debug_trace = false;
#endif

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
/* The board's devices follow, BOOT_DEVICES of them, in the order of their slots. */
#define GRANT_DEVICES 5
#define GRANTED_RANGES (GRANT_DEVICES + BOOT_DEVICES)
extern struct granted_range boot_granted[GRANTED_RANGES];

/*
 * Build the root task where the board's layout says:
 * its objects in the boot pool, which with its regions is made of its BOOT_CAP_ROOT_RAM,
 * and the free RAM its BOOT_CAP_FREE_RAM Untyped.
 */
struct thread *boot_create_root(void);

#endif
