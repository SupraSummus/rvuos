#ifndef RVUOS_ABI_H
#define RVUOS_ABI_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * The kernel ABI shared by the kernel and user programs.
 *
 * Every system call is an invocation on a capability.
 * Registers at the ecall:
 *   a7      operation code, one of OP_*
 *   a0      slot index of the invoked capability in the caller's table
 *   a1..a3  operation arguments; a4..a6 are reserved
 * Registers on return:
 *   a0      status, one of KERR_*
 *   a1..a4  results, operation specific; unchanged unless documented
 * No operation takes a pointer into user memory.
 *
 * Slots that receive a new capability are always in the caller's own table.
 * A process needs no capability to write its own table;
 * the CapTable capability exists to write another process's table.
 */

/* Status codes returned in a0. */
#define KERR_OK           0
#define KERR_INVALID_CAP  1 /* slot out of range, empty, or stale */
#define KERR_WRONG_TYPE   2 /* capability does not accept this operation */
#define KERR_NO_RIGHTS    3 /* capability lacks a right the operation needs */
#define KERR_INVALID_ARG  4
#define KERR_NO_MEMORY    5 /* pool exhausted */
#define KERR_SLOT_IN_USE  6 /* destination slot already holds a capability */
#define KERR_OVERLAP      7 /* region overlaps a pool or an installed region; line already bound */
#define KERR_LIMIT        8 /* a fixed kernel limit was hit, such as PMP entries */
#define KERR_STATE        9 /* the object is not in a state that allows this */

/* Capability and object types. */
#define CAP_NONE     0
#define CAP_REGION   1 /* physical address range; no kernel object behind it */
#define CAP_POOL     2
#define CAP_CAPTABLE 3
#define CAP_PROCESS  4
#define CAP_THREAD   5
#define CAP_DEBUG    6 /* a byte into the kernel's log, and machine halt, for bring-up */
#define CAP_NOTIFICATION 7
#define CAP_TIMER    8 /* signals a notification when a delay has passed */
#define CAP_IRQ_LINE 9 /* a range of interrupt lines; no kernel object behind it */
#define CAP_IRQ      10 /* one line bound to a notification; signals it when the line fires */

/*
 * Rights bits.
 * Region: RIGHT_R, RIGHT_W, RIGHT_X as memory permissions.
 * CapTable: RIGHT_W to copy into or delete from the table.
 * Pool: RIGHT_W to allocate objects.
 * Process, Thread: RIGHT_W to control the object.
 * Notification: RIGHT_W to signal, RIGHT_R to wait.
 * Timer: RIGHT_W to set or cancel.
 * IrqLine: RIGHT_W to bind a line.
 * Irq: RIGHT_W to set or mask.
 * Debug: any right.
 * Copying a capability can only remove rights.
 */
#define RIGHT_R 0x1
#define RIGHT_W 0x2
#define RIGHT_X 0x4
#define RIGHT_ALL 0x7

/*
 * Operations, with the capability type they apply to and their arguments.
 */

/* Debug: append one byte to the kernel's log. a1 = the byte. See BOOT_CAP_LOG. */
#define OP_DEBUG_PUTC 1
/* Debug: halt the machine. a1 = exit code. Does not return. */
#define OP_DEBUG_HALT 2
/*
 * Debug: turn on tracing and self-checking.
 * From then on the kernel prints every system call with its status
 * and runs the invariant checker after each one.
 * The timer tick stops preempting,
 * because a transcript the host build must reproduce
 * cannot contain a switch that lands between two instructions.
 * It cannot be turned off again, so a traced program cannot hide.
 */
#define OP_DEBUG_TRACE 10
/*
 * Debug: what the timer tick does, on request.
 * Time moves by one tick, every Timer that is due signals,
 * the processor goes to the next runnable thread in the round
 * and the caller stays ready.
 * Works while tracing is on, unlike the tick itself;
 * see DESIGN.md, "Verification".
 */
#define OP_DEBUG_TICK 17
/*
 * Debug: what a device interrupt does, on request. a1 = the line.
 * The Irq armed on the line masks it and signals its bits,
 * exactly as the interrupt would; the controller is not consulted.
 * Fails with KERR_STATE when nothing is armed on the line,
 * which is when the controller would not raise it either,
 * and with KERR_INVALID_ARG for a line the controller does not have,
 * LOG_IRQ_LINE among them: the log's level is the log's, not a record's.
 * It is how a replay fires an interrupt, since the host build has no devices;
 * see DESIGN.md, "Verification".
 */
#define OP_DEBUG_IRQ 22

/*
 * CapTable (RIGHT_W): copy a capability from the caller's table.
 * a1 = destination slot in the invoked table,
 * a2 = source slot in the caller's table,
 * a3 = rights mask applied to the copy.
 */
#define OP_CAP_COPY 3
/* CapTable (RIGHT_W): clear a slot. a1 = slot. */
#define OP_CAP_DELETE 4

