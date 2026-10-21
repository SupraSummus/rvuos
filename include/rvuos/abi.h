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
 *   a0      slot of the invoked capability in the caller's tables, see SLOT_TABLE_SHIFT
 *   a1..a3  operation arguments; one an operation does not read, and a bit of a rights mask past RIGHT_ALL,
 *           must be zero, else KERR_INVALID_ARG, so that it may mean something later;
 *           a4 is read by OP_POOL_ALLOC of a process or a thread alone, and a5 and a6 are reserved, and unread
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
 * A thread stopped while it waits stands at its call the same way; see OP_THREAD_STOP.
 *
 * Slots that receive a new capability are always in the caller's own tables.
 * A process needs no capability to write its own tables;
 * the CapTable capability exists to write another process's table.
 */

/*
 * A process names capabilities in up to PROCESS_TABLES tables, mounted in its table slots, see OP_PROCESS_MOUNT.
 * A slot of the caller's is numbered SLOT_IN(table slot, index), the table slot in the bits from SLOT_TABLE_SHIFT up,
 * so the numbers below 1024, as many as a table may have, are the first table's.
 * A number that points where no table is mounted names nothing, KERR_INVALID_CAP.
 * A slot of an invoked table is its index there.
 */
#define SLOT_TABLE_SHIFT 10
#define SLOT_IN(table, index) (((table) << SLOT_TABLE_SHIFT) | (index))

/* Status codes returned in a0. */
#define KERR_OK           0
#define KERR_INVALID_CAP  1 /* slot out of range, empty, or stale, or in a table slot no table is mounted in */
#define KERR_WRONG_TYPE   2 /* capability does not accept this operation */
#define KERR_NO_RIGHTS    3 /* capability lacks a right the operation needs */
#define KERR_INVALID_ARG  4
#define KERR_NO_MEMORY    5 /* pool exhausted, or an Untyped with something made of it */
#define KERR_SLOT_IN_USE  6 /* destination slot already holds a capability, or table slot a table */
#define KERR_OVERLAP      7 /* region overlaps or, on the ESP32-C6, touches one installed in the same process; line already bound */
#define KERR_LIMIT        8 /* a fixed kernel limit was hit, such as a thread's places */
#define KERR_STATE        9 /* the object is not in a state that allows this */

/* Capability and object types. */
#define CAP_NONE     0
#define CAP_FRAME    1 /* memory a process may map; no kernel object behind it */
#define CAP_POOL     2
#define CAP_CAPTABLE 3
#define CAP_PROCESS  4
#define CAP_THREAD   5
#define CAP_PRINT    6 /* writing into the kernel's log */
#define CAP_NOTIFICATION 7
#define CAP_UNTYPED  8 /* memory that may become a frame, a pool or two halves; never mapped */
#define CAP_IRQ_LINE 9 /* a range of interrupt lines; no kernel object behind it */
#define CAP_IRQ      10 /* one line bound to a notification; signals it when the line fires */
#define CAP_CLOCK    11 /* the machine's time, the counter's frame, and its watchdog */
#define CAP_TIME     12 /* a range of the processor's units of time; no kernel object behind it */
#define CAP_HALT     13 /* halting the machine */
#define CAP_TEST     14 /* driving the machine as a test does: tracing, moving the clock, interrupting, preempting */
#define CAP_ROUTE_LINE 15 /* a range of the router's channels; no kernel object behind it */
#define CAP_ROUTE    16 /* one channel, wiring an event of one device to a task of another */

