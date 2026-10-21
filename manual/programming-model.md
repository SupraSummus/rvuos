# rvuos user manual: the programming model

Chapter 5 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 5. The programming model

### 5.1 Processes, threads and regions

A **process** is a protection domain:
`PROCESS_TABLES` table slots, two today, which hold the capability tables it names capabilities in,
the region slots its creator asked for, one at least and no more than the core has PMP entries for them,
and the threads that run in it.
A **thread** is an execution context inside a process:
a register frame and a state.
Threads of one process share its regions and its capability tables.

A **frame** is a physical address range with maximum rights,
and a **region** is a frame installed into one of a process's slots,
which gives that process access to the range with the rights chosen at install time.
Regions are all a process can see of memory.
An **Untyped** is memory that has not become anything yet:
it cannot be installed, and it is made into a frame, a pool or its two halves.
The kernel does not know what a code segment, a stack or a heap is;
whoever builds a process decides its layout.

### 5.2 Capabilities and the tables

A process names everything by **slot number**, a slot of one of its capability tables:
`SLOT_IN(t, i)`, `t << SLOT_TABLE_SHIFT | i`, is index `i` of the table in table slot `t`,
and `SLOT_TABLE_SHIFT` is 10, so the numbers below 1024 are the first table's,
and a process with one table names its slots by their indices.
A slot is empty or holds one capability:
a type, a set of rights, and two words the kernel interprets by type.
Userspace never sees a slot's links, nor the address of an object,
but whoever holds a capability may ask what it is, with no right, `OP_CAP_INFO`, section 6.15:
its type, its rights and what it names, what a frame covers, say.

Every system call is an invocation on the capability in one slot.
The kernel resolves the slot,
checks the capability's type and rights against the operation,
and only then looks at the arguments;
section 6.1 gives the exact order.
An empty slot, an index out of range, or a number whose table slot holds no table fails with `KERR_INVALID_CAP`.

A process holds each of its tables by a capability of its own,
derived from the `CapTable` capability it was allocated with, for the first table,
or mounted with, `OP_PROCESS_MOUNT`, section 6.7, for any table slot.
A table may lie in any pool,
and several processes may be allocated with one table, or mount it, and then share it.
Revoking below that capability, or destroying the pool the table lies in,
takes the table from the process, as `OP_PROCESS_UNMOUNT` does,
and from then on every number pointing into that table slot fails with `KERR_INVALID_CAP`;
a process whose table slots are all empty fails every call.
The table itself stays as long as its pool does,
and so does everything in it.

A process moves to another table by itself, every capability keeping its number:
it mounts the new table in table slot 1,
moves each capability `i` to index `i` of it with `OP_CAP_MOVE`, where `SLOT_IN(1, i)` names it meanwhile,
the new table's own capability last,
then unmounts table slot 0 and mounts the new table there, both through `SLOT_IN(1, process)`,
and unmounts table slot 1.
So a process outgrows the table it was made with, or leaves memory it is to give back,
which a revoke refuses to destroy while one of its tables lies there, section 5.5.
No other thread of the process may make a call meanwhile.
`self_migrate` in `user/lib/self.h` does it.

Rights are three bits:

| Bit | Value | Frame, Untyped | Other types |
|---|---|---|---|
| `RIGHT_R` | 1 | read | `Notification`: wait; `Thread`: read its overruns |
| `RIGHT_W` | 2 | write | control the object: allocate, install, configure, signal, set, bind, copy or move into a table; `Print`: write into the log; `Halt`: halt the machine; `Test`: drive it as a test does |
| `RIGHT_X` | 4 | execute | `Time`: the threads bound through it run on spare time |

Every operation that produces a capability
puts it into a slot of one of the caller's own tables,
named by an argument, and that slot must be empty.
A process needs no capability to receive into its own tables.
The `CapTable` capability exists for writing into a table:
a parent fills a child's table with `OP_CAP_COPY`, `OP_CAP_DERIVE` or `OP_CAP_MOVE` before starting it,
and a process that holds a capability to its own table
can copy and move within it, clear its own slots, and revoke below them.

Every capability remembers what it was derived from,
and that is what `OP_CAP_REVOKE` follows.
The tree grows in these ways:

| Operation | The new capability hangs |
|---|---|
| `OP_CAP_DERIVE` | below the source |
| `OP_CAP_COPY` | beside the source, under the source's parent; a copy of a root is a root; an Untyped is never copied |
| `OP_CAP_MOVE` | where the source hung, which it leaves empty, with what hung below the source below it |
| `OP_FRAME_CARVE`, `OP_IRQ_CARVE`, `OP_TIME_CARVE`, `OP_ROUTE_CARVE`, `OP_CLOCK_FRAME` | below the invoked capability |
| `OP_UNTYPED_RETYPE` of a frame, `OP_UNTYPED_SPLIT` | below the invoked Untyped |
| `OP_UNTYPED_RETYPE` of a pool | below the pool's own node, which hangs below the invoked Untyped |
| `OP_PROCESS_INSTALL` | the installed region hangs below the frame |
| `OP_POOL_ALLOC` | below the invoked `KernelPool` capability |
| `OP_POOL_ALLOC` of a `Process` | and the process's hold on its first table below the `CapTable` capability |
| `OP_PROCESS_MOUNT` | the process's hold on the table below the `CapTable` capability |
| `OP_POOL_ALLOC` of a `Thread` | and the thread's hold on its process below the `Process` capability |
| `OP_IRQ_BIND` | below the `KernelPool` capability; the line goes with what was derived from it |
| `OP_IRQ_BIND` | and the `Irq`'s hold on its notification below the `Notification` capability |
| `OP_TIME_BIND` | the thread's hold on its units hangs below the invoked `Time` capability |
| `OP_ROUTE_BIND` | below the `KernelPool` capability; the channel goes with what was derived from it |
| `OP_ROUTE_END` | the `Route`'s hold on the end hangs below the `Frame` capability |
| boot | nowhere, but for the root task's frames and the boot pool's block, below `BOOT_CAP_ROOT_RAM`, the boot pool's own node, below that block, and the capabilities to the boot pool and its objects, below that node |

