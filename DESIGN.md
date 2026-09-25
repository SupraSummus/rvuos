# rvuos design

This document records the architecture of rvuos
and the reasoning behind each decision.
Code that disagrees with it is a bug in one of the two.
Work items are in `TODO.md`.

## Goals

rvuos is a microkernel for RISC-V microcontrollers.
It has four goals, in priority order.

1. **Isolation without an MMU.**
   Processes cannot read, write, or execute each other's memory,
   nor the kernel's memory,
   on cores that provide only machine mode, user mode,
   and Physical Memory Protection.
2. **Capability-based authority.**
   Every operation a process can perform is named by a capability
   the process holds.
   There is no ambient authority:
   no global thread identifiers, no global device names,
   no operation that succeeds merely because the caller exists.
3. **The kernel never allocates.**
   The kernel has no heap and no dynamic pools of its own.
   All kernel objects live in memory that userspace explicitly
   handed to the kernel for that purpose.
   A process that creates many kernel objects pays for them
   with its own memory.
4. **Bounded work.**
   A system call, a tick or an interrupt costs what constants of the machine allow,
   however many objects anyone has created.
   The kernel does not meet this yet; see "Bounded work".

## Programs are plain binaries

A program's only dependency on rvuos is the register ABI
and the initial state its creator hands it:
a program counter, a stack pointer,
regions installed in its slots,
and capabilities placed in its table.
The kernel knows no executable format, no header, no runtime library.
A binary built with any toolchain against `include/rvuos/abi.h`,
or with no header at all and the right `ecall` sequences,
is a valid task.

This is a deliberate contrast with TockOS,
where applications are built with libtock,
wrapped in the Tock binary format, and loaded by the kernel.
In rvuos, loading is a userspace job:
a loader task holds memory and capabilities,
carves regions, copies or maps the program image,
installs the regions in a new process with the rights it decides,
and starts a thread at the entry point.
Which capabilities sit in which slots when a program starts
is a convention between loader and program,
not something the kernel enforces;
the `BOOT_CAP_*` slots are the kernel's own instance of that convention
for the one program it starts itself.
It follows that a program dropped onto a board must either be
position independent or be linked for the address the loader chooses,
which is open decision 3 below.

## Non-goals

- Virtual memory, paging, or address translation.
  All addresses are physical.
- POSIX compatibility.
  There is no `fork`, no file descriptors, no signals.
- Binary compatibility with seL4, L4, or F9.
- Multicore, in the first iteration.
  The design must not preclude it,
  but nothing is built for it yet.
- Formal verification.
  The design borrows from seL4 where it makes verification easier,
  because those choices also make the kernel simpler,
  but verification itself is out of scope.
- Code loaded into the kernel at run time.
  Machine mode is not subject to PMP,
  so a capability to load a module would be a capability to be the kernel,
  which no rights bits can narrow and no self-check can follow;
  it would also hand goals 1 to 3 to whoever holds it.
  What varies per board is chosen at link time,
  the files of `kernel/board/<board>/`,
  and every line of machine-mode code stays in the repository
  where the host build and the fuzzer can see it.
  What varies per application runs in user mode,
  the logger among it; see "The kernel log".

## Prior art and how rvuos differs

**seL4** is the reference point for goals 2 and 3.
seL4 retypes `Untyped` memory into kernel objects
and tracks derivations in a capability derivation tree
so that revoking a parent destroys every child.
That tree is most of seL4's complexity.
rvuos keeps the "kernel never allocates" principle
and keeps a derivation tree, in a simpler shape:
three links per slot, no bookkeeping per object,
and a revoke that walks only what it takes;
see "The derivation tree".
Kernel memory comes back at pool granularity,
and the pools form a tree of their own by who created them,
one pointer per pool; see "Kernel pools and revocation".
seL4 also assumes an MMU and page-granular mapping;
rvuos has a handful of protection regions per process instead.

**F9** (`https://github.com/f9micro/f9-kernel`) is an L4-style kernel
for ARM Cortex-M whose eight MPU regions are the same budget rvuos faces.
It addresses threads by global identifiers, which is ambient authority,
and sizes its kernel objects at compile time;
rvuos rejects both.

**TockOS** runs isolated processes on Cortex-M and RISC-V microcontrollers
using the MPU or PMP, and is written in Rust.
Its "grant" mechanism lets kernel components allocate per-process state
inside the process's own memory region.
That is the same insight as seL4's retyping,
without the derivation tree.
rvuos adopts this shape for kernel object memory.
Tock is not capability-based in the seL4 sense:
processes talk to kernel drivers by driver number,
not by held capability.

## Hardware model

### Privilege modes

The kernel runs in machine mode.
Processes run in user mode.
Supervisor mode is not used even when the core has it,
so the same kernel runs on cores that lack it.

All traps, including interrupts, land in machine mode.
The kernel is the only code that runs in machine mode.
The trap vector is in vectored mode on every board,
a table whose every entry jumps to the one entry point,
because the ESP32-C6 has no direct mode.

### Physical Memory Protection

Isolation rests entirely on PMP.
PMP is a small set of CSR pairs, `pmpaddrN` and `pmpcfgN`,
each describing a physical address range
and the read, write, and execute permissions that apply to it
while the core is in user mode.
Machine mode is not subject to PMP
unless an entry is locked,
and rvuos does not lock entries.

Consequences that shape the design:

- **The entry count is a hard, small budget.**
  The privileged specification allows up to 16 entries in version 1.11
  and up to 64 in version 1.12.
  Real microcontrollers ship with 4 to 16.
  A process therefore has a small fixed number of *region slots*,
  not a page table.
- **Every region is a NAPOT block.**
  A region's size is a power of two and its base a multiple of its size,
  which is what one NAPOT entry describes.
  A region costs one entry wherever it lies,
  so an install fits while the process has fewer regions than the core has entries,
  and two regions are either disjoint or one contains the other.
  The price is rounding: a range that is not a power of two
  is rounded up or made of several blocks, one slot each.
  Open decision 8 records why this replaced TOR.
  The ESP32-C6 faults user fetches under a NAPOT entry for the whole address space,
  which rvuos never writes, since no region is larger than RAM.
- **The smallest region is eight bytes, or the grain.**
  A platform rounds every PMP boundary to a grain of `2^(G+2)` bytes,
  one value per hart, and ignores the address bits below it.
  QEMU and the ESP32-C6 have four bytes, RP2350 has 32.
  The smallest NAPOT block is eight bytes; four would need NA4, which rvuos does not use.
  The kernel probes the grain at boot
  and a `Region` capability that is not a block of at least that size never exists:
  the boot layout is checked once and carving requires it,
  so every capability is exactly one PMP entry.
  Whether it may be installed at all still depends on pools
  that capabilities elsewhere made over the same range;
  open decision 14 is about that.
  `OP_REGION_INFO` returns the smallest region, since user mode cannot read the PMP CSRs.
- **An access must lie within one entry.**
  The lowest-numbered entry that matches any byte of an access decides,
  and it must match every byte or the access fails,
  whatever either entry grants.
  Two regions that touch share a boundary
  that no single load, store or instruction fetch may cross;
  the creator lays out a process so that nothing straddles one.
- **The CSRs are WARL and may be hardwired.**
  RP2350 fixes three entries to its ROM and peripheral ranges.
  The probe at boot writes each entry's address and mode and reads them back,
  and the budget is the leading run of entries that take both.
- **Context switch cost is the PMP reload.**
  Switching processes rewrites every `pmpaddr` and `pmpcfg` in use.
  Threads within one process share regions
  and switching between them touches no PMP state.
- **Sharing is free.**
  Granting a region to another process installs the same physical range
  into one of that process's slots.
  Bulk data never passes through a kernel copy.
- **The kernel must validate every user pointer.**
  Machine mode bypasses PMP,
  so a bad pointer from userspace would let the kernel
  read or write anything.
  rvuos avoids the problem rather than solving it:
  system call arguments travel in registers,
  no operation takes a pointer,
  and no operation moves data between processes,
  so the kernel never dereferences an address userspace chose.
  The one place it writes user memory at all
  is a range that becomes a pool, where it writes its objects,
  and that range belongs to the kernel from that moment on.

### Timer