/*
 * Rights bits.
 * Frame: RIGHT_R, RIGHT_W, RIGHT_X as memory permissions.
 * Untyped: the memory permissions of the frames made of it; a pool needs RIGHT_R and RIGHT_W.
 * CapTable: RIGHT_W to copy into or delete from the table, and to make it a process's, by an allocation or a mount.
 * Pool: RIGHT_W to allocate objects.
 * Process, Thread: RIGHT_W to control the object.
 * Thread: RIGHT_X to lend it time, see OP_NOTIFY_LEND, which a capability with RIGHT_X alone allows and nothing else.
 *   RIGHT_R to read its overruns, see OP_THREAD_OVERRUNS, which RIGHT_R alone allows and nothing else.
 * Notification: RIGHT_W to signal, RIGHT_R to wait.
 *   Besides its rights a Notification capability carries the bits it may signal,
 *   every bit for a new one and fewer once carved, see OP_NOTIFY_CARVE; copying keeps them.
 *   They are an aligned block: 1, 2, 4, 8, 16 or 32 bits, starting at a multiple of their count.
 * IrqLine: RIGHT_W to bind a line.
 * Irq: RIGHT_W to set or mask.
 * Time: RIGHT_W to bind a thread, RIGHT_X to let the threads bound through it run on spare time.
 * RouteLine: RIGHT_W to bind a channel.
 * Route: RIGHT_W to connect its ends and to arm it.
 * Print: RIGHT_W to write into the kernel's log.
 * Halt: RIGHT_W to halt the machine.
 * Test: RIGHT_W to drive the machine as a test does: trace, move the clock, interrupt, preempt.
 *   Three types, so that a child that only prints holds nothing that halts or drives the machine,
 *   and a supervisor that may halt nothing that drives it.
 * Clock: RIGHT_R to read the time, and the counter through a frame,
 *   RIGHT_W to hold the machine to the watchdog, which halts it unless fed.
 * Any capability: no right to describe it, see OP_CAP_INFO, and a Pool none to tell what an object takes of it,
 *   see OP_POOL_INFO.
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
 * Print (RIGHT_W): write into the kernel's log, see BOOT_CAP_LOG.
 * a1, a2, a3 = up to PRINT_BYTES bytes, the lowest byte of a1 first,
 * which end at the first zero byte: the log is text.
 */
#define OP_PRINT 1
#define PRINT_BYTES 12
/* Halt (RIGHT_W): halt the machine. a1 = exit code. Does not return. */
#define OP_HALT 2
/*
 * Test (RIGHT_W): turn on tracing and self-checking.
 * From then on the kernel prints every system call with its status,
 * and the results of a describing one, see OP_CAP_INFO,
 * and runs the invariant checker after each one.
 * The timer stops preempting, and the clock moves only by OP_TEST_TICK from where the counter was,
 * because a transcript the host build must reproduce
 * cannot contain a switch that lands between two instructions.
 * Every virtual time is brought to the clock, see OP_TIME_BIND,
 * so that the host build starts from the same.
 * It cannot be turned off again, so a traced program cannot hide,
 * and once it is on the call changes nothing.
 * A kernel built without the self-check, every board's but QEMU virt's by default, fails it with KERR_WRONG_TYPE.
 */
#define OP_TEST_TRACE 10
/*
 * Test (RIGHT_W): what the timer does, on request, a step of the clock on.
 * The clock moves a millisecond of the counter's counts, charged to the caller as it ran,
 * every timer line that is due signals, a thread whose virtual time came has time again,
 * and the caller's turn ends: it goes to the back of the queue its virtual time puts it on,
 * the next thread has its turn, and the caller stays ready.
 * Works while tracing is on, unlike the timer itself;
 * see DESIGN.md, "Verification".
 * Fails with KERR_STATE while tracing is off,
 * since a step no time brought would have every deadline fire early.
 */
#define OP_TEST_TICK 17
/*
 * Test (RIGHT_W): what a device interrupt does, on request. a1 = the line.
 * The Irq armed on the line masks it and signals its bits,
 * exactly as the interrupt would; the controller is not consulted.
 * Fails with KERR_STATE while tracing is off, as OP_TEST_TICK does,
 * or when nothing is armed on the line, which is when the controller would not raise it either,
 * and with KERR_INVALID_ARG for a line the controller does not have,
 * LOG_IRQ_LINE among them: the log's level is the log's, not a record's,
 * and so are the timer lines, which only OP_TEST_TICK moves.
 * It is how a replay fires an interrupt, since the host build has no devices;
 * see DESIGN.md, "Verification".
 */
#define OP_TEST_IRQ 22
/*
 * Test (RIGHT_W): a step of the clock landing within a restartable call, on request. a1 = n.
 * The n-th place from now at which any restartable call could stop between two steps, it stops,
 * interrupt pending or not, and does what OP_TEST_TICK does,
 * so another thread may run before the caller makes its call again. Zero disarms it.
 * It is how a replay has a thread run while another's call is half done,
 * since under tracing the timer stops no call; see DESIGN.md, "Verification".
 * Fails with KERR_STATE while tracing is off, as OP_TEST_TICK does.
 */