`OP_CAP_REVOKE` on a slot clears everything below it, in every table and every process:
derived capabilities, their copies, what was derived from those,
every region installed from any of them,
and every thread's units bound through any of them;
a thread made through any of them loses its process and stops, section 5.6,
an `Irq` bound through any of them loses its notification and is disarmed, section 5.9,
and a `Route` connected through any of them loses that end and is disarmed, section 5.13.
Below an Untyped it destroys every pool made of it, section 5.5.
The slot itself stays.
Revoking a copy takes nothing from the original, and the other way round;
to take both back, revoke below what they were both derived from.
So derive to lend, and copy to keep a second handle to what you hold.
A copy cannot take back what was derived from its source, since that hangs below the source and not below the copy;
to hand over a capability together with that power, move it.
An Untyped can only be derived, and only while nothing was made of it:
the derived one is the whole of it, and the source makes nothing until that one is gone.

`OP_CAP_DELETE` clears one slot and hands what hung below it to the slot's parent,
so deleting your own copy of something you lent does not take it back.
Destroying a pool clears every slot in the tables that pool held the same way,
so what a process handed out through its table outlives the process,
below whatever the process derived it from.

Copying and deriving both apply a rights mask, so rights only ever narrow.
A move keeps the rights as they were, since what hangs below may hold all of them.

Slot 0 of the root task's table is left empty on purpose,
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
| `Thread` | `CAP_THREAD` | a register frame, a state and its places | `OP_POOL_ALLOC` |
| `Notification` | `CAP_NOTIFICATION` | one word of sticky bits | `OP_POOL_ALLOC` |
| `IrqLine` | `CAP_IRQ_LINE` | none: the slot holds the first line and a count | boot, `OP_IRQ_CARVE` |
| `Irq` | `CAP_IRQ` | one line bound to a notification, with a deadline on a timer line | `OP_IRQ_BIND` |
| `Time` | `CAP_TIME` | none: the slot holds the first unit and a count | boot, `OP_TIME_CARVE` |
| `RouteLine` | `CAP_ROUTE_LINE` | none: the slot holds the first channel and a count | boot, `OP_ROUTE_CARVE` |
| `Route` | `CAP_ROUTE` | one channel, its two ends and whether it is armed | `OP_ROUTE_BIND` |
| `Print` | `CAP_PRINT` | none | boot |
| `Halt` | `CAP_HALT` | none | boot |
| `Test` | `CAP_TEST` | none | boot |
| `Clock` | `CAP_CLOCK` | none: there is one counter | boot |

`Untyped`, `Frame`, `IrqLine`, `Time`, `RouteLine`, `Print`, `Halt`, `Test` and `Clock` capabilities have no kernel object behind them.
Carving a frame, a range of lines, units or channels, retyping an Untyped into a frame and splitting one,
are pure table operations
that touch no kernel memory,
which is why the root task can hand out memory, lines and time
before it has created a single pool.

### 5.4 Memory: Untyped, frames and installing

Every frame and every Untyped is a **block**:
its size is a power of two and its base a multiple of its size,
which is the range one PMP entry in NAPOT mode describes.
The smallest block is eight bytes, or the PMP's grain if that is coarser;
`OP_CAP_INFO` on any frame returns it in `a4`.
A range of any other size is rounded up or made of several blocks,
one region slot each.

An Untyped is **free** while nothing made of it is left, and **made** while something is;
`OP_CAP_INFO` says which in `a4`: what lies first below it, `CAP_FRAME`, `CAP_UNTYPED` or `CAP_POOL`, or 0 while it is free.
That is the first of what lies there:
once a delete handed up what lay below the deleted capability, several kinds may lie side by side.
One whose delete stopped part way makes nothing until it is gone, even once `a4` says 0, section 6.4:
what it made became roots, which lie in its memory.
A free Untyped makes one thing of the whole of its memory:
`OP_UNTYPED_SPLIT` its lower and its upper half, two Untypeds,
`OP_UNTYPED_RETYPE` a frame or a pool,
and `OP_CAP_DERIVE` an Untyped of all of it, for lending, section 6.4.
A made one makes nothing (`KERR_NO_MEMORY`),
so what one Untyped makes never overlaps,
and a frame never overlaps a pool.
Once what it made is gone, whether deleted, destroyed or revoked, it is free again, the whole of it.

Where a block lies is the program's to choose, by the half it takes;
the kernel keeps no allocator.
To get a small block out of a large Untyped, halve it down,
and keep the halves not used yet: they are the program's free memory.
A half the program does not want to keep a slot for may be deleted;
what was made of it then hangs below its parent,
which is free again once all of that is gone.
A frame and an Untyped carry the rights of the Untyped they were made of.
`OP_FRAME_CARVE` hands out a block within a frame.
Programs that lay memory out with fixed offsets
should check the smallest size once and fail early if the layout does not fit.

Rules for installing a frame into a process:

- The rights installed must be a non-empty subset of the frame's rights.
- Write without read is refused,
  because PMP reserves that encoding and hardware may do anything with it.
- On ARM execute without read is refused too, since the MPU fetches only what a thread may read.
- Regions installed in one process may not overlap each other.
- The install takes effect at once, even for the running process.

PMP budget.
Every installed region is one NAPOT entry, so `n` regions cost `n` entries.
A process is made with its region slots, `a4` of `OP_POOL_ALLOC`,
from one to the entries the core has for regions, section 10;
more, or none, is `KERR_INVALID_ARG`, so every slot may be filled at once.
`OP_CAP_INFO` on any process tells its region slots in `a2` and that most in `a3`.
The root task's process has `ROOT_REGION_SLOTS`, eight, or the most where that is fewer.

An access must lie entirely within one PMP entry.
Two regions that touch share a boundary
that no single load, store or instruction fetch may cross,
so a process's creator lays it out so that nothing straddles one.

