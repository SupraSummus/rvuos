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
 * The setup binds every thread, the root thread too, to no units and spare time,
 * where they take turns as one queue and going round visits every runnable thread,
 * so the actor could not run;
 * once a record has bound a thread to units, and so to time the others do not have,
 * the round may come back before it visited them all.
 * host_event in host/shim.c performs the same calls from the kernel's state,
 * so the passing is in both transcripts.
 *
 * A third thread shares the second one's process and starts stopped,
 * so a record resumes it when two threads should wait at once
 * and a third is needed to wake them.
 * It lives in the first thread's pool, not its process's,
 * so a destroy of the second thread's pool leaves it stopped without a process.
 *
 * The second thread has a process, a table and a pool of its own,
 * so that every switch between the threads reloads the PMP
 * and a pool the second thread creates lies below its own in the tree,
 * where a record on the first thread can destroy both at once.
 * The second table holds a copy of every slot the first one holds,
 * so a record means the same on whichever thread performs it,
 * with exceptions, since an Untyped is derived, never copied.
 * Each thread's REPLAY_CAP_RAM is memory of its own, free when the records begin:
 * the first thread's is the lower half of the free RAM,
 * and the second thread's is derived from the top quarter, the quarter below being its pool.
 * The halves between go, so all three hang right below the free RAM,
 * and the second table's BOOT_CAP_FREE_RAM and BOOT_CAP_ROOT_RAM are empty.
 * Revoking the first one's free RAM destroys the second thread's pool, and what it made of its own.
 * The first thread lives in the boot pool, of which the second holds a copy,
 * so a record on the second thread can destroy the first, as a successor can its root task.
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
#define REPLAY_CAP_NOTIFY  17
#define REPLAY_CAP_THREAD  18 /* the second thread */
#define REPLAY_CAP_POOL    19 /* the second thread's pool */
#define REPLAY_CAP_TABLE   20 /* the second thread's table */
#define REPLAY_CAP_PROCESS 21 /* the second thread's process */
#define REPLAY_CAP_RAM     22 /* the thread's own memory, an Untyped */
/* The halves the second thread's pool and memory are cut from, empty afterwards. */
#define REPLAY_CAP_HALF     23 /* the upper half of the free RAM */
#define REPLAY_CAP_POOL_RAM 24 /* its lower half, the second thread's pool */
#define REPLAY_CAP_LENT     25 /* its upper half, which the second thread's memory is derived from */
_Static_assert(REPLAY_CAP_NOTIFY == BOOT_CAP_COUNT, "the setup's slots follow the boot capabilities");
/* The third thread, stopped until a record resumes it: the last slot, which the corpus leaves alone. */
#define REPLAY_CAP_THIRD   (REPLAY_TABLE_SLOTS - 1)

/* The second thread's table has as many slots as the root task's. */
#define REPLAY_TABLE_SLOTS ROOT_TABLE_SLOTS

/*
 * The second and third threads' stacks, in the root task's data region below the root's.
 * host/shim.c checks that they lie inside that region.
 */
#define REPLAY_THREAD_SP 0x80118000u
#define REPLAY_THIRD_SP  0x8011c000u

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
    /* The free RAM in halves, and the upper in halves again. */
    { OP_UNTYPED_SPLIT, 0, BOOT_CAP_FREE_RAM, REPLAY_CAP_RAM, REPLAY_CAP_HALF, 0 },
    { OP_UNTYPED_SPLIT, 0, REPLAY_CAP_HALF, REPLAY_CAP_POOL_RAM, REPLAY_CAP_LENT, 0 },
    /* The second thread's pool, table, process and thread, and the third thread in the first one's pool. */
    { OP_UNTYPED_RETYPE, 0, REPLAY_CAP_POOL_RAM, CAP_POOL, REPLAY_CAP_POOL, 0 },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_CAPTABLE, REPLAY_CAP_TABLE, REPLAY_TABLE_SLOTS },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_PROCESS, REPLAY_CAP_PROCESS, REPLAY_CAP_TABLE },
    { OP_POOL_ALLOC, 0, REPLAY_CAP_POOL, CAP_THREAD, REPLAY_CAP_THREAD, REPLAY_CAP_PROCESS },
    { OP_POOL_ALLOC, 0, BOOT_CAP_POOL, CAP_THREAD, REPLAY_CAP_THIRD, REPLAY_CAP_PROCESS },
    /* It runs the same code on the same data as the first thread; the input it never reads. */
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, 1, BOOT_CAP_DATA, RIGHT_R | RIGHT_W },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, REPLAY_UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W },
    { OP_PROCESS_INSTALL, 0, REPLAY_CAP_PROCESS, REPLAY_LOG_SLOT, BOOT_CAP_LOG, RIGHT_R | RIGHT_W },
    /*
     * Its own memory, derived into its table;
     * then the halves between go, and what they made hangs below the free RAM.
     */
    { OP_CAP_DERIVE, 0, REPLAY_CAP_TABLE, REPLAY_CAP_RAM, REPLAY_CAP_LENT, RIGHT_ALL },
    { OP_CAP_DELETE, 0, BOOT_CAP_CAPTABLE, REPLAY_CAP_HALF, 0, 0 },
    { OP_CAP_DELETE, 0, BOOT_CAP_CAPTABLE, REPLAY_CAP_POOL_RAM, 0, 0 },
    { OP_CAP_DELETE, 0, BOOT_CAP_CAPTABLE, REPLAY_CAP_LENT, 0, 0 },
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
    REPLAY_COPY(BOOT_CAP_TIME),
    REPLAY_COPY(REPLAY_CAP_NOTIFY),
    REPLAY_COPY(REPLAY_CAP_THREAD),
    REPLAY_COPY(REPLAY_CAP_POOL),
    REPLAY_COPY(REPLAY_CAP_TABLE),
    REPLAY_COPY(REPLAY_CAP_PROCESS),
    REPLAY_COPY(REPLAY_CAP_THIRD),
    /*
     * Every thread earns no unit and runs on spare time, so the three take turns as one queue
     * and every unit is left for the records to bind.
     */
    { OP_TIME_BIND, 0, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, 0 },
    { OP_TIME_BIND, 0, BOOT_CAP_TIME, REPLAY_CAP_THREAD, 0, 0 },
    { OP_TIME_BIND, 0, BOOT_CAP_TIME, REPLAY_CAP_THIRD, 0, 0 },
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
