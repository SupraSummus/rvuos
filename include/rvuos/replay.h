#ifndef RVUOS_REPLAY_H
#define RVUOS_REPLAY_H

/*
 * The state the replay driver builds before it turns tracing on.
 *
 * The host build boots the kernel without running any user code,
 * so it must reach exactly the kernel state the driver reaches on QEMU
 * or the two transcripts diverge for reasons that are not kernel bugs.
 * Keeping the setup here, as records both sides perform,
 * is what stops user/fuzzdrv.c and host/shim.c from drifting apart.
 *
 * The driver runs a second thread
 * so that a record which blocks one thread has somewhere to go.
 * Both threads take records from the same cursor,
 * so the order of the calls follows the kernel's own choice
 * of what runs, on the target and on the host alike.
 *
 * A record may name the thread that performs it, its actor.
 * A thread that finds the next record is another thread's
 * passes the processor on with OP_DEBUG_TICK
 * and looks at the cursor when the processor comes back:
 * moved, some thread took the record;
 * not moved, the processor went round and none took it,
 * so the thread that started the round performs the record itself.
 * The setup binds every thread to the root thread's share,
 * where going round visits every runnable thread, so the actor could not run;
 * once a record has moved a thread to a share of its own,
 * the round may come back before it visited them all.
 * host_event in host/shim.c performs the same calls from the kernel's state,
 * so the passing is in both transcripts.
 *
 * A third thread shares the second one's process and starts stopped,
 * so a record resumes it when two threads should wait at once
 * and a third is needed to wake them.
 *
 * The second thread has a process, a table and a pool of its own,
 * so that every switch between the threads reloads the PMP
 * and a pool the second thread creates lies below its own in the tree,
 * where a record on the first thread can destroy both at once.
 * The second table holds a copy of every slot the first one holds,
 * so a record means the same on whichever thread performs it,
 * with one exception: an Untyped is derived, never copied,
 * so the second thread's BOOT_CAP_FREE_RAM is an Untyped of its own, cut from the first one's.
 * Revoking the first one's destroys the second thread's pool, and what it made of its own.
 */

#include "rvuos/abi.h"

/* Driver threads a record may name as its actor: 1 is the root thread, 2 the second, 3 the third. */
#define REPLAY_THREADS 3

/* Whether thread `me` passes the record on rather than performing it. */
static inline int replay_passes(const struct replay_record *r, unsigned me)
{
    return r->actor != 0 && r->actor <= REPLAY_THREADS && r->actor != me;
}

/*
 * Region slots. The input is mapped only while the driver copies the records out,
 * so the records run in a process that maps what the driver needs from then on and nothing else.
 */
#define REPLAY_REGION_SLOT 7
#define REPLAY_UART_SLOT 5
#define REPLAY_LOG_SLOT 6

/* Capability slots the setup fills, above the boot capabilities, in both tables. */
#define REPLAY_CAP_NOTIFY  16
#define REPLAY_CAP_THREAD  17 /* the second thread */
#define REPLAY_CAP_POOL    18 /* the second thread's pool */
#define REPLAY_CAP_TABLE   19 /* the second thread's table */
#define REPLAY_CAP_PROCESS 20 /* the second thread's process */
#define REPLAY_CAP_SCRATCH 21 /* the second thread's Untyped on its way over; empty afterwards */
_Static_assert(REPLAY_CAP_NOTIFY == BOOT_CAP_COUNT, "the setup's slots follow the boot capabilities");
/* The third thread, stopped until a record resumes it: the last slot, which the corpus leaves alone. */
#define REPLAY_CAP_THIRD   (REPLAY_TABLE_SLOTS - 1)

/*
 * The second thread's pool, the first block of the free RAM, with as many slots as the root task has,
 * and its Untyped, the next block of its size.
 */
#define REPLAY_POOL_SIZE    0x1000u
#define REPLAY_TABLE_SLOTS  64
#define REPLAY_UNTYPED_SIZE 0x100000u

/*
 * The second and third threads' stacks, in the root task's data region below the root's.
 * host/shim.c checks that they lie inside that region.
 */
#define REPLAY_THREAD_SP 0x80208000u
#define REPLAY_THIRD_SP  0x8020c000u

#define REPLAY_COPY(slot) { OP_CAP_COPY, 0, REPLAY_CAP_TABLE, slot, slot, RIGHT_ALL }

/*
 * The threads' entry points are the one value the two builds differ on:
 * the driver patches OP_THREAD_CONFIGURE with the address of its record loop,
 * and the host leaves the zero, because the kernel stores
 * a thread's program counter without looking at it
 * and the host never fetches an instruction.
 */
