# rvuos user manual: the programming model

This is chapter 5 of the user manual.
[`MANUAL.md`](../MANUAL.md) lists all chapters,
and explains the words this chapter uses.

## 5. The programming model

### 5.1 Processes, threads and regions

A **process** is a protection domain.
It has three parts:

- `PROCESS_TABLES` table slots, two today.
  They hold the capability tables in which the process names its capabilities.
- Its region slots.
  Its creator chooses how many:
  at least one, and at most the number of PMP entries the core has for regions.
- The threads that run in it.

A **thread** is an execution context inside a process:
a set of registers and a state.
The threads of one process share its regions and its capability tables.

A **frame** is a range of physical addresses, of memory or of a device's registers,
with the most rights that any use of it may have.
A **region** is a frame installed into one of a process's region slots.
It gives that process access to the range,
with the rights chosen when the frame was installed.
A process sees memory only through its regions.

An **Untyped** is memory that has not become anything yet.
It cannot be installed.
It is made into a frame, into a pool, or into two halves.

The kernel does not know what a code segment, a stack or a heap is.
Whoever builds a process decides its layout.

### 5.2 Capabilities and the tables

A process names everything by a **slot number**.
A slot number points to a slot in one of the process's capability tables.
`SLOT_IN(t, i)` is index `i` of the table in table slot `t`.
Its value is `t << SLOT_TABLE_SHIFT | i`, and `SLOT_TABLE_SHIFT` is 10.
So the numbers below 1024 are slots of the first table,
and a process with one table names its slots by their index.

A slot is either empty or holds one capability.
A capability is a type, a set of rights, and two words.
The kernel reads the two words according to the type.
A program never sees how slots are linked, or where an object lies in memory.
But whoever holds a capability can ask what it is, with `OP_CAP_INFO` (section 6.16).
This needs no right.
The answer gives the capability's type, its rights and what it names,
for example the memory that a frame covers.

Every system call is an invocation of the capability in one slot.
The kernel first finds the slot.
Then it checks the capability's type and rights against the operation.
Only after that does it look at the arguments.
Section 6.1 gives the exact order.
The call fails with `KERR_INVALID_CAP` in these cases:
the slot is empty,
the index is out of range,
or the table slot in the number holds no table.

**How a process holds its tables.**
A process holds each of its tables through a capability of its own.
For the first table, this capability is derived from the `CapTable` capability that the process was allocated with.
For a table mounted with `OP_PROCESS_MOUNT` (section 6.7),
it is derived from the `CapTable` capability that the mount names.
A table can lie in any pool.
Several processes can be allocated with one table, or mount it.
They then share the table.

Two things take a table away from a process, as `OP_PROCESS_UNMOUNT` does:
revoking below that capability,
or destroying the pool that the table lies in.
After that, every slot number that points into that table slot fails with `KERR_INVALID_CAP`.
A process whose table slots are all empty fails every call.
The table itself lasts as long as its pool, and so does everything in it.

**Moving to a new table.**
A process can move to another table by itself,
and every capability keeps its slot number.
The steps are:

1. Mount the new table in table slot 1.
2. Move each capability `i` to index `i` of the new table with `OP_CAP_MOVE`.
   During the move, `SLOT_IN(1, i)` names the moved capability.
   Move the new table's own capability last.
3. Unmount table slot 0, and then mount the new table there.
   Do both through `SLOT_IN(1, process)`,
   where `process` is the index of the process's capability to itself.
4. Unmount table slot 1.

This lets a process grow beyond the table it was made with.
It also lets a process leave memory that it has to give back.
A revoke refuses to destroy memory while one of the caller's tables lies in it (section 5.5).
No other thread of the process may make a call during the move.
`self_migrate` in `user/lib/self.h` does these steps.

**Rights.**
Rights are three bits:

| Bit | Value | Frame, Untyped | Other types |
|---|---|---|---|
| `RIGHT_R` | 1 | read | `Notification`: wait; `Thread`: read its overruns |
| `RIGHT_W` | 2 | write | control the object: allocate, install, configure, signal, set, bind, copy or move into a table; `Print`: write into the log; `Halt`: halt the machine; `Test`: drive it as a test does |
| `RIGHT_X` | 4 | execute | `Time`: the threads bound through it also run on spare time |

**Where a new capability goes.**
Every operation that makes a capability puts it into a slot of one of the caller's own tables.
An argument names that slot, and the slot must be empty.
A process needs no capability to receive into its own tables.
The `CapTable` capability is for writing into a table.
For example, a parent fills a child's table with `OP_CAP_COPY`, `OP_CAP_DERIVE` or `OP_CAP_MOVE`
before it starts the child.
A process that holds a capability to its own table can copy and move within that table,
clear its own slots, and revoke below them.

**The derivation tree.**
Every capability remembers the capability it was derived from.
Together they form a tree, and `OP_CAP_REVOKE` follows it.
In this manual, a capability is **below** another one when it lies under it in this tree.
A capability that lies below nothing is a **root**.
The tree grows in these ways:

| Operation | Where the new capability goes |
|---|---|
| `OP_CAP_DERIVE` | below the source |
| `OP_CAP_COPY` | next to the source, under the source's parent; a copy of a root is a root; an Untyped is never copied |
| `OP_CAP_MOVE` | in the source's place; the source slot becomes empty, and everything that was below the source is now below the new capability |
| `OP_FRAME_CARVE`, `OP_NOTIFY_CARVE`, `OP_IRQ_CARVE`, `OP_TIME_CARVE`, `OP_ROUTE_CARVE`, `OP_DMA_CARVE` | below the invoked capability |
| `OP_UNTYPED_RETYPE` of a frame, `OP_UNTYPED_SPLIT` | below the invoked Untyped |
| `OP_UNTYPED_RETYPE` of a pool | below the pool's own node, which is below the invoked Untyped |
| `OP_PROCESS_INSTALL` | the installed region goes below the frame |
| `OP_POOL_ALLOC` | below the invoked `KernelPool` capability |
| `OP_POOL_ALLOC` of a `Process` | also: the process's hold on its first table goes below the `CapTable` capability |
| `OP_PROCESS_MOUNT` | the process's hold on the table goes below the `CapTable` capability |
| `OP_POOL_ALLOC` of a `Thread` | also: the thread's hold on its process goes below the `Process` capability |
| `OP_IRQ_BIND` | below the `KernelPool` capability; the invoked line capability is cleared, with everything below it |
| `OP_IRQ_BIND` | also: the `Irq`'s hold on its notification goes below the `Notification` capability |
| `OP_TIME_BIND` | the thread's hold on its units goes below the invoked `Time` capability |
| `OP_ROUTE_BIND` | below the `KernelPool` capability; the invoked channel capability is cleared, with everything below it |
| `OP_ROUTE_END` | the `Route`'s hold on the end goes below the `Frame` capability |
| `OP_DMA_BIND` | below the `KernelPool` capability; the invoked channel capability is cleared, with everything below it |
| `OP_DMA_END` | the `Dma`'s hold on the end goes below the `Frame` capability |
| boot | roots, except: the root task's frames and the boot pool's block are below `BOOT_CAP_ROOT_RAM`; the boot pool's own node is below that block; the capabilities to the boot pool and its objects are below that node |

