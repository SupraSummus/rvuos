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
 * A call marked "restartable" may stop for a pending interrupt with its progress kept
 * and resume the thread at its ecall, every register unchanged,
 * so the thread makes the same call again and it goes on from there.
 * The call made again checks everything afresh.
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
#define KERR_NO_MEMORY    5 /* pool exhausted, or no block of the size left in an Untyped */
#define KERR_SLOT_IN_USE  6 /* destination slot already holds a capability */
#define KERR_OVERLAP      7 /* region overlaps one installed in the same process; line already bound */
#define KERR_LIMIT        8 /* a fixed kernel limit was hit, such as PMP entries */
#define KERR_STATE        9 /* the object is not in a state that allows this */

/* Capability and object types. */
#define CAP_NONE     0
#define CAP_FRAME    1 /* memory a process may map; no kernel object behind it */
#define CAP_POOL     2
#define CAP_CAPTABLE 3
#define CAP_PROCESS  4
#define CAP_THREAD   5
#define CAP_DEBUG    6 /* a byte into the kernel's log, and machine halt, for bring-up */
#define CAP_NOTIFICATION 7
#define CAP_UNTYPED  8 /* memory that may become frames, pools or smaller Untypeds; never mapped */
#define CAP_IRQ_LINE 9 /* a range of interrupt lines; no kernel object behind it */
#define CAP_IRQ      10 /* one line bound to a notification; signals it when the line fires */
#define CAP_CLOCK    11 /* the machine's counter: its rate, and a frame to read it through */

/*
 * Rights bits.
 * Frame: RIGHT_R, RIGHT_W, RIGHT_X as memory permissions.
 * Untyped: the memory permissions of the frames made of it; a pool needs RIGHT_R and RIGHT_W.
 * CapTable: RIGHT_W to copy into or delete from the table.
 * Pool: RIGHT_W to allocate objects.
 * Process, Thread: RIGHT_W to control the object.
 * Notification: RIGHT_W to signal, RIGHT_R to wait.
 * IrqLine: RIGHT_W to bind a line.
 * Irq: RIGHT_W to set or mask.
 * Debug, Clock: any right.
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
 * Time moves by one tick, every timer line that is due signals,
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
 * LOG_IRQ_LINE among them: the log's level is the log's, not a record's,
 * and so are the timer lines, which only OP_DEBUG_TICK moves.
 * It is how a replay fires an interrupt, since the host build has no devices;
 * see DESIGN.md, "Verification".
 */
#define OP_DEBUG_IRQ 22

/*
 * CapTable (RIGHT_W): copy a capability from the caller's table.
 * a1 = destination slot in the invoked table,
 * a2 = source slot in the caller's table,
 * a3 = rights mask applied to the copy.
 * The copy is a sibling of the source in the derivation tree:
 * derived from what the source was derived from, and as good as the source.
 * Revoking below the source does not take it; revoking below their common parent does.
 * An Untyped is derived, never copied: a copy would be a second allocator over the same memory,
 * so copying one fails with KERR_WRONG_TYPE.
 */
#define OP_CAP_COPY 3
/*
 * CapTable (RIGHT_W): clear a slot. a1 = slot.
 * What was derived from the slot is not cleared: it is adopted by the slot's parent,
 * or, when the slot is a root, each child becomes a root, which is restartable.
 * Clearing an empty slot succeeds.
 */
#define OP_CAP_DELETE 4
/*
 * CapTable (RIGHT_W): clear everything derived from a slot, in every table and every process,
 * and leave the slot itself. a1 = slot.
 * A region installed from a frame below the slot is uninstalled,
 * and a pool retyped from an Untyped below it is destroyed, as OP_POOL_DESTROY destroys one.
 * Fails with KERR_INVALID_CAP for an empty slot,
 * and with KERR_STATE when the slot is an Untyped whose memory holds
 * the calling thread or its process's table.
 * Restartable.
 */
#define OP_CAP_REVOKE 23
/*
 * CapTable (RIGHT_W): derive a capability from one in the caller's table.
 * The arguments are OP_CAP_COPY's.
 * The result is a child of the source in the derivation tree,
 * so revoking below the source takes it, and everything derived from it in turn.
 * An Untyped can be derived only while nothing was made of it, else KERR_STATE,
 * and deriving uses the whole of it up: the source makes nothing more
 * until the derived one and what was made of it are gone.
 */
#define OP_CAP_DERIVE 24

