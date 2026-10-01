# rvuos design: objects, capabilities and communication

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Kernel objects and capabilities

### Capabilities

A capability is a kernel-held reference to a kernel object
together with a set of rights.
Userspace never sees a capability's contents.
It names capabilities by index into its process's capability table,
and the kernel validates the index, the object type, and the rights
on every use.

Capabilities can be copied, derived or moved into another process's table
by a process that holds both the source capability
and a capability to the destination table,
the first two optionally with reduced rights.
Copy and derive differ in one thing:
what a later revoke of the source takes.
A copy stands beside its source and is as good as it;
a derivation stands below it and goes when the source is revoked below;
see "The derivation tree".
A move leaves no source behind:
the capability goes to the destination with its place in the tree,
so it takes back what the source could,
which a copy, standing beside the source, cannot.

A process needs no capability to write its own table.
Every operation that produces a capability
puts it into a slot of the caller's table,
named by an argument.
The `CapTable` capability exists for writing someone else's table,
which is how a parent populates a child before starting it.

#### Invocation ABI

Every system call is an invocation on one capability.
The full register contract lives in `include/rvuos/abi.h`
and is summarised here.

| Register | On `ecall` | On return |
|---|---|---|
| `a7` | operation code | unchanged |
| `a0` | slot of the invoked capability | status, zero on success |
| `a1` to `a6` | arguments | results |

On ARMv7-M `r0` to `r6` stand for `a0` to `a6`, `r12` for `a7`, and `svc` makes the call;
see "Architectures".

Operation codes are a single flat numbering across all types.
The kernel resolves the slot, checks that the type accepts the operation
and that the capability carries the rights it needs,
and only then looks at the arguments.
A wrong type and a missing right are distinct errors
so that a caller can tell a programming mistake from a policy decision.

#### Slot format

A slot is twenty-four bytes: type, rights, an index, padding, two words of content,
and three links of the derivation tree.
The index is an installed region's slot in its process,
so that uninstalling it finds the process without a walk.
For object capabilities the first word of content is the object's address
and the second is unused.
A slot carries nothing that has to be checked against the object,
because a destroy clears the slots naming what it takes;
see "Kernel pools and revocation".
A `Frame` has no object behind it:
its two words are the base address and the size,
and the rights are the memory permissions it may grant.
An `Untyped` has none either:
its first word is its block, base and size as NAPOT encodes them,
its second is unused, since whether it is free the derivation tree says,
and its rights are those of the frames made of it.
Carving a frame, retyping an Untyped into a frame and splitting one into halves
are therefore pure table operations that touch no kernel memory,
which is what lets the root task hand out memory
before it has created a single pool.
An `IrqLine` capability has the same shape for the same reason:
its words are the first line and the count, there is no object,
and the root task hands lines out as it hands out memory.
A `Time` capability is the same again, with units of the processor's time for lines.
A `Clock` capability, like `Debug`, has neither object nor content:
there is one counter on the machine.

### Object types

| Object | Purpose |
|---|---|
| `Untyped` | Memory that may become a frame, a pool or two halves. Never installed. No object. |
| `Frame` | A physical address range with maximum rights. Installed into a process's PMP slots. No object. |
| `KernelPool` | Memory retyped from an Untyped and handed to the kernel. All other objects are allocated from pools. |
| `CapTable` | A process's capability table. Allocated from a pool. |
| `Process` | A protection domain: a capability to its `CapTable`, a set of region slots, and its threads. |
| `Thread` | An execution context inside a process: its registers, its state, and what its faults signal. |
| `Notification` | A word of sticky signal bits. The only way a thread can stop and be started again. |
| `IrqLine` | A range of interrupt lines: the log's and the controller's, or the timer lines. Bound one at a time into an `Irq`. |
| `Irq` | One line bound to a notification. Signals it when the line fires. |
| `Time` | A range of the processor's units of time. A thread runs only while bound to some, or to none with spare time. No object. |
| `Debug` | A byte into the kernel's log, and machine halt, for bring-up and tests. No object. |
| `Clock` | The machine's counter: its rate, and the one frame it can be read through. No object. |

