#ifndef RVUOS_HOST_OPS_H
#define RVUOS_HOST_OPS_H

/*
 * What each record's fields mean, for the mutator and for printing a record by name.
 * Only the mutator's: an entry that is wrong draws worse arguments for its operation
 * and hides nothing, since every record still reaches the kernel as it is.
 */

#include <stdint.h>

#include "rvuos/replay.h"

/* What an argument register holds. */
enum arg_kind {
    ARG_NONE,
    ARG_WORD,         /* any word: a byte to log, a halt code, a program counter, a word to store */
    ARG_DST,          /* an empty slot of the caller's table, which receives what the operation makes */
    ARG_DST_INVOKED,  /* an empty slot of the invoked table, which receives a copy of a2 */
    ARG_SLOT_INVOKED, /* a filled slot of the invoked table */
    ARG_ANY,          /* a filled slot of the caller's table */
    ARG_FRAME,        /* ... holding a frame, and so on */
    ARG_POOL,
    ARG_NOTIFICATION,
    ARG_THREAD,
    ARG_TYPE,         /* an object type: what a retype or an allocation makes */
    ARG_ALLOC,        /* what an allocation's type wants: a count of slots, a table's slot, a process's slot */
    ARG_RIGHTS,
    ARG_REGION,       /* a region slot of the process */
    ARG_OFFSET,       /* into a frame, a range of lines or of units, or a thread's registers */
    ARG_SIZE,         /* a frame's, a power of two */
    ARG_COUNT,        /* of lines or units */
    ARG_BITS,
    ARG_LINE,
    ARG_DELAY,        /* microseconds */
    ARG_FLAGS,        /* OP_IRQ_SET's */
    ARG_STEPS,        /* OP_DEBUG_PREEMPT's */
    ARG_ADDRESS,      /* of a load or a store */
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
    default:
        return CAP_NONE;
    }
}

/* What an operation puts in its ARG_DST slots when not a type of its own: the type a1 names. */
#define MAKES_A1 0xff

/* One operation: its name, the type of the capability it is invoked on, what it makes, and a1..a3. */
struct op_info {
    const char *name;
    uint8_t cap;
    uint8_t makes;
    uint8_t arg[3];
};

#define OP(op, cap, makes, a1, a2, a3) [op] = { #op, cap, makes, { a1, a2, a3 } }

static const struct op_info op_table[OP_COUNT] = {
    OP(OP_DEBUG_PUTC, CAP_DEBUG, 0, ARG_WORD, ARG_NONE, ARG_NONE),
    OP(OP_DEBUG_HALT, CAP_DEBUG, 0, ARG_WORD, ARG_NONE, ARG_NONE),
    OP(OP_CAP_COPY, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_RIGHTS),
    OP(OP_CAP_DELETE, CAP_CAPTABLE, 0, ARG_SLOT_INVOKED, ARG_NONE, ARG_NONE),
    OP(OP_FRAME_CARVE, CAP_FRAME, CAP_FRAME, ARG_OFFSET, ARG_SIZE, ARG_DST),
    OP(OP_UNTYPED_RETYPE, CAP_UNTYPED, MAKES_A1, ARG_TYPE, ARG_DST, ARG_NONE),
    OP(OP_POOL_ALLOC, CAP_POOL, MAKES_A1, ARG_TYPE, ARG_DST, ARG_ALLOC),
    OP(OP_PROCESS_INSTALL, CAP_PROCESS, 0, ARG_REGION, ARG_FRAME, ARG_RIGHTS),
    OP(OP_PROCESS_UNINSTALL, CAP_PROCESS, 0, ARG_REGION, ARG_NONE, ARG_NONE),
    OP(OP_DEBUG_TRACE, CAP_DEBUG, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_FRAME_INFO, CAP_FRAME, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_CONFIGURE, CAP_THREAD, 0, ARG_WORD, ARG_WORD, ARG_WORD),
    OP(OP_THREAD_RESUME, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_NOTIFY_SIGNAL, CAP_NOTIFICATION, 0, ARG_BITS, ARG_NONE, ARG_NONE),
    OP(OP_NOTIFY_WAIT, CAP_NOTIFICATION, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_DEBUG_TICK, CAP_DEBUG, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_CAP_MOVE, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_NONE),
    OP(OP_IRQ_CARVE, CAP_IRQ_LINE, CAP_IRQ_LINE, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_IRQ_BIND, CAP_IRQ_LINE, CAP_IRQ, ARG_POOL, ARG_NOTIFICATION, ARG_DST),
    OP(OP_IRQ_SET, CAP_IRQ, 0, ARG_BITS, ARG_DELAY, ARG_FLAGS),
    OP(OP_DEBUG_IRQ, CAP_DEBUG, 0, ARG_LINE, ARG_NONE, ARG_NONE),
    OP(OP_CAP_REVOKE, CAP_CAPTABLE, 0, ARG_SLOT_INVOKED, ARG_NONE, ARG_NONE),
    OP(OP_CAP_DERIVE, CAP_CAPTABLE, 0, ARG_DST_INVOKED, ARG_ANY, ARG_RIGHTS),
    OP(OP_CLOCK_INFO, CAP_CLOCK, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_CLOCK_FRAME, CAP_CLOCK, CAP_FRAME, ARG_DST, ARG_NONE, ARG_NONE),
    OP(OP_UNTYPED_INFO, CAP_UNTYPED, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_TIME_CARVE, CAP_TIME, CAP_TIME, ARG_OFFSET, ARG_COUNT, ARG_DST),
    OP(OP_TIME_BIND, CAP_TIME, 0, ARG_THREAD, ARG_OFFSET, ARG_COUNT),
    OP(OP_UNTYPED_SPLIT, CAP_UNTYPED, CAP_UNTYPED, ARG_DST, ARG_DST, ARG_NONE),
    OP(OP_THREAD_WATCH, CAP_THREAD, 0, ARG_NOTIFICATION, ARG_BITS, ARG_NONE),
    OP(OP_DEBUG_PREEMPT, CAP_DEBUG, 0, ARG_STEPS, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_FAULT, CAP_THREAD, 0, ARG_NONE, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_READ_REG, CAP_THREAD, 0, ARG_OFFSET, ARG_NONE, ARG_NONE),
    OP(OP_THREAD_WRITE_REG, CAP_THREAD, 0, ARG_OFFSET, ARG_WORD, ARG_NONE),
};

#undef OP

_Static_assert(OP_COUNT == 36 && OP_THREAD_WRITE_REG == OP_COUNT - 1, "a new operation needs an entry in op_table");

/* The loads and stores the driver makes itself, which invoke no capability. */
static const struct op_info op_access[2] = {
    { "REPLAY_OP_LOAD", CAP_NONE, 0, { ARG_ADDRESS, ARG_NONE, ARG_NONE } },
    { "REPLAY_OP_STORE", CAP_NONE, 0, { ARG_ADDRESS, ARG_WORD, ARG_NONE } },
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
