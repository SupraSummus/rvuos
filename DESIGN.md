# rvuos design

This document records the architecture of rvuos
and the reasoning behind each decision.
Code that disagrees with it is a bug in one of the two.
Work items are in `TODO.md`.

## Goals

rvuos is a microkernel for RISC-V microcontrollers.
It has three goals, in priority order.

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

## Prior art and how rvuos differs

**seL4** is the reference point for goals 2 and 3.
seL4 retypes `Untyped` memory into kernel objects
and tracks derivations in a capability derivation tree
so that revoking a parent destroys every child.
That tree is most of seL4's complexity.
rvuos keeps the "kernel never allocates" principle
but coarsens revocation to whole memory pools.
The pools form a tree by who created them, one pointer per pool,
and that replaces the derivation tree over capabilities;
see "Kernel pools and revocation" for what it gives up.
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
- **TOR mode is preferred over NAPOT.**
  NAPOT entries must be power-of-two sized and naturally aligned,
  which wastes RAM on a device with hundreds of kilobytes in total.
  TOR entries describe an arbitrary range
  bounded below by the previous entry's address.
  The kernel sorts a process's regions by base address
  so that `n` regions that touch each other cost `n + 1` entries,
  and `n` disjoint regions cost at most `2n`.
  Not every core has TOR; see open decision 8.
- **Every region lies on the grain.**
  A platform rounds every PMP boundary to a grain of `2^(G+2)` bytes,
  one value per hart, and ignores the address bits below it.
  QEMU and the ESP32-C6 have four bytes, RP2350 has 32.
  The kernel probes the grain at boot
  and a `Region` capability off the grain never exists:
  the boot layout is checked once and carving requires it,
  so every capability can be installed exactly as it reads.
  `OP_REGION_INFO` returns the grain, since user mode cannot read the PMP CSRs.
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
  is zeroing a range that becomes a pool,
  and that range belongs to the kernel from that moment on.

### Timer

The machine timer, `mtime` and `mtimecmp`, is the kernel's clock
and the kernel handles it directly; nothing of it reaches userspace.
Where the registers live and how fast they count is the board's business,
so `kernel/timer.c` is per board, as `uart.c` is.
On every tick the kernel sets `mtimecmp` a period ahead of `mtime`,
not ahead of the previous compare value,
so a long system call costs one late tick and not a burst of them.

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

Capabilities can be copied to another process's table,
optionally with reduced rights,
by a process that holds both the source capability
and a capability to the destination table.
There is no derivation tree.
A copy is as good as the original
until the object behind it is destroyed.

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

A slot is twelve bytes: type, rights, padding, and two words.
For object capabilities the first word is the object's address.
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

### Object types

| Object | Purpose |
|---|---|
| `Region` | A physical address range with maximum rights. Installed into a process's PMP slots. |
| `KernelPool` | A region handed to the kernel. All other objects are allocated from pools. |
| `CapTable` | A process's capability table. Allocated from a pool. |
| `Process` | A protection domain: a `CapTable`, a set of region slots, and its threads. |
| `Thread` | An execution context inside a process: its registers and its state. |
| `Notification` | A word of sticky signal bits. The only way a thread can stop and be started again. |
| `Irq` | The right to receive one hardware interrupt as a notification. |
| `Debug` | Console output and machine halt for bring-up and tests. No object. |

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
which is fine for the object counts a microcontroller can hold.
The `Region` capability that was consumed is cleared,
so the caller cannot install the same range later;
copies of it elsewhere fail the overlap check.
The kernel zeroes the memory before use.

The pool's own descriptor is the first object in the pool's memory,
so the kernel keeps no per-pool state outside the pool
except a list head for the overlap scans.
Objects are bump-allocated behind it, eight-byte aligned,
never freed individually, and each records its pool.

