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
 * On ARMv7-M the call is svc, r0..r6 stand for a0..a6 and r12 for a7.
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
#define KERR_NO_MEMORY    5 /* pool exhausted, or an Untyped with something made of it */
#define KERR_SLOT_IN_USE  6 /* destination slot already holds a capability */
#define KERR_OVERLAP      7 /* region overlaps or, on the ESP32-C6, touches one installed in the same process; line already bound */
#define KERR_LIMIT        8 /* a fixed kernel limit was hit, such as PMP entries */
#define KERR_STATE        9 /* the object is not in a state that allows this */

/* Capability and object types. */
#define CAP_NONE     0
#define CAP_FRAME    1 /* memory a process may map; no kernel object behind it */
#define CAP_POOL     2
#define CAP_CAPTABLE 3
#define CAP_PROCESS  4
#define CAP_THREAD   5
#define CAP_DEBUG    6 /* writing into the kernel's log, and halting and driving the machine, for tests */
#define CAP_NOTIFICATION 7
#define CAP_UNTYPED  8 /* memory that may become a frame, a pool or two halves; never mapped */
#define CAP_IRQ_LINE 9 /* a range of interrupt lines; no kernel object behind it */
#define CAP_IRQ      10 /* one line bound to a notification; signals it when the line fires */
#define CAP_CLOCK    11 /* the machine's time, the counter's frame, and its watchdog */
#define CAP_TIME     12 /* a range of the processor's units of time; no kernel object behind it */

/*
 * Rights bits.
 * Frame: RIGHT_R, RIGHT_W, RIGHT_X as memory permissions.
 * Untyped: the memory permissions of the frames made of it; a pool needs RIGHT_R and RIGHT_W.
 * CapTable: RIGHT_W to copy into or delete from the table.
 * Pool: RIGHT_W to allocate objects.
 * Process, Thread: RIGHT_W to control the object.
 * Thread: RIGHT_X to lend it time, see OP_NOTIFY_LEND, which a capability with RIGHT_X alone allows and nothing else.
 * Notification: RIGHT_W to signal, RIGHT_R to wait.
 *   Besides its rights a Notification capability carries the bits it may signal,
 *   every bit for a new one and fewer once carved, see OP_NOTIFY_CARVE; copying keeps them.
 * IrqLine: RIGHT_W to bind a line.
 * Irq: RIGHT_W to set or mask.
 * Time: RIGHT_W to bind a thread, RIGHT_X to let the threads bound through it run on spare time.
 * Debug: RIGHT_W to write into the kernel's log,
 *   RIGHT_X to halt the machine or drive it as a test does: trace, tick, interrupt, preempt;
 *   a child that only prints holds it with RIGHT_W alone.
 * Clock: RIGHT_R to read the time, and the counter through a frame,
 *   RIGHT_W to hold the machine to the watchdog, which halts it unless fed.
 * Copying a capability can only remove rights.
 */
#define RIGHT_R 0x1
#define RIGHT_W 0x2
#define RIGHT_X 0x4
#define RIGHT_ALL 0x7

/*
 * Operations, with the capability type they apply to and their arguments.
 */

/*
 * Debug (RIGHT_W): write into the kernel's log, see BOOT_CAP_LOG.
 * a1, a2, a3 = up to DEBUG_WRITE_BYTES bytes, the lowest byte of a1 first,
 * which end at the first zero byte: the log is text.
 */
#define OP_DEBUG_WRITE 1
#define DEBUG_WRITE_BYTES 12
/* Debug (RIGHT_X): halt the machine. a1 = exit code. Does not return. */
#define OP_DEBUG_HALT 2
/*
 * Debug (RIGHT_X): turn on tracing and self-checking.
 * From then on the kernel prints every system call with its status
 * and runs the invariant checker after each one.
 * The timer tick stops preempting,
 * because a transcript the host build must reproduce
 * cannot contain a switch that lands between two instructions.
 * Every thread's account is filled, see OP_TIME_BIND,
 * so that the host build starts from the same accounts.
 * It cannot be turned off again, so a traced program cannot hide,
 * and once it is on the call changes nothing.
 */
#define OP_DEBUG_TRACE 10
/*
 * Debug (RIGHT_X): what the timer tick does, on request.
 * Time moves by one tick, charged to the caller while it has time,
 * every timer line that is due signals, a thread whose account reached a tick has time again,
 * and the caller's turn ends: it goes to the back of the queue its account puts it on,
 * the next thread has its turn, and the caller stays ready.
 * Works while tracing is on, unlike the tick itself;
 * see DESIGN.md, "Verification".
 */