**Revoke.**
`OP_CAP_REVOKE` on a slot clears everything below it, in every table of every process.
This includes:

- the capabilities derived from it, their copies, and what was derived from those,
- every region installed from any of them,
- every binding of a thread to units made through any of them.

Objects that hold something through any of them lose it:

- A thread made through any of them loses its process and stops (section 5.6).
- An `Irq` bound through any of them loses its notification and is disarmed (section 5.9).
- A `Route` connected through any of them loses that end and is disarmed (section 5.13).
- A `Dma` connected through any of them loses that end, and its channel stops (section 5.14).

A revoke below an Untyped destroys every pool made from it (section 5.5).
The revoked slot itself stays as it is.

**Copy or derive?**
Revoking a copy takes nothing from the original.
Revoking the original takes nothing from a copy.
To take both back, revoke below the capability that both were derived from.
So derive a capability to lend it,
and copy a capability to keep a second handle to something you hold.
A copy cannot take back what was derived from its source,
because that is below the source and not below the copy.
To hand over a capability together with that power, move it.

An Untyped can only be derived, and only while nothing was made from it.
The derived Untyped covers all of its source.
The source cannot make anything until the derived one is gone.

**Delete.**
`OP_CAP_DELETE` clears one slot.
What was below that slot moves up to the slot's parent.
So if you delete your own copy of something you lent, you do not take it back.
Destroying a pool clears every slot of the tables in that pool in the same way.
So what a process handed out through its table lives on after the process,
below whatever the process derived it from.

Copying and deriving both apply a rights mask,
so rights stay the same or become fewer, never more.
A move keeps the rights unchanged, because the capabilities below it may hold all of them.

Slot 0 of the root task's table is empty on purpose,
so that an uninitialised index fails.
Programs are encouraged to keep the same convention.

### 5.3 Objects

| Type | Constant | Object behind it | Created by |
|---|---|---|---|
| `Untyped` | `CAP_UNTYPED` | none: the slot holds the block | boot, `OP_UNTYPED_SPLIT` |
| `Frame` | `CAP_FRAME` | none: the slot holds base and size | boot, `OP_FRAME_CARVE`, `OP_UNTYPED_RETYPE` |
| `KernelPool` | `CAP_POOL` | the pool's descriptor, at its base | `OP_UNTYPED_RETYPE` |
| `CapTable` | `CAP_CAPTABLE` | a table of `n` slots | `OP_POOL_ALLOC` |
| `Process` | `CAP_PROCESS` | capabilities to its tables, region slots, a PMP image | `OP_POOL_ALLOC` |
| `Thread` | `CAP_THREAD` | its registers, a state and its places | `OP_POOL_ALLOC` |
| `Notification` | `CAP_NOTIFICATION` | one word of sticky bits | `OP_POOL_ALLOC` |
| `IrqLine` | `CAP_IRQ_LINE` | none: the slot holds the first line and a count | boot, `OP_IRQ_CARVE` |
| `Irq` | `CAP_IRQ` | one line bound to a notification, with a deadline on a timer line | `OP_IRQ_BIND` |
| `Time` | `CAP_TIME` | none: the slot holds the first unit and a count | boot, `OP_TIME_CARVE` |
| `RouteLine` | `CAP_ROUTE_LINE` | none: the slot holds the first channel and a count | boot, `OP_ROUTE_CARVE` |
| `Route` | `CAP_ROUTE` | one channel, its two ends and whether it is armed | `OP_ROUTE_BIND` |
| `DmaLine` | `CAP_DMA_LINE` | none: the slot holds the first channel and a count | boot, `OP_DMA_CARVE` |
| `Dma` | `CAP_DMA` | one channel and its two ends | `OP_DMA_BIND` |
| `Print` | `CAP_PRINT` | none | boot |
| `Halt` | `CAP_HALT` | none | boot |
| `Test` | `CAP_TEST` | none | boot |
| `Clock` | `CAP_CLOCK` | none: there is one counter | boot |

These capabilities have no kernel object behind them:
`Untyped`, `Frame`, `IrqLine`, `Time`, `RouteLine`, `DmaLine`, `Print`, `Halt`, `Test` and `Clock`.
Some operations change only the tables and touch no kernel memory:
carving a frame, a range of lines, units or channels,
retyping an Untyped into a frame,
and splitting an Untyped.
This is why the root task can hand out memory, lines and time
before it has created any pool.

### 5.4 Memory: Untyped, frames and installing

Every frame and every Untyped is a **block**:
its size is a power of two, and its base is a multiple of its size.
One PMP entry in NAPOT mode describes exactly such a range.
The smallest block is eight bytes, or the PMP's grain if that is larger.
`OP_CAP_INFO` on any frame returns the smallest block size in `a4`.
A range of any other size is rounded up,
or made of several blocks, which use one region slot each.

An Untyped is **free** while nothing made from it remains.
It is **made** while something made from it remains.
`OP_CAP_INFO` tells which, in `a4`:
0 while the Untyped is free,
or else the type of the first capability below it: `CAP_FRAME`, `CAP_UNTYPED` or `CAP_POOL`.
It shows only the first one.
When a delete has moved up what was below the deleted capability,
several kinds can lie below the Untyped side by side.
A delete of an Untyped that is a root may stop part way (section 6.4).
Such an Untyped makes nothing until it is gone, even when `a4` says 0.
The reason is that what it made became roots, and they lie in its memory.

A free Untyped makes one thing out of all of its memory:

- `OP_UNTYPED_SPLIT` makes two Untypeds, its lower half and its upper half.
- `OP_UNTYPED_RETYPE` makes a frame or a pool.
- `OP_CAP_DERIVE` makes an Untyped of all of it, for lending (section 6.4).

A made Untyped makes nothing; the call fails with `KERR_NO_MEMORY`.
So the things that one Untyped makes never overlap,
and a frame never overlaps a pool.
When everything it made is gone, whether deleted, destroyed or revoked,
the Untyped is free again, all of it.

**Choosing where a block lies.**
The program chooses where a block lies, by the half it takes.
The kernel has no allocator.
To get a small block out of a large Untyped, split it in halves again and again.
Keep the halves you do not use yet: they are the program's free memory.
You may delete a half if you do not want to keep a slot for it.
What was made from that half then moves below its parent,
and the parent is free again once all of that is gone.
A frame or an Untyped has the rights of the Untyped it was made from.
`OP_FRAME_CARVE` hands out a block inside a frame.
A program that lays memory out at fixed offsets
should check the smallest block size once, and fail early if its layout does not fit.

**Rules for installing a frame into a process:**

- The installed rights must be a non-empty subset of the frame's rights.
- Write without read is refused,
  because PMP reserves that encoding, and the hardware may do anything with it.
- On ARM, execute without read is refused too,
  because the MPU fetches only what a thread may read.
- Regions installed in one process must not overlap each other.
- An install takes effect at once, even for the running process.

**PMP budget.**
Every installed region uses one NAPOT entry, so `n` regions use `n` entries.
The number of a process's region slots is fixed when the process is made, by `a4` of `OP_POOL_ALLOC`.
It is from one to the number of entries the core has for regions (section 10).
More, or none, fails with `KERR_INVALID_ARG`.
So all region slots can be filled at the same time.
`OP_CAP_INFO` on any process returns its number of region slots in `a2`,
and the largest number allowed in `a3`.
The root task's process has `ROOT_REGION_SLOTS` region slots, eight,
or the largest number allowed if that is less.