The machine timer, `mtime` and `mtimecmp`, is the kernel's clock
and the kernel handles it directly.
Userspace sees it only through the timer lines,
whose deadlines are counted in ticks; see "Time".
Where the registers live and how fast they count is the board's business,
so `timer.c` is per board, as `irq.c` is.
On every tick the kernel sets `mtimecmp` a period ahead of `mtime`,
not ahead of the previous compare value,
so a long system call costs one late tick and not a burst of them.
QEMU `virt` counts at 10 MHz in a SiFive CLINT.
The ESP32-C6 has a CLINT of Espressif's,
whose counter and interrupt stay off until a control word starts them,
and which counts at the CPU clock the ROM left;
the kernel measures one tick of it against the 16 MHz system timer at boot
rather than trust a clock it did not set.

A process reads the counter through a read-only region at `COUNTER_ADDR`, not with `rdtime`:
the ESP32-C6 has no `time` CSR, but its CLINT has `UTIME`, a read-only copy of `mtime` for user mode.
On QEMU the kernel clears `mcounteren` at boot, so `rdtime` traps on every board; see "Time".

### Interrupt controller

Device interrupts reach the core as one signal, the machine external interrupt,
and an interrupt controller in front of it says which line it was.
The kernel owns the controller as it owns the timer:
it masks every line at boot,
and a line is unmasked only while an `Irq` object is armed on it; see "Interrupts".
The controller is the board's business, so `kernel/irq.c` is per board too.
QEMU `virt` has a PLIC: a line is claimed by reading a register,
which hands over the highest-priority pending line and holds it,
and completed by writing the line back.
The kernel masks the line between the two,
so a level that stays high, as a device's does until it is serviced,
does not come back before the driver asks for it.
The kernel gives every line the same priority,
so which of several pending lines is claimed first is not promised.
The PLIC has no source 0, and the kernel gives that number to its log,
whose line no controller raises; see "The kernel log".
QEMU's PLIC model does not recompute what it forwards on an enable write;
its `irq.c` says what it does about that.

The ESP32-C6 has no PLIC in that sense.
Each peripheral drives a source of the interrupt matrix,
the matrix routes a source to one of 31 CPU interrupts or to none,
and the core takes CPU interrupt `n` with mcause `n`.
rvuos makes a line a source, numbered as Espressif numbers them,
routes every unmasked line to one CPU interrupt, 2,
and masks a line by routing it nowhere.
A claim reads the matrix's status words, which show every source's level,
for the lowest line that is high and routed;
nothing is held between claim and completion,
since masking the line already lowers the CPU interrupt.
Source 0, the Wi-Fi MAC's, is shadowed by the log's line and cannot be bound.

### One address space

There is a single physical address space shared by everything.
Processes are linked at fixed addresses chosen by whoever builds the image,
or built as position-independent code.
RISC-V code is naturally PC-relative,
so position independence costs little,
and the root task can place processes wherever memory is free.

Code normally executes in place from flash.
Data, stacks, and shared regions live in SRAM.
A process's regions are typically:
a read-execute code region in flash,
a read-write data region in SRAM,
and zero or more shared or device regions.

## Kernel objects and capabilities

### Capabilities

A capability is a kernel-held reference to a kernel object
together with a set of rights.
Userspace never sees a capability's contents.
It names capabilities by index into its process's capability table,
and the kernel validates the index, the object type, and the rights
on every use.

Capabilities can be copied or derived into another process's table,
optionally with reduced rights,
by a process that holds both the source capability
and a capability to the destination table.
The two differ in one thing:
what a later revoke of the source takes.
A copy stands beside its source and is as good as it;
a derivation stands below it and goes when the source is revoked below;
see "The derivation tree".

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
A `Region` capability has no object behind it:
its two words are the base address and the size,
and the rights are the memory permissions it may grant.
Carving a sub-region is therefore a pure table operation
that touches no kernel memory,
which is what lets the root task hand out memory
before it has created a single pool.
An `IrqLine` capability has the same shape for the same reason:
its words are the first line and the count, there is no object,
and the root task hands lines out as it hands out memory.
A `Clock` capability, like `Debug`, has neither object nor content:
there is one counter on the machine.

### Object types

| Object | Purpose |
|---|---|
| `Region` | A physical address range with maximum rights. Installed into a process's PMP slots. |
| `KernelPool` | A region handed to the kernel. All other objects are allocated from pools. |
| `CapTable` | A process's capability table. Allocated from a pool. |
| `Process` | A protection domain: a capability to its `CapTable`, a set of region slots, and its threads. |
| `Thread` | An execution context inside a process: its registers and its state. |
| `Notification` | A word of sticky signal bits. The only way a thread can stop and be started again. |
| `IrqLine` | A range of interrupt lines: the log's and the controller's, or the timer lines. Bound one at a time into an `Irq`. |
| `Irq` | One line bound to a notification. Signals it when the line fires. |
| `Debug` | A byte into the kernel's log, and machine halt, for bring-up and tests. No object. |
| `Clock` | The machine's counter: its rate, and the one region it can be read through. No object. |

Every object's size follows from its type,
a `CapTable` from its slot count,
so a pool can be a bump allocator
whose contents can be walked without a free list.

### Kernel pools and revocation

A process turns a `Region` into a `KernelPool` by a system call.
From that moment the range belongs to the kernel.
The call fails if the range overlaps an existing pool
or a region currently installed in any process,
so user mode can never hold a mapping to memory
that carries kernel objects.
The mirror check applies when installing a region:
a range that overlaps a pool cannot be installed.
Both checks are linear scans over pools and processes,
which was thought fine for the object counts a microcontroller can hold.
Goal 4 rules them out,
and open decision 14 is what would replace them.
The `Region` capability that was consumed is revoked,
with everything derived from it,
and the `KernelPool` capability takes its place in the derivation tree,
so whoever could revoke the region can revoke the pool capability.
Copies beside the region and the regions above it survive
and fail the overlap check while the pool exists.
The kernel zeroes each object as it allocates it, not the whole region.

The pool's own descriptor is the first object in the pool's memory,
so the kernel keeps no per-pool state outside the pool
except a list head for the overlap scans.
Objects are bump-allocated behind it, eight-byte aligned,
never freed individually, and each records its pool.

Kernel memory comes back at pool granularity.
Objects are never freed individually,
so a pool is the unit of trust for the memory behind objects:
revoking the capabilities to an object leaves the object in its pool,
unreachable, until the pool is destroyed.
Capabilities themselves are revoked one derivation at a time;
see "The derivation tree".

Pools form a tree.
A pool's parent is the pool the thread that created it lives in,
and destroying a pool destroys every pool below it.
This is for the process that lends memory.
Without the tree, a child that turned lent memory into a pool
would outlive the parent's destroy of the child's pool,
threads running and all,
and the memory would stay inert to the parent's region capability forever,
because a pool cannot be built over a pool.
The boot pool is the root.

Capabilities must not dangle after a destroy,
so a destroy clears every capability that names an object in the pool,
by walking every capability table in every pool.
The sweep is exact rather than best effort,
because there is no capability the kernel cannot enumerate:
every capability lives in a `CapTable`,
every `CapTable` is an object in a pool,
and every pool is on the list.
It has the same shape as `pool_overlaps` and `installed_overlaps`
and costs the same, once for every pool the destroy takes.
The derivation tree adds a second thing the sweep takes:
every slot the dying pools held is revoked with everything derived from it,
so a grant dies with the table it was made through,
and no ring in a surviving table leads into memory that is user memory again.
Links cross pools in both directions,
so the sweep runs over every dying pool before any of them is zeroed.

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

Objects reference each other in two ways,
and destroy must cope with both.
A capability names an object, and the sweep clears it.
A process holds its table this way;
see "A process's table".
A structural pointer, such as a thread to its process,
is not checked on use,
so a structural parent must live in the same pool as its child
or outlive it.
The kernel enforces the same-pool rule at allocation:
a `Thread` must be allocated from the pool its `Process` lies in,
and an `Irq` from the pool its `Notification` lies in.
A thread waiting on a notification inside a destroyed pool
is woken with `KERR_INVALID_CAP` and no bits,
because the wait can no longer be answered.