Revocation happens at pool granularity.
This is the deliberate simplification relative to seL4:
there is no per-object derivation tree,
so a pool is the unit of trust.
A process that wants to revoke one object cheaply
allocates it from its own small pool.

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
and costs the same, once per destroy rather than once per system call.

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
A structural pointer, such as a process to its table
or a thread to its process, is not checked on use,
so a structural parent must live in the same pool as its child
or outlive it.
The kernel enforces the same-pool rule at allocation:
a `Process` must be allocated from the pool its `CapTable` lies in,
and a `Thread` from the pool its `Process` lies in.
A thread waiting on a notification inside a destroyed pool
is woken with `KERR_INVALID_CAP` and no bits,
because the wait can no longer be answered.

A destroy refuses with `KERR_STATE`
when the calling thread lives in the pool or in one below it.
A thread, its process and its table share one pool by the rule above,
so that one check is the whole of it,
and it is also why the boot pool can never be destroyed:
every pool lies below it.
The memory is zeroed on the way out,
because it is about to be user memory again
and still holds other processes' capability tables.
It comes back as a `Region` capability
with the rights the region carried when it became a pool,
which the pool records for that purpose,
so a destroy cannot manufacture a right the memory never had.
The pools below give their memory back to nobody in particular:
region capabilities for it that survived the sweep
pass the overlap check again,
which is open decision 7's working default
and is what puts lent memory back in the lender's hands.
The pool list has the newest pool first and a child is newer than its parent,
so one walk of the list meets every pool below the destroyed one before it,
and no parent read on the way up has been zeroed yet.

#### Why not a derivation tree

The pool tree cannot do what seL4's can:
revoke one region capability and everything derived from it
while its holder goes on living.
A parent that wants a region back from a child
destroys the child's pool, or asks.

The precise version was considered and rejected.
Derivation links in every slot double it,
every copy, carve, delete and pool creation maintains them,
and a pool destroy removes whole tables of nodes
whose parents and children live in tables that survive.
Installed regions are derived authority too,
so either region slots join the tree
or a revoke uninstalls by address range,
and two processes sharing a region could then unmap each other.
An earlier sketch by the same author,
`os4cm4` in the LANoT repository,
put three such links next to the object pointer in a sixteen-byte slot
and never got as far as maintaining them.
One word per pool buys the property that matters:
nothing a process creates in kernel memory outlives the process.

### Region slots

A `Process` has a fixed array of eight region slots.
Installing a `Region` capability into a slot
requires the region capability
and a `Process` capability with the write right.
The kernel recomputes and caches the PMP register image
for the process at that point,
not on every context switch.

Rights on the installed slot are the intersection
of the region capability's rights
and the rights requested at install time.
Installed regions in one process may not overlap,
because PMP resolves overlaps by entry number
and the result would depend on sort order rather than policy.
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

Interrupts will arrive as signals on the notification bound to an `Irq`,
which is roadmap step 6 and needs no new mechanism.

## Scheduling

**The kernel keeps no run queue.**
A thread's own state says whether it may run,
and a runnable thread is found by walking the pools,
as `pool_overlaps` and `installed_overlaps` are.
A queue would record a second time what the state already says.
The walk costs what those checks already cost.

**The walk is the policy: round-robin.**
The walk continues from the running thread and wraps around,
so runnable threads take turns in the order they were allocated,
and the only scheduling state the kernel holds
is which thread is running.

**The machine timer provides the tick.**
A thread runs until it waits on a notification
or until the tick takes the processor from it,
and either way the walk picks the next runnable thread.
The tick has a fixed period, `TIMER_HZ` in `kernel/timer.h`,
and one tick is one slice: a preempted thread goes to the back of the round.
Interrupts are taken in user mode only:
machine mode runs with `MIE` clear from the trap to the `mret`,
so a system call is never interrupted and the kernel needs no locks.

When a thread waits and nothing is runnable,
no thread can become runnable again,
because only a running thread can signal,
the tick wakes nobody,
and device interrupts do not exist yet,
so the kernel says `no runnable thread` and stops the machine.
That becomes a `wfi` when device interrupts arrive.