A memory access must lie entirely within one PMP entry.
When two regions touch, no single load, store or instruction fetch may cross the boundary between them.
So whoever creates a process lays it out so that no access crosses such a boundary.

Sharing needs no copy.
If the same range is installed into two processes, both can access it,
and no byte passes through the kernel.
The cost is one region slot and one PMP entry in each process.

### 5.5 Pools

A process gives memory to the kernel with `OP_UNTYPED_RETYPE` to `CAP_POOL`.
This makes the whole Untyped into a pool.
The Untyped must have both read and write rights,
and must be at least `POOL_MIN_SIZE`, 64 bytes.
The kernel puts the pool's descriptor at the pool's base,
and returns a `KernelPool` capability with all rights.
From then on, the memory belongs to the kernel.
No frame covers it, because the things that one Untyped makes never overlap.
The kernel zeroes each object when it allocates it,
so old data that the process left in the memory never becomes part of an object.

Objects are allocated from a pool one after another (bump allocation), aligned to 16 bytes.
They are never freed one by one.
Their sizes depend on the architecture, the board and the build, and they may change.
So a program asks the pool:

- `OP_POOL_INFO` tells how much an object of a type takes, as the pool counts it.
  For `CAP_POOL`, it tells the size of the descriptor (section 6.6).
- `OP_CAP_INFO` on the pool tells how much room is left in it.

A pool for a set of objects needs room for the descriptor and for the objects.
Round that up to a power of two, and to at least `POOL_MIN_SIZE`, because an Untyped is a block.
Today's sizes, after the kernel rounds them:

| Object | Bytes |
|---|---|
| pool descriptor | 48 |
| `CapTable` with `n` slots | 16 + 16 × n |
| `Process` with `r` region slots | 48 + 20 × r + r rounded up to 4, rounded up to 16; 16 more on the ESP32-C6 |
| `Thread` with `k` places | 224 + 40 × k on RISC-V, 160 + 40 × k on ARM, rounded up to 16 |
| `Notification` | 16 |
| `Irq` | 48 |
| `Route` | 64 |
| `Dma` | 64 |

For example, take a minimal child process:
a table of 10 slots, a process with two region slots, a thread with one place, and two notifications.
On RISC-V it costs 624 bytes, including the descriptor.
On the ESP32-C6 it costs 16 bytes more, and on ARM it costs 560 bytes.
Each extra region slot costs 20 bytes,
plus a few bytes for the configuration of its entry.

**Objects in different pools.**
A process's tables, a thread's process and an `Irq`'s notification can each lie in any pool
(sections 5.2, 5.6 and 5.9).
Each of them is held through a capability,
and destroying the pool it lies in clears that capability.

**Capabilities to a pool.**
Every capability to an object is below the `KernelPool` capability it was allocated through.
Every `KernelPool` capability is below the pool's own node.
The kernel keeps this node in the pool, and no process can name it.
So revoking below a `KernelPool` capability takes back what was allocated through it.
Deleting the last `KernelPool` capability does not destroy the pool.
The pool stays, and nothing can reach it,
until the Untyped it was made from is revoked.
During all that time, `OP_CAP_INFO` on that Untyped says that a pool is below it.

**Destroying a pool.**
To destroy a pool, revoke below the Untyped it was made from,
or below any capability above that Untyped.
No operation on the pool itself destroys it, so a `KernelPool` capability can only allocate.
A pool takes the whole of its Untyped.
So if you plan to give a pool's memory back, give the pool an Untyped of its own.
The destroy does all of this:

- It destroys every object in the pool.
- It clears every capability, in every table of every process,
  that names the pool or one of its objects.
- It clears every slot of the pool's own tables, as `OP_CAP_DELETE` does.
  So what was derived from those slots moves up to their parents.
- It wakes every thread that waits on a notification in the pool.
  The wait returns `KERR_INVALID_CAP` and no bits.
- It stops every thread that ran in a process in the pool,
  whatever pool the thread itself lies in.
- It disarms every `Irq` bound to a notification in the pool,
  whatever pool the `Irq` lies in, and masks its line.
  The line stays bound.
- It masks the interrupt line of every `Irq` in the pool, and this frees the line.
- It zeroes the memory that its objects used, and leaves the rest as it was.
  So if whoever destroys the pool should not read the old data,
  clear the memory before you make it into a pool.
- It gives the memory back to the Untyped that the pool was made from.

The revoke fails with `KERR_STATE` in two cases:

- The calling thread, its process, or a table that the process mounts lies in the Untyped's memory.
  So a thread cannot destroy what it runs on,
  or a table that it names capabilities in.
  It must first unmount that table, or move out of it.
- The table in which the caller names the Untyped lies in that memory.
  The reason is that the capability the call is made through
  would disappear in the middle of destroying that table's pool.

The boot pool holds the root task, so the root task cannot destroy it.
But any other thread that holds `BOOT_CAP_POOL_RAM`,
the block the boot pool was made from, can destroy it.
The root task is then destroyed with it (section 7).

A destroy can be restarted.
It may stop for an interrupt, and it goes on when the call is made again.
A pool that is being destroyed allocates nothing; the call fails with `KERR_STATE`.
A capability to it that still exists describes it as being destroyed, with no room left.

**Taking memory back from a borrower.**
A lender may want memory back from a borrower that still runs.
To make this possible, it lends the memory as an Untyped derived from its own,
and later revokes below its own Untyped (section 5.2).
This takes back the borrower's Untyped, its halves, the frames made from it and their mappings.
It destroys every pool that the borrower made from it,
wherever the borrower passed it on, and whatever the borrower deleted in between.
After that, the lender's Untyped is free, all of it.

### 5.6 Threads

A thread is in one of three states: **stopped**, **ready** or **waiting**.

- A new thread is stopped.
- `OP_THREAD_CONFIGURE` sets a stopped thread's program counter, stack pointer and argument.
  The thread finds its argument in `a0` (`r0` on ARM),
  where a function finds its first argument.
  The other registers start at zero.
  The kernel does not check any of these values.
- `OP_THREAD_RESUME` makes a stopped thread ready.
  The thread gets the processor when it is bound to units or to spare time,
  and its turn comes (section 5.11).
- A thread that waits on a notification is waiting.
  A signal makes it ready again.
- A thread that faults stops at the instruction that faulted; see **Faults** below.
- `OP_THREAD_STOP` stops a thread, whatever it is doing,
  and says which of the three states it was in;
  see **Stopping a thread** below.

A thread holds its process through a capability
derived from the `Process` capability that the thread was allocated with.
This works like a process holding its tables (section 5.2),
so the thread can lie in any pool.
Two things take the process away from the thread:
revoking below that capability,
or destroying the pool that the process lies in.
The thread then stops for good:
`OP_THREAD_RESUME` refuses it with `KERR_STATE`.

Apart from its state, a thread is either bound to units of time or not bound.
A new thread is not bound.
A thread that is not bound keeps its state, but does not run.
`OP_TIME_BIND` binds a thread.
Revoking below the capability it was bound through unbinds it.
This is how a scheduler takes a thread's time away and gives it back.
`OP_THREAD_STOP` stops a thread, whatever it is doing.

