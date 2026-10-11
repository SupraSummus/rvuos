# rvuos design: objects, capabilities and communication

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Kernel objects and capabilities

### Capabilities

A capability is a kernel-held reference to a kernel object
together with a set of rights.
Userspace never sees a capability's links nor the address of an object;
its type, its rights and what it names, `OP_CAP_INFO` tells whoever holds it.
It names capabilities by slot numbers, which say which of its process's tables and the index in it,
and the kernel validates the number, the object type, and the rights
on every use; see "A process's tables".

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

A process needs no capability to write its own tables.
Every operation that produces a capability
puts it into a slot of one of the caller's tables,
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
The kernel resolves the slot, checks that the type accepts the operation,
that the capability carries the rights it needs,
and that what the operation does not read is zero, from one table, `op_shapes`,
and only then looks at the arguments.
A wrong type and a missing right are distinct errors
so that a caller can tell a programming mistake from a policy decision.

Describing is the exception: `OP_CAP_INFO` comes before the types' checks, every type takes it,
and it needs no right, nor does `OP_POOL_INFO` on a pool.
Describing grants no authority: it changes nothing and returns nothing a call takes as a reference,
only the capability's own words, the constants of the machine its type's operations depend on,
and the accounting its holder needs, a pool's room and what an Untyped made.
It does tell what nothing told before:
a clock with no right reads the counter's rate and the tick, though not the time,
an Untyped's holder the kind of what lies first below it,
and a `KernelPool` capability with no right a pool's room, and so what its other holders allocate; open decision 33.

#### Slot format

A slot is twenty-four bytes: type, rights, an index, padding, two words of content,
and three links of the derivation tree.
The index is an installed region's slot in its process,
so that uninstalling it finds the process without a walk.
For object capabilities the first word of content is the object's address
and the second is unused,
but for a `Notification`, where it is the bits the capability may signal; see "Communication and synchronisation".
A slot carries nothing that has to be checked against the object,
because a destroy clears the slots naming what it takes;
see "Kernel pools and revocation".
A `Frame` has no object behind it:
its two words are the base address and the size,
and the rights are the memory permissions it may grant.
An `Untyped` has none either:
its first word is its block, base and size as NAPOT encodes them,
its second is 0 but while its delete as a root goes, see below,
since whether it is free the derivation tree says,
and its rights are those of the frames made of it.
Carving a frame, retyping an Untyped into a frame and splitting one into halves
are therefore pure table operations that touch no kernel memory,
which is what lets the root task hand out memory
before it has created a single pool.
An `IrqLine` capability has the same shape for the same reason:
its words are the first line and the count, there is no object,
and the root task hands lines out as it hands out memory.
A `Time` capability is the same again, with units of the processor's time for lines.
A `Clock` capability, like `Print`, `Halt` and `Test`, has neither object nor content:
there is one counter and one watchdog on the machine.

### Object types

| Object | Purpose |
|---|---|
| `Untyped` | Memory that may become a frame, a pool or two halves. Never installed. No object. |
| `Frame` | A physical address range with maximum rights. Installed into a process's PMP slots. No object. |
| `KernelPool` | Memory retyped from an Untyped and handed to the kernel. All other objects are allocated from pools. |
| `CapTable` | A process's capability table. Allocated from a pool. |
| `Process` | A protection domain: capabilities to its `CapTable`s, a set of region slots, and its threads. |
| `Thread` | An execution context inside a process: its registers, its state, what its faults signal, and a place for each core it may run on. |
| `Notification` | A word of sticky signal bits. The only way a thread can wait to be started again by a signal. |
| `IrqLine` | A range of interrupt lines: the log's and the controller's, or the timer lines. Bound one at a time into an `Irq`. |
| `Irq` | One line bound to a notification. Signals it when the line fires. |
| `Time` | A range of the processor's units of time. A thread runs only while bound to some, or to none with spare time. No object. |
| `Print` | Writing into the kernel's log. No object. |
| `Halt` | Halting the machine. No object. |
| `Test` | Driving the machine as a test does: tracing, ticking, interrupting, preempting. No object. |
| `Clock` | The machine's time, read with `RIGHT_R`, also through the counter's frame, and its watchdog, fed with `RIGHT_W`. No object. |