Sharing costs no copy: installing the same range into two processes
gives both access, and no byte passes through the kernel.
What it does cost is a region slot and a PMP entry in each process.

### 5.5 Pools

A process hands memory to the kernel with `OP_UNTYPED_RETYPE` of a `CAP_POOL`,
which makes the whole of the Untyped a pool.
The Untyped must carry both read and write rights,
and be at least `POOL_MIN_SIZE`, 64 bytes.
The kernel places the pool's descriptor at its base
and returns a `KernelPool` capability with all rights.
The memory belongs to the kernel from then on;
no frame covers it, since what one Untyped makes never overlaps.
The kernel zeroes each object as it allocates it,
so what the process left in the memory never becomes part of an object.

Objects are bump-allocated from a pool, sixteen-byte aligned,
and never freed individually.
Sizes depend on the architecture, the board and the build, and the kernel may change them,
so a program asks the pool: `OP_POOL_INFO` tells what an object of a type takes, as the pool counts it,
`CAP_POOL` the descriptor, section 6.6,
and `OP_CAP_INFO` on the pool what is left of it, its room.
A pool for a set of objects needs the descriptor and them,
rounded up to a power of two of at least `POOL_MIN_SIZE`, since an Untyped is a block.
Today's sizes, as the kernel rounds them:

| Object | Bytes |
|---|---|
| pool descriptor | 48 |
| `CapTable` with `n` slots | 16 + 16 × n |
| `Process` with `r` region slots | 48 + 20 × r + r rounded up to 4, rounded up to 16; 16 more on the ESP32-C6 |
| `Thread` with `k` places | 224 + 40 × k on RISC-V, 160 + 40 × k on ARM, rounded up to 16 |
| `Notification` | 16 |
| `Irq` | 48 |
| `Route` | 64 |

A minimal child process, table of 10 slots, process of two region slots, thread of one place and two notifications,
costs 624 bytes including the descriptor on RISC-V, 16 more on the ESP32-C6, and 560 on ARM;
each region slot more costs 20 bytes and a few for its entry's configuration.

**Objects in different pools.**
A process's tables, a thread's process and an `Irq`'s notification may each lie in any pool,
sections 5.2, 5.6 and 5.9:
each is held by a capability that a destroy of its pool clears.

**Capabilities to a pool.**
Every capability to an object hangs below the `KernelPool` capability it was allocated through,
and every `KernelPool` capability below the pool's own node,
which the kernel keeps in the pool and no process can name.
So revoking below a `KernelPool` capability takes what was allocated through it,
and deleting the last `KernelPool` capability leaves the pool standing,
reachable through nothing, until the Untyped it was made of is revoked;
`OP_CAP_INFO` on that Untyped tells a pool below it all the while.

**Destroying a pool** is revoking below the Untyped it was made of, or below one above that;
no operation on the pool does it, so a `KernelPool` capability only allocates.
A pool is the whole of its Untyped, so keep the Untyped of a pool you mean to give back on its own.
The destroy:

- destroys every object in it,
- clears every capability, in every table of every process,
  that names the pool or one of its objects,
- clears every slot of the pool's own tables as `OP_CAP_DELETE` does,
  so what was derived from those slots goes to their parents,
- wakes every thread waiting on a notification in the pool
  with `KERR_INVALID_CAP` and no bits,
- stops every thread, in whatever pool, that ran in a process in the pool,
- disarms every `Irq`, in whatever pool, bound to a notification in the pool,
  and masks its line, which stays bound,
- masks the interrupt line of every `Irq` in the pool, which frees the line,
- zeroes the memory its objects took and leaves the rest as it was,
  so clear memory before it becomes a pool if whoever destroys the pool should not read it,
- gives the memory back to the Untyped the pool was made of.

The revoke fails with `KERR_STATE`
when the calling thread, its process or a table the process mounts lies in the Untyped's memory,
so a thread cannot destroy what it runs on
nor a table it names capabilities in, which it unmounts or moves out of first,
and when the table it names the Untyped in does,
since the capability the call is made through would go half way through the destroy of that table's pool.
The boot pool holds the root task, which therefore cannot destroy it,
but any other thread holding `BOOT_CAP_POOL_RAM`, the block the boot pool was made of, can,
and the root task goes with it, section 7.
A destroy is restartable: it may stop for an interrupt and go on when the call is made again,
and a pool being destroyed allocates nothing, `KERR_STATE`;
a capability to it that still stands describes it as being destroyed, with no room.

A lender that wants memory back from a living borrower
lends it as an Untyped derived from its own
and revokes below its own, section 5.2.
That takes the borrower's Untyped, the halves, the frames it made and their mappings,
and destroys every pool the borrower made of it,
wherever the borrower passed it on and whatever it deleted in between,
and leaves the lender's Untyped free, the whole of it.

### 5.6 Threads

A thread is **stopped**, **ready** or **waiting**.

- A new thread is stopped.
- `OP_THREAD_CONFIGURE` sets a stopped thread's program counter, its stack pointer,
  and its argument, which it finds in `a0`, `r0` on ARM, as a function finds its first.
  The other registers start at zero.
  The kernel validates none of them.
- `OP_THREAD_RESUME` makes a stopped thread ready.
  It gets the processor once it is bound to units, or to spare time, and its turn comes, section 5.11.
- A thread waiting on a notification is waiting;
  a signal makes it ready again.
- A thread that faults is stopped where it faulted; see **Faults** below.
- `OP_THREAD_STOP` stops a thread whatever it is doing, and says which of the three it was;
  see **Stopping a thread** below.

A thread holds its process by a capability derived from the `Process` capability it was allocated with,
as a process holds its tables, section 5.2, so the thread may lie in any pool.
Revoking below that capability, or destroying the pool the process lies in,
takes the process from the thread, and the thread stops for good:
`OP_THREAD_RESUME` refuses it with `KERR_STATE`.