A destroy refuses with `KERR_STATE`
when the calling thread lives in the pool or in one below it,
or its process's table does.
A thread and its process share one pool by the rule above,
so those two checks are the whole of it:
the caller keeps what it runs on and the table the memory comes back to.
The first is also why the boot pool can never be destroyed:
every pool lies below it.
Its objects are zeroed on the way out,
because the memory is about to be user memory again
and they hold other processes' capability tables.
The rest comes back as it went in,
so a lender clears a region before it lends it if the borrower should not read it.
It comes back as a `Region` capability
with the rights the region carried when it became a pool,
which the pool records for that purpose,
so a destroy cannot manufacture a right the memory never had.
The pools below give their memory back to nobody in particular:
region capabilities for it that survived the sweep
pass the overlap check again,
and the derivation tree says which those are;
see decision 7 under "Open decisions".
The pool list has the newest pool first and a child is newer than its parent,
so one walk of the list meets every pool below the destroyed one before it,
and no parent read on the way up has been zeroed yet.

### The derivation tree

The pool tree cannot do what seL4's derivation tree can:
take back one region from a child, and everything the child did with it,
while the child goes on living.
A derivation tree over slots does that,
and rvuos keeps one in the shape that costs the least.

**What is a node.**
Every filled slot of every capability table,
every region installed in a process,
which is a slot of the same layout living in the `Process`,
and every process's table slot, which lives there too;
see `kernel/object.h`.
Roots are the boot capabilities and every capability to a new object,
since an object owes nothing to the pool capability it was allocated through.

**What hangs below what.**
A carve hangs below the region or line it was carved from,
a derivation below its source,
an installed region below the capability it was installed from,
the counter's region below the clock it was derived through,
and a process's table slot below the `CapTable` capability the process was made with.
A copy stands beside its source, under the same parent,
so a process can duplicate what it holds without making one copy the master of the other;
beside a root it is a root.
A conversion takes the converted slot's place:
the pool capability of `OP_REGION_TO_POOL` and the `Irq` capability of `OP_IRQ_BIND`
hang where the region or the line hung,
and the consumed slot goes with everything derived from it.
The region a destroy gives back hangs where the pool capability hung,
or rather below the nearest ancestor that is not a capability to the pool,
since those the destroy clears;
when that ancestor went with the destroy, the region comes back as a root.

**What the operations do to it.**
`OP_CAP_REVOKE` clears everything below a slot and leaves the slot.
`OP_CAP_DELETE` and `OP_PROCESS_UNINSTALL` clear one node
and hand what was below it to its parent,
so deleting one's own copy of a grant does not take the grant back.
A pool destroy revokes every slot the dying pools held,
so the derivation a process handed out dies with the process;
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
A delete puts the node's children right after it, through the first and the last,
and then takes the node out as a leaf;
a conversion puts its result beside the consumed slot and takes the slot out.
A revoke is deletes of the first child, one after another,
each handing its children up to the front of the ring,
until the node it revokes below has none:
a step per node, with no stack,
the tree whole between any two steps,
and the next step always at the node's first child.
Only a pool destroy still walks a ring, to find a node's parent.
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
the pool does that, and a pool whose capability was revoked
stays until the pool it lies below is destroyed.
A root has no parent to revoke it from:
what a process allocates it holds until the pool goes.
A capability copied beside its source can be taken back only from their common parent,
which is the point of copying rather than deriving.
An earlier sketch by the same author,
`os4cm4` in the LANoT repository,
put three links next to the object pointer in a sixteen-byte slot
and never got as far as maintaining them;
what made this one tractable was making installed regions nodes,
so that a revoke unmaps by derivation and never by address range,
and letting a destroy revoke the dying slots' derivation
instead of reparenting it across tables.

### A process's table

A `Process` holds its `CapTable` by a capability in a slot of its own,
not by a pointer.
Allocating the process fills the slot from the `CapTable` capability it names,
below that capability in the derivation tree.
No system call names the slot: a process's slot numbers index its table.

A revoke above the slot, or the destroy of the table's pool, clears it like any other,
so the table may lie in any pool, and several processes may share one.
A process whose slot is empty fails every call with `KERR_INVALID_CAP`.
Within one call only a revoke, which then returns,
or a destroy, which keeps the table it found, can take it.
It costs sixteen bytes per process and a test on every call;
before, the table had to lie in the process's own pool so a pointer could not dangle.
Open decision 15 is about what else the slot could do.

### Region slots

A `Process` has a fixed array of eight region slots.
Installing a `Region` capability into a slot
requires the region capability
and a `Process` capability with the write right.
The kernel recomputes and caches the PMP register image
for the process at that point,
not on every context switch.
The installed region hangs below the capability it was installed from
in the derivation tree, so revoking below that capability unmaps it,
whichever process it was installed in.

Rights on the installed slot are the intersection
of the region capability's rights
and the rights requested at install time.
Installed regions in one process may not overlap,
because PMP resolves overlaps by entry number
and the result would depend on slot order rather than policy.
A region cannot be installed writable but not readable:
PMP reserves the encoding R=0, W=1,
QEMU quietly drops the write right,
and hardware may do anything.

How a process's memory is laid out is entirely the creator's business.
The kernel does not know what a code segment or a stack is;
the root task carves regions out of the memory it owns,
installs them into the child's slots with the rights it chooses,
and starts a thread at whatever entry point and stack pointer it likes.

### Threads

A `Thread` is an execution context inside a process:
a register frame, a state, and the process it belongs to.
Creating one takes a `Process` capability with the write right,
and the new thread is allocated from that process's pool,
as the same-pool rule requires.

A thread is either stopped or ready.
`OP_THREAD_CONFIGURE` sets a stopped thread's program counter
and stack pointer, and `OP_THREAD_RESUME` makes it ready.
The kernel validates neither value:
it knows no executable format and no calling convention,
so a thread that starts nowhere useful faults,
which is its creator's business.

A fault stops the machine, even when another thread could run.
Telling a thread's creator instead would need somewhere to tell it,
and nothing in the object model is that somewhere yet;
`TODO.md` carries the item.

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

## Time

**There is no sleep.**
A thread that wants to stop for a while wants to stop until something happens,
and a notification is the one way to stop;
what is missing is only something that happens at a time.
So time reaches userspace as one more interrupt, on a timer line.
The kernel has `TIMER_LINES` of them, numbered after the controller's,
and the tick raises them where a device would raise its own.
An `Irq` is bound to a timer line exactly as to a device's line, with `OP_IRQ_BIND`,
armed with `OP_IRQ_SET` for a set of bits and a delay,
and signals those bits when the delay has passed.
Sleeping is arming a timer line and waiting on its notification.
A wait with a timeout is the same two calls,
because a notification is a word of bits:
a driver gives the device one bit and the timer line another.
No operation takes a deadline and no thread state is a sleep,
so "a waiting thread names a live notification" stays the whole story.
Yield is open decision 10.

**The contract is a lower bound.**
The delay is in microseconds.
The call lands anywhere within a tick,
so the kernel fires at the first tick that surely lies past the delay:
the delay rounded up to whole ticks, plus one.
The tick's period is the board's business and not part of the ABI.
An upper bound is not promised because the kernel could not keep one:
the woken thread is ready, not running, and waits its turn in the round.
A timer line that fires disarms its `Irq`, as a device's line does,
and setting an armed one moves its deadline rather than adding a second.
A periodic task arms the line again each time it wakes;
what that costs it in drift is open decision 16.

**Why a fixed number of lines.**
The tick has to find the due timers without looking at the others.
Timers as objects in pools would have it walk every pool, which goal 4 rules out;
a sorted queue moves that walk to the set, and a wheel to the bucket that comes due.
A limit per process, thread or pool bounds nothing, since anyone can make more of those.
A limit on the whole machine does: the tick looks at the `TIMER_LINES` lines and nothing else.
Lines make it a limit of authority rather than of who comes first:
the root task receives them all in `BOOT_CAP_TIMER_LINES`,
apart from the controller's so that no board's count shows,
and hands them out and takes them back as it does memory.
The count is a constant of the kernel, like the region slots,
while the `Irq`s bound to the lines live in their holders' pools.
A program that needs more timers than it holds keeps its own deadlines
and arms one line for the nearest.

**Why an interrupt.**
A deadline argument on `OP_NOTIFY_WAIT` would be smaller,
but it would make time a special case of waiting
rather than a signal like an interrupt.
A time server in userspace, a driver holding an `Irq` for a second hardware timer,
would cost the kernel nothing and remains the right shape for a board that has one;
the timer lines are what make a sleep one system call
rather than a round trip to that server.