Every object's size follows from its type,
a `CapTable` from its slot count,
so a pool can be a bump allocator
whose contents can be walked without a free list.

### Kernel pools and revocation

Memory reaches the kernel through an `Untyped` capability,
memory that may become something else and that no process can map.
An Untyped is free or made, and the derivation tree says which:
while nothing lies below it, it makes one thing of the whole of its memory,
and while something does, it makes nothing.
`OP_UNTYPED_SPLIT` makes it two `Untyped`s, its lower and its upper half,
and `OP_UNTYPED_RETYPE` a `Frame` or a `KernelPool` of all of it,
each below it in the derivation tree.
What one Untyped makes therefore never overlaps,
and it is free again, the whole of it, once nothing it made is left.
From the retype on, a pool's memory belongs to the kernel.
The kernel zeroes each object as it allocates it, not the whole block.

Where a block lies is the program's to choose, by which half it takes,
and the kernel keeps nothing an allocator would: no cursor, no free list.
A program that wants small blocks out of a large one halves it down
and holds the halves it does not use yet, which are its free memory.
A half deleted goes as any capability does, what was made of it to its parent,
so a program may let the halves between go and keep only what it made,
and the parent is free again once all of that is gone.
The root task starts with its own memory laid out so; see "Boot".

An Untyped is derived, never copied,
since a copy would be a second allocator over the same memory.
A derived Untyped is the whole of its source,
so only a free one derives,
and the source makes nothing while the derived one lives, as for anything else it made.
That is how memory is lent:
the lender keeps its Untyped,
and a revoke below it takes back whatever the borrower made of the memory and leaves it free.

A `Frame` is memory a process may map and nothing else:
it never becomes a pool, so its copies are harmless,
and an install looks only at the slots of its own process.
Isolation follows from the tree rather than from a scan.
A frame and a pool below different children of one Untyped never meet,
because an Untyped makes a child only of memory nothing else it made lies in,
and every frame that no Untyped covers is a root over memory no pool can lie in:
device memory, flash and the log.
The root task's own regions are frames of the Untyped the root task lives in; see "Boot".
Memory that is to become a pool while frames of it exist
is first revoked back to its Untyped,
which uninstalls those frames wherever they are.
The self-check still checks isolation by address.

The pool's own descriptor is the first object in the pool's memory,
and it holds the pool's own node of the derivation tree,
the node the retype made, below the Untyped.
The Pool capability the retype returns hangs below that node,
and so does every capability to the pool and to its objects,
because an object's capability hangs below the Pool capability it was allocated through;
a copy stands beside its source and a derivation below it, so none leaves the subtree.
The node lies in the pool, out of every process's reach,
so a process may delete its Pool capability and the memory stays accounted for:
the pool then lies below the Untyped, reachable through nothing,
until a revoke below the Untyped destroys it,
as an object whose capabilities were revoked stays in its pool.
The kernel keeps no per-pool state outside the pool
but a list head that only the self-check and the host harness walk.
Objects are bump-allocated behind the descriptor, eight-byte aligned,
and each records its pool and how far back the object before it lies.

Kernel memory comes back at pool granularity.
Objects are never freed individually,
so a pool is the unit of trust for the memory behind objects:
revoking the capabilities to an object leaves the object in its pool,
unreachable, until the pool is destroyed.
Capabilities themselves are revoked one derivation at a time;
see "The derivation tree".

A pool is destroyed by a revoke below an Untyped it lies below, which meets its node;
no operation on a pool destroys it, so a Pool capability only allocates; see open decision 19.
A pool is the whole of the Untyped it was made of, so a revoke below that Untyped destroys it alone,
and a program that gives pools back one by one keeps their Untypeds.
The destroy is a revoke below the pool's own node
and then a walk over the pool's own objects, and both are the size of the pool.
The revoke takes every capability to the pool and to its objects, wherever they lie.
The walk then takes the objects newest first,
and the used mark goes back as it goes,
so a destroy stopped half way leaves a pool with fewer objects.
Each object gives up the nodes it holds, a table's slots and a process's,
as `OP_CAP_DELETE` gives one up: what was derived from it goes to its parent.
So no ring in a surviving table leads into memory that is the Untyped's again,
and a grant made through a dying table stands where its source stood,
for whoever holds that source's ancestors to revoke.
A revoke there instead would take the Untypeds the table held,
and with them pools the destroy would have to destroy in turn,
which is recursion the kernel does not have.
The memory goes back to the Untyped the pool hangs below,
which is free again once nothing else it made is left.