**Ending a thread.**
A thread cannot exit.
A thread that has nothing left to do first tells someone so, in its own way.
Then it does one of these:

- It stops itself, through a capability to its own thread with `RIGHT_W`.
- It stops at a breakpoint.
  A breakpoint is a fault, and the thread's watch hears it (see **Faults** below).
- It waits forever on a notification that nobody signals.

Whoever hears about it can stop it, or take away its units or its process.
A capability to a thread with `RIGHT_W` can also set the thread's watch.
So a creator may want to keep a child from changing its own watch.
It then gives the child no such capability,
and the child ends with a breakpoint.

**Stopping a thread.**
`OP_THREAD_STOP` moves a thread to stopped, whatever it is doing.
It returns the previous state in `a1`:
`THREAD_WAS_STOPPED`, `THREAD_WAS_READY` or `THREAD_WAS_WAITING` (section 6.8).

- A stopped thread stays as it is.
  If it has a fault to report, it keeps it.
  So a watcher can stop a thread at any time,
  and a fault that happened in the meantime is not lost.
- A ready thread stops where it is:
  between two instructions, or at a call it has not made yet.
  If another core is running the thread, the thread stops once that core has trapped,
  and the call waits for that.
  A call that the thread was making there is not made: its pc stays at the `ecall` or `svc`.
- A waiting thread leaves its notification and lends its time to nobody.
  It stays at the call it waited in, with every register as the call found it.
  `OP_THREAD_READ_REG` shows the call instruction at its pc,
  the operation in `a7` and the slot in `a0`.
  A resume makes the call again,
  and the call checks everything again, as a call made again after an interrupt does.
  Bits signalled in the meantime are taken, unless another waiter took them.
  The thread joins the back of the waiters.
  If the slot was cleared in the meantime, the call returns `KERR_INVALID_CAP`.
  A watcher can instead answer the wait, as when it emulates an instruction:
  it writes `a0`, `a1` and the pc past the call with `OP_THREAD_WRITE_REG`,
  and resumes the thread.
- The caller can stop itself, through a capability to its own thread.
  When it is resumed, it goes on after the call.
  If nothing else is left to run, the machine halts with `no runnable thread`.

A stopped thread keeps its process, its watch, its units and its virtual time,
as it does after a fault.
Threads that lend it their time keep waiting,
but lend it nothing while it is stopped.
A stop is not a fault.
It signals nothing, not even to the watch,
and `OP_THREAD_FAULT` has nothing to report.

On ARM only, the core may fail to save a thread's registers on its stack
when it traps for the stop.
The thread has then lost those registers,
and the stop leaves it stopped where it faulted.
The log reports the fault, and `OP_THREAD_FAULT` reports it,
but nothing is signalled.

**Faults.**
A thread faults by an access fault, an illegal instruction, a misaligned access or a breakpoint.
It then stops where it faulted.
Its registers stay as they were, and its program counter points at the instruction.
The processor goes on to the next thread, and the machine keeps running.
Nothing else changes:
the process, the thread's units and the process's other threads stay as they were.
The kernel writes `user fault` and the cause into its log (section 4).

`OP_THREAD_WATCH` gives a thread a **watch**:
a notification, and the bits that the thread's faults signal on it.
So whoever waits there hears about a fault like any other signal.
One wait can return a fault's bit together with a device's bit or a timer's bit.
A watcher of many threads gives each thread its own bit.
The thread holds the notification through a capability derived from the one named,
as it holds its process.
So revoking below that capability, or destroying the notification's pool, clears the watch.

A thread without a watch also stops at a fault, but nobody hears about it.
The watcher decides what happens next, with the normal operations:

- `OP_THREAD_RESUME` runs the faulting instruction again,
  for example after the watcher installed the region that the thread tried to reach.
- `OP_THREAD_CONFIGURE`, before the resume, gives the thread a new program counter, stack pointer and argument,
  so it starts again from them.
- Taking away its process ends the thread.
- Taking away its units frees them.
  A stopped thread still earns its units.

A watcher can also stop a thread.
A thread that faulted then stays as it is, with its fault still to report.
The bits say which thread faulted.
`OP_THREAD_FAULT` says why, while the thread stays stopped.
A watcher that emulates the instruction writes the thread's registers with `OP_THREAD_WRITE_REG`,
including the program counter past the instruction,
and then resumes the thread (section 6.8).

### 5.7 Notifications

A notification is one 32-bit word of sticky bits.

- `OP_NOTIFY_SIGNAL` ORs bits into the word.
  It never waits.
  Signalling zero bits is refused.
- `OP_NOTIFY_WAIT` returns every set bit and clears them.
  If no bit is set, it waits until at least one bit is set.
  It never returns zero bits.
- If a thread is waiting when a signal arrives,
  that thread wakes with every bit set so far, and the word is cleared.
- A signal that arrives before any thread waits is not lost.
- When several threads wait on one notification,
  one of them wakes and takes all the bits.
  Which one is not promised.

`RIGHT_W` allows signalling, and `RIGHT_R` allows waiting.
So a client can be allowed to announce something
without being allowed to consume the announcement.

**The bits a capability may signal.**
Besides its rights, a `Notification` capability carries the bits it may signal.
The capability that `OP_POOL_ALLOC` returns may signal every bit.
`OP_NOTIFY_CARVE` makes a capability for some of the bits.
The new capability is below the one it was carved from, as when a frame is carved from a frame.
The bits form an aligned block, like a frame:
1, 2, 4, 8, 16 or 32 bits, starting at a multiple of their count.
So one bit is a valid block, and so are a pair of bits or a byte of bits,
but bits 0 and 2 together are not.
A signal sets only those bits of `a1` that its capability may signal.
If `a1` names none of them, the signal is refused with `KERR_NO_RIGHTS`.
Copying keeps the bits, as it keeps or narrows rights.
An `Irq` signals only the bits that the capability it was bound with may signal.
A thread's watch signals only the bits of the capability it was set with.

So a server with many clients gives each client a capability carved to the client's own bit.
The bits that a wait returns then say who signalled, whatever bits a client names.
A client does not need to know its bit:
if it names every bit, `0xffffffff`, it signals all the bits it may.

**No data passes through the kernel.**
Two processes exchange bytes through a region installed in both,
and use notifications to say when.
The build targets `rv32imac`, so user mode has the `A` extension, with atomic instructions.
ARMv7-M has `ldrex` and `strex` for the same purpose.
So user programs build their own locks, events, ring buffers and snapshots in shared memory,
as `lib/lock.h`, `lib/event.h`, `lib/ring.h` and `lib/seqlock.h` do (section 8.4).
What user programs cannot build is "stop me until someone tells me to go on".
A notification is exactly that.
A waiter that knows which thread it waits for, such as a lock's holder or a server,
can lend that thread its time (section 5.12).

So a round trip takes four system calls:
signal and wait on one side, wait and signal on the other.

A trap breaks the thread's reservation.
So an `sc.w` fails whenever a system call, the timer or an interrupt came between it and its `lr.w`.
A loop around the pair tries again.
It never succeeds if there is a system call inside the loop.