**Reading the time.**
A timer line says that a delay has passed, not what time it is.
The `Clock` capability gives the time:
`OP_CLOCK_INFO` returns the counter's rate and address,
and `OP_CLOCK_REGION` derives a read-only `Region` holding the counter, below the clock.
A process with that region installed reads the time with loads, without a trap.
The rate is the board's, and on the ESP32-C6 measured at boot, so a program takes it from the clock.
The region is the smallest block holding the counter's two words;
on a PMP grain coarser than eight bytes it also shows the timer registers beside them, read only.

**Why a capability.**
The time is authority, and goal 2 allows no ambient authority:
a process given no clock and no timer line cannot tell time passing,
unless it builds a clock from a second thread or learns the time from someone who has one.
Gating `rdtime` would have needed `mcounteren` switched per process, and the ESP32-C6 has neither.
A region already carries rights, derivation and revoke, so the clock adds one type and no object.
The rate alone tells nothing about time, so it has no capability of its own.

## Scheduling

**The kernel keeps a run queue.**
A ready thread other than the running one waits for the processor on the run queue,
a ring through the thread's own `queue_next` and `queue_prev`.
A waiting thread is on its notification's waiters through the same two words,
and no thread is on both, so the queue costs no memory.
The next thread to run is the oldest on the queue,
found in constant time however many objects exist;
the walk over the pools that found it before broke goal 4.

**The queue is the policy: round-robin.**
A thread that becomes ready, resumed, woken or preempted,
joins the back of the queue,
so ready threads take turns in the order they became ready,
and the only scheduling state the kernel holds
is the queue and which thread is running.

**The machine timer provides the tick.**
A thread runs until it waits on a notification
or until the tick takes the processor from it,
and either way the oldest thread on the run queue gets it.
The tick has a fixed period, `TIMER_HZ` in `kernel/timer.h`,
and one tick is one slice: a preempted thread goes to the back of the round.
Interrupts are taken in user mode only:
machine mode runs with `MIE` clear from the trap to the `mret`,
so a system call is never interrupted and the kernel needs no locks.

When a thread waits and nothing is runnable,
only an `Irq` armed on a timer line or a device's line can make one runnable again,
because only a running thread can signal otherwise.
The kernel counts those `Irq`s as they are armed and disarmed,
so it knows without a walk.
With either armed the kernel stalls in `wfi`
until the tick or a device interrupt is pending,
takes it by hand since machine mode runs with `MIE` clear,
and looks for a runnable thread again;
with neither it says `no runnable thread` and stops the machine.

**Nothing more lives in the kernel.**
Round-robin on a tick is the least policy that makes
a spinning thread harmless and a woken thread eventually run.
Fixed priorities were the intended end state of this section
and are now open decision 9.

## Interrupts

**An interrupt is a signal, as a tick is.**
The kernel owns the trap vector and the interrupt controller,
and no driver code runs in machine mode.
A driver holds an `IrqLine` capability naming one line,
binds it with `OP_IRQ_BIND` to a notification in a pool of its choosing,
which makes an `Irq` object there,
arms the `Irq` with `OP_IRQ_SET` for the bits it wants,
and waits on the notification.
When the line fires, the kernel masks it, disarms the `Irq`
and signals the bits, then returns to whatever was running;
the driver runs when its turn comes, as a thread a timer line woke does.
Having serviced the device, the driver arms the `Irq` again,
which is what unmasks the line.
A timer line takes the same `Irq`:
bound at creation to a notification in its own pool,
armed with bits and a delay, disarmed by signalling,
so a driver that gives the device one bit and a timer line another
has a wait with a timeout for the same two calls; see "Time".
The machine timer is the one interrupt the kernel keeps for itself,
because the tick is its own; see "Scheduling".

**Armed and unmasked are one state.**
The controller forwards a line exactly while an `Irq` is armed on it,
which the self-check reads back from the controller line by line.
A line the kernel claims therefore always has an `Irq` to signal,
and a device whose level stays high, as a UART's does until it is serviced,
costs one trap and not a storm:
the mask outlasts the claim, and only the driver lifts it.
`OP_IRQ_SET` with no bits masks the line by hand,
and on a timer line cancels the delay.

**Lines are authority, and the root task holds them all.**
Interrupt lines are hardware, not kernel memory,
so an `IrqLine` capability names no object, as a `Region` names none:
the root task receives every line of the controller at boot, and every timer line,
carves single lines out with `OP_IRQ_CARVE`, a table operation,
and hands them to drivers the way it hands out memory.
Binding consumes the invoked slot, as turning a region into a pool does,
and copies of the line elsewhere are inert while the `Irq` exists,
because a line is bound at most once and a second bind fails with `KERR_OVERLAP`.
When the `Irq`'s pool is destroyed the kernel masks its line
and the object is gone, so those copies work again:
the lender of a line gets it back by destroying the borrower's pool,
or by revoking below the line capability it derived the borrower's from,
exactly as the lender of memory does.

**One `Irq` per line.**
Sharing a line between drivers is open decision 11;
what the kernel does today is refuse the second binding.
The kernel records the bound `Irq` of each line in a table of its own,
a word per line of the controller and per timer line,
so an interrupt or the tick finds its `Irq` without a walk.

**No driver in the kernel.**
The UART is the first device, and it is userspace's alone:
the root task is granted its registers and its line at boot,
and `user/init.c` drives the transmitter from user mode on its interrupt.
The kernel has no console to share it with.
What the kernel has to say goes into its log,
and the log raises a line of its own; see "The kernel log".

## The kernel log

**The kernel has no console.**
Where a kernel's messages go is the application's business:
a UART on one board, USB or a radio on another,
nowhere at all on a device in the field.
Each of those is a driver, and no driver runs in the kernel.
So the kernel writes into a ring of bytes in its own memory,
`KLOG_SIZE` of them behind a header at `KLOG_BASE`, and stops there.
`kputc` appends a byte and moves the head, a count of every byte ever written.
The kernel never waits for a reader:
one more than a ring behind loses the oldest bytes and can tell,
because the head has run past its own count by more than the size.
`OP_DEBUG_PUTC` appends to the same ring,
so a program's output and the kernel's keep their order.

**The reader is granted the ring, and a word in its header.**
`BOOT_CAP_LOG` names the header and the ring,
so the root task installs it as it installs a device's registers
and reads the head and the bytes without a system call.
The header also holds `taken`, the reader's count of what it has read,
which is the one word of kernel memory user mode writes:
the kernel reads it only to tell whether the line is high
and how much of the ring a halt has left to write out,
and a count past the head reads as the head.
It is kernel memory that user mode can see,
which the isolation properties otherwise forbid;
it holds no object and no capability, only what the kernel chose to say,
and `OP_REGION_TO_POOL` refuses the range, since the kernel writes there on its own.
The header and the ring are one 4 KiB block at the top of the kernel's RAM,
right below the root task's code,
so the ring holds the block less its header.

**The log is a device, and its line is line 0.**
A reader that has taken everything wants to stop until there is more,
and stopping is a notification, which a device reaches through an `Irq`.
So the log has an interrupt line, `LOG_IRQ_LINE`, the number the controller does not use,
and the root task receives it in `BOOT_CAP_IRQ_LINES` with the controller's lines,
carves it out, binds it and arms it as it would a UART's.
The line is level: high while the head lies past `taken`,
so the reader acknowledges by writing the header, as it would a device's register.
Arming the `Irq` while the line is high signals at once,
so a byte written between the reader's last look and its arm is not missed,
and a byte written while the `Irq` is armed signals as it lands.
Either way the `Irq` is disarmed by signalling, as a device's is,
and the rest of a burst wakes nobody until the reader arms again.
Nothing else is new: binding, the pool rules, revocation by pool destroy
and one `Irq` per line hold for line 0 as for line 10,
and `klog.c` does for the log's line what the controller does for a device's,
with the head for the level and no enable bit to write.

**Two things the log's line is not.**
It is not a source that can wake the machine:
only the kernel raises it, and the kernel runs only when a thread,
the tick or a device makes it,
so a kernel with nothing runnable and only the log's `Irq` armed
stops with `no runnable thread` rather than stall in `wfi`.
And it is not a line a replay can fire by name:
`OP_DEBUG_IRQ` refuses it, since its level is the log's and not the record's.
A replay reaches it all the same, because the trace is bytes into the log
and the trace of the very call that arms the `Irq` is what raises the line;
`tests/seeds/log-wakes-reader` rests on that.