A generation counter in each object was the alternative, and it cannot work.
The counter lives in the memory being destroyed,
so the next pool built over that range starts its objects at one again,
and a capability held across the destroy
matches a fresh object of the same type at the same address;
a user who writes the range while it is theirs
can forge that match without waiting for the kernel to produce it.
A single epoch in the kernel's own state would survive the destroy,
but trades the problem for arithmetic:
a process that owns a little memory
can create and destroy a minimal pool in a loop
and wrap sixteen bits in under a second.
Slots therefore carry no generation,
and `user/init.c` rebuilds a pool over destroyed memory
to show that a stale capability does not come back with it.

Objects reference each other by capability,
and the destroy's revoke clears every capability to what it takes.
A process holds its table this way, a thread its process and its watch, and an `Irq` its notification,
so each may lie in any pool and outlive what it names:
clearing the capability stops the thread or leaves its faults unheard, disarms the `Irq`,
and leaves the process naming nothing;
see "A process's table", "Threads", "Faults" and "Interrupts".
A thread waiting on a notification inside a destroyed pool
is woken with `KERR_INVALID_CAP` and no bits,
because the wait can no longer be answered.
Nothing is allocated from a pool once its destroy has begun.

A revoke below an Untyped refuses with `KERR_STATE`
when the calling thread, its process or the process's table lies in the Untyped's memory.
That is a test by address, which is enough
because every pool below an Untyped lies in its memory.
The three may lie in different pools, and those tests are the whole of it:
the caller keeps what it runs on and the table it names capabilities in.
The revoke refuses too when the table it names the Untyped in lies in that memory:
the capability the call is made through, a capability to that table,
would go half way through the destroy of the table's pool,
where the call could neither be made again nor end.
The boot pool, which holds the root task, is a pool like any other, made of `BOOT_CAP_POOL_RAM`:
the root task cannot destroy it, since it lives there,
and a thread that holds that block and does not can, as a successor does;
see "The root task is its capabilities".
The objects of a pool are zeroed on the way out,
because the memory is about to be the Untyped's again
and they hold other processes' capability tables.
The rest of the pool comes back as it went in,
so a lender clears memory before it lends it if the borrower should not read it.

A borrower's pool is retyped from an Untyped derived from the lender's,
so a revoke below the lender's destroys it, whatever the borrower made of it,
and the lender gets the memory back without the borrower's leave.
Pools keep no pointer to each other.

### The derivation tree

Pools alone cannot do what seL4's derivation tree can:
take back one frame from a child, and everything the child did with it,
while the child goes on living.
A derivation tree over slots does that,
and rvuos keeps one in the shape that costs the least.

**What is a node.**
Every filled slot of every capability table,
every region installed in a process,
which is a slot of the same layout living in the `Process`,
every process's table slot, which lives there too,
every thread's process, units and watch, which live in the `Thread`,
every `Irq`'s notification, which lives in the `Irq`,
and every pool's own node, which lives in its descriptor;
see `kernel/object.h`.
Roots are the boot capabilities but those made of the root task's memory,
its frames and the boot pool's block, which hang below `BOOT_CAP_ROOT_RAM`,
the boot pool's own node, which hangs below that block,
and those to the boot pool and its objects, which hang below that node,
as the root task's table slot and its thread's process do.
A capability to a pool or an object is never a root,
and neither is a thread's process or watch or an `Irq`'s notification.