**A signal orders memory.**
A thread stores data and then calls `OP_NOTIFY_SIGNAL`.
Another thread, on any core, sees that data once `OP_NOTIFY_WAIT` returns it that signal's bits.
The signal is a release, and the wait is an acquire.
So a buffer that is written and then signalled needs no fence,
as long as its reader reads it after such a wait.
It is different when the reader learns from memory itself that the data is ready.
Memory that is read with no call in between, such as a ring's counters,
still needs a fence on each side.

**A server with many clients** waits on one notification.
Each client holds it with `RIGHT_W` only, carved to a bit of its own,
and shares a region with the server.
One wake can carry the work of several clients,
and the returned bits say whose work it is,
because a client cannot set another client's bit.
A client also cannot forge another client's data, or consume another client's wake.
The server does not need a region for each client:
the clients' regions can all be carved from one frame that the server installs once,
as the hub of `lib/chan.h` does (section 8.4).

### 5.8 Time

There is no sleep call and no yield.
Time reaches user programs as one more interrupt, on a timer line.

The machine has `TIMER_LINES` timer lines, 16.
The root task receives all of them at boot in `BOOT_CAP_TIMER_LINES`,
separately from the interrupt controller's lines.
It carves them and hands them out like any other line (section 5.9).
So the root task decides who holds how many timers.
A timer line is bound with `OP_IRQ_BIND`, like a device's line.
This makes an `Irq` in a pool, bound to a notification in any pool.
`OP_IRQ_SET` arms it with a set of bits and, in `a2`, a delay in microseconds.
When the delay has passed, the line fires:
the `Irq` signals those bits and is disarmed.
Setting a timer line that is already armed moves its deadline and replaces its bits.
Setting it with zero bits cancels it.
A program that needs more timers than it holds keeps its own list of deadlines,
and arms one line for the nearest deadline.

**The delay is a lower bound.**
The kernel rounds the delay up to whole counts of the counter.
The line fires at the first count that lies that many counts past the call;
the kernel sets its timer for that count.
A delay of zero fires at once, when the timer that the call sets comes.
A delay of 10 000 µs fires 10 ms and one count after the call,
plus the kernel's step that takes the interrupt.
No upper bound is promised.
The woken thread is ready, not running, and it waits for its turn.
If it ran ahead of its units and has no spare time,
it also waits for its virtual time to come (section 5.11).

Sleeping:

```c
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, TIMER);

rv_timer_set(TIMER, BIT_TIMER, us);   /* OP_IRQ_SET with the delay in a2 */
rv_wait(NTFN, &bits);
```

A wait with a timeout uses the same two calls with one more bit.
Give the device one bit of the notification, and give the timer line another bit.
Then look at which bits came back.

**Periodic timers.**
A timer line fires once, so a periodic task arms it again each time it wakes.
With `IRQ_SET_PERIOD` in `a3`, the delay is a period.
The period counts from the line's last deadline, or from its bind, and not from the call.
So the task keeps its period, however long each wake takes.
The line fires at the first count after the call
that lies a whole number of periods after that deadline.
The call returns in `a1` how many such counts had already passed.
These are the periods that a task which woke late has skipped.
The task can do their work at once, count them as overruns, or ignore them.
Setting the line again before it fired leaves the deadline where it was.
The period is rounded up to whole counts, and it must not be zero.
Periods are counted on the clock's counter, so they keep pace with the clock.

```c
uint32_t skipped;
for (;;) {
    rv_timer_period(TIMER, BIT_TIMER, 5000, &skipped);   /* every 5 ms */
    rv_wait(NTFN, &bits);
    /* one period's work, or skipped + 1 of them */
}
```

**The clock.**
`BOOT_CAP_CLOCK` names the machine's time: 64 bits, the counter's counts since boot.
With `RIGHT_R`, `OP_CLOCK_READ` returns the time and the counter's rate in hertz:

```c
uint64_t start, end;
uint32_t hz;
rv_clock_read(CLOCK, &start, &hz);
work();
rv_clock_read(CLOCK, &end, &hz);   /* (end - start) / hz seconds */
```

Some threads cannot afford a system call for each read,
for example a thread that measures what the kernel does to it.
Such a thread can read the counter with a load instead, where the board allows user mode to load it.
The board then lists the counter among its devices, read only, as `BOOT_CAP_COUNTER` (section 3),
and the program installs that frame like any other.
The counter's low 32 bits lie `COUNTER_OFFSET` bytes into the frame, and run at the clock's rate.
They wrap around, after 71 minutes at one megahertz,
so the program uses the difference between two reads:

```c
uint32_t base, size;
rv_frame_info(BOOT_CAP_COUNTER, &base, &size);
rv_invoke(OP_PROCESS_INSTALL, PROCESS, 5, BOOT_CAP_COUNTER, RIGHT_R);
const volatile uint32_t *low = (const volatile uint32_t *)(base + COUNTER_OFFSET);

uint32_t at = *low;
spin();
uint32_t counts = *low - at;
```

The rate depends on the board:
10 MHz on QEMU, the CPU clock on the ESP32-C6, and 1 MHz on RP2350.
So a program takes the rate from `OP_CLOCK_READ`.
`OP_CAP_INFO` on the clock also tells the rate.
It needs no right, because the rate is not the time.
It also tells the default span, in counts of the counter:
the longest power of two of counts that fits in a millisecond.
The default span says how soon a request is due when no wait or bind gave a span (section 5.11).
A program's own timing analysis works in it.
The time is the machine's time, not the thread's: it includes the time that other threads ran.
`rdtime` traps on every board.
The ESP32-C6's performance counter is not a clock.
It belongs to the process, and it stops whenever another process runs (section 3).

**The watchdog.**
With `RIGHT_W`, `OP_CLOCK_WATCHDOG` arms the machine's watchdog, or feeds it.
The machine halts with code 7 unless the call comes again within the given number of microseconds.
The limit is ten seconds.
Nothing disarms the watchdog.
The machine has one watchdog, and every clock capability with `RIGHT_W` feeds it.
The feeder decides what the watchdog watches.
For example, a root task can feed it only after each child has reported that it is well.
Then the machine halts when a child hangs.
A root task that feeds it from its own loop halts the machine only when the root task itself hangs.

```c
rv_clock_watchdog(CLOCK, 2000000);            /* arm: two seconds */
rv_timer_period(TIMER, BIT_FEED, 500000, &skipped);
for (;;) {
    rv_wait(NTFN, &bits);
    if ((bits & BIT_FEED) && children_well()) {
        rv_clock_watchdog(CLOCK, 2000000);
    }
    /* ... */
}
```

### 5.9 Interrupts

A driver holds an `IrqLine` capability that names one line.
At boot, the root task receives every line of the interrupt controller, including the log's line,
and it receives every timer line in a second capability.
It carves single lines out with `OP_IRQ_CARVE`, as it makes frames out of memory.

`OP_IRQ_BIND` turns a one-line capability into an `Irq` object.
The `Irq` lies in a pool that the driver chooses,
and it is bound to a notification in any pool.
The invoked slot is consumed.
A line can be bound only once.
A second bind fails with `KERR_OVERLAP` until the first `Irq`'s pool is destroyed.

