#ifndef RVUOS_HOST_OPS_H
#define RVUOS_HOST_OPS_H

/*
 * What each record's fields mean, for the mutator and for printing a record by name,
 * and, to host/history.c, what each operation is invoked on and which arguments it leaves unread,
 * written apart from the kernel's op_shapes so that each holds the other.
 */

#include <stdint.h>

#include "rvuos/replay.h"

/* What an argument register holds. */
enum arg_kind {
    ARG_NONE,
    ARG_WORD,         /* any word: bytes to log, a halt code, a program counter, a word to store */
    ARG_DST,          /* an empty slot of the caller's tables, which receives what the operation makes */
    ARG_DST_INVOKED,  /* an empty slot of the invoked table, which receives a copy of a2 */
    ARG_SLOT_INVOKED, /* a filled slot of the invoked table */
    ARG_ANY,          /* a filled slot of the caller's tables */
    ARG_FRAME,        /* ... holding a frame, and so on */
    ARG_POOL,
    ARG_NOTIFICATION,
    ARG_THREAD,
    ARG_CAPTABLE,
    ARG_TYPE,         /* an object type: what a retype or an allocation makes */
    ARG_ALLOC,        /* what an allocation's type wants: a count of slots, a table's slot, a process's slot */
    ARG_SLOTS,        /* a count of slots for a table, of region slots for a process, of places for a thread,
                         and 0 for another type: OP_POOL_INFO's a3 */
    ARG_RIGHTS,
    ARG_REGION,       /* a region slot of the process */
    ARG_MOUNT,        /* a table slot of the process */
    ARG_OFFSET,       /* into a frame, a range of lines or of units, or a thread's registers */
    ARG_SIZE,         /* a frame's, a power of two */
    ARG_COUNT,        /* of lines or units */
    ARG_BITS,
    ARG_LINE,
    ARG_DELAY,        /* microseconds */
    ARG_FLAGS,        /* OP_IRQ_SET's */
    ARG_STEPS,        /* OP_TEST_PREEMPT's */
    ARG_ADDRESS,      /* of a load or a store */
    ARG_END,          /* a Route's end, ROUTE_EVENT or ROUTE_TASK, OP_ROUTE_SET's on or off, or a Dma's end */
    ARG_NAME,         /* an end within a frame, as the board names it */
    ARG_SPAN,         /* a wait's: 0 to keep, else a power of two of counts */
};

/* The type a source slot holds, CAP_NONE for ARG_ANY. */
static inline uint8_t arg_type(uint8_t kind)
{
    switch (kind) {
    case ARG_FRAME:
        return CAP_FRAME;
    case ARG_POOL:
        return CAP_POOL;
    case ARG_NOTIFICATION:
        return CAP_NOTIFICATION;
    case ARG_THREAD:
        return CAP_THREAD;
    case ARG_CAPTABLE:
        return CAP_CAPTABLE;
    default:
        return CAP_NONE;
    }
}

/* What an operation puts in its ARG_DST slots when not a type of its own: the type a1 names. */
#define MAKES_A1 0xff

/* The type of the capability an operation every type takes is invoked on, which no capability has. */
#define ON_ANY 0xfe

/*
 * One operation: its name, the type of the capability it is invoked on, what it makes, and a1..a3,
 * and whether it describes and changes nothing, which the mutator draws apart from the rest; see host/mutator.c.
 */
struct op_info {
    const char *name;
    uint8_t cap;
    uint8_t makes;
    uint8_t arg[3];
    uint8_t describes;
};

#define OP(op, cap, makes, a1, a2, a3) [op] = { #op, cap, makes, { a1, a2, a3 }, 0 }
#define DESCRIBE(op, cap, a1, a2, a3) [op] = { #op, cap, 0, { a1, a2, a3 }, 1 }