Apart from its state, a thread is bound to units of time or it is not.
A new thread is not, and a thread that is not keeps its state and does not run:
`OP_TIME_BIND` binds it, and revoking below the capability it was bound through unbinds it.
That is how a scheduler takes a thread's time and gives it back; `OP_THREAD_STOP` stops it, whatever it is doing.
No thread exits: a thread that has nothing left to do says so by its own means first,
then stops itself, through a capability to its own thread with `RIGHT_W`,
or stops with a breakpoint, which is a fault its watch hears, below,
or waits forever on a notification nobody signals;
whoever hears it may stop it, or take its units or its process.
A capability to a thread with `RIGHT_W` also sets its watch,
so a creator that keeps a child's thread from its watch gives it none, and the child ends with a breakpoint.

**Stopping a thread.**
`OP_THREAD_STOP` takes a thread to stopped from whatever it is doing, and returns what that was in `a1`,
`THREAD_WAS_STOPPED`, `THREAD_WAS_READY` or `THREAD_WAS_WAITING`; section 6.8.
- A stopped thread stays as it is, with the fault it has to tell, so a stop may come whatever happened meanwhile.
- A ready thread stops where it stands, between two instructions or at a call it has not made yet.
  One another core runs stops once that core has trapped, which the call waits for,
  and a call it was making there is not made: its pc stays at the `ecall` or `svc`.
- A waiting thread leaves its notification, and lends its time to nobody,
  and stands at the call it waited in, with every register as the call found it:
  `OP_THREAD_READ_REG` reads the call instruction at its pc, the operation in `a7` and the slot in `a0`.
  A resume makes the call again, which checks everything afresh, as a call made again after an interrupt does:
  bits signalled meanwhile are taken unless another waiter took them,
  the thread joins the back of the waiters, and a slot cleared meanwhile answers `KERR_INVALID_CAP`.
  A watcher may instead answer the wait as it emulates an instruction:
  it writes `a0`, `a1` and the pc past the call with `OP_THREAD_WRITE_REG`, and resumes the thread.
- The caller may stop itself, through a capability to its own thread, and goes on past the call once resumed;
  with nothing else left to run, the machine halts with `no runnable thread`.

The thread keeps its process, its watch, its units and its virtual time, as at a fault,
and the threads that lend it their time go on waiting, lending it nothing while it is stopped.
A stop is no fault: it signals nothing, the watch included, and leaves `OP_THREAD_FAULT` nothing to tell.
On ARM alone, a thread whose core could not stack its registers as it trapped for the stop has lost them,
and the stop leaves it stopped where it faulted: the log reports the fault and `OP_THREAD_FAULT` tells it,
but nothing is signalled.

**Faults.**
A thread that faults, by an access fault, an illegal instruction, a misaligned access or a breakpoint,
is stopped where it faulted, with its registers as they were and its program counter at the instruction,
and the processor goes to the next thread; the machine goes on.
Nothing else changes: the process, the thread's units and the process's other threads are as they were.
The kernel writes `user fault` and the cause into its log, section 4.
`OP_THREAD_WATCH` gives a thread a **watch**, a notification and bits that its faults signal,
so whoever waits there hears of the fault as of any other signal;
one wait can carry a fault's bit next to a device's or a timer's,
and a watcher of many threads gives each a bit.
The thread holds the notification by a capability derived from the one named, as it holds its process,
so revoking below that capability, or destroying the notification's pool, clears the watch.
A thread without a watch stops at a fault all the same, and nobody hears.
What happens next is the watcher's to decide, with the operations there are:
`OP_THREAD_RESUME` runs the faulting instruction again, after installing the region it reached for, say;
`OP_THREAD_CONFIGURE` first starts it afresh;
taking its process ends it, and taking its units, which a stopped thread still earns, frees them.
A watcher may stop a thread too, which leaves one that faulted as it is, its fault still to tell.
The bits say which thread faulted, and `OP_THREAD_FAULT` says why while it stays stopped there.
A watcher that emulates the instruction writes the thread's registers with `OP_THREAD_WRITE_REG`,
the program counter past the instruction among them, and resumes it; section 6.8.

### 5.7 Notifications

A notification is one 32-bit word of sticky bits.

- `OP_NOTIFY_SIGNAL` ORs bits into the word and never blocks.
  Signalling zero bits is refused.
- `OP_NOTIFY_WAIT` returns every set bit and clears them,
  blocking until at least one is set.
  It never returns zero bits.
- If a thread is waiting when a signal arrives,
  that thread wakes with every bit set so far, and the word is cleared.
- A signal that arrives before a waiter got there is not lost.
- When several threads wait on one notification,
  one of them wakes and takes every bit; which one is not promised.

`RIGHT_W` allows signalling and `RIGHT_R` allows waiting,
so a client can be given the power to announce something
without the power to consume the announcement.

Besides its rights, a `Notification` capability carries the bits it may signal:
every bit for the one `OP_POOL_ALLOC` returns, and fewer once `OP_NOTIFY_CARVE` has made a capability of some of them,
below the one carved, as a frame is carved from a frame.
They are an aligned block, as a frame is: 1, 2, 4, 8, 16 or 32 bits starting at a multiple of their count,
so a client's own bit is one, and a pair or a byte of bits is one too, but bits 0 and 2 together are none.
A signal sets only the bits of `a1` its capability may signal,
and naming none of them is refused with `KERR_NO_RIGHTS`;
copying keeps the bits, as it keeps or narrows rights.
An `Irq` signals only the bits the capability it was bound with may, and a thread's watch only those of the one it was set with.

So a server with many clients gives each a capability carved to the client's own bit,
and the bits a wait returns say who signalled, whatever a client names.
A client need not be told its bit: naming every bit, `0xffffffff`, signals all it may.