#define OP_TEST_PREEMPT 32

/*
 * CapTable (RIGHT_W): copy a capability from the caller's tables.
 * a1 = destination slot in the invoked table,
 * a2 = source slot in the caller's tables,
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
 * An Untyped whose delete as a root has begun makes nothing more (KERR_NO_MEMORY),
 * whatever leaves from below it: those that became roots lie in its memory until it goes.
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
 * A revoke that takes the calling thread's process, the table its a0 lies in,
 * or the capability it is made through ends there with KERR_OK,
 * and what it did not reach stays for another call to revoke.
 * Fails with KERR_INVALID_CAP for an empty slot,
 * and with KERR_STATE when the slot is an Untyped whose memory holds
 * the calling thread, its process, a table the process mounts or the invoked table.
 * Restartable.
 */
#define OP_CAP_REVOKE 23
/*
 * CapTable (RIGHT_W): derive a capability from one in the caller's tables.
 * The arguments are OP_CAP_COPY's.
 * The result is a child of the source in the derivation tree,
 * so revoking below the source takes it, and everything derived from it in turn.
 * An Untyped can be derived only while nothing is made of it, else KERR_NO_MEMORY,
 * and the derived one is the whole of it: the source makes nothing more
 * until the derived one and what was made of it are gone.
 */
#define OP_CAP_DERIVE 24
/*
 * CapTable (RIGHT_W): move a capability from the caller's tables.
 * a1 = destination slot in the invoked table, a2 = source slot in the caller's tables.
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
 * Every frame is a block: its size is a power of two of at least the smallest region,
 * the same for every region on the machine, which OP_CAP_INFO tells of any frame,
 * and its base is a multiple of its size.
 */
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
 *   CAP_PROCESS   the slot of the CapTable capability the process will use, mounted in its table slot 0,
 *                 which needs RIGHT_W; the table may lie in any pool;
 *                 and a4 = its region slots, one for each region it may have installed at once,
 *                 1 to the most the machine allows a process, which OP_CAP_INFO on any process tells,
 *                 else KERR_INVALID_ARG; see OP_PROCESS_INSTALL,
 *   CAP_THREAD    the slot of the Process capability the thread will run in,
 *                 and a4 = its places, one for each core it may hold the time of at once, 1 to the machine's cores,
 *                 else KERR_INVALID_ARG; see OP_TIME_BIND,
 *   CAP_NOTIFICATION  unused.
 * The object's capability is a child of the invoked Pool capability in the derivation tree,
 * so revoking below that capability takes it.
 * Fails with KERR_STATE while the pool is being destroyed.
 * A process holds its table by a capability derived from the one named,
 * so revoking below that capability, or destroying the table's pool,
 * leaves the process naming nothing there: every call that names a slot of it fails with KERR_INVALID_CAP.
 * A thread holds its process the same way, and the process may lie in any pool;
 * revoking below that capability, or destroying the process's pool,
 * stops the thread for good: see OP_THREAD_RESUME.
 * An Irq is not allocated here but bound, with OP_IRQ_BIND.
 */
#define OP_POOL_ALLOC 7
/*
 * Process (RIGHT_W): install a region into one of the process's region slots.
 * a1 = region slot index, below the process's region slots, else KERR_INVALID_ARG,
 * a2 = Frame capability slot in the caller's tables, a3 = rights to install, a subset of the frame's rights.
 * A process has no more region slots than the machine has protection entries for it,
 * so an install never runs out of entries; see OP_POOL_ALLOC.
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
 * Process (RIGHT_W): mount a table into one of the process's table slots,
 * so that its threads name the table's slots SLOT_IN(a1, index).
 * a1 = table slot index, below PROCESS_TABLES, else KERR_INVALID_ARG,
 * a2 = slot of a CapTable capability with RIGHT_W in the caller's tables.
 * Fails with KERR_SLOT_IN_USE when a table is mounted there already.
 * The process's hold on the table is a child of that capability in the derivation tree,
 * so revoking below it unmounts the table, as it does the one the process was allocated with, in table slot 0.
 * A process moves to another table with this, OP_CAP_MOVE and OP_PROCESS_UNMOUNT;
 * see DESIGN.md, "A process's tables".
 */