#define OP_DEBUG_TICK 17
/*
 * Debug (RIGHT_X): what a device interrupt does, on request. a1 = the line.
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
 * Debug (RIGHT_X): a tick landing within a restartable call, on request. a1 = n.
 * The n-th place from now at which any restartable call could stop between two steps, it stops,
 * interrupt pending or not, and does what OP_DEBUG_TICK does,
 * so another thread may run before the caller makes its call again. Zero disarms it.
 * It is how a replay has a thread run while another's call is half done,
 * since under tracing the tick stops no call; see DESIGN.md, "Verification".
 */
#define OP_DEBUG_PREEMPT 32

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
 * a thread made through a Process capability below it stops, as it has no process any more,
 * an Irq bound through a Notification capability below it is disarmed, as it has nothing to signal,
 * and a pool retyped from an Untyped below it is destroyed,
 * with every object in it and every capability anywhere that names the pool or one of those objects.
 * The slots of the pool's tables are cleared as OP_CAP_DELETE clears one:
 * what was derived from them goes to their parents.
 * Threads waiting on a notification in a destroyed pool, or lending a thread in it their time,
 * are woken with KERR_INVALID_CAP and no bits,
 * threads, in whatever pool, whose process was in it stop,
 * and Irqs, in whatever pool, whose notification was in it are disarmed.
 * A revoke that takes the calling thread's process, its process's table,
 * or the capability it is made through ends there with KERR_OK,
 * and what it did not reach stays for another call to revoke.
 * Fails with KERR_INVALID_CAP for an empty slot,
 * and with KERR_STATE when the slot is an Untyped whose memory holds
 * the calling thread, its process, the process's table or the invoked table.
 * Restartable.
 */
#define OP_CAP_REVOKE 23
/*
 * CapTable (RIGHT_W): derive a capability from one in the caller's table.
 * The arguments are OP_CAP_COPY's.
 * The result is a child of the source in the derivation tree,
 * so revoking below the source takes it, and everything derived from it in turn.
 * An Untyped can be derived only while nothing is made of it, else KERR_NO_MEMORY,
 * and the derived one is the whole of it: the source makes nothing more
 * until the derived one and what was made of it are gone.
 */
#define OP_CAP_DERIVE 24
/*
 * CapTable (RIGHT_W): move a capability from the caller's table.
 * a1 = destination slot in the invoked table, a2 = source slot in the caller's table.
 * The destination takes the source's place in the derivation tree:
 * the parent, the siblings and what was derived from it are the source's,
 * so a revoke through it takes what one through the source would have,
 * and a revoke that would have taken the source takes it.
 * The source slot is left empty. The rights stay as they were,
 * since what was derived from the source may hold all of them.
 * An Untyped moves too, with what was made of it: a move makes no second allocator.
 */
#define OP_CAP_MOVE 18

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
 * An Untyped is either free or made: while nothing lies below it in the derivation tree
 * it makes one thing of the whole of its memory, a frame, a pool, two halves or a derived Untyped,
 * and while something does it makes nothing, KERR_NO_MEMORY,
 * so the things one Untyped made never overlap.
 * It is free again, the whole of it, once nothing made of it is left,
 * which a revoke below it brings about at once.
 * Where each block lies is the program's to choose, by which half it takes.
 */

/*
 * Untyped: describe it. Returns a1 = base, a2 = size, a3 = rights,
 * a4 = 1 while something made of it is left, 0 while it is free.
 */
#define OP_UNTYPED_INFO 27
/*
 * Untyped: make the whole of its memory into one thing, below the invoked one in the tree.
 * a1 = the type: CAP_FRAME or CAP_POOL, a2 = destination slot.
 * Returns a1 = the base of the memory.
 * A frame carries the invoked one's rights.
 * A pool needs RIGHT_R and RIGHT_W and an Untyped no smaller than POOL_MIN_SIZE;
 * it lies below the pool's own node in the tree, and the returned Pool capability below that node,
 * so revoking below the Untyped destroys the pool.
 */
#define OP_UNTYPED_RETYPE 6
/*
 * Untyped: split its memory into two halves, each an Untyped with the invoked one's rights,
 * below it in the tree. a1 = slot for the lower half, a2 = slot for the upper half, two slots.
 * A half is no smaller than the smallest region, else KERR_INVALID_ARG.
 */
#define OP_UNTYPED_SPLIT 30

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
 * A thread holds its process the same way, and the process may lie in any pool;
 * revoking below that capability, or destroying the process's pool,
 * stops the thread for good: see OP_THREAD_RESUME.
 * An Irq is not allocated here but bound, with OP_IRQ_BIND.
 */