No data passes through the kernel.
Two processes exchange bytes through a region installed in both
and use notifications to say when.
The build targets `rv32imac`, so the `A` extension is there in user mode,
as `ldrex` and `strex` are on ARMv7-M,
and a lock, an event, a ring buffer or a snapshot in shared memory is userspace's to build,
as `lib/lock.h`, `lib/event.h`, `lib/ring.h` and `lib/seqlock.h` do, section 8.4;
what userspace cannot build is "stop me until someone says otherwise",
and a notification is exactly that.
A waiter that knows whom it waits for, a lock's holder or a server, may lend that thread its time, section 5.12.

A round trip is therefore four system calls:
signal, wait on one side; wait, signal on the other.

A trap breaks the thread's reservation,
so an `sc.w` fails whenever a system call, the timer or an interrupt came between it and its `lr.w`;
a loop around the pair tries again, and never succeeds with a system call inside it.

**A signal orders memory.**
What a thread stored before `OP_NOTIFY_SIGNAL`,
a thread sees once `OP_NOTIFY_WAIT` returns it that signal's bits, on any core:
the signal is a release, and the wait an acquire.
So a buffer written and then signalled needs no fence,
as long as its reader reads it after such a wait and not because memory said it was ready;
memory read with no call between, as a ring's counters are, still needs one on each side.

**A server with many clients** waits on one notification;
each client holds it with `RIGHT_W` only, carved to a bit of its own,
and shares a region with the server.
One wake can carry several clients' work,
and the bits it returns say whose, since a client cannot set another's bit;
it cannot forge another's data or consume another's wake either.
The server need not spend a region on each client:
the clients' regions may all be carved from one frame the server installs once,
as the hub of `lib/chan.h` does, section 8.4.

### 5.8 Time

There is no sleep call and no yield.
Time reaches userspace as one more interrupt, on a timer line.

The machine has `TIMER_LINES` timer lines, 16,
and the root task receives all of them at boot in `BOOT_CAP_TIMER_LINES`,
apart from the controller's lines.
It carves them and hands them out as it does any other line, section 5.9,
so who holds how many timers is its choice.
A timer line is bound with `OP_IRQ_BIND` like a device's,
into an `Irq` in a pool, bound to a notification in any pool.
`OP_IRQ_SET` arms it with a set of bits and, in `a2`, a delay in microseconds;
when the delay has passed the line fires: the `Irq` signals those bits and disarms.
Setting an armed timer line moves its deadline and replaces its bits;
setting bits to zero cancels it.
A program that needs more timers than it holds keeps its own deadlines
and arms one line for the nearest.

The contract is a **lower bound**.
The delay is rounded up to whole counts of the counter,
and the line fires at the first count past the call by that many: the kernel sets its timer for it.
A delay of zero fires at once, as the timer the call sets comes,
and a delay of 10 000 µs fires 10 ms after the call and a count, and the kernel's step that takes the interrupt.
No upper bound is promised, because the woken thread is ready, not running,
and waits for its turn,
and for its virtual time to come if it ran ahead of its units and has no spare time, section 5.11.

Sleeping:

```c
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, TIMER);

rv_timer_set(TIMER, BIT_TIMER, us);   /* OP_IRQ_SET with the delay in a2 */
rv_wait(NTFN, &bits);
```

A wait with a timeout is the same two calls with one more bit:
give the device one bit on the notification and the timer line another,
and look at which bits came back.

A timer line fires once, so a periodic task arms it again each time it wakes.
With `IRQ_SET_PERIOD` in `a3` the delay is a period counted from the line's last deadline,
or from its bind, not from the call, so the task keeps its period whatever each wake costs:
the line fires at the first count after the call
that lies a whole number of periods from that deadline.
The call returns in `a1` how many such counts had already passed,
the periods a task that woke late has skipped;
it can do their work at once, count them as overruns, or ignore them.
Setting it again before the line fired leaves the deadline where it was.
The period is rounded up to whole counts and may not be zero,
and periods are counted on the clock's counter, so they keep pace with it.

```c
uint32_t skipped;
for (;;) {
    rv_timer_period(TIMER, BIT_TIMER, 5000, &skipped);   /* every 5 ms */
    rv_wait(NTFN, &bits);
    /* one period's work, or skipped + 1 of them */
}
```

**The clock.**
`BOOT_CAP_CLOCK` names the machine's time, 64 bits of the counter's counts since boot.
With `RIGHT_R`, `OP_CLOCK_READ` returns the time and the counter's rate in hertz:

```c
uint64_t start, end;
uint32_t hz, counter;
rv_clock_read(CLOCK, &start, &hz, &counter);
work();
rv_clock_read(CLOCK, &end, &hz, &counter);   /* (end - start) / hz seconds */
```

A thread that cannot afford a call for each read, one that measures what the kernel does to it,
reads the counter's low 32 bits with a load instead,
through a read-only frame `OP_CLOCK_FRAME` derives, installed like any frame at the address the read returned.
They wrap, after 71 minutes at a megahertz, so it takes differences of them:

```c
rv_invoke(OP_CLOCK_FRAME, CLOCK, COUNTER_FRAME, 0, 0);
rv_invoke(OP_PROCESS_INSTALL, PROCESS, 5, COUNTER_FRAME, RIGHT_R);

uint32_t at = rv_counter_low(counter);
spin();
uint32_t counts = rv_counter_low(counter) - at;
```

The rate is the board's, 10 MHz on QEMU, the CPU clock on the ESP32-C6 and 1 MHz on RP2350,
so a program takes it from `OP_CLOCK_READ`.
`OP_CAP_INFO` on the clock tells the rate too, with no right, since it is not the time,
and the default span, in counts of the counter, the longest power of two within a millisecond:
how soon a request is due that no wait or bind gave a span for, which a program's own timing analysis works in, section 5.11.
The time is the machine's, not the thread's: it includes other threads' slices.
Revoking below a `Clock` capability uninstalls every region derived through it.
`rdtime` traps on every board.
The ESP32-C6's performance counter is no clock: it is the process's, and stops whenever another process runs, section 3.