Every object's size follows from its type,
a `CapTable` from its slot count and a `Thread` from its places,
so a pool can be a bump allocator
whose contents can be walked without a free list.
`OP_POOL_INFO` tells each size as a pool counts it,
so a program sizes its pools by asking, and the kernel may change the sizes, open decision 29.

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
A delete of a root Untyped is the exception:
with no parent to hand them to, what lies below it becomes roots one at a time, and the delete may stop between two.
Those already roots lie in its memory out of the tree's sight, while another thread may take the rest,
so the delete first marks it going, in its second word, and a going Untyped makes nothing.
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

A pool tells its room.
`OP_CAP_INFO` on a pool tells its size, its room, the bytes an allocation may still take, and whether it is being destroyed,
when the room is nothing, since nothing is allocated from a pool then;
`OP_POOL_INFO` tells what an object of a type would take, as the allocation counts it.
So an allocation that passes its other checks finds no room exactly when what its type takes is more than the room,
while nothing else allocates of the pool in between, which another core or another thread may.
A pool for a set of objects is the descriptor and them, rounded up to a block, a power of two of at least `POOL_MIN_SIZE`.
The allocator and the description take their sizes from one place, `obj_type_size`,
and the self-check's walk and the destroy from another, `obj_size`,
so the tiling holds the two to each other on every build,
and `host/history.c` holds what an allocation takes to the structures' own sizes.
`OP_CAP_INFO` on an Untyped tells what lies first below it,
so a pool that nothing names any more shows there as `CAP_POOL` until a revoke below the Untyped destroys it;
it tells neither what the pool holds nor whether something outside it still holds its objects,
so it is no proof that the revoke harms nobody; open decision 33.

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
and `user/init/root.c` rebuilds a pool over destroyed memory
to show that a stale capability does not come back with it.

Objects reference each other by capability,
and the destroy's revoke clears every capability to what it takes.
A process holds its tables this way, a thread its process and its watch, and an `Irq` its notification,
so each may lie in any pool and outlive what it names:
clearing the capability stops the thread or leaves its faults unheard, disarms the `Irq`,
and leaves the process naming nothing through that table slot;
see "A process's tables", "Threads", "Faults" and "Interrupts".
A thread waiting on a notification inside a destroyed pool, or lending its time to a thread inside one,
is woken with `KERR_INVALID_CAP` and no bits,
because the wait can no longer be answered as it was asked.
A lender and the thread it lends to name each other by address, as a notification and its waiters do,
since only the lender's own wait puts it there,
and the destroy of either takes the other's link to it; see "Communication and synchronisation".
Nothing is allocated from a pool once its destroy has begun.

A revoke below an Untyped refuses with `KERR_STATE`
when the calling thread, its process or a table the process mounts lies in the Untyped's memory.
That is a test by address, which is enough
because every pool below an Untyped lies in its memory.
They may lie in different pools, and those tests are the whole of it:
the caller keeps what it runs on and the tables it names capabilities in.
A process that is to give back the memory a table of its lies in unmounts the table first, or moves out of it;
see "A process's tables".
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
every process's table slots, which live there too,
every thread's process, watch and each of its places' units, which live in the `Thread`,
every `Irq`'s notification, which lives in the `Irq`,
and every pool's own node, which lives in its descriptor;
see `kernel/object.h`.
Roots are the boot capabilities but those made of the root task's memory,
its frames and the boot pool's block, which hang below `BOOT_CAP_ROOT_RAM`,
the boot pool's own node, which hangs below that block,
and those to the boot pool and its objects, which hang below that node,
as the root task's first table slot and its thread's process do.
A capability to a pool or an object is never a root,
and neither is a thread's process or watch or an `Irq`'s notification.