/*
 * Region: describe it. Returns a1 = base, a2 = size, a3 = rights,
 * a4 = the PMP grain in bytes, a power of two of at least 4,
 * the same for every region on the machine.
 * Every region's base and size are multiples of it.
 */
#define OP_REGION_INFO 11
/*
 * Region: derive a smaller region with the same rights.
 * a1 = offset from the region base, a2 = size, a3 = destination slot.
 * Offset and size must be multiples of the PMP grain,
 * so that the new region can be installed exactly as it is.
 */
#define OP_REGION_CARVE 5
/*
 * Region (RIGHT_R and RIGHT_W): hand the memory to the kernel as a pool.
 * a1 = destination slot for the Pool capability.
 * The invoked slot is cleared.
 * The new pool lies below the pool the calling thread lives in
 * and is destroyed with it.
 * Fails with KERR_OVERLAP if the range overlaps an existing pool
 * or a region installed in any process.
 */
#define OP_REGION_TO_POOL 6

/*
 * Pool (RIGHT_W): allocate a kernel object.
 * a1 = object type, a2 = destination slot,
 * a3 = type specific:
 *   CAP_CAPTABLE  the number of slots,
 *   CAP_PROCESS   the slot of the CapTable capability the process will use,
 *   CAP_THREAD    the slot of the Process capability the thread will run in,
 *   CAP_NOTIFICATION  unused,
 *   CAP_TIMER     the slot of the Notification capability the timer signals,
 *                 which needs RIGHT_W.
 * A structural parent is not checked on every use,
 * so an object must be allocated from the same pool as its parent;
 * CAP_PROCESS, CAP_THREAD and CAP_TIMER fail with KERR_INVALID_ARG otherwise.
 */
#define OP_POOL_ALLOC 7
/*
 * Pool (RIGHT_W): destroy the pool, every object in it,
 * every pool created by a thread living in it, recursively,
 * and every capability anywhere that names one of those objects.
 * a1 = destination slot for a Region capability to the memory
 * of the invoked pool, with the rights the region carried
 * when it became a pool.
 * The invoked slot may be the destination.
 * The memory of the pools below comes back through no new capability:
 * region capabilities for it that were held elsewhere work again.
 * Fails with KERR_STATE if the calling thread lives in the pool
 * or in one below it.
 * Threads waiting on a notification in a destroyed pool
 * are woken with KERR_INVALID_CAP and no bits.
 */
#define OP_POOL_DESTROY 16

/*
 * Process (RIGHT_W): install a region into one of the process's region slots.
 * a1 = region slot index, a2 = Region capability slot in the caller's table,
 * a3 = rights to install, a subset of the region's rights.
 * RIGHT_W without RIGHT_R is rejected with KERR_INVALID_ARG,
 * because PMP reserves that encoding.
 * Fails with KERR_OVERLAP if the range overlaps a pool
 * or another region installed in the same process.
 */
#define OP_PROCESS_INSTALL 8
/* Process (RIGHT_W): clear a region slot. a1 = region slot index. */
#define OP_PROCESS_UNINSTALL 9

/*
 * Thread (RIGHT_W): set where a stopped thread will start.
 * a1 = program counter, a2 = stack pointer.
 * Fails with KERR_STATE unless the thread is stopped.
 * The kernel does not check either value:
 * a thread that starts nowhere useful faults, which is its creator's business.
 */
#define OP_THREAD_CONFIGURE 12
/*
 * Thread (RIGHT_W): make a stopped thread runnable.
 * It runs when the thread that started it waits
 * or when the timer tick takes the processor from it,
 * whichever comes first.
 * Fails with KERR_STATE unless the thread is stopped.
 */
#define OP_THREAD_RESUME 13

/*
 * Notification (RIGHT_W): set bits. a1 = the bits to set, which may not be zero.
 * Never blocks. If a thread is waiting, it takes every set bit and wakes.
 */
#define OP_NOTIFY_SIGNAL 14
/*
 * Notification (RIGHT_R): take the bits that are set.
 * Returns a1 = the bits, which is never zero, and clears them.
 * Blocks until some bit is set;
 * the bits are sticky, so a signal that arrives first is not lost.
 */
#define OP_NOTIFY_WAIT 15

/*
 * Timer (RIGHT_W): signal the bound notification once a delay has passed.
 * a1 = the bits to signal, a2 = the delay in microseconds.
 * The signal comes no earlier than the delay
 * and at the kernel's first tick after it, whatever the tick's period is;
 * a delay of zero signals at the next tick.
 * A longer delay than 32 bits of microseconds is several calls.
 * Setting an armed timer replaces its delay and bits;
 * a1 = 0 cancels it, and takes back no signal that already happened.
 */
#define OP_TIMER_SET 18