**Nothing more lives in the kernel.**
Round-robin on a tick is the least policy that makes
a spinning thread harmless and a woken thread eventually run.
Fixed priorities were the intended end state of this section
and are now open decision 9.

## Interrupts

Not implemented yet; roadmap step 6.
The kernel owns the trap vector and the interrupt controller.
A driver in userspace holds an `Irq` capability,
binds it to a `Notification`,
and waits.
When the interrupt fires,
the kernel masks it, signals the notification, and returns.
The driver acknowledges through the `Irq` capability,
which unmasks the line.
No driver code runs in machine mode.

The machine timer is the one exception:
the kernel handles it directly for scheduling.

## Boot

The image in flash contains the kernel followed by the root task.
The kernel:

1. sets up its own stack and trap vector,
2. discovers the PMP entry count and grain by writing the CSRs and reading them back,
3. carves its own static state out of a small fixed SRAM range,
4. constructs the root process by hand, including a `KernelPool`
   in a range the linker reserves,
5. hands the root task capabilities to the remaining RAM
   (later also flash, device ranges and interrupts),
6. drops to user mode into the root task.

The slots the root task finds filled are listed in `include/rvuos/abi.h`
as the `BOOT_CAP_*` constants.
Slot zero is left empty so that an uninitialised index fails.

Everything after that is policy set by the root task.
The kernel does not know what a driver, a file system,
or a shell is.

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
so the kernel's own memory is unreachable.

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
on the PMP grain,
with rights no greater than the root task received for it,
or a live kernel object of the capability's own type.

**Structural soundness.**
Every pool's descriptor sits at its base,
its objects tile the space from the descriptor to the used mark exactly,
each object records the pool it lies in,
and pools are pairwise disjoint.
Every pool but the boot pool names as its parent a live, older pool,
so parents lead to the boot pool
and no destroy has left a pool below it standing.
A process and its table, and a thread and its process,
lie in the same pool.

**Thread state.**
Every thread is stopped, ready, or waiting.
A waiting thread names a live notification and nothing else does.
The thread the kernel is running is one it could run:
it is a live object and it is ready.
A preempted thread stays ready,
so it is one the walk finds again.

**Memory safety.**
No sequence of system calls makes the kernel read or write
outside its own objects,
nor reach any undefined behaviour.
Any kernel panic reachable from user mode is a bug.

Not on the list yet: "rights only narrow through copies".
Until pool destroy exists, confinement implies it.

## Verification

**Host fuzzing** (`make host-test`, `make fuzz`).
The kernel's logic compiles natively with a shim for the console,
the PMP CSRs and halting, and physical addresses indexing a RAM buffer.
An input is a sequence of events the kernel receives,
not the behaviour of one process:
each record is a system call with the thread that makes it,
and the tick is a record too, `OP_DEBUG_TICK`.
libFuzzer feeds the records into the threads' registers
and the self-check runs after every call, under ASan and UBSan.
Since no system call takes a pointer,
the registers are the whole attack surface.
The inputs replayed are checked in under `tests/seeds` and `tests/corpus`.
A seed is written by hand for a scenario the kernel must handle,
is named after that scenario, and is kept for as long as the scenario exists.
The corpus is what the fuzzer found,
and `make corpus-merge` rebuilds it from scratch each time:
an input stays only while it adds coverage on some harness
beyond the seeds and the inputs kept before it.
Coverage is measured against the kernel as it is,
so an input that was unique for an earlier kernel
and covers nothing new today is dropped rather than kept for its history.
`make mutants` plants bugs in the kernel one at a time
and requires the replay to catch each;
it is also the check that a minimisation lost nothing,
and an input that some mutant needs but coverage does not keep
belongs among the seeds under a name.