#define OP_POOL_ALLOC 7
/*
 * Process (RIGHT_W): install a region into one of the process's region slots.
 * a1 = region slot index, a2 = Frame capability slot in the caller's table,
 * a3 = rights to install, a subset of the frame's rights.
 * RIGHT_W without RIGHT_R is rejected with KERR_INVALID_ARG,
 * because PMP reserves that encoding,
 * and so on ARM is RIGHT_X without RIGHT_R, since the MPU fetches only what it lets a thread read.
 * Fails with KERR_OVERLAP if the range overlaps another region installed in the same process,
 * or, on the ESP32-C6, a writable one would end where a readable but not writable one begins.
 * The installed region is a child of the Frame capability in the derivation tree:
 * revoking below that capability uninstalls it.
 */
#define OP_PROCESS_INSTALL 8
/* Process (RIGHT_W): clear a region slot. a1 = region slot index. Clearing an empty slot succeeds. */
#define OP_PROCESS_UNINSTALL 9

/*
 * Thread (RIGHT_W): set where a stopped thread will start, and its argument.
 * a1 = program counter, a2 = stack pointer, a3 = the argument, which the thread finds in a0, r0 on ARM,
 * the first argument of a function.
 * Every other register starts at zero, whatever the thread's last run left.
 * On ARM the program counter carries bit 0 for Thumb, as a function pointer does.
 * Fails with KERR_STATE unless the thread is stopped.
 * The kernel does not check either value:
 * a thread that starts nowhere useful faults, which is its creator's business; see OP_THREAD_WATCH.
 */
#define OP_THREAD_CONFIGURE 12
/*
 * Thread (RIGHT_W): make a stopped thread ready.
 * It runs once it is bound to units of time, see OP_TIME_BIND,
 * when its turn comes.
 * A thread that faulted goes on at the instruction that faulted, which runs again,
 * unless OP_THREAD_WRITE_REG moved its program counter.
 * Fails with KERR_STATE unless the thread is stopped,
 * and once its process was taken, see OP_POOL_ALLOC: it has nothing to run in.
 */
#define OP_THREAD_RESUME 13
/*
 * Thread (RIGHT_W): name what the thread's faults signal, its watch.
 * a1 = the slot of a Notification capability, which needs RIGHT_W and may lie in any pool,
 * a2 = the bits a fault signals there, of which only those the capability may signal are kept,
 * and a2 with none of them fails with KERR_NO_RIGHTS; a2 = 0 clears the watch and ignores a1.
 * A fault, an access fault, an illegal instruction, a misaligned access or a breakpoint,
 * stops the thread at the instruction that faulted, with its registers as they were,
 * and signals the bits as OP_NOTIFY_SIGNAL would; nothing else stops, the machine goes on.
 * A thread without a watch stops all the same, and nobody hears.
 * The kernel's log says what the fault was, and so does OP_THREAD_FAULT.
 * It replaces the watch before and stays, so a thread that faults again signals again.
 * The thread holds the notification by a capability derived from the one named,
 * so revoking below that capability, or destroying the notification's pool, clears the watch.
 * See DESIGN.md, "Faults".
 */
#define OP_THREAD_WATCH 31
/*
 * Thread (RIGHT_W): what stopped a thread that faulted.
 * Returns a1 = the cause, a2 = the program counter, a3 = the address, a4 = the status:
 * on RISC-V mcause, mepc, mtval and zero,
 * on ARMv7-M the exception number, the pc, the address MMFAR or BFAR named, else zero, and CFSR.
 * The program counter is where a resume goes on, as OP_THREAD_READ_REG reads it.
 * Fails with KERR_STATE unless the thread is stopped where it faulted:
 * a resume or a configure since, or no fault at all, leaves nothing to tell.
 * See DESIGN.md, "Faults".
 */
#define OP_THREAD_FAULT 33
/*
 * Thread (RIGHT_W): read one register of a stopped thread.
 * a1 = the register: x0 to x31 on RISC-V, where x0 reads zero, r0 to r14 on ARMv7-M,
 * or THREAD_REG_PC for the program counter, which on ARM carries bit 0 for Thumb, as OP_THREAD_CONFIGURE takes it.
 * Returns a1 = its value.
 * Fails with KERR_STATE unless the thread is stopped,
 * and with KERR_INVALID_ARG for a number that names no register.
 */