Some boards have a separate interrupt controller for each core, and the kernel runs on several cores.
There, the cores interrupt each other on one line.
That line belongs to the kernel for good, and binding it also fails with `KERR_OVERLAP`.
It is line 6 on mps2-an521, and line 26 on RP2350's Cortex-M33, each with both cores.

An `Irq` holds its notification through a capability
derived from the `Notification` capability it was bound with,
as a thread holds its process (section 5.6).
Two things take the notification away from the `Irq`:
revoking below that capability,
or destroying the pool that the notification lies in.
The `Irq` is then disarmed, and its line is masked for good:
`OP_IRQ_SET` refuses it with `KERR_STATE`.
The line stays bound until the `Irq`'s own pool is destroyed.

An `Irq` on a device's line works like one on a timer line:

- `OP_IRQ_SET` with bits **arms** it, which unmasks the line at the controller.
- When the line fires, the kernel masks the line,
  disarms the `Irq` and signals the bits.
- The driver services the device and arms the `Irq` again.
  This is the acknowledgement: only the driver lifts the mask.
- `OP_IRQ_SET` with zero bits masks the line by hand.

An `Irq` is armed exactly while its line is unmasked.
So a level-triggered interrupt that stays high costs one trap, not a storm of traps.

The first core takes every device interrupt, whichever core its driver runs on.
Some boards have a separate controller for each core: mps2-an521, and RP2350 with either kind of core.
There, an arm or a disarm made on another core reaches the first core's controller
when the first core next enters the kernel.
The call interrupts the first core for this.
So the line may come that much later, and a disarmed line never signals.

A device interrupt wakes its driver, but does not run it.
The driver waits for its turn, like a thread that a timer line woke.

Sharing a line between drivers is not supported;
see `DESIGN.md`, open decision 11.

### 5.10 The kernel log

The kernel writes every byte it prints into a ring,
including the bytes that `OP_PRINT` writes.
The ring follows a header of `RVUOS_LOG_HEADER` bytes.
The kernel never waits for a reader.
The ring's size is the kernel's `KLOG_SIZE`: 4 KiB minus the header today.
The header reports the size, so a reader does not need to assume it.
`BOOT_CAP_LOG` names the header and the ring.
The root task installs it like a device's registers, and reads it without system calls.

```c
struct rvuos_log {
    uint32_t head;   /* written by the kernel: count of every byte ever written */
    uint32_t size;   /* the ring's size in bytes */
    uint32_t taken;  /* written by the reader: count of bytes it has read */
};
```

Byte `n` lies at offset `RVUOS_LOG_HEADER + n % size`.
When `head - taken` is larger than `size`, bytes have been overwritten.
The oldest byte still kept is then `head - size`.
A reader that finds itself that far behind sets `taken` to that value.

The log is a device with interrupt line `LOG_IRQ_LINE`, which is 0.
The line is level-triggered: it is high while `head` is past `taken`.
A reader binds and arms the line as it would a UART's line,
and writes `taken` to acknowledge.
Arming while some bytes are not taken yet signals at once.

The log's line cannot wake the machine from an idle stall.
Only the kernel writes the log,
and when no thread can run, nothing runs that could make the kernel write.
So an idle kernel whose only armed `Irq` is the log's halts with `no runnable thread`.
`OP_TEST_IRQ` cannot fire the line either.

`user/init/logger.c` shows a reader:
a logger thread waits on one notification, with one bit for the log and one for the UART,
and copies the ring out one byte per transmitter interrupt.
`user/wifi/logger.c` is a reader that the root task gave the frame to.
It sends the ring over the network.
It moves `taken` only as far as its host confirms it has the bytes,
so a halt writes out whatever no host has received yet.

### 5.11 Scheduling

In short:
each core's time is divided into units, and a thread is bound to some of them.
The kernel runs the thread whose request is due first (earliest deadline first).
Threads with time go before threads on spare time,
which is the time of units that nobody earns.
The rest of this section gives the details.

**Units of time.**
Each core is divided into `TIME_UNITS` units of time, 64.
Each unit is a sixty-fourth of the core.
The units of all cores are numbered one core after another:
the first core's units are 0 to 63, the second core's are 64 to 127, and so on.
The root task receives all of them in `BOOT_CAP_TIME`.
At boot, the root task's own thread earns every unit of the first core, in its first place.
The root task hands units out with `OP_TIME_CARVE` and `OP_CAP_DERIVE`, as it hands out memory.
`OP_CAP_INFO` on `BOOT_CAP_TIME` tells its count, `TIME_UNITS` for each core.
So a program learns from it how many cores there are.

**Places.**
A thread has **places**.
Its creator chooses how many with `OP_POOL_ALLOC`: from one to the number of cores.
Each place is empty, or holds the time of one core.
Two places of one thread never hold the time of the same core.
A thread runs only on the cores whose time its places hold.

A bind's units must all belong to one core.
A bind to no units gives the thread spare time on the core of the first unit named.
A bind fills the place that already holds that core's time, or else an empty place.
If there is neither, the bind fails with `KERR_LIMIT`.
A thread with one place moves to another core only after its place is emptied,
by a revoke above its units.
A place can be bound to no units through a capability without `RIGHT_X`.
It still holds its core's time, and the thread runs there on lent time only.

A ready thread waits in the queues of every core whose time it holds.
The first core to take it runs it.
While it runs on one core, the other cores do not see it.
It joins their queues again when its turn ends.
If a thread's place on a core is emptied while the thread runs there,
the thread leaves that core when the trap ends,
unless it may go on running there on spare time.
`OP_CAP_INFO` on a thread tells its places, and the cores whose time they hold.

**Earning units.**
`OP_TIME_BIND` binds a thread's place to some of a capability's units, or to none of them.
The thread then **earns** those units there.
Only one thread at a time earns a unit.
A bind that names a unit that another thread earns fails with `KERR_OVERLAP`.
So many threads can hold copies of one `Time` capability, and each unit still goes to one thread.
The time of the units that nobody earns is **spare time**.

**Spare time.**
A thread bound through a capability with `RIGHT_X` also runs on spare time.
Spare time is turns that no thread with time wants, and they cost the thread nothing.
The boot grant has `RIGHT_X`.
A thread bound through a capability without `RIGHT_X` runs on its units only.
It gets at most its units' share of each span of its core, plus one request.
If nothing else wants the core for the rest of the time, the core sleeps in `wfi`.
A thread bound to no units runs on spare time only,
or does not run at all without `RIGHT_X`.

**Virtual time.**
Each place of a thread has a **virtual time**.
It is the count of the clock up to which the thread's units have paid for its running.
Each count that the thread runs on `u` units moves it forward by `TIME_UNITS / u` counts,
measured on the machine's counter.
This is the same whether the timer ends the turn or the thread waits.
A turn on spare time costs the thread nothing of its units.
It still moves the virtual time forward, but only up to a span ahead of the clock.
A thread that runs exactly its units' share keeps its virtual time at the clock.
A thread that runs more moves its virtual time ahead of the clock.

A bind keeps a place's virtual time,
and the units that a bind leaves keep it too.
So a place that is bound to other units takes the later of its own virtual time and theirs.
Units that nobody earned since boot keep none.

A thread's units on one core promise it their share of that core
only while the thread runs on none of its other cores,
because a turn on another core puts it at the back of their queues.
To every other thread, they promise the same as before.