**What a halt does with the log is the board's business.**
A board that resets leaves the ring in RAM,
where a logger after the reset can find the last words;
what it needs to trust them is open decision 12.
QEMU virt exits on a halt and keeps no RAM,
so its `khalt` in `kernel/board/qemu/halt.c` is the post-mortem reader:
it writes what no reader has taken to the UART under a line that says the log follows,
so that a transcript tells what a logger carried out from what the halt salvaged.
That is the one place the kernel touches the UART.
The ESP32-C6 parks its core on a halt, which keeps RAM,
and its `khalt` writes the same dump by polling to the USB Serial/JTAG console,
then a line with the code a simulator would have exited with,
so that the same transcript check runs on the board;
a console no host reads is given up on rather than waited for.
The ring is just under 4 KiB, a burst's worth between two looks by a reader,
and a reader that looks less often loses the oldest bytes and can tell.

## Boot

The image contains the kernel followed by the root task.
How it reaches RAM is the board's; see "Boards".
The kernel:

1. makes the board ready, which on the ESP32-C6 means watchdogs off,
   and sets up its own stack and trap vector,
2. discovers the PMP entry count and grain by writing the CSRs and reading them back,
3. carves its own static state out of a small fixed SRAM range,
4. constructs the root process by hand, including a `KernelPool`
   in a range the linker reserves,
5. hands the root task capabilities to the block of RAM the board sets aside for it,
   to the UART's registers,
   to the kernel's log,
   to the machine's counter as a `Clock`,
   and to every line of the interrupt controller with the log's line before them
   (later also flash and other device ranges),
6. programs the tick, masks every interrupt line,
   and drops to user mode into the root task.

A device range is granted read and write, never execute,
the counter read only,
and can never become a pool:
`OP_REGION_TO_POOL` refuses a range outside RAM,
because the kernel writes objects into a pool,
which on a device would drive its registers from machine mode.

Every range the root task is granted is a block,
so a board lays RAM out in blocks and what lies in none of them stays unused.

The slots the root task finds filled are listed in `include/rvuos/abi.h`
as the `BOOT_CAP_*` constants.
Slot zero is left empty so that an uninitialised index fails.

Everything after that is policy set by the root task.
The kernel does not know what a driver, a file system,
or a shell is.

## Boards

A board is the files of `kernel/board/<board>/` and `user/board/<board>/`,
chosen with `make BOARD=<board>`:
`board.h`, where RAM, the root task and the console lie,
how many interrupt lines there are and which mcause the controller raises,
which `kernel/layout.h` and both linker scripts read;
`board.c`, what the board needs before anything else;
`timer.c`, `irq.c` and `halt.c`;
and `console.h`, the device behind `BOOT_CAP_UART` as the root task drives it.
Nothing else in the kernel names an address.

**QEMU `virt`** is the development target and the one `make check` runs.
QEMU loads the image and enters it in machine mode.

**ESP32-C6** runs the demo root task and its transcript check, `make test BOARD=esp32c6`.
The chip's ROM loads the image's segments into SRAM over USB and enters it,
so nothing is written to flash; `tools/esp32c6-run.py` drives that.
Everything the image carries must lie below `0x4086ad08`,
where the ROM keeps its buffers while it loads;
the kernel takes all 512 KiB once it runs, laid out as `MANUAL.md` shows.
The console is the chip's USB Serial/JTAG controller,
a CDC-ACM port on its own USB connector.
Before anything else the kernel turns off the four watchdogs the ROM leaves running
and the access permission management units.
Those silently refuse the CPU in user mode every peripheral,
reads returning zero and writes dropped;
ESP-IDF turns them off at startup too.
PMP is what confines a process, so rvuos loses nothing;
what they could still do is open decision 13.
Measured on the chip: sixteen PMP entries, all unlocked after the ROM,
a four-byte grain, TOR, NAPOT with an entry of RAM's size, `mtval` on access faults,
PMA entries all zero, which leave every range its default attributes,
and machine interrupts taken in user mode whatever `mstatus.MIE` says.

## Bounded work

**The rule.**
The work of a system call, a tick or an interrupt
is bounded by constants of the machine and the kernel:
PMP entries, interrupt lines, region slots, the size of an object.
It does not grow with how many objects, pools or capabilities exist.
Otherwise a process with one small pool allocates thousands of minimal objects,
and every tick and every interrupt, other processes' included,
gets that much slower.

**The exception is destruction.**
A revoke, a pool destroy, and a delete below a root
take as long as what they touch.
That is kept to the subtree below the caller's own capability,
never the whole machine.
Such a loop is paid for:
each step takes away something an earlier call made,
so over a run its steps are bounded by the calls before it.
What it costs is latency, and preemption is the answer to that.

**Preemption.**
A revoke, a delete below a root, and the revoke that begins a conversion
take one step per capability in constant time
and ask `intr_pending` between two steps.
If the tick or a device interrupt is pending, the walk stops,
`syscall_dispatch` puts the thread back on its `ecall` with its registers as they were,
the `mret` takes the interrupt through `mtvec` as in any user code,
and the thread makes the same call again when it next runs, as in seL4.
The kernel only notices the interrupt; the processor takes it.
The progress stays in the derivation tree:
the next step is always at the first child of the slot the call names,
so the kernel needs no record of where it stopped, no second stack and no worker,
and the time lands in the slice of the thread that made the call.
A walk asks only after a step, so every attempt takes something away and the restarts end.
Whether `intr_pending` is right changes when a walk stops, never what it does.
A restart is a new call:
it checks everything again, and revokes what was derived in between too.
So a conversion revokes only once every check that can fail has passed,
`OP_IRQ_BIND`'s room in the pool among them, and builds after.
The pool destroy and the other walks of the table below do not stop yet.

**The check.**
Every loop a trap can run says what bounds it with an annotation from `kernel/work.h`:
a constant, what it pays with, a row of the table below, an argument, or a wait on the hardware.
An annotation is a `_Static_assert`, so it changes no code the compiler makes.
Every kernel link runs `tools/loop-bounds.py`,
which finds the loops in the machine code, inlined and compiler-made ones too,
matches each to its source loop through the debug information and clang's AST,
and fails on one that says nothing.
The table is checked both ways, so it stays the kernel's own list of walks.
Code that leads only to a halt is left out,
and so is the self-check, which may be called only under `if (debug_trace)`.

The claims are checked too.
A bound the loop's own shape limits, a counter below a constant, is compared with that limit at the link;
this is the only check of board code, which the host does not run.
Other bounds rest on an invariant, as `i < img->count` does,
and a paid loop names its unit: a node, a link, an object or a waiter.
The host harness `fuzz-work` counts both after every call:
a bound per entry of its loop,
and paid steps at most twice what the call took away of that unit, plus one;
twice since a pool destroy may pass a capability to the pool on its way up to the region
and clear it again in the sweep.
It finds a false claim only where the corpus reaches, and a wait is not checked at all.
The host answers every `intr_pending` with yes, the worst case,
so each preemptible call stops after its first step, is made again,
and the self-check runs between any two steps;
every host harness requires a stopped call to have taken a node or a link away.

**Where the kernel falls short.**

| Walk | When | Fix |
|---|---|---|
| `pool_overlaps` | `OP_PROCESS_INSTALL` | open decision 14 |
| `pool_overlaps`, `installed_overlaps` | `OP_REGION_TO_POOL` | open decision 14 |
| `cap_revoke_range`, `pool_destroy`, `pool_under`, `pool_overlaps`, `cap_parent` | pool destroy | open decision 14, and preemption |

**Zeroing goes with the object.**
`pool_alloc` zeroes each object, at most `OBJ_MAX_SIZE`, a table of `CAPTABLE_MAX_SLOTS`,
and a destroy zeroes each object as it forgets it, paid for by the call that made it.

A pool destroy revokes what the dying pools held with `cap_revoke_below` too,
but does not stop:
its sweep and the pool walks keep no progress a restart could find.

## Bounded stack

The kernel has one stack, `KERNEL_STACK_SIZE` in `kernel/kernel.ld.S`,
and every entry starts it from the top:
the reset, a trap from user mode, and a trap in the kernel, which halts.
Interrupts stay off in the kernel, so no entry nests in another,
and a thread's state lives in its trap frame, not on the stack.
The deepest the stack goes is therefore the heaviest call chain from an entry,
a constant of the image.