/*
 * IrqLine: derive a smaller range of lines with the same rights.
 * a1 = offset from the first line, a2 = count, a3 = destination slot.
 * Like OP_REGION_CARVE, this is a table operation that touches no kernel memory.
 */
#define OP_IRQ_CARVE 19
/*
 * IrqLine (RIGHT_W): bind the one line the capability names to a notification,
 * as an Irq object.
 * a1 = the slot of the Pool capability the Irq is allocated from, which needs RIGHT_W,
 * a2 = the slot of the Notification capability the Irq signals, which needs RIGHT_W
 *      and must lie in that pool,
 * a3 = destination slot for the Irq capability.
 * The invoked slot is cleared, and may be the destination.
 * The capability must name exactly one line; carve first.
 * Fails with KERR_OVERLAP if an Irq is already bound to the line;
 * the line is free again once that Irq's pool is destroyed.
 * The new Irq is masked: it signals nothing until OP_IRQ_SET arms it.
 */
#define OP_IRQ_BIND 20
/*
 * Irq (RIGHT_W): unmask the line and name the bits the next interrupt signals.
 * a1 = the bits.
 * The interrupt masks the line again as it signals,
 * so a driver hears about a line once until it says otherwise;
 * this call is how it says so, after it has serviced the device.
 * Setting an armed Irq replaces its bits;
 * a1 = 0 masks the line and takes back no signal that already happened.
 * LOG_IRQ_LINE is level: a set that arms it while the log holds bytes
 * the reader has not taken signals at once.
 */
#define OP_IRQ_SET 21

/* One above the highest operation code; the fuzzer's mutator draws below it. */
#define OP_COUNT 23

/*
 * Capability slots the kernel fills in the root task's table at boot.
 * Slot 0 is left empty so that an uninitialised index is an error.
 */
#define BOOT_CAP_NULL      0
#define BOOT_CAP_CAPTABLE  1 /* the root task's own table */
#define BOOT_CAP_PROCESS   2 /* the root task's own process */
#define BOOT_CAP_THREAD    3 /* the root task's only thread */
#define BOOT_CAP_POOL      4 /* the boot pool the root objects live in */
#define BOOT_CAP_DEBUG     5
#define BOOT_CAP_CODE      6 /* Region: the root task's code, read and execute */
#define BOOT_CAP_DATA      7 /* Region: the root task's data and stack */
#define BOOT_CAP_FREE_RAM  8 /* Region: all RAM the kernel does not use */
#define BOOT_CAP_INPUT     9 /* Region, read only: test input the loader placed in RAM */
#define BOOT_CAP_IRQ_LINES 10 /* IrqLine: LOG_IRQ_LINE and every line of the interrupt controller */
#define BOOT_CAP_UART      11 /* Region, read and write: the board's UART registers */
#define BOOT_CAP_LOG       12 /* Region, read and write: the kernel's log, see struct rvuos_log */
#define BOOT_CAP_COUNT     13

/*
 * The kernel's log.
 * The kernel has no console: every byte it prints, OP_DEBUG_PUTC included,
 * goes into a ring in the kernel's memory, which BOOT_CAP_LOG maps.
 * The region starts with this header and the ring follows at RVUOS_LOG_HEADER.
 * The kernel writes head, a count of every byte ever written; byte n lies at n % size.
 * The reader writes taken, the count of bytes it has read,
 * and the kernel trusts it no further than the line below and what a halt writes out.
 * Once head - taken exceeds size, bytes not yet taken have been overwritten
 * and the oldest byte still kept is head - size.
 * The kernel never waits for a reader.
 *
 * Interrupt line LOG_IRQ_LINE is the log's: high while head lies past taken,
 * so a logger binds the line like a device's and waits for the log to grow;
 * see DESIGN.md, "The kernel log".
 */
#define LOG_IRQ_LINE 0
#define RVUOS_LOG_HEADER 32

#ifndef __ASSEMBLER__
struct rvuos_log {
    uint32_t head;
    uint32_t size;
    uint32_t taken;
};
#endif

/*
 * Replay input, as loaded into the BOOT_CAP_INPUT region:
 * a header followed by count records, little-endian.
 * The same records feed the host fuzzer, without the header.
 */
#define REPLAY_MAGIC 0x5a465652u /* "RVFZ" */
#define REPLAY_MAX_RECORDS 256

#ifndef __ASSEMBLER__
struct replay_header {
    uint32_t magic;
    uint32_t count;
};

/*
 * One system call and the thread that makes it.
 * actor 0 is whichever thread runs when the record comes up;
 * 1..REPLAY_THREADS names a replay driver thread, see rvuos/replay.h;
 * any other value means 0.
 */
struct replay_record {
    uint8_t op;
    uint8_t actor;
    uint16_t slot;
    uint32_t a1, a2, a3;
};
#endif

/* Fixed limits visible to user programs. */
#define PROCESS_REGION_SLOTS 8

#endif