static const struct op_info op_table[OP_COUNT] = {
    OP(OP_PRINT, CAP_PRINT, 0, ARG_WORD, ARG_WORD, ARG_WORD),
    OP(OP_HALT, CAP_HALT, 0, ARG_WORD, ARG_NONE, ARG_NONE),
    OP(OP_CAP_COPY, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_RIGHTS),
    OP(OP_CAP_DELETE, CAP_CAPTABLE, 0, ARG_SLOT_INVOKED, ARG_NONE, ARG_NONE),
    OP(OP_FRAME_CARVE, CAP_FRAME, CAP_FRAME, ARG_OFFSET, ARG_SIZE, ARG_DST),
    OP(OP_UNTYPED_RETYPE, CAP_UNTYPED, MAKES_A1, ARG_TYPE, ARG_DST, ARG_NONE),
    OP(OP_POOL_ALLOC, CAP_POOL, MAKES_A1, ARG_TYPE, ARG_DST, ARG_ALLOC),
    OP(OP_PROCESS_INSTALL, CAP_PROCESS, 0, ARG_REGION, ARG_FRAME, ARG_RIGHTS),
    OP(OP_PROCESS_UNINSTALL, CAP_PROCESS, 0, ARG_REGION, ARG_NONE, ARG_NONE),
    OP(OP_TEST_TRACE, CAP_TEST, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_CONFIGURE, CAP_THREAD, 0, ARG_WORD, ARG_WORD, ARG_WORD),
    OP(OP_THREAD_RESUME, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_NOTIFY_SIGNAL, CAP_NOTIFICATION, 0, ARG_BITS, ARG_NONE, ARG_NONE),
    OP(OP_NOTIFY_WAIT, CAP_NOTIFICATION, 0, ARG_SPAN, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_STOP, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_TEST_TICK, CAP_TEST, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_CAP_MOVE, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_NONE),
    OP(OP_IRQ_CARVE, CAP_IRQ_LINE, CAP_IRQ_LINE, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_IRQ_BIND, CAP_IRQ_LINE, CAP_IRQ, ARG_POOL, ARG_NOTIFICATION, ARG_DST),
    OP(OP_IRQ_SET, CAP_IRQ, 0, ARG_BITS, ARG_DELAY, ARG_FLAGS),
    OP(OP_TEST_IRQ, CAP_TEST, 0, ARG_LINE, ARG_NONE, ARG_NONE),
    OP(OP_CAP_REVOKE, CAP_CAPTABLE, 0, ARG_SLOT_INVOKED, ARG_NONE, ARG_NONE),
    OP(OP_CAP_DERIVE, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_RIGHTS),
    OP(OP_CLOCK_READ, CAP_CLOCK, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_TIME_CARVE, CAP_TIME, CAP_TIME, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_TIME_BIND, CAP_TIME, 0, ARG_THREAD, ARG_OFFSET, ARG_COUNT),
    OP(OP_UNTYPED_SPLIT, CAP_UNTYPED, CAP_UNTYPED, ARG_DST, ARG_DST, ARG_NONE),
    OP(OP_THREAD_WATCH, CAP_THREAD, 0, ARG_NOTIFICATION, ARG_BITS, ARG_NONE),
    OP(OP_TEST_PREEMPT, CAP_TEST, 0, ARG_STEPS, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_FAULT, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_READ_REG, CAP_THREAD, 0, ARG_OFFSET, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_WRITE_REG, CAP_THREAD, 0, ARG_OFFSET, ARG_WORD, ARG_NONE),
    OP(OP_NOTIFY_CARVE, CAP_NOTIFICATION, CAP_NOTIFICATION, ARG_BITS, ARG_DST, ARG_NONE),
    OP(OP_CLOCK_WATCHDOG, CAP_CLOCK, 0, ARG_DELAY, ARG_NONE, ARG_NONE),
    OP(OP_NOTIFY_LEND, CAP_NOTIFICATION, 0, ARG_THREAD, ARG_SPAN, ARG_NONE),
    OP(OP_PROCESS_MOUNT, CAP_PROCESS, 0, ARG_MOUNT, ARG_CAPTABLE, ARG_NONE),
    OP(OP_PROCESS_UNMOUNT, CAP_PROCESS, 0, ARG_MOUNT, ARG_NONE, ARG_NONE),
    DESCRIBE(OP_CAP_INFO, ON_ANY, ARG_NONE, ARG_NONE, ARG_NONE),
    DESCRIBE(OP_POOL_INFO, CAP_POOL, ARG_TYPE, ARG_NONE, ARG_SLOTS),
    OP(OP_ROUTE_CARVE, CAP_ROUTE_LINE, CAP_ROUTE_LINE, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_ROUTE_BIND, CAP_ROUTE_LINE, CAP_ROUTE, ARG_POOL, ARG_DST, ARG_NONE),
    OP(OP_ROUTE_END, CAP_ROUTE, 0, ARG_END, ARG_FRAME, ARG_NAME),
    OP(OP_ROUTE_SET, CAP_ROUTE, 0, ARG_END, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_OVERRUNS, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_DMA_CARVE, CAP_DMA_LINE, CAP_DMA_LINE, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_DMA_BIND, CAP_DMA_LINE, CAP_DMA, ARG_POOL, ARG_DST, ARG_NONE),
    OP(OP_DMA_END, CAP_DMA, 0, ARG_END, ARG_FRAME, ARG_NAME),
    OP(OP_DMA_START, CAP_DMA, 0, ARG_OFFSET, ARG_SIZE, ARG_NONE),
    OP(OP_DMA_STOP, CAP_DMA, 0, ARG_NONE, ARG_NONE, ARG_NONE),
};

#undef OP
#undef DESCRIBE

/* The assert sees only the highest code; LLVMFuzzerInitialize holds every code below it to an entry. */
_Static_assert(OP_COUNT == 53 && OP_DMA_STOP == OP_COUNT - 1, "a new operation needs an entry in op_table");

/* A code below OP_COUNT that is no operation any more, see OP_COUNT, which the kernel refuses as any other. */
static inline int op_retired(unsigned op)
{
    return op == 11 || op == 26 || op == 27;
}

/* The loads and stores the driver makes itself, which invoke no capability. */
static const struct op_info op_access[2] = {
    { "REPLAY_OP_LOAD", CAP_NONE, 0, { ARG_ADDRESS, ARG_NONE, ARG_NONE }, 0 },
    { "REPLAY_OP_STORE", CAP_NONE, 0, { ARG_ADDRESS, ARG_WORD, ARG_NONE }, 0 },
};

/* The entry for an operation code, or NULL for one the kernel refuses. */
static inline const struct op_info *op_info(uint8_t op)
{
    if (op == REPLAY_OP_LOAD || op == REPLAY_OP_STORE) {
        return &op_access[op - REPLAY_OP_LOAD];
    }
    return op < OP_COUNT && op_table[op].name != 0 ? &op_table[op] : 0;
}

#endif