**What hangs below what.**
A carve hangs below the frame or line it was carved from,
a retype and a split below the Untyped they were made of,
a derivation below its source,
an installed region below the frame it was installed from,
the counter's frame below the clock it was derived through,
a process's table slot below the `CapTable` capability the process was made with,
a thread's process below the `Process` capability the thread was made with,
a thread's watch below the `Notification` capability it was set with,
and an `Irq`'s notification below the `Notification` capability the `Irq` was bound with.
A pool's own node hangs below the Untyped, and the Pool capability the retype returns below that node.
A capability to a new object hangs below the Pool capability it was allocated through,
and so does the `Irq` capability of `OP_IRQ_BIND`, whose line goes with everything derived from it.
A copy stands beside its source, under the same parent,
so a process can duplicate what it holds without making one copy the master of the other;
beside a root it is a root.
A move puts the node where its source stood, and the source's children below it.
So every capability to a pool or to one of its objects lies below the pool's own node,
which is what lets a destroy find them all without a sweep.

**What the operations do to it.**
`OP_CAP_REVOKE` clears everything below a slot and leaves the slot.
`OP_CAP_DELETE` and `OP_PROCESS_UNINSTALL` clear one node
and hand what was below it to its parent,
so deleting one's own copy of a grant does not take the grant back.
`OP_CAP_MOVE` puts a slot's node in another slot and changes nothing else.
A revoke below an Untyped destroys the pools it meets,
and a pool destroy revokes below the pool's own node,
then clears every slot the pool held as a delete does;
see "Kernel pools and revocation".

**The shape.**
Three links per slot, first child, next and previous,
where the last child's next points up to the parent with the low bit set,
which the four-byte alignment of slots leaves free,
and the first child's previous is the last child.
The children of a node are then a ring that closes through the node.
A node leaves its ring in constant time:
its predecessor is one link away,
and it is the first child exactly when that predecessor links up to the parent.
Three links at most name a node from outside it:
its predecessor's next, or its parent's first child when it is first,
its successor's previous, and the link up from its last child.
So a move copies the node and rewrites those three, in constant time.
A delete puts the node's children right after it, through the first and the last,
and then takes the node out as a leaf.
A revoke is deletes of the first child, one after another,
each handing its children up to the front of the ring,
until the node it revokes below has none:
a step per node, with no stack,
the tree whole between any two steps,
and the next step always at the node's first child.
A pool's own node is the one first child a revoke does not delete at once:
it stays first while the pool is destroyed below it, a step at a time,
and goes last.
No operation walks a ring.
seL4 keeps its tree as a list in pre-order, two links per slot and a word less,
and stores no depth:
whether the next slot lies below one is read off the two capabilities,
their contents and a flag or two.
rvuos cannot read it off,
since a copy and a derivation of the same region can be the same bits;
which one a slot is lives only in the links.
A parent link instead of the previous one would move for every child a delete hands up.

**What it does not do.**
Revoking the capabilities to an object does not free the object;
the pool does that, and a pool whose capabilities were revoked or deleted
stays until the Untyped it lies below is revoked.
A root has no parent to revoke it from.
A capability copied beside its source can be taken back only from their common parent,
which is the point of copying rather than deriving.
An earlier sketch by the same author,
`os4cm4` in the LANoT repository,
put three links next to the object pointer in a sixteen-byte slot
and never got as far as maintaining them;
what made this one tractable was making installed regions nodes,
so that a revoke unmaps by derivation and never by address range,
and giving each pool a node of its own that everything about the pool lies below.

### A process's table

A `Process` holds its `CapTable` by a capability in a slot of its own,
not by a pointer.
Allocating the process fills the slot from the `CapTable` capability it names,
below that capability in the derivation tree.
No system call names the slot: a process's slot numbers index its table.

A revoke above the slot, or the destroy of the table's pool, clears it like any other,
so the table may lie in any pool, and several processes may share one.
A process whose slot is empty fails every call with `KERR_INVALID_CAP`.
Within one call only a revoke can take it, which then returns; see "Bounded work".
It costs sixteen bytes per process and a test on every call;
before, the table had to lie in the process's own pool so a pointer could not dangle.
Open decision 15 is about what else the slot could do.

### Region slots

A `Process` has a fixed array of eight region slots.
Installing a `Frame` into a slot
requires the frame
and a `Process` capability with the write right.
The kernel recomputes and caches the PMP register image
for the process at that point,
not on every context switch.
The installed region hangs below the capability it was installed from
in the derivation tree, so revoking below that capability unmaps it,
whichever process it was installed in.