**The rule.**
The kernel does not recurse and has no frame of run-time size:
no variable-length array, no `alloca`.
A walk that would recurse walks with no stack, as `cap_revoke_below` does.
A call through a pointer counts as a call to every function whose address is taken.

**The check.**
Every kernel link runs `tools/stack-depth.py`,
which reads the frames from `-fstack-usage` and the calls from the disassembly,
fails on a broken rule or a bound above `KERNEL_STACK_SIZE`,
and prints the bound and its chain.
The kernel is compiled with `-ffunction-sections`,
so the linker drops dead functions,
and a C function the tool sees no call to is a call it could not read,
which fails the link too.
The deepest chain runs through the self-check that `OP_DEBUG_TRACE` turns on,
and the stack is sized just above it;
the check, not headroom, is what keeps it from overflowing.
Nothing guards the stack at run time:
an overflow would silently overwrite `.bss` below it, since PMP does not bind machine mode.

## Properties

These invariants must hold in every state a process can reach.
`kernel/selfcheck.c` is their executable form;
keep the two in step.

**Isolation.**
No region installed in any process overlaps a pool,
so user mode never has a mapping to memory that holds kernel objects.
No installed region lies outside the memory
the root task was granted at boot,
and the granted memory does not overlap the boot pool,
so the kernel's own memory is unreachable,
the log excepted: it holds no object and can never become a pool.

**PMP fidelity.**
For every process, the PMP image the kernel built
and the region slots describe the same function
from address to access rights:
no byte is accessible with a right its slot does not grant,
and every byte in a slot is accessible with the slot's rights.
For the running process the same holds for the CSRs actually written.

**Authority confinement.**
Every capability in every table names either
a region within the granted memory,
a NAPOT block no smaller than the smallest region,
with rights no greater than the root task received for it,
or a range of lines the interrupt controller has, the log's among them,
with no more than the right to bind,
or the debug capability or the clock, which name no object,
or a live kernel object of the capability's own type.
Every process's table slot is empty or names a live table.

**Derivation.**
The slots of every live table
and the table slot and region slots of every live process
are the nodes of one forest.
Every link of a filled node lands on a live, filled node;
an empty node has no links.
The children of a node form one ring that closes through the node,
every node on it linked up to that node,
and every node's previous link names the sibling whose next is the node,
the last sibling for the first;
a root has no siblings and no previous link.
A node is derived from its parent:
the same object with no more rights,
a range within the parent's range with no more rights,
or an object built on the parent's range,
a pool on a region, an `Irq` on a line, an installed region on a region,
or the counter's block, or a region installed from it, below a clock.
A revoke or a delete stopped for an interrupt leaves such a forest too,
since every step of theirs does; see "Bounded work".

**Structural soundness.**
Every pool's descriptor sits at its base,
its objects tile the space from the descriptor to the used mark exactly,
each object records the pool it lies in,
and pools are pairwise disjoint.
Every pool but the boot pool names as its parent a live, older pool,
so parents lead to the boot pool
and no destroy has left a pool below it standing.
A thread and its process lie in the same pool.
The line table names exactly the bound `Irq`s,
and every installed region records its slot.

**Thread state.**
Every thread is stopped, ready, or waiting.
A waiting thread names a live notification and nothing else does,
and a notification's queue holds exactly the threads waiting on it.
The run queue holds exactly the ready threads but the running one,
and a thread on neither queue is linked into none.
The thread the kernel is running is one it could run:
it is a live object and it is ready.
A preempted thread stays ready and joins the run queue,
so it runs again.

**Interrupts.**
Every `Irq` names a live notification in its own pool,
so the link, which is not checked on use, never dangles,
and a line there is: the log's, the controller's or a timer line.
At most one `Irq` is bound to any line.
The controller forwards a line exactly while an `Irq` is armed on it,
read back from the controller itself:
an interrupt masks the line as it disarms the `Irq`,
a set masks or unmasks it with the bits,
and a destroy masks the lines of the `Irq`s it takes.
The count of armed sources is exactly the `Irq`s armed on a device's line or a timer line.

**Timer lines.**
An `Irq` armed on a timer line has its deadline ahead of the tick count:
the tick fires every timer line that is due,
and a line that fires disarms its `Irq`.

**The log.**
An `Irq` armed on the log's line has nothing untaken behind it:
the head is where the reader said it had taken to.
A set with bytes untaken signalled at once,
and a byte that made the line rise signalled as it landed,
so the same "armed and unmasked are one state" holds,
with the head for the controller.

**Memory safety.**
No sequence of system calls makes the kernel read or write
outside its own objects,
nor reach any undefined behaviour.
Any kernel panic reachable from user mode is a bug.

Not on the list yet: "rights only narrow through copies" as a property of history.
The derivation invariant carries its structural form,
no node wider than its parent,
and a copy beside its source is checked against their common parent, not against the source.

## Verification

**Host fuzzing** (`make host-test`, `make fuzz`).
The kernel's logic compiles natively with a shim for `kputc`,
the PMP CSRs and halting, and physical addresses indexing a RAM buffer.
An input is a sequence of events the kernel receives,
not the behaviour of one process:
each record is a system call with the thread that makes it,
and the tick is a record too, `OP_DEBUG_TICK`,
which moves time by one tick and fires the timer lines due,
so the host has a clock that ticks when the input says;
a device interrupt is a record as well, `OP_DEBUG_IRQ`,
which does to an armed `Irq` what the controller would,
so the host has devices that fire when the input says.
libFuzzer feeds the records into the threads' registers
and the self-check runs after every call, under ASan and UBSan.
Since no system call takes a pointer,
the registers are the whole attack surface.
The mutator in `host/mutator.c` works on whole records:
it inserts, deletes, swaps and moves them,
sets one field to a value in its range,
and splices two inputs on record boundaries,
so that a handoff between the driver's threads is one mutation, not a guess per byte.
The inputs replayed are checked in under `tests/seeds` and `tests/corpus`.
A seed is written by hand for a scenario the kernel must handle,
is named after that scenario, and is kept for as long as the scenario exists.
The corpus is what the fuzzer found,
and `make corpus-merge` rebuilds it from scratch each time:
an input stays only while it reaches an edge on some harness
that the seeds and the inputs kept before it do not.
How often an edge is hit guides the fuzzer but keeps no input,
since `make qemu-replay` pays one QEMU boot per input.
Coverage is measured against the kernel as it is,
so an input that was unique for an earlier kernel
and covers nothing new today is dropped rather than kept for its history.
`make mutants` plants bugs in the kernel one at a time,
each a patch under `tests/mutants/` headed by the invariant it breaks,
and requires a seed on a host harness to catch each with an invariant report,
or, for a mutant of the stack, `tools/stack-depth.py` to refuse the link,
and for a mutant of the bounded work, `tools/loop-bounds.py`
or the counting harness `fuzz-work`, whose report is an invariant report too.
An input that some mutant needs thus belongs among the seeds under a name,
since the corpus keeps it only while coverage does.
The header also lists exactly the checks that catch the mutant
and the invariant reports the seeds give on it,
and the run fails where the checks disagree:
a check that stops catching a mutant shows, and so does one that starts to,
or a seed that catches it by another invariant than the one it plants.
The list of checks is also what shows a minimisation that lost something.
The QEMU checks are in the list too, so that their strength is known,
but a mutant only they catch counts as missed:
`make test` runs one scenario with the self-check off
and catches a bug only when the transcript changes;
`make qemu-replay` catches one when the default machine's self-check reports it
or the two traces differ.
QEMU's clock counts instructions, not the host's time,
so a run takes the same path however many run beside it.
`make mutants-refresh` carries the patches over a change in the kernel
by the lines they change rather than by their context,
and `make mutants` checks by the headers the ones it had to move.
A three-way merge does not help: the kernel's history shows the mutated lines or their neighbours
changing whenever the context alone was not enough.
A mutant whose own lines changed is planted again by hand.