**What hangs below what.**
A carve hangs below the frame or line it was carved from,
a retype and a split below the Untyped they were made of,
a derivation below its source,
an installed region below the frame it was installed from,
the counter's frame below the clock it was derived through,
a process's table slot below the `CapTable` capability the process was made with or the table was mounted with,
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
`OP_CAP_DELETE`, `OP_PROCESS_UNINSTALL` and `OP_PROCESS_UNMOUNT` clear one node
and hand what was below it to its parent,
so deleting one's own copy of a grant does not take the grant back.
`OP_CAP_MOVE` puts a slot's node in another slot and changes nothing else.
`OP_PROCESS_MOUNT` adds a leaf below the `CapTable` capability it names.
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

### A process's tables

A `Process` holds the tables it names capabilities in by capabilities in slots of its own, not by pointers:
`PROCESS_TABLES` table slots, two today.
Allocating the process fills the first from the `CapTable` capability it names, below that capability in the derivation tree;
`OP_PROCESS_MOUNT` fills any empty one the same way, from a `CapTable` capability with `RIGHT_W`,
and `OP_PROCESS_UNMOUNT` empties one.
A revoke above a table slot, or the destroy of the table's pool, clears it like any other,
so a table may lie in any pool and be shared, by processes or by the table slots of one.

A slot number names a slot of one of the caller's tables, `SLOT_IN(table slot, index)`:
the table slot in the bits from `SLOT_TABLE_SHIFT`, 10, up, and the index below.
No table has more than 1024 slots, so the numbers below 1024 are the first table's, as every number was before.
The depth is fixed: no number goes through a slot that holds a table,
so a lookup costs a shift and a mask more than it did, and needs no guard.
A number that points into an empty table slot names nothing, `KERR_INVALID_CAP`.
Within one call only a revoke, which then returns, or an unmount of the caller's own empties a table slot of the caller's;
see "Bounded work".
A table mounted twice gives each of its slots two numbers, so a call that takes two slots compares the slots.

**A process moves to another table by itself**, a call per capability.
It mounts the new table second and moves each capability to the same index of it with `OP_CAP_MOVE`,
the new table's own capability last;
then, through the second table's numbers, it unmounts the old table, mounts the new one first, and unmounts the second.
Each capability keeps its number and its place in the derivation tree, and is in reach all along, as `SLOT_IN(1, index)` meanwhile.
So a process outgrows its table, or leaves memory it is to give back, without a successor;
no other thread of it may make a call meanwhile.
`self_migrate` in `user/lib/self.h` does it.
It costs every process twenty-four bytes a table slot.
Why two table slots and a fixed depth is open decision 15.

### Region slots

A `Process` has the region slots its creator asked for with `OP_POOL_ALLOC`,
from one to the entries the core has for them, which `OP_CAP_INFO` on any process tells;
see open decision 35.
The root task's has `ROOT_REGION_SLOTS`, eight, or fewer where the core has fewer entries.
Its PMP image follows the slots in the object, an entry for each,
so a process pays for the regions it may have, and an install never runs out of entries.
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
a register frame, a state, the process it belongs to,
and its places, as many as its creator asked for, one for each core it may hold the time of at once; see "Cores".
Creating one takes a `Process` capability with the write right,
and the new thread may be allocated from any pool.

A thread holds its process as a process holds each of its tables,
by a capability in a slot of its own, below the capability it was made with.
A revoke above the slot, or the destroy of the process's pool, clears it,
and clearing it stops the thread, as clearing an installed region unmaps it:
a ready thread leaves its queue, a waiting one its notification's waiters and lends nothing any more,
and the running one gives the processor up as its call returns.
`OP_THREAD_RESUME` refuses a thread without a process,
so it stays stopped until its own pool goes.
The running thread can lose its process only to a revoke it makes itself, see "Bounded work",
and can be stopped only by itself.
A thread another core runs can lose it, or be stopped, by any call, which makes that core trap first; see "Cores".
It costs twenty bytes per thread and a test on every call and every switch;
before, a thread had to lie in its process's pool so a pointer could not dangle.