Rights on the installed slot are the intersection
of the frame's rights
and the rights requested at install time.
No frame overlaps a pool, by the derivation tree,
so the install looks at no other process and no pool.
Installed regions in one process may not overlap,
because PMP resolves overlaps by entry number
and the result would depend on slot order rather than policy.
A region cannot be installed writable but not readable:
PMP reserves the encoding R=0, W=1,
QEMU quietly drops the write right,
and hardware may do anything.
On the ESP32-C6, `PMP_SPLIT_STORE_AS_READ` in its `board.h`,
a region the process may write may not end where one it may read but not write begins,
whichever is installed second, since a misaligned store would write the upper one; see "Boards".
The overlap check walks the regions already, so the rule costs nothing more.
It is the board's, as its layout is; QEMU and the host build have neither.

How a process's memory is laid out is entirely the creator's business.
The kernel does not know what a code segment or a stack is;
the root task makes frames of the memory it owns,
installs them into the child's slots with the rights it chooses,
and starts a thread at whatever entry point and stack pointer it likes.

### Threads

A `Thread` is an execution context inside a process:
a register frame, a state, and the process it belongs to.
Creating one takes a `Process` capability with the write right,
and the new thread may be allocated from any pool.

A thread holds its process as a process holds its table,
by a capability in a slot of its own, below the capability it was made with.
A revoke above the slot, or the destroy of the process's pool, clears it,
and clearing it stops the thread, as clearing an installed region unmaps it:
a ready thread leaves its queue, a waiting one its notification's waiters,
and the running one gives the processor up as its call returns.
`OP_THREAD_RESUME` refuses a thread without a process,
so it stays stopped until its own pool goes.
The running thread can lose its process only to a revoke it makes itself;
see "Bounded work".
It costs twenty bytes per thread and a test on every call and every switch;
before, a thread had to lie in its process's pool so a pointer could not dangle.

A thread is either stopped or ready.
`OP_THREAD_CONFIGURE` sets a stopped thread's program counter
and stack pointer, and `OP_THREAD_RESUME` makes it ready.
It runs only while it is bound to units of time, which is apart from its state;
see "Scheduling".
The kernel validates neither value:
it knows no executable format and no calling convention,
so a thread that starts nowhere useful faults,
which is its creator's business: the thread stops, and its watch hears it; see "Faults".

### Faults

**A fault is the thread's alone.**
An access fault, an illegal instruction, a misaligned access or a breakpoint in user mode
stops the thread that made it and nothing else:
its process, its other threads and the units it earns stay as they were,
and the processor goes to the next thread, as when a thread waits.
The thread stops where it faulted, `mepc` at the instruction and every register as the fault found it,
which is a state `OP_THREAD_CONFIGURE` and `OP_THREAD_RESUME` already take:
a resume runs the instruction again, and a configure before it starts the thread afresh.
Before, a fault stopped the machine whatever else could run,
so one thread that jumped nowhere took every process with it.

**A thread's watch hears it.**
Whoever holds the thread with `RIGHT_W` names what its faults signal with `OP_THREAD_WATCH`:
a notification, and the bits.
The thread holds the notification as it holds its process,
by a capability in a slot of its own, below the `Notification` capability the watch was set with,
which needs `RIGHT_W`, since the kernel signals on the setter's behalf, as it does for an `Irq`.
A fault signals the bits there as `OP_NOTIFY_SIGNAL` would,
so a watcher waits for a fault as for anything else.
A watcher of many threads gives each a bit, as a server gives each client one.
The watch stays until it is cleared or replaced, so a thread that faults again signals again.
A revoke above the slot, or the destroy of the notification's pool, clears it,
and a thread without a watch stops at a fault all the same, and nobody hears.
The node is out of the thread's reach:
a watch named by a slot of the thread's own table would need no node,
but the thread could then signal a fault it never made, or delete its watch.
It costs twenty-four bytes per thread, one node more for a destroy to look at, and a signal per fault.