#define OP_PROCESS_MOUNT 39
/*
 * Process (RIGHT_W): clear a table slot.
 * a1 = table slot index, below PROCESS_TABLES, else KERR_INVALID_ARG.
 * Clearing an empty one succeeds.
 * The table stays as long as its pool does; the numbers that pointed into this table slot name nothing.
 */
#define OP_PROCESS_UNMOUNT 40

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
 * unless OP_THREAD_WRITE_REG moved its program counter,
 * and one stopped while it waited makes its wait again, see OP_THREAD_STOP.
 * One resumed while it still pays for a turn it lent, or before the core that ran it has trapped,
 * joins its queue as that turn ends.
 * Fails with KERR_STATE unless the thread is stopped,
 * and once its process was taken, see OP_POOL_ALLOC: it has nothing to run in.
 */
#define OP_THREAD_RESUME 13
/*
 * Thread (RIGHT_W): stop the thread, whatever it is doing.
 * a1, a2 and a3 are zero, else KERR_INVALID_ARG, so that they may be given a meaning later.
 * Returns a1 = what it found the thread doing: THREAD_WAS_STOPPED, THREAD_WAS_READY or THREAD_WAS_WAITING.
 * A stopped thread is left as it is, with the fault it has to tell, if any; see OP_THREAD_FAULT.
 * A ready one stops where it stands, between two instructions or at a call it has not made yet.
 * One another core runs stops once that core has trapped, which the call waits for,
 * and that trap is dropped, whatever the thread is by then:
 * a call it was making is not made, its pc staying at it, and a fault is not reported; see DESIGN.md, "Cores".
 * On ARM a trap that could not stack the thread's registers lost r0 to r3, r12, lr, pc and xpsr;
 * the stop then leaves the thread stopped where it faulted, with the kernel's report in the log, and signals nothing.
 * A waiting one leaves the notification's waiters, and the ring of lenders of the thread its OP_NOTIFY_LEND names,
 * and stands at the call it waited in, every register as that call found it:
 * a resume makes the call again, which checks everything afresh as a restartable call made again does,
 * and takes the bits signalled meanwhile unless another waiter took them,
 * or a watcher answers it, writing a0, a1 and the pc past the call with OP_THREAD_WRITE_REG.
 * The caller may stop itself, through a capability to its own thread; once resumed, it goes on past its call.
 * The thread keeps its process, its watch, its units and its virtual time, as at a fault,
 * and the threads that lend it their time go on waiting, lending it nothing while it is stopped.
 * A turn it pays for, lent to another thread, runs on, charged to it, until it ends,
 * at its request's end or as that thread waits, as one whose payer another core or an interrupt woke does.
 * It signals nothing, the watch included: a stop by another thread is known to the one that made it,
 * and a thread that stops itself says so first by its own means.
 * See DESIGN.md, "Threads".
 */
#define OP_THREAD_STOP 16
/* What OP_THREAD_STOP found the thread doing. */
#define THREAD_WAS_STOPPED 0
#define THREAD_WAS_READY   1
#define THREAD_WAS_WAITING 2
/*
 * Thread (RIGHT_W): name what the thread's faults signal, its watch.
 * a1 = the slot of a Notification capability, which needs RIGHT_W and may lie in any pool,
 * a2 = the bits a fault signals there, of which only those the capability may signal are kept,
 * and a2 with none of them fails with KERR_NO_RIGHTS, and those kept not an aligned block, as a capability's bits are,
 * with KERR_INVALID_ARG; a2 = 0 clears the watch and ignores a1.
 * A fault, an access fault, an illegal instruction, a misaligned access or a breakpoint,
 * stops the thread at the instruction that faulted, with its registers as they were,
 * and signals the bits as OP_NOTIFY_SIGNAL would; nothing else stops, the machine goes on.
 * A thread without a watch stops all the same, and nobody hears.
 * OP_THREAD_STOP signals nothing.
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
 * A stop of a running or waiting thread is no fault, and leaves nothing to tell but a lost frame on ARM,
 * and a stop of one stopped where it faulted keeps the fault; see OP_THREAD_STOP.
 * See DESIGN.md, "Faults".
 */