A thread is stopped, ready or waiting.
`OP_THREAD_CONFIGURE` sets a stopped thread's program counter, stack pointer
and argument, the first register of a call, and zeroes the rest,
and `OP_THREAD_RESUME` makes it ready.
It runs only while a place of it is bound to units of time, or to none with spare time, which is apart from its state;
see "Scheduling".
The kernel validates none of the three:
it knows no executable format and no calling convention beyond where a function finds its first argument,
so a thread that starts nowhere useful faults,
which is its creator's business: the thread stops, and its watch hears it; see "Faults".

**Any thread can be stopped.**
`OP_THREAD_STOP` takes a thread to stopped from whatever it is doing, and says what that was,
through a capability to it with `RIGHT_W`, the one that configures and resumes it.
A stopped thread stays as it is, a fault to tell among it, so a supervisor may stop a child whatever happened meanwhile.
A ready one leaves its queue, and one another core runs makes that core trap first, which drops that trap; see "Cores".
A waiting one leaves its notification's waiters, and its borrower's ring of lenders,
and stands at the call it waited in, its pc back by the call's size, every register as the call found it,
since a call that blocks writes none.
It keeps its process, its watch, its places' units and accounts, and its own lenders, whose time buys nothing while it is stopped;
a turn it pays for, lent to another thread, runs on to its end, charged to it.
Nothing is signalled, the watch included: a stop is always asked for, and the watch hears what nobody asked for.
The loss of a process stops a thread the same way, a waiter standing at its call too, so one rule holds for every stop.
It costs a flag in each core, set by a stop and read by every trap on several cores, and no walk:
the stop's work is the loss of a process's, which the kernel did already.

**Why at the call, and not a status.**
A waiter stopped could instead be answered, `KERR_STATE` say, and resumed past its call.
At its call it is the shape a restartable call that stopped for an interrupt has, `include/rvuos/abi.h`:
a resume makes the call again, which checks everything afresh,
so stop and resume are invisible to the thread but for what any call made again sees,
bits another waiter took, a place lost among the waiters, a slot cleared meanwhile.
A watcher may answer the wait itself, writing `a0`, `a1` and the pc past the call as it emulates an instruction.
The host build follows it without a register written, so tracing allows the stop; see "Verification".
A status would need a new error or an overloaded `KERR_STATE`, and every wait in `user/lib/` a retry on it.

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
which needs `RIGHT_W`, since the kernel signals on the setter's behalf, as it does for an `Irq`,
and so only the bits that capability may signal.
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
revoke below the Untyped it was made of; stop it, which leaves it as it is; or leave it stopped.
A thread that is done says so first by its own means, as `child_report` in `user/lib/child.h` does,
then stops itself, through a capability to its own thread with `RIGHT_W`,
which is as near as rvuos comes to an exit and signals nothing, since whoever hears the report may stop it anyway.
That capability also reaches the thread's watch, which the node above keeps out of a thread's reach,
so a thread kept from its watch ends with a breakpoint instead, a fault like any other, which its watcher hears.

**The watcher can ask what the fault was, and move the thread on.**
The kernel writes `user fault` and the frame's `mcause`, `mepc` and `mtval` into its log,
on ARMv7-M the exception, the fault status, the pc and the fault address,
while the bits say only which thread.
`OP_THREAD_FAULT` returns the same to whoever holds the thread with `RIGHT_W`,
from the frame, which keeps them while the thread stays stopped where it faulted, a stop of it since among that.
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
A capability also carries the bits it may signal, in the second word of its slot:
every bit for the one an allocation returns,
and fewer for one `OP_NOTIFY_CARVE` made, below the one carved,
as a carve of a frame is less memory below the frame.
A signal sets only the bits it names that its capability may signal,
and one that names none of them is refused;
an `Irq` or a watch signals through the capability it was bound or set with, and so only its bits too.
A signal wakes one waiter, which takes every bit;
which one, when there are several, is not promised.
Today it is the one whose time pays for the signalling core's turn, if it waits there, and else the one that has waited longest:
a notification heads a queue of its waiters, threaded through them,
so a signal finds one without a walk.