**Requests.**
A thread runs in **requests**.
Each request is due one span after the virtual time it starts at.
The span is the one given last:

- A wait gives a span with `OP_NOTIFY_WAIT` or `OP_NOTIFY_LEND` (section 6.9).
  A wait that gives no span keeps the span the thread had.
- A bind gives a span with `OP_TIME_BIND` (section 6.13).
  A bind that gives no span sets the default span.

The default span is the longest power of two of counts that fits in a millisecond.
`OP_CAP_INFO` on a `Clock` tells it (section 6.16).

A request is over once the thread has run its units' share of the span,
that is, once its virtual time has reached the deadline.
A request starts when the thread is made ready or bound,
and the next request starts when one is over.
It starts at the later of the thread's virtual time and the clock,
and only once the virtual time is at most a span ahead of the clock.
A thread has **time** on a core while it has a request open there,
or while its virtual time is within a span of the clock.
So a thread that wants the core all the time runs about a span ahead,
and is due later than a thread that wakes with a span that is not longer.
A thread that waits gives up the rest of its request.
Waiting does not save anything for later.

**Queues.**
Each core has three queues of ready threads.

- A thread with time waits in the run queue.
- A thread without time that may run on spare time waits in the spare queue.
- A thread without time that has units, but may not run on spare time, waits in the spent queue,
  until its virtual time comes within a span of the clock.

A thread that becomes ready joins the back of its queue,
whether it was resumed, woken, bound or preempted.
The kernel runs the thread on the run queue whose request is due first.
If the run queue is empty, it runs the thread that has waited longest on the spare queue.
That thread runs for its span while another thread waits for spare time.

A thread that is made ready, and whose request is due before the running thread's request,
takes the processor at once.
When the running thread's request ends, the timer takes the processor from it in two cases:
another thread is due first,
or the running thread's units do not give it a next request yet.

A turn on spare time is one that no thread with time wants,
so it ends as soon as such a thread wants the core.
A thread with time that becomes ready while its core runs a turn on spare time
takes the processor at once,
as the call or the interrupt that made it ready returns.
If the running thread stands at a call that stopped for an interrupt,
it first makes that call again.

**Lending.**
A thread that waits with `OP_NOTIFY_LEND` lends its units to the thread it names (section 5.12).
A borrower without time on a core waits in that core's run queue
while its lender's units give it time there.
The turns it takes this way move the lender's virtual time on that core.

**No tick.**
There is no periodic tick.
The kernel sets each core's timer for the first count that could change what runs:
a timer line's deadline,
a virtual time that comes,
or the end of the running request, while that matters.
So a thread that is alone on the processor is not interrupted at all.

**What a thread is promised.**
Take a thread with `u` units whose virtual time is at or behind the clock,
and which is woken with a span `s`.
Within `s` of its wake, it has run `u * s / TIME_UNITS` counts of the counter,
or it has waited again before that.
This holds whatever the other threads do,
while tracing is off and the thread keeps its binding.
The kernel's longest step and the switches can add to the time `s`.
A thread that ran ahead of its units is late by what it ran ahead, at most one request.

For example, a driver must do work of `c` counts within `D` counts of its interrupt.
It waits with a span `s`: the longest power of two of counts that fits in `D`.
It needs at least `TIME_UNITS * c / s` units.

A shorter span makes the thread's requests come sooner, and makes them shorter.
It costs the thread a switch for each request while another thread waits.
A longer span puts its requests behind shorter ones.
A span costs no other thread anything, so a wait needs no right to give one.
A thread that always has work gets its units' share of the processor, give or take a request.
`DESIGN.md`, "Scheduling", says what a request promises,
and what a periodic task needs of its units.
A thread that a process adds earns only units split off the process's own units,
or runs on spare time.
It takes nothing that another thread earns.

**Too few units.**
A thread that has fewer units than its work needs is not refused.
It is served one request at a time.
While another thread with time wants the core,
each request after the first one waits about a span.
Spare time hides this while the core is otherwise idle.

The switch to a thread is charged to that thread.
One unit at the default span gives a request of about the size of that switch:

- 2048 cycles at the ESP32-C6's 160 MHz,
- 1200 cycles at the RP2350's 150 MHz, near half of `make bench`'s `threads` round,
- 512 cycles at the nRF52840's 64 MHz, about a sixth of the switch there.

A request shorter than the switch runs nothing of the thread's own work.
So a thread on the path of an event earns units for its work and for its switch.
Measure that work while no other thread takes the core from it,
because a cycle counter that a process's threads share counts the work of all of them.

**Overruns.**
`OP_THREAD_OVERRUNS`, with `RIGHT_R` on a thread,
tells how many of the thread's requests it ran to the end without waiting.
A thread whose units fit its work runs out none of its requests, whatever the other threads do.
So a count that grows says that its units do not fit its work.

**System calls.**
A system call is never interrupted:
machine mode runs with interrupts off from the trap to the return.
On several cores, one call runs at a time, on whichever core makes it,
and a call on another core waits for it.

**When nothing can run.**
Sometimes nothing is runnable,
but an `Irq` is armed on a timer line or a device's line,
or a thread waits for its virtual time to come.
Then the kernel stalls in `wfi` until the nearest timer line's deadline,
the virtual time that comes, or the interrupt,
and takes no interrupt in between.
On several cores, a core with nothing to run waits in the same way while another core runs.
The other core wakes it when it gives it a thread.
When no core runs a thread and none of these things is pending,
the kernel prints `no runnable thread` and halts with code 5,
with or without a watchdog.

**No priorities.**
There are no priorities and no yield.
A spinning thread cannot starve the others,
because the end of its request preempts it while another thread is due first.
But it uses up its turns, moves its virtual time ahead,
and keeps the machine out of `wfi`, unless it is held to its units.
A thread that waits costs nothing until it is signalled.

The counter measures each turn, and waiting does not undo any of it.
So a thread that waits between bursts of work still pays for the time it ran.

**Example: a tenth of the processor.**
To hold a process to about a tenth of the processor,
the root task gives it six units without `RIGHT_X`.
The root task's own thread earns every unit at boot.
So the root task first binds its thread to the first 58 units only, to free the last six.
Then it carves those six and lends them without spare time:

```c
rv_time_bind(BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, 58, 0);
rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, 58, 6, TIME);
rv_invoke(OP_CAP_DERIVE, CHILD_TABLE, CHILD_TIME, TIME, RIGHT_W);
```

The child binds its threads through `CHILD_TIME` with `OP_TIME_BIND`.
`TIME` itself keeps `RIGHT_X`, so a bind through it would also give spare time.
Revoking below `TIME` takes the child's units back, and the child's threads stop.

### 5.12 Lent time

While a thread waits, it can lend its time to another thread.
`OP_NOTIFY_LEND` waits on a notification like `OP_NOTIFY_WAIT`, and also names the borrower.
It names the borrower through a capability to the borrower's thread with `RIGHT_X`.
That right allows lending time to the thread, and nothing else.

A borrower that has no time of its own on a core (section 5.11)
runs there on the units of the thread that has lent it time the longest,
if that thread also holds the time of that core.
The turns it takes this way move that lender's virtual time.
If the lender's own time pays for its current turn,
it hands the rest of that turn to the borrower at once, if the borrower waits for a turn.
A signal from the borrower that wakes the lender hands the rest of the turn back.