#define OP_THREAD_FAULT 33
/*
 * Thread (RIGHT_W): read one register of a stopped thread.
 * a1 = the register: x0 to x31 on RISC-V, where x0 reads zero, r0 to r14 on ARMv7-M,
 * or THREAD_REG_PC for the program counter, which on ARM carries bit 0 for Thumb, as OP_THREAD_CONFIGURE takes it.
 * Returns a1 = its value.
 * Fails with KERR_STATE unless the thread is stopped, which OP_THREAD_STOP makes any thread,
 * and with KERR_INVALID_ARG for a number that names no register.
 * One stopped while it waited reads at its call: the pc at its ecall or svc, a7 the operation, a0 to a3 the call's arguments.
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
 * a1 = the span of the request the wake begins: the counts of the counter past the thread's virtual time it is due,
 * on each core whose time it holds, as OP_TIME_BIND's a4 is, a power of two up to 2^SPAN_MAX, else KERR_INVALID_ARG;
 * 0 keeps the span its binding or its last wait gave.
 * A span holds for the requests after that one too, until another.
 * So a thread says at each wait how soon what wakes it must be done:
 * woken while its virtual time is at or behind the clock, it runs units / TIME_UNITS of the span within the span;
 * see DESIGN.md, "Scheduling".
 * Returns a1 = the bits, which is never zero, and clears them.
 * Blocks until some bit is set;
 * the bits are sticky, so a signal that arrives first is not lost.
 * A stop takes the waiter off and leaves it at this call, which a resume makes again; see OP_THREAD_STOP.
 */
#define OP_NOTIFY_WAIT 15
/*
 * Notification (RIGHT_R): wait as OP_NOTIFY_WAIT does, lending a thread the caller's time meanwhile.
 * a1 = the slot of a Thread capability with RIGHT_X, the borrower; the caller itself fails with KERR_INVALID_ARG.
 * a2 = the span, as OP_NOTIFY_WAIT's a1, which the borrower's requests on the caller's time take too,
 * so a client says how soon its server must answer.
 * Returns a1 = the bits, at once if some bit is set, which lends nothing.
 * While it waits, a borrower without time of its own on a core runs there on the units of the thread
 * that has lent it time longest, if that one holds the time of that core too,
 * and a caller whose own time pays for its turn hands the rest of the turn to the borrower,
 * if it is that one and the borrower waits for a turn.
 * A signal from the borrower that wakes the caller hands the rest of the turn back, see OP_NOTIFY_SIGNAL.
 * A borrower that lends in turn, without time for its own borrower, passes on what it borrows, through up to 64 lenders,
 * on a core every one of them holds the time of.
 * A caller whose borrower is destroyed wakes with KERR_INVALID_CAP and no bits, as one whose notification is.
 * A stop takes the caller off its notification and its borrower's lenders and leaves it at this call,
 * which a resume makes again, looking the borrower up afresh; see OP_THREAD_STOP.
 * A stopped borrower keeps its lenders, whose time buys nothing until it is resumed.
 * See DESIGN.md, "Communication and synchronisation".
 */