**QEMU** (`make test`, `make qemu-replay`).
The kernel has no console, so a transcript reaches QEMU's UART two ways:
through a logger in user mode while the machine runs,
and through the halt, which writes the whole log out under a line saying so;
see "The kernel log".
`make test` boots the real kernel and checks the root task's transcript:
the root task starts a logger thread on the log's line and the UART's,
builds a second process, exchanges a word with it
through a shared region and two notifications,
and ends in a deliberate fault,
and `tests/run.sh` checks that the logger carried the transcript out
before the halt did, by where the halt's line falls in it.
The replay driver carries the log to the UART after every record,
by polling and with no system call, so the transcripts stay the same on both builds,
and the host writes the same mark into the header after every event;
the halt writes out what the last record left.
`make qemu-replay` boots the replay driver once per corpus input
with the input placed in RAM by QEMU's loader.
The driver runs two threads that take records from one cursor,
so a record that blocks one of them leaves the kernel
something else to run and the blocking paths are replayed too.
A third shares the second's process and starts stopped,
so a record can resume it when two threads should wait at once.
A record that names a thread is passed to it with `OP_DEBUG_TICK`
by the protocol in `include/rvuos/replay.h`,
which the host runs from the kernel's state;
the passing is system calls, so both transcripts carry it.
The second thread has a pool, a process and a table of its own,
so every switch between them reloads the PMP under the self-check's eye,
and a pool it creates lies below its own,
so a record on the first thread can destroy both at once
and the cascade is replayed like any other path.
The state both builds start from is in `include/rvuos/replay.h`
rather than written out twice.
`OP_DEBUG_TRACE` makes the kernel print one line per call
and run the self-check after it, reading the PMP CSRs back.
`tests/differential.py` requires the host and QEMU transcripts
to match line for line.
The host has no instruction fetch to fault,
so it declares the root task dead
when its code or data is no longer mapped with the needed rights;
that is the one place where the host models rather than executes.

The same gap bounds which threads a replay may cross.
The host can follow a thread only if it never has to guess
what that thread's code does,
which holds for the driver's own threads
and not for a thread the records configured.
So a thread carries `THREAD_UNTRACED`
unless its program counter was set before tracing began,
and rather than run one while tracing,
the kernel stops the machine with `untraced thread`.
That is a constraint verification puts on the kernel,
and it costs nothing outside trace mode.

The tick is the other such constraint.
A preemption lands between two instructions,
and the host runs records rather than instructions,
so it cannot say where one would land.
While tracing is on, the tick therefore preempts nobody
and moves no time: timer lines fire only through `OP_DEBUG_TICK`.
The interrupt is still taken and acknowledged on QEMU,
so the replay exercises the interrupt entry and return,
but a switch happens only when a thread waits
or when a record asks for the tick through `OP_DEBUG_TICK`.
A traced run in which every thread waits therefore stops
even with a timer line armed, since no record is left to fire it,
and the host build, which has no clock, agrees.
A tick pending in a revoke stops it on QEMU as anywhere,
but a stopped call is not traced, only the attempt that finishes,
so where the tick lands leaves the transcript alone,
and the host may stop every such call without a line of its own.
The stall in `wfi` for an armed timer line is checked by the demo in `user/init.c`.
A device interrupt is delivered under tracing as it is otherwise,
because a line left claimed would storm
and one masked without its `Irq` disarmed would break an invariant;
the replay driver holds no device and enables none,
so none arrives during a replay, and `OP_DEBUG_IRQ` fires them by name.
The path from the controller to the `Irq` is checked by the demo in `user/init.c`,
whose logger drives the UART's transmitter on its interrupt.
The kernel loses nothing by that:
it runs with interrupts off, so a tick lands only in user mode,
and every such landing is the same to it, a ready thread whose frame is saved.
What the demo in `user/init.c` alone still checks
is the interrupt landing between two instructions.
The replay driver starts its second thread after turning tracing on,
so that no tick runs it before the records it takes are in place.
Preemption itself is checked by the demo in `user/init.c`:
the two processes take turns through a shared word and no notification,
which nothing but the tick can get them past.

**Hardware.**
QEMU's PMP may differ from a real core in Smepmp behaviour,
and it has a four-byte grain, so a coarser grain is never seen there.
The host shim models the grain instead, reading back the address bits below it as hardware would,
and the corpus is replayed with a four-byte and a 32-byte grain.
On the ESP32-C6, `make test BOARD=esp32c6` boots the demo
and checks the same transcript as under QEMU;
that is the only check that reaches its timer, matrix and USB console.
The replay records are transport-agnostic
and can be fed to the board once something loads them there;
the replay driver's second stack is QEMU's address, see `rvuos/replay.h`,
so it is not built for the board yet.

## Open decisions

These are recorded here so they are not implicit in the code.
Each has a working default the code follows
until the maintainer decides otherwise.

1. **Target hardware.**
   Decision: QEMU `virt` for RV32 stays the development target,
   using only machine and user mode,
   and the ESP32-C6 is the first real board; see "Boards".
   QEMU implements 16 PMP entries;
   `PMP_MAX_ENTRIES` caps how many the kernel uses,
   so the design can be exercised with a budget of 8 or fewer.
   The ESP32-C6 is a single RV32IMAC core with user mode, PMP and 512 KiB of SRAM,
   and it runs the demo root task from RAM, loaded by its ROM.
   Booting from flash is not done yet;
   what it needs from Espressif's second-stage bootloader,
   and whether execute-in-place goes through a cache the kernel must control,
   are listed in `TODO.md`.
   Other candidate: RP2350 on its Hazard3 cores,
   whose NAPOT-only PMP open decision 8 no longer rules out.
   Cores without PMP, GD32VF103 among them, cannot run rvuos.

2. **Implementation language.**
   Working default: C, compiled with clang for `riscv32-unknown-elf`,
   `-march=rv32imac -mabi=ilp32`, linked with lld,
   with a thin assembly entry and no libc.
   Rust was considered and rejected for now:
   the kernel is small, mostly CSR and fixed-layout work
   that would be `unsafe` anyway,
   and C keeps the toolchain and the userspace story simple.
   Discipline replaces the type system where it matters:
   `-Wall -Wextra -Werror`, no pointer arguments in system calls,
   and every capability check in one place.
3. **Position-independent processes versus fixed link addresses.**
   Working default: fixed addresses chosen by the image builder,
   because it needs no runtime relocation.
   Revisit when the root task starts loading processes dynamically.
4. **Kernel PMP entries.**
   The kernel does not lock entries and runs in machine mode,
   so it needs no PMP entries for itself.
   Working default: reserve none.
   Revisit if Smepmp support is added
   to protect the kernel from its own stray pointers.
5. **Synchronous endpoints.**
   Working default: none.
   Shared memory and notifications carry everything,
   for the reasons in "Communication and synchronisation".
   Revisit when a real server with many clients exists
   and the four system calls per round trip,
   or the missing guarantee that one answer reaches one asker,
   can be measured rather than guessed.
   An `Endpoint` object would not replace notifications:
   interrupts need those either way.

6. **Badged notification capabilities.**
   Working default: the signalling thread names the bits,
   so a client's identity to a server is a convention
   between the two of them and not something the kernel enforces.
   The alternative is seL4's: the bits a capability may set
   live in the capability, and copying it can only narrow them,
   exactly as rights narrow.
   That makes a client unable to speak for another
   and fits in the second word of an object capability, which is unused.
   It needs a way to set the mask when a capability is handed out;
   `OP_CAP_DERIVE` is the natural place, with the mask in `a4`,
   which the dispatcher already carries.
   Decide before a server with mutually distrusting clients exists.

7. **What a pool destroy gives back, and to whom.**
   Decided, by the derivation tree.
   `OP_REGION_TO_POOL` revokes the region capability it consumes
   with everything derived from it,
   and the pool capability takes its place.
   Copies beside that region and the regions above it survive,
   inert while the pool exists because installing one fails the overlap check,
   and working again after the destroy,
   which is when the destroyed pool's own memory comes back
   below the same ancestor.
   So the memory returns along the derivation it left by:
   to the lender's own capability and the copies the lender made beside it,
   never to a borrower's derived one.
   A lender that wants it back from a living borrower
   revokes below its own region capability,
   which takes the borrower's pool capability too;
   the pool itself then waits for the pool above it to go.
   The working default before this, memory returning to everyone
   who ever held a capability for the range,
   was what the checks did on their own,
   and the tree is what made "everyone" precise.

8. **TOR or NAPOT.**
   Decided: NAPOT only; see "Physical Memory Protection".
   Before, a region was any range on the grain, written as TOR entries.
   That saved RAM to rounding,
   but a region cost one or two entries depending on its neighbours,
   two ranges could overlap in part,
   and cores without TOR, RP2350 among them, were out.
   RP2350's PMP is no longer in the way;
   its hardwired entries and `mtval` reading zero still are.