**The watchdog.**
With `RIGHT_W`, `OP_CLOCK_WATCHDOG` arms the machine's watchdog, or feeds it:
the machine halts with code 7 unless the call comes again within the microseconds it names, ten seconds at most.
Nothing disarms it, and there is one for the machine, which every clock with `RIGHT_W` feeds.
What it watches is the feeder's to decide: a root task that feeds it only once each child has said it is well
halts a machine whose children hang, where one that feeds it from its own loop halts only a machine where the root task hangs.

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

A driver holds an `IrqLine` capability naming one line.
The root task receives every line of the controller at boot, with the log's,
and every timer line in a second capability,
and carves single lines out with `OP_IRQ_CARVE`,
as it makes frames of memory.

`OP_IRQ_BIND` turns a one-line capability into an `Irq` object
in a pool of the driver's choosing,
bound to a notification in any pool.
The invoked slot is consumed.
A line is bound at most once; a second bind fails with `KERR_OVERLAP`
until the first `Irq`'s pool is destroyed.
Where each core has a controller of its own and the kernel runs on several,
the line they interrupt each other on is bound to the kernel for good, and binding it fails so too:
line 6 on mps2-an521 and line 26 on RP2350's Cortex-M33, each with both cores.

An `Irq` holds its notification by a capability
derived from the `Notification` capability it was bound with,
as a thread holds its process, section 5.6.
Revoking below that capability, or destroying the pool the notification lies in,
takes the notification from the `Irq`, which is disarmed and its line masked for good:
`OP_IRQ_SET` refuses it with `KERR_STATE`, and the line stays bound until the `Irq`'s own pool goes.

An `Irq` on a device's line works like one on a timer line:

- `OP_IRQ_SET` with bits **arms** it, which unmasks the line at the controller.
- When the line fires, the kernel masks the line,
  disarms the `Irq` and signals the bits.
- The driver services the device and arms the `Irq` again,
  which is the acknowledgement: only the driver lifts the mask.
- `OP_IRQ_SET` with zero bits masks the line by hand.

An `Irq` is armed exactly while its line is unmasked,
so a level that stays high costs one trap and not a storm.
The first core takes every device interrupt, whichever core its driver runs on;
where each core has a controller of its own, as on mps2-an521 and on RP2350 of either kind,
an arm or a disarm made on another core reaches the first core's as that core next enters the kernel,
which the call interrupts it for, so the line may come that much later, and a disarmed one never signals.
A device interrupt wakes its driver but does not run it;
the driver waits for its turn like a thread a timer line woke.

Sharing a line between drivers is not supported;
`DESIGN.md`, open decision 11.

### 5.10 The kernel log

The kernel writes every byte it prints, `OP_PRINT`'s included,
into a ring behind a header of `RVUOS_LOG_HEADER` bytes,
and never waits for a reader.
The ring's size is the kernel's `KLOG_SIZE`, 4 KiB less the header today;
the header reports it, so a reader need not assume it.
`BOOT_CAP_LOG` names the header and the ring;
the root task installs it like a device's registers and reads without a system call.

```c
struct rvuos_log {
    uint32_t head;   /* written by the kernel: count of every byte ever written */
    uint32_t size;   /* the ring's size in bytes */
    uint32_t taken;  /* written by the reader: count of bytes it has read */
};
```

Byte `n` lies at offset `RVUOS_LOG_HEADER + n % size`.
Once `head - taken` exceeds `size`, bytes have been overwritten
and the oldest byte still kept is `head - size`;
a reader that finds itself that far behind resets `taken` accordingly.

The log is a device with interrupt line `LOG_IRQ_LINE`, which is 0.
The line is level: high while `head` lies past `taken`.
A reader binds and arms it as it would a UART's,
and writes `taken` to acknowledge.
Arming while bytes are untaken signals at once.
The log's line cannot wake the machine from an idle stall:
only the kernel writes the log, and with no thread runnable
nothing runs that could make it write,
so an idle kernel with only the log's `Irq` armed halts with `no runnable thread`.
The line cannot be fired by `OP_TEST_IRQ` either.

`user/init/logger.c` shows the reader:
a logger thread that waits on one notification with the log's bit and the UART's,
and carries the ring out one byte per transmitter interrupt.
`user/wifi/logger.c` is a reader the root task gave the frame to, which carries the ring over the network
and moves `taken` only as far as its host says it has the bytes, so a halt writes out what no host had.

### 5.11 Scheduling

- Each core is divided into `TIME_UNITS` units of time, 64, each a sixty-fourth of it.
  The units of every core are numbered one core after the other:
  the first core's are 0 to 63, the second's 64 to 127, and so on.
  The root task receives them all in `BOOT_CAP_TIME`, its own thread earning every one of the first core's in its first place,
  and hands them out with `OP_TIME_CARVE` and `OP_CAP_DERIVE` as it hands out memory.
  `OP_CAP_INFO` on `BOOT_CAP_TIME` tells its count, `TIME_UNITS` for each core,
  so a program learns how many cores there are from it.
- A thread has **places**, as many as its creator asked for with `OP_POOL_ALLOC`, from one to the machine's cores,
  each empty or holding the time of one core, never two of them one core's.
  A thread runs on the cores its places hold the time of, and on no other.
  A bind's units must all be of one core; a bind to none gives the thread spare time on the core of the first unit named.
  A bind fills the place that holds that core's time, or an empty one, and fails with `KERR_LIMIT` with none;
  a thread with one place moves to another core only once its place is emptied, by a revoke above its units;
  a place bound to none without `RIGHT_X` holds its core's time all the same, and runs there on lent time alone.
  A ready thread waits on the queues of every core it holds the time of, and the first core to take it runs it;
  while it runs on one, the others do not see it, and it joins their queues again as its turn ends.
  A thread whose place on a core is emptied while it runs there leaves that core as the trap ends, unless it may run on there on spare time.
  `OP_CAP_INFO` on a thread tells its places and the cores they hold the time of.