#define OP_NOTIFY_LEND 38
/*
 * Notification: derive a capability that may signal fewer bits, with the same rights.
 * a1 = the bits, of which those the invoked capability may signal are kept, a2 = destination slot.
 * Fails with KERR_INVALID_ARG when none is kept, or when those kept are not an aligned block:
 * 1, 2, 4, 8, 16 or 32 bits, starting at a multiple of their count, which six bits of the slot name.
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
 * A timer line fires once the delay has passed, no earlier:
 * the delay is rounded up to whole counts of the counter, and the line fires at the first count past the call by that many,
 * as the timer the kernel sets for it interrupts; a delay of zero fires at the next count.
 * A longer delay than 32 bits of microseconds is several calls.
 * With IRQ_SET_PERIOD the delay is a period, rounded up to whole counts and not zero,
 * counted from the line's last deadline, or its bind, rather than from the call:
 * the line fires at the first count after the call
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
 * Under tracing the time is the record's, which only OP_TEST_TICK moves; see DESIGN.md, "Verification".
 * The rate and the default span are OP_CAP_INFO's too, which needs no right.
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
 * at the first count past the call by at least that many, as a timer line fires; see OP_IRQ_SET.
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
 * Time (RIGHT_W): bind a thread's place to units of the capability, which it then earns there.
 * a1 = the slot of the Thread capability, which needs RIGHT_W,
 * a2 = the offset of the first unit, which must lie within the capability even for none,
 * a3 = how many units, zero for none,
 * a4 = the span, how many counts of the counter past its virtual time each of the thread's requests there is due:
 *      0 for the default span, the longest power of two within a millisecond, which OP_CAP_INFO on a Clock tells,
 *      else a power of two up to 2^SPAN_MAX, else KERR_INVALID_ARG,
 *      until a wait gives another, see OP_NOTIFY_WAIT and DESIGN.md, "Scheduling".
 * The units must all be of one core, or the call fails with KERR_INVALID_ARG: the core of the first unit, for none too.
 * The bind fills the thread's place that holds that core's time, or an empty one,
 * and fails with KERR_LIMIT when every place holds another core's; see OP_POOL_ALLOC.
 * Each count the thread runs there on u units moves the place's virtual time on TIME_UNITS / u counts,
 * running on spare time moves it too, up to a span ahead of the clock,
 * and a request starts only once the virtual time lies at most a span ahead of the clock;
 * with RIGHT_X on the capability the thread also runs on that core's spare time, for free.
 * None without RIGHT_X holds the core's time all the same: the thread runs there on lent time alone, see OP_NOTIFY_LEND.
 * The thread runs on the cores its places hold the time of; see DESIGN.md, "Cores" and "Scheduling".
 * Fails with KERR_OVERLAP if another thread earns one of the units.
 * The place leaves the units it earned before, which keep its virtual time, and takes the latest of its own and the new units',
 * so that no bind forgives what a thread ran ahead of its units; its request starts afresh.
 * The binding is a child of the invoked capability, so revoking below that capability empties the place:
 * a thread whose places are all empty keeps its state and does not run until it is bound again.
 * A thread whose place is emptied or bound anew while it runs there, the caller itself among them, goes on
 * while it has time or spare time, and gives the processor up as the trap ends otherwise;
 * a turn on spare time ends as a thread with time becomes ready, see DESIGN.md, "Scheduling".
 */
#define OP_TIME_BIND 29
#define SPAN_MAX 29u /* the longest span OP_TIME_BIND, OP_NOTIFY_WAIT and OP_NOTIFY_LEND take, 2^SPAN_MAX counts */

/*
 * Any capability: describe it. a1 = the page, 0, the only one so far, and a2 = a3 = 0;
 * anything else fails with KERR_INVALID_ARG and writes nothing.
 * Every type takes it, and it needs no right:
 * describing changes nothing and returns nothing a call takes as a reference;
 * it tells the capability's own words, the constants of the machine its type's operations depend on,
 * and the accounting its holder needs.
 * Returns a1 = the type, one of CAP_*, in the low byte and the rights in the byte above,
 * see CAP_INFO_TYPE and CAP_INFO_RIGHTS, and in a2, a3 and a4 by the type:
 *   CAP_FRAME         the base, the size, and the size of the smallest region, a power of two of at least 8
 *   CAP_UNTYPED       the base, the size, and 0 while nothing lies below it, else the type of what lies first there:
 *                     CAP_FRAME for a frame or a region installed from one, CAP_UNTYPED for a half or a derived Untyped,
 *                     and CAP_POOL for a pool, which lies there whether anything names it or not,
 *                     until a revoke below destroys it;
 *                     once a delete handed up what lay below the deleted capability, it is one of a mixture
 *   CAP_POOL          its size, its room, the bytes an allocation may still take of it,
 *                     and 1 while it is being destroyed, when its room is 0, else 0
 *   CAP_CAPTABLE      its number of slots, 0, 0
 *   CAP_PROCESS       its region slots, every one of which may be filled at once,
 *                     the most a process may have on this machine, a protection entry each, see OP_POOL_ALLOC, and 0
 *   CAP_THREAD        its places, the cores whose time they hold, bit c for core c, and 0
 *   CAP_NOTIFICATION  the bits it may signal, see OP_NOTIFY_CARVE, 0, 0; never the bits that are set
 *   CAP_IRQ_LINE      the first line, the count, and 1 for timer lines, 0 for the log's and the controller's;
 *                     the cores' line, where there is one, reads as the controller's, and binding it fails with KERR_OVERLAP
 *   CAP_IRQ           its line, the bits it may signal, 0 once its notification was taken, see OP_IRQ_BIND,
 *                     and 1 on a timer line, else 0; never the bits it is armed with
 *   CAP_TIME          the first unit, the count, and 0;
 *                     unit u is of core u / TIME_UNITS, so BOOT_CAP_TIME's count is TIME_UNITS for each core the kernel runs
 *   CAP_ROUTE_LINE    the first channel, the count, and 0
 *   CAP_ROUTE         its channel, the ends connected, bit ROUTE_EVENT and bit ROUTE_TASK, and 1 while armed, else 0
 *   CAP_CLOCK         the counter's rate in Hz, as OP_CLOCK_READ gives it, the default span in counts of the counter,
 *                     the longest power of two within a millisecond, see OP_TIME_BIND, and 0
 *   CAP_PRINT, CAP_HALT and CAP_TEST  0, 0, 0
 * No result is an object capability's address or a link of the derivation tree,
 * though an Untyped's base may be where a pool made of it keeps its descriptor.
 * A pool's room and what lies below an Untyped are as the call found them; a call on another core may change them at once.
 */