A borrower can itself lend, when it has no time for its own borrower.
It then passes on what it borrows.
So a client's time pays for its server, and for whatever that server waits on.
This works through up to 64 lenders,
and only on a core whose time all of them hold.

So a thread that waits for a lock's holder lends the holder its time,
and a client that waits for its server's answer does the same.
A holder or a server with few units then keeps a waiter with many units waiting
no longer than its work takes.
`lib/lock.h` has a lock that stores the name of its holder in its word,
so that the threads that want to take it know whom to lend to.
Taking and giving back the lock when nobody else wants it makes no system call (section 8.4).

```c
/* each thread names itself, here 1, and holds the others' threads with RIGHT_X, here the thread named 2 */
struct named_lock lock = { WORD, NOTE, 1, { [2] = THREAD_2 } };
named_lock_take(&lock);
/* ... the work the lock guards ... */
named_lock_give(&lock);
```

A wrong name costs the waiter the time it lends, and nothing else.
If the borrower is destroyed, the waiter wakes with `KERR_INVALID_CAP` and no bits,
just as when its notification is destroyed.
So a taker learns that the holder its lock names is gone, and can take the lock over.

A holder that faults is stopped, and keeps the lock.
Its watcher hears about the fault (section 5.6).
A holder stopped with `OP_THREAD_STOP` also keeps the lock, but nobody hears about it,
and the takers that lend to it keep waiting.
A stopped borrower keeps its lenders,
but their time buys it nothing until it is resumed.
A stopped lender lends nothing more:
a stop takes it off its wait, and off its borrower's ring of lenders.
If a stopped lender's time pays for the borrower's current turn,
it pays until that turn ends: at the end of its request, or at the borrower's wait.

A lock does not keep anybody from the memory it guards.
Regions do that.

### 5.13 Routes

Some chips connect an event of one device to a task of another device in hardware.
The event then starts the task with no thread in between, within a cycle or two.
For example, a timer's compare event can start a radio's sending,
or a radio's end-of-frame event can make a timer count.
Some deadlines are shorter than a thread's wait for its turn.
They can be met only this way,
because a driver in user mode runs only when its turn comes (section 5.11).

The kernel lends the chip's channels, as it lends interrupt lines.
The root task receives every channel that the kernel lends in `BOOT_CAP_ROUTES`, a `RouteLine`.
It carves single channels out with `OP_ROUTE_CARVE`.
On a board without a router, the slot is empty.
On QEMU's boards, four channels exist for tests, and they connect nothing.

`OP_ROUTE_BIND` turns a one-channel capability into a `Route` object, in a pool the driver chooses.
The new `Route` is connected to nothing and disarmed.
The call consumes the invoked slot.
A channel can be bound only once.

`OP_ROUTE_END` connects the `Route`'s event or its task.
Each end is given by a `Frame` capability over the device it lies in:
with `RIGHT_R` for the event, and `RIGHT_W` for the task.
The call also takes the end's name within the frame, which the board defines (section 3).
`OP_ROUTE_SET` arms the `Route` once both ends are connected, and it also disarms the `Route`.
A route triggers only what the driver could trigger by writing to the frames itself.
It moves no data.

```c
/* TIMER4 counts every frame the radio ends: its COUNT task on the radio's END event */
rv_invoke(OP_ROUTE_CARVE, BOOT_CAP_ROUTES, 0, 1, CHANNEL);
rv_invoke(OP_ROUTE_BIND, CHANNEL, POOL, ROUTE, 0);
rv_invoke(OP_ROUTE_END, ROUTE, ROUTE_EVENT, BOOT_CAP_RADIO, 0x10c); /* EVENTS_END */
rv_invoke(OP_ROUTE_END, ROUTE, ROUTE_TASK, BOOT_CAP_TIMER4, 0x008); /* TASKS_COUNT */
rv_invoke(OP_ROUTE_SET, ROUTE, 1, 0, 0);
```

A `Route` holds each end through a capability
derived from the `Frame` capability it was connected through.
So whoever lent the device takes the connection back by revoking below its frame,
and this disarms the `Route`.
Connecting an end that the `Route` already had also disarms it.
So a `Route` never connects a new end to the other end's old partner.
Destroying the `Route`'s pool disarms it too, and frees the channel.

### 5.14 DMA

A DMA controller's channel moves bytes by itself.
This saves the CPU from feeding a device one word at a time.
But the channel reaches whatever address it is given, and PMP does not stop it.
So on boards where the DMA controller is separate from the devices it feeds,
the kernel keeps the controller's registers to itself.
It lends the channels, as it lends a router's channels,
and points a channel only at memory that the driver holds.

The root task receives every channel that the kernel lends in `BOOT_CAP_DMA`, a `DmaLine`.
It carves single channels out with `OP_DMA_CARVE`.
On a board that lends no channel, the slot is empty.
On QEMU's boards, four channels exist for tests, and they move nothing.

`OP_DMA_BIND` turns a one-channel capability into a `Dma` object, in a pool the driver chooses.
The new `Dma` is connected to nothing and moves nothing.
The call consumes the invoked slot.
A channel can be bound only once.

`OP_DMA_END` connects the `Dma`'s source or its sink, each through a `Frame` capability:
with `RIGHT_R` for the source, and `RIGHT_W` for the sink.
The board says which end of each channel is memory; that end is named 0.
The other end is a device.
It is named by its DMA request number, which the controller takes for it,
through a frame over the device's registers (section 3).

`OP_DMA_START` moves bytes at an offset into the frame of the memory end, for a given length.
The length is a whole number of the board's grains,
and at most the board's longest transfer (section 3).
The device sets the pace.
The kernel checks the range,
so a driver cannot point a channel past its own frame, whatever it writes in the meantime.
A channel moves only what the driver could move with loads and stores to the two frames;
it just does so without the CPU.
`OP_CAP_INFO` tells whether a transfer may still reach the memory.
A start while one may still do so fails with `KERR_STATE`.
`OP_DMA_STOP` stops a transfer before its end.

```c
/* SPI2 sends n bytes from offset of FRAME, which the program maps too, while the program goes on */
rv_invoke(OP_DMA_CARVE, BOOT_CAP_DMA, 0, 1, CHANNEL);
rv_invoke(OP_DMA_BIND, CHANNEL, POOL, DMA, 0);
rv_invoke(OP_DMA_END, DMA, DMA_SOURCE, FRAME, 0);
rv_invoke(OP_DMA_END, DMA, DMA_SINK, BOOT_CAP_SPI2, DMA_REQUEST_SPI2);
/* ... the bytes written into FRAME, SPI2 set to take its data from the DMA for n bytes ... */
rv_invoke(OP_DMA_START, DMA, offset, n, 0);
```

A `Dma` holds each end through a capability
derived from the `Frame` capability it was connected through.
So whoever lent the memory or the device takes it back by revoking below its frame.
This stops the channel before the revoke returns, whether a transfer is running or not.
Connecting an end that the `Dma` already had also stops the channel.
So does destroying the `Dma`'s pool, which also frees the channel.

The bytes that a transfer reads may change while it runs.
This is the driver's responsibility:
it writes the next bytes into another buffer, or it waits for the transfer to end.