- `OP_TIME_BIND` binds a thread's place to some of a capability's units, or to none of them,
  and the thread **earns** them there.
  A unit is earned by one thread at a time:
  a bind that names a unit another thread earns fails with `KERR_OVERLAP`,
  so copies of a `Time` capability may be held by many, and each unit still goes to one thread.
  The time of the units nobody earns is spare.
- A thread bound through a capability with `RIGHT_X` also runs on **spare time**:
  turns no thread with time wants, which cost it nothing.
  The boot grant has `RIGHT_X`.
  A thread bound through a capability without it runs on its units alone,
  at most its units' part of each span of its core and a request,
  and the core sleeps in `wfi` for the rest if nothing else wants it.
  A thread bound to no units runs on spare time alone, or not at all without `RIGHT_X`.
- Each place of a thread has a **virtual time**, a count of the clock its units have paid for its running up to:
  each count the thread runs on `u` units moves it on `TIME_UNITS / u` counts, counted on the machine's counter,
  whether the timer ends the turn or the thread waits;
  a turn on spare time costs nothing of its units, though it moves the virtual time on too, up to a span ahead of the clock.
  A thread that runs its units' part keeps its virtual time at the clock, and one that runs more puts it ahead.
  A bind keeps a place's virtual time, and the units it leaves keep it too,
  so a place bound to other units takes the latest of its own and theirs; units nobody earned since boot keep none.
  A thread's units on one core promise it their part of that core only while it runs on none of its other cores,
  since a turn elsewhere puts it at the back of their queues; to every other thread they promise what they did.
- A thread runs in **requests**, each due a span past the virtual time it starts at:
  the span its last wait gave, `OP_NOTIFY_WAIT` or `OP_NOTIFY_LEND`, or its binding's, `OP_TIME_BIND`, sections 6.9 and 6.13,
  or the default span for none, the longest power of two of counts within a millisecond, which `OP_CAP_INFO` on a `Clock` tells, section 6.15.
  A request is over once the thread has run its units' part of the span, its virtual time having reached the deadline.
  A request starts as the thread is made ready or bound, and the next as one is over,
  at the latest of its virtual time and the clock, and only once its virtual time lies at most a span ahead of the clock.
  A thread has **time** on a core while it has a request open there or its virtual time lies within a span of the clock.
  So a thread that wants the core all along runs about a span ahead, and is due after a thread that wakes with a span no longer.
  A thread that waits gives up the rest of its request, and saves nothing for later by waiting.
- Ready threads wait for the processor in three queues, which each core has.
  A thread with time waits on the run queue;
  one without time waits on the spare queue if it may run on spare time,
  and for its virtual time to come within a span of the clock if not.
  A thread that becomes ready, whether resumed, woken, bound or preempted,
  joins the back of its queue.
  The thread whose request is due first on the run queue runs,
  or, if the run queue is empty, the one that has waited longest on the spare queue,
  which runs for its span while another waits for spare time.
  A thread made ready whose request is due before the running thread's takes the processor at once,
  and as the running thread's request ends, the timer takes the processor from it if another is due first,
  or if its units give it no next request yet.
  A turn on spare time is one no thread with time wants, so it ends as soon as one wants it:
  a thread with time that becomes ready while its core runs a turn on spare time takes the processor at once,
  as the call or the interrupt that made it ready returns,
  after the running thread has made again a call that stopped for an interrupt, if it stands at one.
- A thread waiting with `OP_NOTIFY_LEND` lends the thread it names its units, section 5.12:
  a borrower without time on a core waits on its run queue while its lender's units give it time there,
  and the turns it takes so move the lender's virtual time on that core.
- There is no tick: the kernel sets each core's timer for the first count that could change what runs,
  a timer line's deadline, a virtual time that comes, or the end of the running request while it matters,
  so a thread alone on the processor is not interrupted at all.
- So a thread with `u` units whose virtual time is at or behind the clock, woken within a span `s`,
  has run `u * s / TIME_UNITS` counts of the counter within `s` of its wake, or waits again before, whatever the other threads do,
  besides the kernel's longest step and the switches, while tracing is off and the thread keeps its binding.
  A thread that ran ahead of its units is later by what it ran ahead, a request at most.
  A driver that must do work of `c` within `D` of its interrupt waits within the longest power of two of counts that `D` holds,
  `s`, and is given `TIME_UNITS * c / s` units at least.
  A shorter span brings its requests nearer and makes them shorter, and costs the thread a switch for each while another thread waits;
  a longer one puts them behind shorter ones.
  A span costs no other thread anything, so a wait needs no right to give one.
  A thread with work all along gets its units' part of the processor, give or take a request;
  `DESIGN.md`, "Scheduling", says what a request promises and what a periodic task needs of its units.
  A thread a process adds earns only units split off the process's own, or runs on spare time:
  it takes nothing another thread earns.
- A thread given fewer units than its work needs is not refused: it is served a request at a time,
  and while another thread with time wants the core, each request after the first waits about a span,
  which spare time hides while the core is otherwise idle.
  The switch to a thread is charged to it, and one unit at the default span is a request about as long as that switch:
  2048 cycles at the ESP32-C6's 160 MHz, 1200 at the RP2350's 150 MHz, near half of `make bench`'s `threads` round,
  and 512 at the nRF52840's 64 MHz, about a sixth of the switch there.
  A request shorter than that switch runs nothing of its own.
  So a thread on an event's way earns the units for its work and its switch,
  that work measured while no other thread takes the core from it, since a cycle counter a process's threads share counts them all.
- `OP_THREAD_OVERRUNS`, with `RIGHT_R` on a thread, tells how many of its requests it ran to the end of without waiting.
  A thread whose units fit its work runs none out, whatever the other threads do, so a count that grows says they do not.
- A system call is never interrupted:
  machine mode runs with interrupts off from the trap to the return.
  On several cores one call runs at a time, whichever core makes it, and another core's call waits for it.