#define OP_CAP_INFO 41
#define CAP_INFO_TYPE(a1)   ((a1) & 0xffu)
#define CAP_INFO_RIGHTS(a1) (((a1) >> 8) & 0xffu)
/*
 * Pool: what an object of a type would take of it. No right needed, as for OP_CAP_INFO.
 * a1 = the type: CAP_CAPTABLE, CAP_PROCESS, CAP_THREAD or CAP_NOTIFICATION, as OP_POOL_ALLOC makes them,
 *      CAP_IRQ, as OP_IRQ_BIND makes one of a pool, CAP_ROUTE, as OP_ROUTE_BIND does,
 *      or CAP_POOL, the descriptor at the base of a pool's memory;
 * a2 = 0; a3 = the number of slots for CAP_CAPTABLE, 1 to 1024, of region slots for CAP_PROCESS,
 * 1 to the most the machine allows, see OP_CAP_INFO, of places for CAP_THREAD, 1 to the machine's cores,
 * and 0 for every other type.
 * Anything else fails with KERR_INVALID_ARG and writes nothing.
 * Returns a1 = the bytes, a multiple of sixteen, as the pool counts them,
 * and a2 = the pool's room, as OP_CAP_INFO tells it; a3 and a4 are unchanged.
 * An allocation of the type that passes its other checks fails with KERR_NO_MEMORY exactly when a1 is more than a2,
 * and with KERR_STATE while the pool is being destroyed, whose room is 0,
 * unless something else allocates of the pool in between, on another core or while a bind stopped to be made again.
 * A pool of n bytes has a room of n less what CAP_POOL takes before anything is allocated of it.
 * The sizes are the kernel build's, its architecture and board among them, and may change from one kernel to the next;
 * see DESIGN.md, open decision 29.
 */
#define OP_POOL_INFO 42

/*
 * RouteLine: derive a smaller range of channels with the same rights.
 * a1 = offset from the first channel, a2 = count, a3 = destination slot.
 * Like OP_IRQ_CARVE, this is a table operation that touches no kernel memory,
 * and the new capability is a child of the invoked one.
 */
#define OP_ROUTE_CARVE 43
/*
 * RouteLine (RIGHT_W): bind the one channel the capability names, as a Route object,
 * which wires an event of one device to a task of another once both are connected and it is armed;
 * see DESIGN.md, "Routes".
 * a1 = the slot of the Pool capability the Route is allocated from, which needs RIGHT_W,
 * a2 = destination slot for the Route capability, a3 = 0.
 * As OP_IRQ_BIND, the bind consumes the invoked slot, and what was derived from it, so a2 may name it,
 * and the Route capability is a child of the Pool capability.
 * Fails with KERR_OVERLAP if a Route is already bound to the channel;
 * the channel is free again once that Route's pool is destroyed.
 * Fails with KERR_NO_MEMORY if the pool has no room for the Route, and with KERR_STATE while it is being destroyed.
 * The new Route connects nothing and is disarmed.
 */
#define OP_ROUTE_BIND 44
/*
 * Route (RIGHT_W): connect one end of the route; connecting an end it had disarms it.
 * a1 = ROUTE_EVENT or ROUTE_TASK, which end,
 * a2 = the slot of a Frame capability over the device the end lies in,
 *      which needs RIGHT_R for the event and RIGHT_W for the task,
 * a3 = the end, as the board names an event or a task within that device's registers; see MANUAL.md, section 3.
 * Fails with KERR_INVALID_ARG for an end the frame does not hold, or any end on a board without a router.
 * The Route holds the frame by a capability derived from the one named, which a revoke above takes,
 * as it takes the end, disarming the route; connecting the end again replaces it, and disarms the route too.
 */