**Why this and not a synchronous endpoint.**
The `A` extension gives userspace `lr.w`/`sc.w` and the `amo*` instructions,
which are enough for a lock or a ring buffer in shared memory.
What atomics cannot do is stop a thread:
on one hart a spinlock is a deadlock while nothing preempts
and a burnt time slice once something does.
Between threads on two cores a short spin is sound, but a holder the tick preempted still costs the spinner its turn.
So the one service userspace cannot build for itself
is "stop running me until someone says otherwise",
and a notification is that service and nothing else.
A thread that stops itself, `OP_THREAD_STOP`, is no such service:
a resume that comes before the stop is refused and lost, where a signal before a wait stays.
A lending wait, below, adds a second, "and give my time to whoever I wait for", which only the waiter can name.
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

**A signal orders memory for the wait that takes its bits.**
What a thread stored before a signal, a thread sees once a wait returns it that signal's bits, on any core:
a signal is a release, and such a wait an acquire.
It costs nothing: one core is one hart, which sees its own stores in order,
and with several the signal gives the kernel's lock up after setting the bits,
and the waiter's core takes it before the wait returns; see "Cores".
So memory handed over by a notification needs no fence,
and a kernel that ever signals without the lock still owes the release and the acquire.

**Exclusion needs no kernel.**
The sticky bits do what a futex's comparison does:
a give that lands between a taker's look at the word and its wait leaves a bit, and the wait returns at once,
so a lock in shared memory, `user/lib/lock.h`, needs no call that compares a word in user memory,
and none at all while nobody else wants it.
A notification alone is a lock too, a bit signalled once that takers wait for and givers signal, at two calls a hold.

**A waiter may lend its time.**
A lock in memory cannot tell the kernel who holds it, so a taker that waits cannot give the holder its time,
nor a client that waits for its answer give its server its time.
A futex's kernel finds a holder in the word, but this kernel reads no user memory, so the waiter names it:
`OP_NOTIFY_LEND` waits as `OP_NOTIFY_WAIT` does,
and lends a thread, named by a capability to it with `RIGHT_X`, the caller's time meanwhile; "Scheduling" says how.
`RIGHT_X` on a thread lends it time and does nothing else, so a holder of a lock, or a server, hands its waiters that right alone.
A name that is wrong costs the waiter the time it lends, and gives nobody any power,
since what a lender spends is its own account.
Lent time does let the borrower run past its own units, which is why it takes a right the borrower's owner hands out,
and a thread its scheduler stopped by unbinding it does not run on lent time; open decision 9.
The lock of `user/lib/lock.h` that names its holder writes the holder's name into the word as it takes it,
so a hold nobody else wants makes no call, and a taker that finds it held lends the thread of that name its time.
A client lends its server's thread its time while it waits for the answer, which open decision 5 asks for.
The time goes one step, from the thread that has lent the borrower time longest, and only within a core:
a borrower keeps its lenders on a ring, oldest first, so it finds its lender without a walk.
Priority inheritance takes the most of every waiter, and goes on through a holder that waits in turn,
walks that goal 4 rules out.

**A signal hands a lender its turn back.**
A signal wakes the thread whose time pays for the signaller's turn first, if it waits there,
and that thread takes the rest of the turn back; see "Scheduling".
So a give that signals the taker whose time it ran on lets the taker take the lock before the giver can again on that core,
and an answer that signals the client whose time its server ran on reaches the client at once.
The kernel hands nobody the lock: it hands the turn, which is the taker's own.
A taker that lends nothing, or lends from another core, takes its chances with a lock in memory, which is not fair.