9. **Scheduling policy beyond round-robin.**
   Working default: none in the kernel.
   The earlier plan was fixed priorities on `Thread`,
   set by whoever holds the capability and capped by the setter's own,
   round-robin within a priority, no priority inheritance.
   The alternative is a scheduler in userspace
   that decides which threads are runnable at all.
   It needs a way to stop a thread it started,
   `OP_THREAD_SUSPEND` in `TODO.md`,
   and a tick of its own, a timer line,
   the shape every device interrupt takes.
   What it cannot do is choose between two runnable threads
   for less than a system call per switch.
   Decide when a workload needs one thread to run before another
   and taking turns measurably fails it.

10. **Yield.**
    Working default: none.
    `OP_DEBUG_TICK` is the tick on request,
    so a yield would be that under a name that invites spinning,
    and a spinning thread is always runnable and keeps the machine from `wfi`,
    while a waiting thread says what it waits for.
    The cases that ask for one have other answers:
    a lock whose holder was preempted is a short spin on `lr`/`sc`
    and then a wait on a notification the unlocker signals,
    and polling a device ends with an `Irq`.
    A sleep bounded from above, "no longer than", was considered as the same operation;
    it would have to round the delay down and end in a spin of up to one tick.
    Revisit when a workload measures the cost of a slice burnt
    behind a preempted lock holder.

11. **Sharing an interrupt line.**
    Working default: one `Irq` per line, and a second bind fails.
    Boards wire several devices to one line,
    and then either one driver serves them all
    or several `Irq`s hang on the line,
    every one is signalled when it fires,
    and the line stays masked until every one is armed again,
    which is a count the kernel would keep per line.
    The `IrqLine` capability would then be copied rather than carved,
    and `KERR_OVERLAP` on a bind would go.
    Signalling every `Irq` on the line is a step each,
    and goal 4 allows it only if their number per line has a bound.
    Decide when a board with a shared line is worth that count.

12. **The log across a reset.**
    Working default: the kernel resets the log's header at boot
    and leaves the ring's bytes alone,
    so on a board whose boot does not clear RAM
    the previous run's last words are there to be read,
    but nothing says how many of them are valid or where they end.
    A logger that reports a crash after a reset needs the old head,
    which means a header the kernel does not reset,
    a marker that tells a cold boot from a warm one,
    and a checksum against RAM that came up as noise.
    The kernel could also keep the old ring as it is
    and start a fresh one behind it, at twice the memory.
    Decide with the first board, whose reset behaviour decides what survives.
    The ESP32-C6 parks on a halt and keeps RAM,
    but what its ROM overwrites on the way back in is not measured yet.

13. **Access permission management on the ESP32-C6.**
    Working default: the kernel turns the APM filters off at boot,
    because they refuse the CPU in user mode every peripheral,
    and PMP alone confines a process; see "Boards".
    PMP says nothing about DMA:
    a peripheral that is a bus master reads and writes where its driver points it,
    so a driver granted such a device can reach any RAM.
    The APM units are what could confine a master,
    by region and by security mode,
    which would make them the kernel's to program when a DMA driver asks.
    Decide when the first driver of a DMA-capable peripheral exists.

14. **Memory authority: `Untyped` and `Frame`.**
    Working default: one `Region` type that can be installed and can become a pool,
    kept apart by overlap checks by address.
    The checks scan every pool and every object, which goal 4 rules out.
    And a `Region` capability does not say what it can do:
    whether an install or a pool succeeds depends on pools
    made through any capability over the same range,
    so a peer holding a copy can make a capability inert
    without anyone revoking it.
    The proposal is seL4's split, in rvuos's shape.
    - **`Untyped`** is memory that may become something else,
      and cannot be installed.
      `OP_UNTYPED_RETYPE` takes the next block of a given size at its watermark,
      aligned to its size, moves the watermark past it,
      and makes it an `Untyped`, a `Frame` or a pool, hanging below.
      The children of an `Untyped` are therefore disjoint by construction.
      A retype that finds no children resets the watermark first.
    - **An `Untyped` is derived, never copied**,
      since a copy would be a second allocator over the same memory.
      Deriving requires no children and moves the source's watermark to its end.
    - **`Frame`** is today's `Region` without `OP_REGION_TO_POOL`.
      Its aliases are harmless, since no `Frame` becomes kernel memory,
      and an install checks only the slots of its own process.
    - **Isolation follows from the tree.**
      A `Frame` and a pool below different children of one `Untyped` never meet,
      and every other `Frame` is a root over memory no `Untyped` covers.
      Memory that is to become a pool while frames of it exist
      is first revoked back to its `Untyped`,
      which uninstalls those frames wherever they are.
      The self-check keeps checking isolation by address.
    - **Capabilities to objects hang below the pool capability**
      they were allocated through, where today they are roots,
      and a pool capability is derived, never copied.
      Destroying a pool is then revoking the capability the retype made,
      plus a walk over the pool's own tables
      to take the slots they hold out of other rings.
      Both are the size of the pool, and the sweep over every table goes.
      The memory returns to the `Untyped` it came from,
      so decision 7 has nothing left to decide.
      A borrower's pool is retyped from an `Untyped` derived from the lender's
      and goes with a revoke below the lender's,
      so the pool tree goes too.
    - **Device memory, flash, the log and the root task's own regions are `Frame` roots.**
      No `Untyped` covers them,
      so the tests for RAM and for the log in `OP_REGION_TO_POOL` become the type.
      `BOOT_CAP_FREE_RAM` becomes an `Untyped`.
    - **The watermark costs no slot space:**
      a block's base and size fit one word, encoded as `pmpaddr` encodes NAPOT.

    The price is seL4's.
    The root task retypes before it installs anything,
    alignment at the watermark wastes memory,
    and a block given back in the middle is not reusable
    until every child of its `Untyped` is gone.
    Still open:
    a destroy when the calling thread or its table lives in the pool,
    which today walks up the pool tree;
    whether deleting the capability a retype made refuses or destroys;
    where a `Frame`'s rights come from;
    and how a preempted destroy records where it stopped:
    a revoke needs no record, since its next step is always at the first child,
    but the walk over the pool's own tables does.
    Decide before the memory model changes again.

15. **What a process's table slot could do.**
    Working default: the slot is filled once, at allocation, and its rights mean nothing.
    - **Replacing the table.**
      An operation on a `Process` could refill the slot;
      every thread would see the new table from its next call.
      With flat slot numbers that does not help a full table grow:
      moving its capabilities needs a move that relinks a node,
      since a copy leaves behind what was derived from its source.
    - **Nested tables.**
      A slot number could be a path, read from the most significant bit:
      each `CapTable` capability skips a guard, kept in its unused second word,
      then takes the bits that index its table.
      A table grows under a new two-slot root, the old table in the first slot
      with a guard one bit shorter, so every old name keeps its meaning.
      At least one bit per level bounds a lookup at 32 steps.
      Open: whether a name that ends at a slot holding a table means the slot
      or descends into the table; seL4 passes a depth.
    - **Rights.**
      A slot without `RIGHT_W` could seal the table against its own process.

    Decide with the first program whose creator cannot size its table in advance.

16. **Drift of a periodic timer.**
    Working default: a timer line fires once, and a periodic task arms it again when it wakes,
    so each period counts from the set and the task drifts by the wait for the processor.
    A task that holds the clock can take the drift off itself,
    arming each delay as its next deadline less the counter's reading, rounded to the tick.
    `OP_IRQ_SET` could take a flag that counts the delay from the last deadline instead,
    which keeps the line one-shot.
    A periodic mode is affordable too, since the tick fires at most `TIMER_LINES` lines,
    but periods fired before the thread wakes merge into one signal unseen.
    Decide with the first task that needs a steady period.

17. **CPU time per thread.**
    Working default: none; the clock gives the machine's time, which includes other threads' slices.
    Only the kernel sees a switch, so only it can count:
    one read of the counter per switch, added to the thread leaving, eight bytes per thread.
    Reading a thread's time would be an operation on the clock that names a thread,
    because a thread that can read its own time has a clock.
    A read of the counter by system call could join it,
    for a board whose counter no region can show, or to spare a region slot.
    Decide with the first program that needs it.