#define OP_ROUTE_END 45
#define ROUTE_EVENT 0
#define ROUTE_TASK  1
/*
 * Route (RIGHT_W): arm the route, a1 = 1, which needs both its ends, else KERR_STATE,
 * or disarm it, a1 = 0; anything else fails with KERR_INVALID_ARG.
 * While it is armed the router triggers the task each time the event comes, with no processor in between;
 * an event that came before the arm, or while the route was disarmed, triggers nothing.
 */
#define OP_ROUTE_SET 46

/*
 * Thread (RIGHT_R): how many of the thread's requests it ran to the end of without waiting, see OP_TIME_BIND.
 * a1, a2 and a3 are zero, else KERR_INVALID_ARG.
 * Returns a1 = that count, of every core the thread holds the time of, modulo 2^32: a reader takes differences.
 * A thread whose units fit its work waits within every request and runs none out, whatever the other threads do.
 * A request runs out as the kernel charges the turn past its end, at the timer while another thread wants the core,
 * else at the thread's next wait or change of turn; one on lent time counts as the borrower's.
 */
#define OP_THREAD_OVERRUNS 47

/*
 * One above the highest operation code; the fuzzer's mutator draws below it.
 * Codes 11 and 27 are no operation's: they were OP_FRAME_INFO and OP_UNTYPED_INFO, which OP_CAP_INFO replaced.
 */
#define OP_COUNT 48

/*
 * Capability slots the kernel fills in the root task's table at boot.
 * Slot 0 is left empty so that an uninitialised index is an error.
 */
#define BOOT_CAP_NULL      0
#define BOOT_CAP_CAPTABLE  1 /* the root task's own table */
#define BOOT_CAP_PROCESS   2 /* the root task's own process */
#define BOOT_CAP_THREAD    3 /* the root task's only thread, with a place on every core */
#define BOOT_CAP_POOL      4 /* the boot pool the root objects live in */
#define BOOT_CAP_PRINT     5 /* Print: writing into the kernel's log */
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
#define BOOT_CAP_HALT      18 /* Halt: halting the machine */
#define BOOT_CAP_TEST      19 /* Test: driving the machine as a test does */
/*
 * RouteLine: every channel of the board's router the kernel does not keep, with RIGHT_W;
 * empty on a board without one. See DESIGN.md, "Routes".
 */
#define BOOT_CAP_ROUTES    20
/*
 * Frames over the devices the board lists, BOOT_DEVICES of them, from this slot up in the board's order;
 * QEMU's lists none.
 * BOOT_DEVICES comes from the kernel's board.h, or a program's devices.h, which names each slot.
 */
#define BOOT_CAP_DEVICES   21
/* The root task's own slots start here; on QEMU it is 21. */
#define BOOT_CAP_COUNT     (BOOT_CAP_DEVICES + BOOT_DEVICES)

/*
 * The kernel's log.
 * The kernel has no console: every byte it prints, OP_PRINT's included,
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
    uint32_t a1, a2, a3, a4, a5, a6;
};
#endif

/* Fixed limits visible to user programs. */
/* The most region slots a process has on any machine; OP_CAP_INFO on a process tells this machine's. */
#define PROCESS_REGION_SLOTS_MAX 16
#define PROCESS_TABLES 2 /* table slots per process, see SLOT_TABLE_SHIFT */
#define ROOT_TABLE_SLOTS (BOOT_CAP_COUNT + 46) /* slots in the root task's table, 67 on QEMU */
/* Region slots of the root task's process, or the most a process may have where that is fewer; see OP_CAP_INFO. */
#define ROOT_REGION_SLOTS 8
#define POOL_MIN_SIZE 64 /* the smallest Untyped OP_UNTYPED_RETYPE makes a pool of */
#define TIMER_LINES 16 /* on the whole machine; see BOOT_CAP_TIMER_LINES */
#define TIME_UNITS 64 /* of each core, numbered core by core; see BOOT_CAP_TIME */

#endif