#define OP_THREAD_READ_REG 34
/*
 * Thread (RIGHT_W): write one register of a stopped thread.
 * a1 = the register, as for OP_THREAD_READ_REG; a write to x0 changes nothing. a2 = the value.
 * A pc written is taken as OP_THREAD_CONFIGURE takes it, and on ARM leaves an IT block, keeping the flags.
 * A watcher emulates the instruction a thread faulted at by writing what it would have done, the pc past it among that,
 * and resuming the thread, which OP_THREAD_FAULT tells of until then.
 * Under tracing the thread cannot run any more, as after OP_THREAD_CONFIGURE; see DESIGN.md, "Verification".
 * Fails as OP_THREAD_READ_REG does.
 */
#define OP_THREAD_WRITE_REG 35
#define THREAD_REG_PC 32

/*
 * Notification (RIGHT_W): set bits. a1 = the bits to set, which may not be zero.
 * Of those, only the bits the capability may signal are set, see OP_NOTIFY_CARVE,
 * and a1 with none of them fails with KERR_NO_RIGHTS.
 * Never blocks. If a thread is waiting, it takes every set bit and wakes:
 * the one whose time pays for the caller's turn, see OP_NOTIFY_LEND, if it waits there,
 * and it takes the rest of the turn back, the caller going to the back of its queue;
 * otherwise, today, the one that waited longest.
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
 * Notification (RIGHT_R): wait as OP_NOTIFY_WAIT does, lending a thread the caller's time meanwhile.
 * a1 = the slot of a Thread capability with RIGHT_X, the borrower; the caller itself fails with KERR_INVALID_ARG.
 * Returns a1 = the bits, at once if some bit is set, which lends nothing.
 * While it waits, a borrower without time of its own runs on the account of the thread that has lent it time longest,
 * if that one runs on the borrower's core,
 * and a caller whose own time pays for its turn hands the rest of the turn to the borrower,
 * if it is that one and the borrower waits for a turn.
 * A signal from the borrower that wakes the caller hands the rest of the turn back, see OP_NOTIFY_SIGNAL.
 * The time goes one step, to the borrower, never on to whatever the borrower waits for or lends to.
 * A caller whose borrower is destroyed wakes with KERR_INVALID_CAP and no bits, as one whose notification is.
 * See DESIGN.md, "Communication and synchronisation".
 */
#define OP_NOTIFY_LEND 38
/*
 * Notification: derive a capability that may signal fewer bits, with the same rights.
 * a1 = the bits, of which those the invoked capability may signal are kept, a2 = destination slot.
 * Fails with KERR_INVALID_ARG when none is kept.
 * Like OP_FRAME_CARVE, this is a table operation that touches no kernel memory,
 * and the new capability is a child of the invoked one.
 * A client given its own bit this way cannot signal another's; see DESIGN.md, "Communication and synchronisation".
 */
#define OP_NOTIFY_CARVE 36
/* Every bit: what a new Notification capability may signal, and what a signal names to set all its capability may. */
#define NOTIFY_ALL_BITS 0xffffffffu

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
 *      and may lie in any pool; the Irq signals only the bits that capability may,
 * a3 = destination slot for the Irq capability.
 * The invoked slot is cleared with everything derived from it, and may be the destination;
 * the Irq capability is a child of the Pool capability, as an allocated object's is.
 * The Irq holds its notification by a capability derived from the one named,
 * so revoking below that capability, or destroying the notification's pool,
 * disarms the Irq and masks its line for good: see OP_IRQ_SET.
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
 * a1 = the bits, of which only those the Irq may signal are kept, see OP_IRQ_BIND,
 * and a1 with none of them fails with KERR_NO_RIGHTS;
 * a2 = on a timer line, the delay in microseconds, and unused elsewhere;
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
 * Fails with KERR_STATE once the Irq's notification was taken, see OP_IRQ_BIND:
 * it has nothing to signal, and its line stays bound until the Irq's pool is destroyed.
 */
#define OP_IRQ_SET 21
#define IRQ_SET_PERIOD 0x1

/*
 * Clock (RIGHT_R): read the machine's time, 64 bits counting up from boot at a fixed rate.
 * Returns a1 = the low word, a2 = the high word, a3 = the rate in Hz, measured at boot on the ESP32-C6,
 * and a4 = the address of the counter's low 32 bits, which OP_CLOCK_FRAME shows.
 * Under tracing the time is the tick count's; see DESIGN.md, "Verification".
 */