static const struct replay_record replay_prologue[] = {
    { OP_PROCESS_INSTALL, 0, BOOT_CAP_PROCESS, REPLAY_REGION_SLOT, BOOT_CAP_INPUT, RIGHT_R },
    { OP_POOL_ALLOC, 0, BOOT_CAP_POOL, CAP_NOTIFICATION, REPLAY_CAP_NOTIFY, 0 },
    /* The second thread's pool, table, process and thread. */
    { OP_UNTYPED_RETYPE, 0, BOOT_CAP_FREE_RAM, CAP_POOL, REPLAY_POOL_SIZE, REPLAY_CAP_POOL },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_CAPTABLE, REPLAY_CAP_TABLE, REPLAY_TABLE_SLOTS },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_PROCESS, REPLAY_CAP_PROCESS, REPLAY_CAP_TABLE },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_THREAD, REPLAY_CAP_THREAD, REPLAY_CAP_PROCESS },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_THREAD, REPLAY_CAP_THIRD, REPLAY_CAP_PROCESS },
    /* It runs the same code on the same data as the first thread; the input it never reads. */
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, 1, BOOT_CAP_DATA, RIGHT_R | RIGHT_W },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, REPLAY_UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, REPLAY_LOG_SLOT, BOOT_CAP_LOG, RIGHT_R | RIGHT_W },
    /* Its own Untyped, derived into its table and then below the free RAM once the scratch slot goes. */
    { OP_UNTYPED_RETYPE, 0, BOOT_CAP_FREE_RAM, CAP_UNTYPED, REPLAY_UNTYPED_SIZE, REPLAY_CAP_SCRATCH },
    { OP_CAP_DERIVE, 0, REPLAY_CAP_TABLE, BOOT_CAP_FREE_RAM, REPLAY_CAP_SCRATCH, RIGHT_ALL },
    { OP_CAP_DELETE, 0, BOOT_CAP_CAPTABLE, REPLAY_CAP_SCRATCH, 0, 0 },
    /* Its table mirrors the first one, the boot capabilities included. */
    REPLAY_COPY(BOOT_CAP_CAPTABLE),
    REPLAY_COPY(BOOT_CAP_PROCESS),
    REPLAY_COPY(BOOT_CAP_THREAD),
    REPLAY_COPY(BOOT_CAP_POOL),
    REPLAY_COPY(BOOT_CAP_DEBUG),
    REPLAY_COPY(BOOT_CAP_CODE),
    REPLAY_COPY(BOOT_CAP_DATA),
    REPLAY_COPY(BOOT_CAP_INPUT),
    REPLAY_COPY(BOOT_CAP_IRQ_LINES),
    REPLAY_COPY(BOOT_CAP_UART),
    REPLAY_COPY(BOOT_CAP_LOG),
    REPLAY_COPY(BOOT_CAP_TIMER_LINES),
    REPLAY_COPY(BOOT_CAP_CLOCK),
    REPLAY_COPY(BOOT_CAP_SHARES),
    REPLAY_COPY(REPLAY_CAP_NOTIFY),
    REPLAY_COPY(REPLAY_CAP_THREAD),
    REPLAY_COPY(REPLAY_CAP_POOL),
    REPLAY_COPY(REPLAY_CAP_TABLE),
    REPLAY_COPY(REPLAY_CAP_PROCESS),
    REPLAY_COPY(REPLAY_CAP_THIRD),
    /* Every thread runs on the root thread's share, so the three take turns as one ring. */
    { OP_SHARE_BIND, 0, BOOT_CAP_SHARES, REPLAY_CAP_THREAD, 0, 0 },
    { OP_SHARE_BIND, 0, BOOT_CAP_SHARES, REPLAY_CAP_THIRD, 0, 0 },
    { OP_THREAD_CONFIGURE, 0, REPLAY_CAP_THREAD, 0, REPLAY_THREAD_SP, 0 },
    { OP_THREAD_CONFIGURE, 0, REPLAY_CAP_THIRD, 0, REPLAY_THIRD_SP, 0 },
};

#define REPLAY_PROLOGUE_COUNT \
    (sizeof(replay_prologue) / sizeof(replay_prologue[0]))

/*
 * Performed once the driver has copied the records out, still untraced.
 * The kernel has no console, so the driver carries the log to the UART itself
 * after every record, with no system call: it reads the ring and writes the mark
 * in the log's header; host_event does the same to the kernel's state.
 * The halt writes out what the last record left, see DESIGN.md, "The kernel log".
 */
static const struct replay_record replay_after_input[] = {
    { OP_PROCESS_UNINSTALL, 0, BOOT_CAP_PROCESS, REPLAY_REGION_SLOT, 0, 0 },
    { OP_PROCESS_INSTALL, 0, BOOT_CAP_PROCESS, REPLAY_UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W },
    { OP_PROCESS_INSTALL, 0, BOOT_CAP_PROCESS, REPLAY_LOG_SLOT, BOOT_CAP_LOG, RIGHT_R | RIGHT_W },
};

#define REPLAY_AFTER_INPUT_COUNT \
    (sizeof(replay_after_input) / sizeof(replay_after_input[0]))

/*
 * Performed by both builds once tracing is on, as the first traced record.
 * Tracing turns the tick's preemption off,
 * so from then on the second thread runs only when the first one blocks,
 * which is after the records are in place.
 */
static const struct replay_record replay_start =
    { OP_THREAD_RESUME, 0, REPLAY_CAP_THREAD, 0, 0, 0 };

/* What a thread performs to pass a record on. */
static const struct replay_record replay_tick =
    { OP_DEBUG_TICK, 0, BOOT_CAP_DEBUG, 0, 0, 0 };

#endif