**A borrower that goes wakes its lenders.**
A thread destroyed wakes every thread that lends it its time with `KERR_INVALID_CAP` and no bits, a paid step each,
as a notification's destroy wakes its waiters: the call each made names something that is gone.
So a taker learns that the holder its lock names is gone, and may take the lock over, knowing the data it guards may be broken.
A holder that faults keeps the lock, stopped, which its watcher hears, and so does one stopped, which nobody hears.
A stopped borrower keeps its lenders, which go on waiting, their time buying it nothing until it is resumed:
it is still there, and waking them is a walk only a destroy pays for.

**Why not a mutex in the kernel.**
A `Mutex` object did all of this with its holder in the kernel, open decision 27:
a call to take it and one to give it back even while nobody else wanted it,
and an unlock that handed it to its oldest waiter, whose waiters could lend the holder their time.
`make lab` showed that its gain was the lending and the turn, not the ownership:
handed in order to a waiter that waited for its turn, the mutex made a convoy beside a process that spins,
and the lending wait does what its lending did with the lock left in memory; see `user/lab/NOTES.md`.

**A server with many clients** waits on one notification,
not on many, because the bits are the clients.
Each client holds the server's notification with `RIGHT_W` only, carved to its own bit,
and shares a region with the server and with nobody else.
The bits a wait returns therefore say who signalled:
a client cannot set another's bit, whatever it names,
and it need not know its own, since naming every bit signals all it may.
This is seL4's badge, as a mask that narrows as rights do; see open decision 6.
One wake can then carry several clients' work,
and a burst from one client costs one wake rather than one per request.
Thirty-two clients fill the word; past that it takes
a second notification and a second server thread,
or a bit meaning "read the client table"
that clients mark with an `amo*`, which they may then all signal.

`user/lib/chan.h` is that ring buffer between two children, with a bit each way,
each end holding the other's notification carved to its own bit.
Each end keeps where its rings lie and their shape in memory the other end cannot write,
since a ring's own header, shared, would let a peer steer the other's writes outside the ring;
all a peer can spoil is a packet.
It can still make a ring seem as full as it likes, so a server takes a ring's worth from each client for each wake.
A channel starts zeroed, a hub's end connected again among them,
since the memory's last user, a client taken down or a child whose block it was, wrote what the next has no business reading;
"Kernel pools and revocation" leaves that to whoever lends memory.

A region for each client would bound a server's clients by its regions, seven at most on RP2350's Hazard3,
less the server's code, data and devices.
So `user/lib/chan.h` has a hub too: one frame of a channel for each client, which the server installs once,
of which each client is given its own channel, carved.
A parent watching many children meets the same bound through their pages, and answers it the same way,
with room for its children's data, `self_room` in `user/lib/self.h`, which it installs once and carves each child's data from.
The Wi-Fi system's network process serves its clients through a hub, see `user/wifi/NOTES.md`.

**State one process publishes to many**, a link's state or a setting, needs no ask at all.
`user/lib/seqlock.h` keeps two copies of it and a count that sends readers to the one not being written,
so neither side makes a call or waits for the other, and readers, which write nothing, may hold it read only.
Linux's seqlock keeps one copy, which suits a writer the tick cannot take, as Linux's are,
but a reader of a writer in user mode stopped halfway could only spin until the writer's next turn.
With two, a write fills the copy readers are not sent to before it sends them there,
so a writer stopped, faulted or taken down halfway costs its readers nothing, nor does one that starts again.
The Wi-Fi system's network process publishes its addresses so, which its clock reads without an ask.

**Handing memory over**, with the kernel's word that the giver no longer reaches it,
is an uninstall from the one and an install into the other by a third process that holds the frame,
`child_unmap` and `child_map` in `user/lib/child.h`;
the uninstall returns once the giver's threads have lost the region, on another core too; see "Cores".
Moving the frame's capability would not do, since what was installed from it stays installed wherever it goes.
So there is no object for a buffer passed from hand to hand: it would save one call of the six a handover takes.