**QEMU** (`make test`, `make qemu-replay`).
`make test` boots the real kernel and checks the root task's transcript:
the root task builds a second process, exchanges a word with it
through a shared region and two notifications,
and ends in a deliberate fault.
`make qemu-replay` boots the replay driver once per corpus input
with the input placed in RAM by QEMU's loader.
The driver runs two threads that take records from one cursor,
so a record that blocks one of them leaves the kernel
something else to run and the blocking paths are replayed too.
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
While tracing is on, the tick therefore preempts nobody.
The interrupt is still taken and acknowledged on QEMU,
so the replay exercises the interrupt entry and return,
but a switch happens only when a thread waits
or when a record asks for the tick's decision through `OP_DEBUG_TICK`.
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
The host shim models the grain instead, rounding addresses as hardware would,
and the corpus is replayed with a four-byte and a 32-byte grain.
The replay records are transport-agnostic
and can be fed to a board over UART once there is one.

## Open decisions

These are recorded here so they are not implicit in the code.
Each has a working default the code follows
until the maintainer decides otherwise.

1. **Target hardware.**
   Decision: the first stage runs on a simulator only, with no hardware.
   Development target is QEMU `virt` for RV32,
   using only machine and user mode.
   QEMU implements 16 PMP entries;
   `PMP_MAX_ENTRIES` caps how many the kernel uses,
   so the design can be exercised with a budget of 8 or fewer.
   Preferred candidate for real hardware is the ESP32-C6,
   a single RV32IMAC core with user mode, PMP and about 512 KB of SRAM:
   16 PMP entries, a four-byte grain, TOR and NAPOT,
   `mtval` on every access fault,
   pending the datasheet checks listed in `TODO.md`.
   Other candidate: RP2350 on its Hazard3 cores, behind open decision 8.
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
   It needs a way to set the mask when a capability is copied,
   which the copy operation has no argument left for.
   Decide before a server with mutually distrusting clients exists.

7. **What a pool destroy gives back, and to whom.**
   `REGION_TO_POOL` clears only the capability it was invoked on.
   Copies of that region capability elsewhere survive,
   and while the pool exists they are inert,
   because installing one fails the overlap check against the pool.
   After a destroy that check no longer fires and they work again,
   so the memory returns to everyone who ever held a region capability
   for the range,
   rather than to whoever handed it to the kernel.
   Working default: exactly that, because it is what the checks already do,
   and the pool tree leans on it for the memory of the pools below.
   The alternative is for destroy to clear region capabilities
   overlapping the range in the same sweep
   that clears capabilities to the objects,
   which leaves the memory named by nobody
   and needs an answer to who may name it next.
   Decide before roadmap step 7.
   The other half of this question, what happens to capabilities
   naming objects inside the pool, is decided
   and lives in "Kernel pools and revocation".

8. **NAPOT for cores without TOR.**
   RP2350 has eight dynamic PMP entries, a 32-byte grain, NAPOT only,
   `mtval` hardwired to zero,
   and three hardwired entries that grant every process
   its ROM and peripherals.
   Under NAPOT a region is a power of two aligned to its own size,
   so a 96-byte range costs two entries,
   and eight slots against eight entries leaves no slack.
   Working default: TOR only, RP2350 out of scope.
   The alternative keeps the `Region` capability as it is
   and adds a second `rebuild_pmp` that splits each slot into NAPOT entries,
   failing the install when the split does not fit.
   Decide when a board without TOR is worth that second image builder.

9. **Scheduling policy beyond round-robin.**
   Working default: none in the kernel.
   The earlier plan was fixed priorities on `Thread`,
   set by whoever holds the capability and capped by the setter's own,
   round-robin within a priority, no priority inheritance.
   The alternative is a scheduler in userspace
   that decides which threads are runnable at all.
   It needs a way to stop a thread it started,
   `OP_THREAD_SUSPEND` in `TODO.md`,
   and a tick of its own, a notification the timer signals,
   the shape every device interrupt takes in roadmap step 6.
   What it cannot do is choose between two runnable threads
   for less than a system call per switch.
   Decide when a workload needs one thread to run before another
   and taking turns measurably fails it.