**What the watcher does is policy.**
The kernel stops the thread, signals its watch and does no more, since every answer is an operation already:
resume it once what it reached for is mapped, which maps a region on demand;
configure it to start again; take its process, which ends it, or its units, which a stopped thread still earns;
revoke below the Untyped it was made of; or leave it stopped.
A breakpoint is a fault like any other, so a thread that is done can stop with one and its watcher hears it:
that is as near as rvuos comes to an exit, and it needs nothing more.

**The watcher can ask what the fault was, and move the thread on.**
The kernel writes `user fault` and the frame's `mcause`, `mepc` and `mtval` into its log,
on ARMv7-M the exception, the fault status, the pc and the fault address,
while the bits say only which thread.
`OP_THREAD_FAULT` returns the same to whoever holds the thread with `RIGHT_W`,
from the frame, which keeps them while the thread stays stopped where it faulted.
A new thread's frame is zero, which is a cause too, so a flag, `THREAD_FAULTED`, tells a fault from none.
`OP_THREAD_READ_REG` and `OP_THREAD_WRITE_REG` reach any general register of a stopped thread and its program counter,
so a watcher can emulate the instruction the thread faulted at, write the program counter past it, and resume it;
what ARM keeps from such a watcher is open decision 23.
A thread that faults again and again fills the log only while its watcher resumes it every time.

**Why a signal, and not a message.**
seL4 sends a fault as a message through an endpoint and restarts the thread by the reply.
rvuos has no endpoints, open decision 5, and a fault needs none:
the stopped state, the operations that start a thread again, and the signal are all there already.

## Communication and synchronisation

Processes exchange data through memory they both have a region for,
and say when through notifications.
No data passes through the kernel.

A `Notification` is one word of sticky bits.
`OP_NOTIFY_SIGNAL` sets bits and never blocks;
`OP_NOTIFY_WAIT` takes every set bit and clears them,
blocking until there is one.
The bits are sticky, so a signal that arrives
before the waiter got there is not lost.
`RIGHT_W` may signal and `RIGHT_R` may wait,
so a driver can be given the power to announce something
without the power to consume the announcement.
A signal wakes one waiter, which takes every bit;
which one, when there are several, is not promised.
Today it is the one that has waited longest:
a notification heads a queue of its waiters, threaded through them,
so a signal finds one without a walk.

**Why this and not a synchronous endpoint.**
The `A` extension gives userspace `lr.w`/`sc.w` and the `amo*` instructions,
which are enough for a lock or a ring buffer in shared memory.
What atomics cannot do is stop a thread:
on one hart a spinlock is a deadlock while nothing preempts
and a burnt time slice once something does.
So the one service userspace cannot build for itself
is "stop running me until someone says otherwise",
and a notification is that service and nothing else.
Message registers, a reply capability good for one use,
queues of senders and receivers, and capabilities inside messages
are all mechanism an endpoint needs and this does not.
Handing over a capability already has an answer:
a `CapTable` capability with `RIGHT_W`.

The cost: a round trip is four system calls rather than two,
and the kernel no longer guarantees that one answer
reaches the thread that asked.
Whether to revisit that is open decision 5.

**A trap breaks the reservation.**
An `mret` may keep a reservation, and stores from the same hart do not break it,
so a thread preempted between its `lr.w` and its `sc.w` could overwrite another thread's store.
`trap_return` therefore makes an `sc.w` of its own on every return,
as the unprivileged specification asks,
and a thread's `sc.w` fails whenever a trap came between it and its `lr.w`.

**A server with many clients** waits on one notification,
not on many, because the bits are the clients.
Each client holds the server's notification with `RIGHT_W` only,
owns a bit, and shares a region with the server and with nobody else.
One wake can then carry several clients' work,
and a burst from one client costs one wake rather than one per request.
Thirty-two clients fill the word; past that it takes
a second notification and a second server thread,
or a bit meaning "read the client table"
that clients mark with an `amo*`.
Which client owns which bit is a convention the kernel does not enforce,
so a client can set another's bit and cost the server
one wasted look at a ring buffer it cannot read;
it cannot forge data or consume another client's wake.
Putting the bits in the capability, as seL4's badges are,
is open decision 6.

Interrupts arrive as signals on the notification bound to an `Irq`,
which needed no new mechanism; see "Interrupts".