Interrupts arrive as signals on the notification bound to an `Irq`,
which needed no new mechanism; see "Interrupts".

### Bulk data

**Bulk data is lent, not sent.**
A frame a radio received, a block a disk read, a picture a program drew for a display:
data too large to copy at every hop, or that a device writes or reads by itself, stays where it lies,
and a reference to it travels instead.
`user/lib/bulk.h` keeps each connection's buffers in the lender's memory,
lends them to the borrower one at a time through a ring of references, and has them back through another;
a reference says which buffer, where in it the bytes begin and how many, and a word the two give a meaning to.
The rings are a channel's, so a buffer costs a few loads and stores, and a signal at most when the other side may wait for it,
and the bytes cross no ring and no kernel.

**The kernel decides who may touch what, and until when; never which buffer.**
The parent makes the memory of a lender's connections one frame, which the lender installs once,
carves each connection's blocks out of it and installs them in the borrower with the rights the trust between the two allows,
gives each side the other's inbox carved to its bit,
and uninstalls the blocks from the borrower when it closes the connection.
The hardware holds every access to that, which is the kernel's word.
Which buffer is whose at a moment is the two sides' protocol, which each keeps in memory the other cannot write.
A region for each buffer would take one of a process's few region slots, an install and a revoke a buffer,
and a buffer a power of two at a multiple of its size;
so the kernel knows no buffer, as it knows no packet,
and handing memory over for good remains the uninstall and install above.

**Who lends follows whom each trusts.**
The lender is the side its borrowers trust and that outlives them: a driver to the stack it serves, a server to its clients.
Its memory holds all of a connection,
so a borrower taken down unmaps nothing the lender reads,
and it keeps which buffers it lent in its page, where no borrower writes.
It takes back only a buffer it lent and has not had back,
so a forged reference costs its forger the reference and nothing else,
and no buffer is lent twice at once.

**The rights are the trust, a block at a time.**
A connection is two blocks:
the buffers lent to read, which the borrower maps read only,
and the two rings with the buffers lent to fill, which it maps to write too.
A buffer the lender reads itself, a frame its device wrote among them, lies in the first,
so the borrower can change no frame its driver has yet to parse, lent or not.
The hardware's word is a block's, though, not a buffer's:
a borrower may read every buffer lent to read, lent to it or not yet,
so a lender puts into a connection's buffers only what that borrower may see,
everything a device that writes whatever arrives received, for one,
and a connection starts zeroed, nothing of its last borrower's in it.
A buffer lent to fill comes back with what the borrower wrote there, which it can change still.

**Parse what you trust, carry what you do not.**
Bytes are as trustworthy as the least trusted side that can write them.
A side decides in place only on bytes no less trusted side can write;
what the other side can write it reads once, into memory of its own, and decides on that copy,
and the rest it carries, copying it, summing it or handing it to a device, deciding nothing on it twice:
a change under its feet then costs the writer its own bytes and nobody else anything.
So a network stack reads a client's request header once and carries the datagram,
and a driver reads a frame's destination once and lets its device read the rest.

**Two places want a copy, whatever the mechanism.**
Bytes a less trusted side wrote, laid beside what a more trusted side asserts of them, a header in front of a payload,
are copied into one buffer by the more trusted side,
since a header it wrote into the other's buffer could be changed once checked;
a device that gathers a frame from several places spares that copy.
And bytes one source shares out among borrowers that may not read each other's, a stack's frames among its clients,
are copied once, where they are shared out, into each borrower's own buffers;
a reference passed on uncopied is for a borrower that may read the whole block it lies in.