- When nothing is runnable and an `Irq` is armed on a timer line or a device's line,
  or a thread waits for its virtual time to come,
  the kernel stalls in `wfi` until the nearest timer line's deadline, the virtual time that comes or the interrupt arrives,
  and takes no interrupt in between.
  On several cores a core with nothing to run waits so while another runs, which wakes it when it gives it a thread.
  When no core runs a thread and there is none of these, it prints `no runnable thread` and halts with code 5,
  watchdog or not.

There are no priorities and no yield.
A spinning thread cannot starve the others, because the end of its request preempts it while another is due first,
but it burns its turns and puts its virtual time ahead, and keeps the machine out of `wfi`
unless it is held to its units;
a thread that waits costs nothing until it is signalled.

A turn is charged by the counter, and a wait forgives none of it,
so a thread that waits between bursts pays for the time it ran all the same.

To hold a process to about a tenth of the processor, the root task gives it six units without `RIGHT_X`.
Its own thread earns every unit at boot, so it leaves the six first,
then carves them and lends them without spare time:

```c
rv_time_bind(BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, 58, 0);
rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, 58, 6, TIME);
rv_invoke(OP_CAP_DERIVE, CHILD_TABLE, CHILD_TIME, TIME, RIGHT_W);
```

The child binds its threads through `CHILD_TIME` with `OP_TIME_BIND`;
`TIME` itself keeps `RIGHT_X`, so a bind through it would give spare time too.
Revoking below `TIME` takes the child's units back, and its threads stop.

### 5.12 Lent time

A thread that waits may lend another thread its time meanwhile:
`OP_NOTIFY_LEND` waits on a notification as `OP_NOTIFY_WAIT` does, and names the borrower
through a capability to its thread with `RIGHT_X`, which lends it time and allows nothing else.
A borrower without time of its own on a core, section 5.11, runs there on the units of the thread that has lent it time longest,
if that one holds the time of the core too, and the turns it takes so move that thread's virtual time;
a lender whose own time pays for its turn hands the rest of it to the borrower at once, if the borrower waits for a turn.
A signal from the borrower that wakes the lender hands it the rest of the turn back.
A borrower that lends in turn, having no time for its own borrower, passes on what it borrows,
so a client's time pays for its server and for whatever that server waits on, through up to 64 lenders,
and only on a core every one of them holds the time of.

So a thread waiting for whoever holds a lock lends it its time, and so does a client waiting for its server's answer:
a holder or a server with few units keeps a waiter with many no longer than its work takes.
`lib/lock.h` has a lock that names its holder in its word, so that its takers know whom to lend to,
and a hold nobody else wants still makes no system call, section 8.4.

```c
/* each thread names itself, here 1, and holds the others' threads with RIGHT_X, here the thread named 2 */
struct named_lock lock = { WORD, NOTE, 1, { [2] = THREAD_2 } };
named_lock_take(&lock);
/* ... the work the lock guards ... */
named_lock_give(&lock);
```

A name that is wrong costs the waiter the time it lends, and nothing else.
A waiter whose borrower is destroyed wakes with `KERR_INVALID_CAP` and no bits, as one whose notification is,
so a taker learns that the holder its lock names is gone, and may take the lock over.
A holder that faults keeps the lock, stopped, and its watcher hears it, section 5.6,
and so does a holder stopped with `OP_THREAD_STOP`, which nobody hears: takers that lend to it wait on.
A stopped borrower keeps its lenders, whose time buys it nothing until it is resumed,
and a stopped lender lends nothing more: a stop takes it off its wait, and its borrower's ring of lenders.
A stopped lender whose time pays for the borrower's turn pays to the end of that turn, at its request's end or the borrower's wait.
A lock keeps nobody from the memory it guards: that is the regions' work.

### 5.13 Routes

Some chips wire an event of one device to a task of another in hardware, so that the one starts the other
with no thread in between, within a cycle or two of the event:
a timer's compare that starts a radio's sending, a radio's end of frame that counts in a timer.
A deadline shorter than a thread's wait for its turn is met that way or not at all, since a driver in user mode
runs only when its turn comes, section 5.11.
The kernel lends the chip's channels as it lends interrupt lines.
The root task receives every channel it lends in `BOOT_CAP_ROUTES`, a `RouteLine`, and carves single channels out with `OP_ROUTE_CARVE`;
on a board without a router the slot is empty, and on QEMU's boards four channels wire nothing, for tests.

`OP_ROUTE_BIND` turns a one-channel capability into a `Route` object in a pool of the driver's choosing,
connected to nothing and disarmed, and consumes the invoked slot; a channel is bound at most once.
`OP_ROUTE_END` connects the `Route`'s event and its task, each by a `Frame` capability over the device it lies in,
with `RIGHT_R` for the event and `RIGHT_W` for the task, and the end's name within the frame, which the board gives, section 3.
`OP_ROUTE_SET` arms the `Route`, once both ends are connected, and disarms it.
A wire triggers only what the driver could trigger by writing the frames itself; it moves no data.

```c
/* TIMER4 counts every frame the radio ends: its COUNT task on the radio's END event */
rv_invoke(OP_ROUTE_CARVE, BOOT_CAP_ROUTES, 0, 1, CHANNEL);
rv_invoke(OP_ROUTE_BIND, CHANNEL, POOL, ROUTE, 0);
rv_invoke(OP_ROUTE_END, ROUTE, ROUTE_EVENT, BOOT_CAP_RADIO, 0x10c); /* EVENTS_END */
rv_invoke(OP_ROUTE_END, ROUTE, ROUTE_TASK, BOOT_CAP_TIMER4, 0x008); /* TASKS_COUNT */
rv_invoke(OP_ROUTE_SET, ROUTE, 1, 0, 0);
```

A `Route` holds each end by a capability derived from the `Frame` capability it was connected through,
so whoever lent the device takes the wire back by revoking below its frame, which disarms the `Route`;
connecting an end it had disarms it, so it never wires a new end to the other's old partner.
The destroy of the `Route`'s pool disarms it too, and frees the channel.