/*
 * Frame: describe it. Returns a1 = base, a2 = size, a3 = rights,
 * a4 = the size of the smallest region, a power of two of at least 8,
 * the same for every region on the machine.
 * Every frame is a block: its size is a power of two of at least that,
 * and its base is a multiple of its size.
 */
#define OP_FRAME_INFO 11
/*
 * Frame: derive a smaller frame with the same rights.
 * a1 = offset from the frame base, a2 = size, a3 = destination slot.
 * The size must be a power of two no smaller than the smallest region
 * and the offset a multiple of the size,
 * so that the new frame is a block and costs one PMP entry wherever it is installed.
 * The new frame is a child of the invoked one in the derivation tree.
 */
#define OP_FRAME_CARVE 5

/*
 * Untyped: describe it. Returns a1 = base, a2 = size, a3 = rights,
 * a4 = the watermark: the offset the next OP_UNTYPED_RETYPE looks from.
 */
#define OP_UNTYPED_INFO 27
/*
 * Untyped: make the next block of its memory into something, as a child of the invoked one.
 * a1 = the type: CAP_UNTYPED, CAP_FRAME, or CAP_POOL,
 * a2 = the size, a power of two no smaller than the smallest region,
 *      and for a pool no smaller than POOL_MIN_SIZE,
 * a3 = destination slot.
 * Returns a1 = the base of the block.
 * The block is the first one of the size, aligned to it, at or past the watermark,
 * and the watermark moves past it, so what one Untyped makes never overlaps;
 * an Untyped that nothing made of it is left of starts again from its base.
 * Fails with KERR_NO_MEMORY when the block does not fit.
 * A frame and an Untyped carry the invoked one's rights.
 * A pool needs RIGHT_R and RIGHT_W, lies below the pool's own node in the tree,
 * and the returned Pool capability below that node;
 * revoking below the Untyped destroys the pool.
 */
#define OP_UNTYPED_RETYPE 6

/*
 * Pool (RIGHT_W): allocate a kernel object.
 * a1 = object type, a2 = destination slot,
 * a3 = type specific:
 *   CAP_CAPTABLE  the number of slots,
 *   CAP_PROCESS   the slot of the CapTable capability the process will use,
 *                 which needs RIGHT_W; the table may lie in any pool,
 *   CAP_THREAD    the slot of the Process capability the thread will run in,
 *   CAP_NOTIFICATION  unused.
 * The object's capability is a child of the invoked Pool capability in the derivation tree,
 * so revoking below that capability takes it.
 * Fails with KERR_STATE while the pool is being destroyed.
 * A process holds its table by a capability derived from the one named,
 * so revoking below that capability, or destroying the table's pool,
 * leaves the process naming nothing: every call it makes fails with KERR_INVALID_CAP.
 * A thread's process is not checked on every use,
 * so the thread must be allocated from the same pool as its process;
 * CAP_THREAD fails with KERR_INVALID_ARG otherwise.
 * An Irq is not allocated here but bound, with OP_IRQ_BIND.
 */
#define OP_POOL_ALLOC 7
/*
 * Pool (RIGHT_W): destroy the pool, every object in it,
 * and every capability anywhere that names one of those objects.
 * The slots of the pool's tables are cleared as OP_CAP_DELETE clears one:
 * what was derived from them goes to their parents.
 * The memory goes back to the Untyped the pool was retyped from,
 * which makes something of it again once nothing else made of it is left.
 * Fails with KERR_STATE if the calling thread or its process's table
 * lives in the pool, and for the boot pool, which holds the root task.
 * Threads waiting on a notification in a destroyed pool
 * are woken with KERR_INVALID_CAP and no bits.
 * Restartable.
 */
#define OP_POOL_DESTROY 16

/*
 * Process (RIGHT_W): install a region into one of the process's region slots.
 * a1 = region slot index, a2 = Frame capability slot in the caller's table,
 * a3 = rights to install, a subset of the frame's rights.
 * RIGHT_W without RIGHT_R is rejected with KERR_INVALID_ARG,
 * because PMP reserves that encoding.
 * Fails with KERR_OVERLAP if the range overlaps another region installed in the same process.
 * The installed region is a child of the Frame capability in the derivation tree:
 * revoking below that capability uninstalls it.
 */
#define OP_PROCESS_INSTALL 8
/* Process (RIGHT_W): clear a region slot. a1 = region slot index. Clearing an empty slot succeeds. */
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
 * IrqLine: derive a smaller range of lines with the same rights.
 * a1 = offset from the first line, a2 = count, a3 = destination slot.
 * Like OP_FRAME_CARVE, this is a table operation that touches no kernel memory,
 * and the new capability is a child of the invoked one.
 */