**A network stack, for one.**
Its driver lends it the frames the radio received, to read:
the MAC writes each into a buffer of the driver's read block, the driver checks it and lends it where it lies,
and the stack parses it there, since only the driver and its MAC can write it;
the driver keeps a buffer or two back for frames it reads itself, its handshake's.
The stack lends each client, a connection each, the datagrams for the client's ports to read,
copying each once out of the driver's frame, since clients may not read each other's, and hands the frame back.
For what a client sends, the stack lends it buffers to fill;
it reads the request's header once into its own memory, checks that the port is the client's,
and writes its own headers into a buffer the driver lent it to fill, the payload copied after them;
the driver reads the destination once and lays its own header in front of the frame in the MAC's buffer, copying it,
or has the MAC gather the two where they lie.
So the side the other trusts holds the memory at each boundary, the driver for the stack and the stack for its clients;
a datagram is copied once on its way in and twice on its way out, or once if the MAC gathers,
where the Wi-Fi system copied it twice in and three times out, through channels.
It lends both boundaries now, and copies as many times still; `TODO.md`.

**Room is not news.**
Both sides do the same four things: take a buffer to fill, hand it over with its bytes,
take the next one handed over with bytes, and give that back once its bytes are used.
A buffer handed over with bytes wakes the other side, unless a buffer it was woken for is still unread;
a buffer given back empty is room, which the library takes in as the side next passes it,
and which wakes the other side only if that side asked for room, by finding none to take.
Lending made every buffer's return a reference of its own, where a channel's reader freed a slot without a word,
and with a wake for each, the Wi-Fi system's driver woke half again as often as with channels, each wake a wait and two switches.
A side that asks writes its word before it looks at its ring a last time,
and a side that gives room back writes the reference before it reads the word, so one of the two sees the other.
The words lie in the block both write, so a borrower can cost its lender wakes, as it can with bytes,
or spare it some it asked for, which costs the borrower its own: the lender takes the room in on its next wake for anything else,
and serves its other connections meanwhile.
The parent lends the borrower every buffer to fill as it connects it, so neither side does anything to begin.

**Neither side waits for the other.**
A side takes for each wake no more references than the other's ring held as its turn began, a ring's worth at most,
and leaves the rest for its next wake,
so a peer that keeps writing, or makes its ring seem to hold a billion, keeps it from nothing else.
A lender lends what it has and keeps what it needs for itself, a buffer for frames it reads itself, say;
a borrower that keeps what it was lent has less lent to it, and the lender drops what it cannot lend.
Each ring has two slots for every buffer, so an honest side never finds the other's ring even half full.

**A bus master answers to its driver alone.**
A device that reaches memory by itself reaches all of it, PMP or not,
and its driver is trusted as the root task is, open decision 13.
Its connections are as safe as its own checks:
it hands its device only buffers its end, which its parent wrote, says are the connection's,
and once a connection is closed it stops the device using them before it says it let go,
since the kernel takes a block back from processes, never from a device.

**What it costs.**
A lender spends one region on the connections it was made with,
and a borrower one, or two if it is lent buffers to read.
A buffer lent and handed back between two processes costs about what 64 bytes there and back through a channel cost,
whatever its size, the difference being the checks each side makes of what it is handed,
so lending pays from a few hundred bytes a message; `make bench` holds `bulk` beside `channel` and `channel-1k`.
Each ring holding a slot for every buffer twice over costs sixteen bytes a slot.

**Weighed and left.**
- **Copies by the kernel**, as Hubris's leases: decision 34.
- **A region for each buffer**, as Xen's grant tables map pages: above.
- **One block both write**, as a channel's: the borrower could change a frame its driver has yet to parse.
- **The borrower's memory for what it writes**: the lender reading a block of the borrower's data
  would cost each side one region, rather than the borrower two,
  but a borrower taken down would unmap it from the lender, which would fault at its next touch
  unless the parent first waited for its word.
- **seL4's device driver framework** passes buffers of a shared region through a free and an active queue each way,
  and copies between a driver's region and each client's in a component of its own;
  `bulk.h` keeps a ring each way, the lender's free buffers in its page, which no borrower writes, rather than in a queue.