#define OP_CLOCK_READ 25
/*
 * Clock (RIGHT_R): derive a read-only frame holding the counter's low 32 bits, a child of the clock.
 * a1 = destination slot.
 * Installed, it lets a process read them with a load and no call.
 * They wrap, after 71 minutes at a megahertz, so a program takes differences of them;
 * the whole time is OP_CLOCK_READ's, since where the rest of the counter lies, if anywhere, is the board's.
 * It is the smallest block holding the word, so a coarse PMP grain shows its neighbours too.
 */
#define OP_CLOCK_FRAME 26
/*
 * Clock (RIGHT_W): the watchdog. a1 = microseconds, more than zero and at most WATCHDOG_US_MAX,
 * else KERR_INVALID_ARG.
 * The machine halts with code 7 unless the watchdog is fed again within a1 microseconds:
 * at the first tick that surely lies past them, as a timer line fires; see OP_IRQ_SET.
 * The first call arms it, and nothing disarms it; each call sets the time anew, nearer or further.
 * There is one watchdog on the machine, which every capability with RIGHT_W to the clock feeds.
 * See DESIGN.md, "The watchdog".
 */
#define OP_CLOCK_WATCHDOG 37
#define WATCHDOG_US_MAX 10000000u /* ten seconds */

/*
 * Time: derive a smaller range of units with the same rights.
 * a1 = offset from the first unit, a2 = count, a3 = destination slot.
 * Like OP_IRQ_CARVE, this is a table operation that touches no kernel memory,
 * and the new capability is a child of the invoked one.
 */
#define OP_TIME_CARVE 28
/*
 * Time (RIGHT_W): bind a thread to units of the capability, which it then earns.
 * a1 = the slot of the Thread capability, which needs RIGHT_W,
 * a2 = the offset of the first unit, which must lie within the capability even for none,
 * a3 = how many units, zero for none.
 * Each unit earns the thread's account a part of a tick every tick, and a turn costs it a tick;
 * with RIGHT_X on the capability the thread also runs on spare time, for free.
 * The thread runs on the core its units are of, which for none is the first unit's;
 * the units must all be of one core, or the call fails with KERR_INVALID_ARG.
 * See DESIGN.md, "Scheduling".
 * Fails with KERR_OVERLAP if another thread earns one of the units.
 * The thread leaves the units it earned before, and keeps what its account held, up to what the new units hold.
 * The binding is a child of the invoked capability, so revoking below that capability unbinds the thread:
 * it keeps its state and its account is emptied, and it does not run until it is bound again.
 * A thread unbound or moved while it runs, the caller itself among them, finishes the turn it had.
 */
#define OP_TIME_BIND 29

/* One above the highest operation code; the fuzzer's mutator draws below it. */
#define OP_COUNT 39

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
#define BOOT_CAP_TIME      15 /* Time: every unit of every core, with RIGHT_W and RIGHT_X; the root thread earns the first core's */
/*
 * Untyped: the block of RAM the root task lives in, which holds its code, data and input
 * and the boot pool's block; all of it is made already, so it makes nothing
 * until those are gone, and the root task, which runs on it, cannot revoke below it.
 */
#define BOOT_CAP_ROOT_RAM  16
/*
 * Untyped: the block the boot pool was made of, below BOOT_CAP_ROOT_RAM.
 * Revoking below it destroys the boot pool, and the root task's table, process and thread with it,
 * which the root task, living there, cannot do.
 */
#define BOOT_CAP_POOL_RAM  17
/*
 * Frames over the devices the board lists, BOOT_DEVICES of them, from this slot up in the board's order;
 * QEMU's lists none.
 * BOOT_DEVICES comes from the kernel's board.h, or a program's devices.h, which names each slot.
 */
#define BOOT_CAP_DEVICES   18
/* The root task's own slots start here; on QEMU it is 18. */
#define BOOT_CAP_COUNT     (BOOT_CAP_DEVICES + BOOT_DEVICES)

/*
 * The kernel's log.
 * The kernel has no console: every byte it prints, OP_DEBUG_WRITE's included,
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
 * One system call and the thread that makes it,
 * or a load or a store the replay driver makes itself, see rvuos/replay.h.
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
#define ROOT_TABLE_SLOTS (BOOT_CAP_COUNT + 46) /* slots in the root task's table, 64 on QEMU */
#define POOL_MIN_SIZE 64 /* the smallest Untyped OP_UNTYPED_RETYPE makes a pool of */
#define TIMER_LINES 16 /* on the whole machine; see BOOT_CAP_TIMER_LINES */
#define TIME_UNITS 64 /* of each core, numbered core by core; see BOOT_CAP_TIME */

#endif