#define OP_IRQ_CARVE 19
/*
 * IrqLine (RIGHT_W): bind the one line the capability names to a notification,
 * as an Irq object; a timer line binds the same way as a controller's.
 * a1 = the slot of the Pool capability the Irq is allocated from, which needs RIGHT_W,
 * a2 = the slot of the Notification capability the Irq signals, which needs RIGHT_W
 *      and must lie in that pool,
 * a3 = destination slot for the Irq capability.
 * The invoked slot is cleared with everything derived from it, and may be the destination;
 * the Irq capability is a child of the Pool capability, as an allocated object's is.
 * The capability must name exactly one line; carve first.
 * Fails with KERR_OVERLAP if an Irq is already bound to the line;
 * the line is free again once that Irq's pool is destroyed.
 * Fails with KERR_NO_MEMORY if the pool has no room for the Irq.
 * Every check comes before the clearing, which is restartable.
 * The new Irq is masked: it signals nothing until OP_IRQ_SET arms it.
 */
#define OP_IRQ_BIND 20
/*
 * Irq (RIGHT_W): unmask the line and name the bits the next interrupt signals.
 * a1 = the bits; a2 = on a timer line, the delay in microseconds, and unused elsewhere;
 * a3 = on a timer line, 0 or IRQ_SET_PERIOD, and unused elsewhere.
 * The interrupt masks the line again as it signals,
 * so a driver hears about a line once until it says otherwise;
 * this call is how it says so, after it has serviced the device.
 * Setting an armed Irq replaces its bits, and on a timer line its delay;
 * a1 = 0 masks the line and takes back no signal that already happened.
 * LOG_IRQ_LINE is level: a set that arms it while the log holds bytes
 * the reader has not taken signals at once.
 * A timer line fires once the delay has passed, no earlier,
 * at the kernel's first tick after it, whatever the tick's period is;
 * a delay of zero fires at the next tick.
 * A longer delay than 32 bits of microseconds is several calls.
 * With IRQ_SET_PERIOD the delay is a period, rounded up to whole ticks and not zero,
 * counted from the line's last deadline, or its bind, rather than from the call:
 * the line fires at the first tick after the call
 * that lies a whole number of periods from that deadline.
 * Returns a1 = the periods skipped, those that had passed by the call.
 * Any other a3, or a zero period, fails with KERR_INVALID_ARG; a1 = 0 ignores a2 and a3.
 */
#define OP_IRQ_SET 21
#define IRQ_SET_PERIOD 0x1

/*
 * Clock: describe the machine's counter, 64 bits counting up from boot at a fixed rate.
 * Returns a1 = the rate in Hz, measured at boot on the ESP32-C6,
 * and a2 = the address of the low word; the high word follows.
 * RV32 reads the high word, the low word and the high word again,
 * and starts over while the high word moved.
 */
#define OP_CLOCK_INFO 25
/*
 * Clock: derive a read-only frame holding the counter, a child of the clock. a1 = destination slot.
 * Installed, it lets a process read the counter with loads.
 * It is the smallest block holding the two words, so a coarse PMP grain shows their neighbours too.
 */
#define OP_CLOCK_FRAME 26

/* One above the highest operation code; the fuzzer's mutator draws below it. */
#define OP_COUNT 28

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
#define BOOT_CAP_CODE      6 /* Frame: the root task's code, read and execute */
#define BOOT_CAP_DATA      7 /* Frame: the root task's data and stack */
#define BOOT_CAP_FREE_RAM  8 /* Untyped: the block of RAM the board sets aside for the root task */
#define BOOT_CAP_INPUT     9 /* Frame, read only: test input the loader placed in RAM */
#define BOOT_CAP_IRQ_LINES 10 /* IrqLine: LOG_IRQ_LINE and every line of the interrupt controller */
#define BOOT_CAP_UART      11 /* Frame, read and write: the board's UART registers */
#define BOOT_CAP_LOG       12 /* Frame, read and write: the kernel's log, see struct rvuos_log */
#define BOOT_CAP_TIMER_LINES 13 /* IrqLine: every timer line, TIMER_LINES of them */
#define BOOT_CAP_CLOCK     14 /* Clock: the machine's counter */
#define BOOT_CAP_COUNT     15

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
 * Replay input, as loaded into the BOOT_CAP_INPUT frame:
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
#define POOL_MIN_SIZE 64 /* the smallest pool OP_UNTYPED_RETYPE makes */
#define TIMER_LINES 16 /* on the whole machine; see BOOT_CAP_TIMER_LINES */

#endif
