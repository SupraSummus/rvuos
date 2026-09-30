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
   however many objects anyone has created,
   destruction aside, which is paid for and preempted; see "Bounded work".

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
User mode runs with `mstatus.MIE` set, from the `MPIE` the kernel sets before its first `mret`:
the specification takes machine interrupts in user mode whatever `MIE` holds,
but RP2350's Hazard3 takes none there while it is clear, erratum RP2350-E7.

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
  The kernel probes the grain at boot, taking none finer than the board's `PMP_GRAIN_MIN`,
  since RP2350's address registers do not show theirs and the probe finds four bytes there,
  and a `Frame` or `Untyped` that is not a block of at least that size never exists:
  the boot layout is checked once and carving and retyping require it,
  so every frame is exactly one PMP entry.
  Whether it may be installed depends on nothing but the process it goes into,
  since no frame overlaps a pool; see "Kernel pools and revocation".
  `OP_FRAME_INFO` returns the smallest region, since user mode cannot read the PMP CSRs.
- **An access must lie within one entry.**
  The lowest-numbered entry that matches any byte of an access decides,
  and it must match every byte or the access fails,
  whatever either entry grants.
  Two regions that touch share a boundary
  that no single load, store or instruction fetch may cross;
  the creator lays out a process so that nothing straddles one.
  A core may split a misaligned access and check each part on its own, though, and both boards do,
  so a misaligned load across two readable regions may go through,
  and a store that faults may leave its first part written.
  The ESP32-C6 checks the second part for the wrong kind of access; see "Boards".
- **The CSRs are WARL and may be hardwired.**
  RP2350 fixes entries 8 to 10 to its ROM, peripheral and SIO ranges, with every right for user mode:
  their configuration is read only, though their address seems to take a write.
  The probe at boot writes each entry's mode, with no rights, and then its address, and reads them back,
  and the budget is the leading run of entries that take both, eight on RP2350.
  RP2350 also transposes R and X in every configuration field, erratum RP2350-E6,
  which `pmp.c` swaps on the way to the CSRs and back.
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
Where the registers live, what starts the counter and how fast it counts are the board's business:
`board.h` names the two registers,
and `timer.c`, per board as `irq.c` is, starts the counter and says how many counts make a tick.
The rest is the same on every board and lives once in `kernel/clint.c`:
reading `mtime`, setting `mtimecmp` and counting the ticks.
The kernel counts ticks on the counter rather than on interrupts:
every trap counts the whole periods `mtime` has passed since the last tick it counted,
so the tick count keeps pace with the counter however late or seldom the interrupt is taken,
and a long system call costs one late tick and not a burst of them.
`mtimecmp` is set only for the first tick that could change what runs; see "Scheduling".
The counter also says how far into the tick a change of turn falls, which is what a turn is charged by.
QEMU `virt` counts at 10 MHz in a SiFive CLINT.
The ESP32-C6 has a CLINT of Espressif's,
whose counter and interrupt stay off until a control word starts them,
and which counts at the CPU clock the ROM left;
the kernel measures one tick of it against the 16 MHz system timer at boot
rather than trust a clock it did not set.
RP2350's is the RISC-V platform timer in the Secure bank of SIO,
counting the microseconds a tick generator divides from the crystal.

A process reads the counter through a read-only region at `COUNTER_ADDR`, not with `rdtime`:
the ESP32-C6 has no `time` CSR, but its CLINT has `UTIME`, a read-only copy of `mtime` for user mode.
On QEMU the kernel clears `mcounteren` at boot, so `rdtime` traps on every board; see "Time".
User mode does not reach RP2350's SIO timer, so there the counter is TIMER0's, counting the same microseconds;
TIMER0 keeps its high half below its low one, so the word above the counter is not its high half; see `TODO.md`.

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

RP2350's Hazard3 keeps its controller in CSRs of its own, the Xh3irq extension:
arrays of one bit per system IRQ that enable it, show it pending and force it,
each reached through a 16-bit window.
rvuos makes a line an IRQ, gives every IRQ the same priority,
and a claim takes the lowest line both pending and enabled;
as on the ESP32-C6, nothing is held between claim and completion.
IRQ 0, TIMER0's, is shadowed by the log's line.

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

**The log says what the fault was.**
The kernel writes `user fault` and the frame's `mcause`, `mepc` and `mtval` into its log,
while the bits say only which thread; no call returns the cause, open decision 21.
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
the woken thread is ready, not running, and waits for its turn,
and for its account to reach a tick if it spent it and has no spare time.
A timer line that fires disarms its `Irq`, as a device's line does,
and setting an armed one moves its deadline rather than adding a second.

**A period counts from the deadline.**
A periodic task arms the line again each time it wakes,
and a delay counted from the wake would put every wake's lateness into the period.
`OP_IRQ_SET` with `IRQ_SET_PERIOD` counts from the line's last deadline instead,
or from its bind if it never fired:
the line fires at the first tick after the call
that lies a whole number of periods from that deadline,
and the call returns how many such ticks had already passed.
The cases a task needs follow from that one rule.
Set on each wake, the line keeps its period without drift.
A task that woke late skips the periods it missed and learns how many,
so it can do their work at once, note the overrun, or ignore it.
Set again before it fired, the line keeps its deadline.
The deadline is never more than a period away, however stale the last one.
The period is rounded up to whole ticks, as a delay is.

**Why not a periodic line.**
A line the kernel arms again on its own would save a task one call per period,
but periods fired before the thread wakes would merge into one bit,
and a task that wanted one wake would race the next to disarm the line.
It would also be the one line that does not mask itself as it signals.

**Why not an absolute deadline.**
A deadline in the clock's counts would be the simplest rule to state,
but every task that sleeps would need the clock as well as the line,
and under tracing, where time moves only by record,
the host's tick count would have to start where QEMU's does.

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
and `OP_CLOCK_FRAME` derives a read-only `Frame` holding the counter, below the clock.
A process with that region installed reads the time with loads, without a trap.
The rate is the board's, and on the ESP32-C6 measured at boot, so a program takes it from the clock.
The frame is the smallest block holding the counter's two words;
on a PMP grain coarser than eight bytes it also shows the timer registers beside them, read only.

**Why a capability.**
The time is authority, and goal 2 allows no ambient authority:
a process given no clock and no timer line cannot tell time passing,
unless it builds a clock from a second thread or learns the time from someone who has one.
On the ESP32-C6 it can time its own turn on the core's performance counter,
which the kernel stops at zero whenever another process's thread runs; see open decision 22.
Gating `rdtime` would have needed `mcounteren` switched per process, and the ESP32-C6 has neither.
A region already carries rights, derivation and revoke, so the clock adds one type and no object.
The rate alone tells nothing about time, so it has no capability of its own.

## Scheduling

**The processor is authority, as memory is.**
A process that got a turn per ready thread could buy the processor with threads,
which cost a few hundred bytes of a pool each.
So the processor is a fixed number of units, handed out as capabilities as memory and lines are,
and each unit is earned by one thread at a time.

**A thread earns units of time.**
The processor is `TIME_UNITS` units, a constant of the kernel as the timer lines are,
and the root task receives them all in `BOOT_CAP_TIME`, its thread earning every one.
A `Time` capability names a range of units and no object, as an `IrqLine` names lines,
and `OP_TIME_CARVE` takes a smaller range out of one.
`OP_TIME_BIND` binds a thread to some of a capability's units, or to none,
and moves it off the units it earned before.
The kernel records which thread earns each unit and refuses a bind that names a unit another thread earns,
so a capability may be copied, but each unit's time goes to one thread,
and the time of the units nobody earns is spare.
The binding is a node of the derivation tree held in the thread, below the capability it was bound through,
so revoking above that capability unbinds the thread, as it unmaps a region installed from a frame.
A thread with no units and no spare time keeps its state and does not run:
that is how a scheduler in userspace stops and starts a thread, open decision 9.
A thread that loses its units while it runs finishes the turn it had,
so only a wait, the tick, a fault, or a revoke that takes its own process takes the processor from a running thread,
and a call stopped for an interrupt is made again before anything else runs.

**A thread has an account.**
An account counts in parts of a count of the counter, `COUNT_PARTS` of them to a count.
Every tick a thread's account gains a tick's counts of parts for each unit it earns,
so a thread that earns the whole processor gains a tick every tick.
It holds at most `ACCOUNT_TICKS` ticks' worth per unit, a tenth of a second's,
so a thread that idled comes back with that much at most.
The ESP32-C6 measures the counter's rate at boot, so the timer starts before the root thread's account is filled.
A new thread's account is empty, and the root thread's starts full.
A thread bound to other units keeps what its account held, up to what the new units hold,
and an unbound one loses it, so no bind puts time into an account.
`OP_DEBUG_TRACE` fills every account; no call tells what one holds, which would be a clock.

**A thread has time while its account holds a tick.**
A turn lasts at most to the next tick, and every count of a turn the thread began with time costs it `COUNT_PARTS`,
while it earns its units as in any tick.
A turn with time began with a tick in the account, so it never costs the account more than it holds.
A thread with less than a tick has no time until its account reaches a tick again.
Spare time is the turns no thread with time wants.
A thread bound through a capability with `RIGHT_X` runs on it, for free, while its account fills as usual.
The boot grant has `RIGHT_X`, so a thread bound through it to no units runs whenever the threads with time let it.
A thread without `RIGHT_X` runs at most its units' part and a full account,
and the processor sleeps in `wfi` for the rest: that is how the root task caps a thread's time.

**The queues are the policy: round-robin, time before spare time.**
A ready thread that may run, and whose turn it is not, waits on one of three queues,
each a ring of threads, oldest first, through the thread's own `queue_next` and `queue_prev`,
which a waiting thread uses for its notification's waiters instead:
the run queue while it has time, the spare queue without time if it may run on spare time,
and the spent queue otherwise, where a thread with units waits for its account.
The oldest thread on the run queue has the next turn, or with the run queue empty the oldest on the spare queue.
A thread that waits hands the rest of the tick to the next, and the tick ends the turn:
the thread goes to the back of the queue its account and binding now put it on.
A thread that becomes ready, resumed, woken or bound, joins the back of its queue.
Finding the next thread takes constant time however many objects exist,
and the scheduler's state is the three queues, each thread's account and the tick it was counted to,
the thread that earns each unit, the release, the running thread, and how far into the tick its turn is charged.
With every thread on spare time alone this is the plain round-robin over threads,
which is how the replay driver runs; see "Verification".

**A turn is charged by the counter.**
A change of turn reads how far into the tick the counter is
and charges the thread leaving for the counts since its turn began or since the tick, whichever is later;
a tick charges the thread whose turn it is for the rest of the tick and earns it its units,
whether the timer interrupted for it or a later trap counted it.
Whether a turn is paid is decided as it begins and at each tick, and holds until the next.
Only the running thread's account is counted at each tick;
the others are brought up to the count when the kernel looks at them.
A thread with units that waits without time has time again once its account reaches a tick,
and the kernel keeps the release, the nearest tick at which that happens for any such thread.
Only at the release does it look at the `TIME_UNITS` units, as the tick looks at the timer lines;
every thread with units earns one of them, so the look finds them all.
A trap that counts several ticks at once charges them as if each had been taken, in constant time;
`account_after` in `kernel/sched.c` says how.
So a thread that waits just before every tick pays for its time all the same,
and one whose turn begins late in a tick pays only for the rest of it.

**What it promises.**
Threads with time take equal turns, and then threads on spare time do.
A thread that wants the processor all along gets its units' part of it,
give or take a tenth of a second's worth of the accounts, and without spare time no more.
A thread a process adds earns only units split off the process's own, or runs on spare time,
so it takes nothing another thread earns, and a child gets time only through units its creator lends it.
Spare time goes round by thread, so more threads there get more of it:
it is the time nobody earned, and a thread that needs a part of the processor is given units.
A thread with time waits for its turn at most `TIME_UNITS` ticks,
since only threads with units have time and each takes a tick.
No other latency is promised.
A thread with no ready work spends nothing, so an idle holder of units costs the others nothing.

**Why a fixed number of units.**
Units made of memory would buy time with memory again, unless each were conserved on its own.
A limit per process or per thread bounds nothing, since anyone can make more of those.
A fixed count is conserved for free, and it bounds the release:
only a thread with units waits for its account, and each earns a unit,
so the release looks at the units and at nothing else,
where accounts on objects made of memory would need a queue of refills sorted by time,
as seL4's scheduling contexts have.
Sixty-four makes a unit a fine enough part of the processor and the release a look at 64 words.

**Why threads earn, and not shares.**
Before the units, the processor was 32 shares: kernel objects threads were bound to,
each with a budget moved by a call of its own, and an account its threads spent together.
More shares bought more turns and more budget more time, two knobs for one thing, and units are one.
The price is that threads cannot spend one account together:
a process splits its units among its threads, or runs them on spare time.
A share had time again only once its account was full, which kept a capped share's idle time in one stretch;
a thread has time again at a tick, which bounds how long it waits; open decision 18.

**The machine timer provides the tick.**
A thread runs until it waits on a notification, until it faults,
or until the tick takes the processor from it.
The tick has a fixed period, `TIMER_HZ` in `kernel/timer.h`,
and one tick is one turn: a preempted thread goes to the back of its queue.
A turn that begins when a thread waits mid-tick lasts only to the next tick;
`TODO.md` carries a turn counted from the switch.
Interrupts are taken in user mode only:
machine mode runs with `MIE` clear from the trap to the `mret`,
so a system call is never interrupted and the kernel needs no locks.

When a thread waits and nothing is runnable,
only an `Irq` armed on a timer line or a device's line can make one runnable again,
because only a running thread can signal otherwise,
or the release, when a thread on the spent queue waits for its account.
The kernel counts those `Irq`s as they are armed and disarmed,
so it knows without a walk, and the spent queue it looks at.
With any of them the kernel stalls in `wfi`
until the nearest deadline of an armed timer line, the release,
or a device interrupt is pending,
takes it by hand since machine mode runs with `MIE` clear,
and looks for a runnable thread again;
with none it says `no runnable thread` and stops the machine.

**The timer interrupts only for a tick that could change what runs.**
A tick that ends a turn only to hand the processor back to the same thread moves the counts and nothing else.
So while another thread could have the next turn, `mtimecmp` is set for the next tick,
and otherwise for the nearest of the deadline of an armed timer line, the release,
and the tick the running thread's account drains, unless it may go on alone on spare time,
where its account changes nothing that runs.
The account drains only at a tick, since a turn with time never outruns it before the next tick,
so the timer stays on the tick grid.
In the stall it is the nearer of the deadline and the release.
A deferral reaches at most 2^31 counts, several seconds on either board,
so that `timer_next` does not take it for a debugger's stop;
a longer one wakes there and is set again.

**Every trap counts the ticks the timer let pass.**
On entry, before anything reads the count or an account, a trap charges and counts them as the tick would have,
and a trap whose count reaches the tick the timer was set for ends the turn as it returns.
It reads the counter only while the timer is set past the next tick or its interrupt is pending;
a change of turn reads it besides, to charge the turn.
Whatever could call for another tick happens in a trap and marks the timer's tick stale:
settling a thread, binding or unbinding one, a switch, a tick, an arm, the stall.
A trap sets the timer again only then, and choosing the tick costs no walk,
since an arm brings the nearest deadline forward and the tick's look at the timer lines makes it exact.
The counts, the charges, the deadlines and the turns come out as if every tick had been taken, a late tick aside,
while a thread alone on the processor is not interrupted every tick.
The price is a few loads on every trap: on QEMU a tenth of the cheapest system call,
a sixth while the counter is read.

**Nothing more lives in the kernel.**
Round-robin on a tick is the least policy that makes
a spinning thread harmless and a woken thread eventually run,
units each earned by one thread are the least that make a thread cost nobody but whoever gave it units,
and an account per thread is the least that caps its time without keeping the others busy.
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
bound at creation to a notification,
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
so an `IrqLine` capability names no object, as a `Frame` names none:
the root task receives every line of the controller at boot, and every timer line,
carves single lines out with `OP_IRQ_CARVE`, a table operation,
and hands them to drivers the way it hands out memory.
Binding consumes the invoked slot with everything derived from it,
and the `Irq` capability hangs below the Pool capability the `Irq` was allocated through,
as every object's does, so the pool's destroy finds it.
Copies of the line elsewhere are inert while the `Irq` exists,
because a line is bound at most once and a second bind fails with `KERR_OVERLAP`.
When the `Irq`'s pool is destroyed the kernel masks its line
and the object is gone, so those copies work again:
the lender of a line gets it back by revoking below the Untyped the borrower's pool was made of,
exactly as the lender of memory does.
Revoking below the line capability the lender derived the borrower's from
takes the line only while it is unbound;
once bound, the line is the `Irq`'s until its pool goes.

**An `Irq` holds its notification as a thread holds its process,**
by a capability in a slot of its own, below the `Notification` capability it was bound with.
A revoke above the slot, or the destroy of the notification's pool, clears it,
which disarms the `Irq` and masks its line.
`OP_IRQ_SET` refuses an `Irq` without a notification,
so it stays disarmed, its line bound, until its own pool goes.
It costs twenty-four bytes per `Irq` and a test on every set;
before, an `Irq` had to lie in its notification's pool so a pointer could not dangle.

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
and it can never become a pool, since the kernel writes there on its own:
`BOOT_CAP_LOG` is a frame, and no Untyped covers it.
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
RP2350 has no serial port on USB, and its bootrom wipes RAM on entering BOOTSEL,
so there the logger's console is a block of RAM,
and the halt makes a USB CDC-ACM port of the chip's controller, `kernel/board/rp2350/cdc.c`,
writes the block and then the dump to it, and reboots into BOOTSEL once the host has read them.
It is the one USB stack in the kernel, and it runs only once the kernel has stopped.
The ring is just under 4 KiB, a burst's worth between two looks by a reader,
and a reader that looks less often loses the oldest bytes and can tell.

## Boot

The image contains the kernel followed by the root task.
How it reaches RAM is the board's; see "Boards".
The kernel:

1. makes the board ready, which on the ESP32-C6 means watchdogs off
   and on RP2350 its clocks set and a watchdog armed,
   and sets up its own stack and trap vector,
2. discovers the PMP entry count and grain by writing the CSRs and reading them back,
3. carves its own static state out of a small fixed SRAM range,
4. constructs the root process by hand, including a `KernelPool`
   in a block of the root task's own memory,
5. hands the root task an `Untyped` for that memory, `BOOT_CAP_ROOT_RAM`,
   with its code, data and input frames and the boot pool below it,
   an `Untyped` for the block of RAM the board sets aside for it,
   frames for the UART's registers,
   to the kernel's log,
   to the machine's counter as a `Clock`,
   and to every line of the interrupt controller with the log's line before them
   (later also flash and other device ranges),
   and every unit of the processor's time, with its own thread earning them all,
6. programs the tick, masks every interrupt line,
   and drops to user mode into the root task.

A device range is granted read and write, never execute,
the counter read only,
and can never become a pool:
it is a frame, and no Untyped covers it,
because the kernel writes objects into a pool,
which on a device would drive its registers from machine mode.
The two Untypeds the root task starts with cover RAM and nothing else.

Every range the root task is granted is a block,
so a board lays RAM out in blocks and what lies in none of them stays unused.

The slots the root task finds filled are listed in `include/rvuos/abi.h`
as the `BOOT_CAP_*` constants.
Slot zero is left empty so that an uninitialised index fails.

Everything after that is policy set by the root task.
The kernel does not know what a driver, a file system,
or a shell is.

### The root task is its capabilities

After boot the kernel names the root task nowhere.
The root task lives in memory it holds: `BOOT_CAP_ROOT_RAM` is an Untyped
with its code, data and input and the boot pool's block, `BOOT_CAP_POOL_RAM`, right below it,
what a loader leaves that halved an Untyped down to them, let the halves between go
and kept the block it made the pool of, so that the pool can go alone.
So it makes nothing while they are there,
and the rest of its memory, past the boot pool, is its holder's again once they are gone.
It cannot revoke below that memory or below the boot pool's, since it lives there,
as no process can what it runs on.
Before, the boot pool lay in a range the linker reserved, below no Untyped,
and its destroy was refused by name.
Everything else the root task does it does through its table,
so a process holding the same capabilities in the same places of the derivation tree can do all it could.

That lets the root task hand its place over and end, as a bootloader chains to the next stage.
`user/init.c` does it at the end of the demo:

1. The root task builds a successor out of free RAM, as it builds any process.
2. It moves every capability it holds into the successor's table with `OP_CAP_MOVE`, each to the same slot,
   the capability it moves them through last, and waits on a notification it kept.
   A copy would not do: what the root task lent hangs below its own capabilities,
   and an Untyped is never copied.
3. The successor stops the root task's thread for good,
   binding it to no units through a capability without `RIGHT_X`,
   and revokes below `BOOT_CAP_POOL_RAM`, which destroys the boot pool,
   the root task's thread, process and table and everything else allocated there.
4. It copies its own table, process and thread into the slots the destroy emptied,
   so a program written as a root task runs on as one.

The successor in the demo runs the root task's code, so it keeps those frames;
one whose code lies elsewhere revokes below `BOOT_CAP_ROOT_RAM` instead,
which unmaps them wherever they are and gives it the whole block back.
Only a holder of `BOOT_CAP_POOL_RAM` or `BOOT_CAP_ROOT_RAM` can destroy the root task:
a capability to the boot pool allocates from it and nothing more; see open decision 19.

## Boards

A board is the files of `kernel/board/<board>/` and `user/board/<board>/`,
chosen with `make BOARD=<board>`:
`board.h`, where RAM, the root task, the console and the timer's registers lie,
how many interrupt lines there are and which mcause the controller raises,
whether the core checks a misaligned store's second word for reading, `PMP_SPLIT_STORE_AS_READ`,
which `kernel/layout.h` and both linker scripts read,
whether it transposes R and X in `pmpcfg`, `PMP_CFG_RX_TRANSPOSED`,
and the finest grain it may have, `PMP_GRAIN_MIN`;
`board.c`, what the board needs before anything else and the CSRs it sets back for each process;
`timer.c`, which starts the timer `kernel/clint.c` drives;
`irq.c` and `halt.c`;
`console.h`, the device behind `BOOT_CAP_UART` as the root task drives it;
and `csrs.h`, the user-mode CSRs the demo checks the kernel sets back.
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
The core has user-mode traps, the N extension,
and the ROM leaves `mideleg` at `0x111`,
which delegates the user software, timer and external interrupts to a handler in user mode;
the kernel clears it, so every trap is its own.
User mode can also write CSRs, and nothing shuts it out of them:
the N extension's `ustatus`, `uie`, `utvec`, `uepc` and `ucause`,
the performance counter through `0x800` to `0x802`, user mode's names for `mpcer`, `mpcmr` and `mpccr`,
which the TRM leaves out,
and the dedicated GPIO at `0x803` and `0x805`.
The ROM leaves the counter counting cycles.
The kernel sets them all back at boot and whenever another process's thread runs,
so nothing passes through them from one process to the next; see open decision 22.
`uscratch` faults in user mode, whatever the TRM says, and `uip` takes no write with nothing delegated.
Measured on the chip: sixteen PMP entries, all unlocked after the ROM,
a four-byte grain, TOR, NAPOT with an entry of RAM's size, `mtval` on access faults,
PMA entries all zero, which leave every range its default attributes,
and machine interrupts taken in user mode whatever `mstatus.MIE` says.

A misaligned access that crosses a word is split in two,
and the second word is checked for writing if a store comes next and for reading otherwise.
So a load followed at once by a store faults wherever the two regions differ in rights,
and faults again when resumed: Espressif's erratum DIG-694, which lists nothing more.
And a store followed by anything else writes up to three bytes of a region above it
that the process may read but not write, and raises nothing.
Fetches, AMOs, and parts that land where the process may not read fault as they should;
a fault on the second word reports the access's address plus four in `mtval`.
So the install keeps such regions apart, see "Region slots",
and the layout leaves a block between the log and the code and one between the data and the input.
A program compiled for strict alignment meets neither half.

**RP2350** runs the demo root task and the escape suite on its Hazard3 cores, `make BOARD=rp2350 test escape`.
The bootrom's BOOTSEL mode takes the image's segments into SRAM over USB and reboots into them,
finding the image by the block `image.S` puts at `RAM_BASE`;
`tools/rp2350-run.py` drives that, and reads the transcript the halt writes.
Only core 0 runs; core 1 waits in the bootrom.
A watchdog armed at boot and never fed reboots the chip into BOOTSEL after about seventeen seconds,
so a run that hangs comes back without a hand on the board, and no run lasts longer.
Measured on an A2 chip: eight PMP entries before three hardwired ones,
a 32-byte grain the probe does not see, `mtval` always zero,
and misaligned accesses that raise misaligned exceptions rather than split.
User mode reaches a peripheral only where ACCESSCTRL lets it in, which at reset it does for few;
the hardwired PMP entries leave every peripheral and the Non-secure bank of SIO to user mode,
so a peripheral ACCESSCTRL opens is open to every process, frame or no frame.
TIMER0 is opened, for the clock; see `TODO.md`.

## Bounded work

**The rule.**
The work of a system call, a tick or an interrupt
is bounded by constants of the machine and the kernel:
PMP entries, interrupt lines, timer lines, units of time, region slots, the size of an object.
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
A revoke, a delete below a root, a pool destroy, and the revoke that begins a bind
take one step per capability, object or waiter in constant time
and ask `intr_pending` between two steps.
If the tick or a device interrupt is pending, the walk stops,
`syscall_dispatch` puts the thread back on its `ecall` with its registers as they were,
the `mret` takes the interrupt through `mtvec` as in any user code,
and the thread makes the same call again when it next runs, as in seL4.
The kernel only notices the interrupt; the processor takes it.
The progress stays in the derivation tree and in the pool:
the next step is always at the first child of the slot the call names,
or at the newest object of the pool being destroyed and the slot its descriptor says,
or, for a notification of it, at the first thread still waiting on it,
so the kernel needs no record of where it stopped, no second stack and no worker,
and the time lands in the slice of the thread that made the call.
A pool's destroy goes on from where it stopped whichever revoke takes the next step,
below the Untyped that began it or below one above that.
A walk asks only after a step, so every attempt takes something away and the restarts end.
Whether `intr_pending` is right changes when a walk stops, never what it does.
A restart is a new call:
it checks everything again, and revokes what was derived in between too.
A revoke that takes its caller's table, the caller's process,
or the capability the call was made through ends at that step:
nothing can make the call again,
so where an interrupt landed would otherwise decide how far it got.
A pool the revoke destroys takes none of them,
since a revoke below an Untyped refuses when the caller's process or table,
or the table the call names, lies in the Untyped's memory;
see "Kernel pools and revocation".
So a bind revokes only once every check that can fail has passed,
its room in the pool among them, and builds after.

**The check.**
Every loop a trap can run says what bounds it with an annotation from `kernel/work.h`:
a constant, what it pays with, a row of the table below, an argument, or a wait on the hardware.
An annotation is a `_Static_assert`, so it changes no code the compiler makes.
The kernel's link runs `tools/loop-bounds.py`,
which finds the loops in the machine code, inlined and compiler-made ones too,
matches each to its source loop through the debug information and clang's AST,
and fails on one that says nothing.
The table is checked both ways, so it stays the kernel's own list of walks.
Code that leads only to a halt is left out,
and so is the self-check, which may be called only under `if (debug_trace)`.

The claims are checked too.
A bound the loop's own shape limits, a counter below a constant, is compared with that limit at the link;
this is the only check of board code, which the host does not run.
The shape is read from the source, so a loop the compiler unrolled is held to its bound too.
A wait must wait on every way round, which the link checks too:
a `wfi`, a load from a fixed address outside RAM, or a call to a function holding a `wfi`.
Other bounds rest on an invariant, as `i < img->count` does,
and a paid loop names its unit: a node, a link, an object or a waiter.
The host harness `fuzz-work` counts both after every call:
a bound per entry of its loop,
and paid steps at most twice what the call took away of that unit, plus one;
twice since a delete below a root makes a root of a node, a link each,
and a pool destroy may then clear that node as one its tables hold.
It also fails a paid loop that takes two steps without asking `intr_pending` between them,
so a walk that is paid for is a walk that is preempted,
and a `memset` or `memcpy` longer than the `CALL_BOUND` before it.
It finds a false claim only where the corpus reaches.
The host answers every `intr_pending` with yes, the worst case,
so each preemptible call stops after its first step, is made again,
and the self-check runs between any two steps;
every host harness requires a stopped call to have taken a node, a link, an object or a waiter away,
and to leave its caller running, holding its table and the capability it made the call through,
so that it can make the call again.

**Where the kernel falls short.**
Nowhere, since `Untyped` and `Frame` replaced the overlap checks and the sweep;
see "Kernel pools and revocation" and open decision 14.
A walk the kernel takes on again is listed here with its fix,
and the check requires the table to name every one and nothing else.

| Walk | When | Fix |
|---|---|---|

**Zeroing goes with the object.**
`pool_alloc` zeroes each object, at most `OBJ_MAX_SIZE`, a table of `CAPTABLE_MAX_SLOTS`,
and a destroy zeroes each object as it forgets it, paid for by the call that made it.

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
The kernel's link runs `tools/stack-depth.py`,
which reads the frames from `-fstack-usage` and the calls from the disassembly,
fails on a broken rule or a bound above `KERNEL_STACK_SIZE`,
and prints the bound and its chain.
Both tools read the kernel linked alone, once for all its images.
An image adds its root task in `.user_code`, which neither tool reads,
and its link fails unless the rest of it is the checked kernel byte for byte.
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

These invariants must hold in every state a process can reach,
or, where they say so, across every call.
`kernel/selfcheck.c` is the executable form of those about one state;
keep the two in step.

**Isolation.**
No region installed in any process overlaps a pool,
so user mode never has a mapping to memory that holds kernel objects.
No installed region lies outside the memory
the root task was granted at boot,
and the granted memory does not overlap the kernel's, which lies below its log,
so the kernel's own memory is unreachable,
the log excepted: it holds no object and can never become a pool.
Memory that may hold kernel objects, an Untyped or a pool,
overlaps another node standing for memory, an Untyped, a pool, a frame or an installed region,
only where one lies below the other,
or where one of the two is a root Untyped that made something,
which a delete below it leaves as it makes roots of its children one by one.
So no frame overlaps a pool, and no Untyped makes a block over anything not below it.

**PMP fidelity.**
For every process, the PMP image the kernel built
and the region slots describe the same function
from address to access rights:
no byte is accessible with a right its slot does not grant,
and every byte in a slot is accessible with the slot's rights.
For the running process the same holds for the CSRs actually written.
On the ESP32-C6 no process has a region it may write ending where one it may only read begins;
see "Region slots".

**Authority confinement.**
Every capability in every table names either
a frame or an Untyped within the granted memory,
a NAPOT block no smaller than the smallest region,
with rights no greater than the root task received for it
or a range of lines the interrupt controller has, the log's among them,
with no more than the right to bind,
or a range of the units of time there are, with no more than the rights to bind and to run on spare time,
or the debug capability or the clock, which name no object,
or a live kernel object of the capability's own type.
Every process's table slot is empty or names a live table,
every thread's process slot is empty or names a live process,
every thread's watch is empty or names a live notification,
and every `Irq`'s notification slot is empty or names a live notification.

**Derivation.**
The slots of every live table,
the table slot and region slots of every live process,
the process, units and watch of every live thread,
the notification of every live `Irq`
and the own node of every pool
are the nodes of one forest.
Every link of a filled node lands on a live, filled node;
an empty node has no links.
The children of a node form one ring that closes through the node,
every node on it linked up to that node,
and every node's previous link names the sibling whose next is the node,
the last sibling for the first;
a root has no siblings and no previous link,
and a capability to a pool or an object is never one, nor a thread's process or watch or an `Irq`'s notification.
A node is derived from its parent:
the same object with no more rights,
a range within the parent's range with no more rights,
a pool's own node on an Untyped, an installed region on a frame,
a thread's units on a capability to units that holds them, the first of them even for none,
a thread's process or watch or an `Irq`'s notification on a capability to that object,
to its pool, or on its pool's own node,
the counter's block, or a region installed from it, below a clock,
or a capability to a pool or an object below a capability to that pool or its own node.
A revoke, a delete or a destroy stopped for an interrupt leaves such a forest too,
since every step of theirs does; see "Bounded work".

**Structural soundness.**
Every pool's descriptor sits at its base and holds the pool's own node,
its objects tile the space from the descriptor to the used mark exactly,
each object records the pool it lies in and where the one before it lies,
the newest is the one the descriptor names,
and pools are pairwise disjoint.
A pool half destroyed is a pool like any other, with fewer objects.
The line table names exactly the bound `Irq`s,
and every installed region records its slot.

**Thread state.**
Every thread is stopped, ready, or waiting,
and has one binding to units or none, a leaf of the derivation tree
made through a capability with the right to bind.
A thread's process is a leaf too, and a thread without one is stopped.
So is a thread's watch, set through a capability with the right to signal, and it signals some bit.
A waiting thread names a live notification and nothing else does,
and a notification's queue holds exactly the threads waiting on it.
A ready thread but the running one that may run waits on exactly one of the scheduler's queues and names it:
the run queue while it has time, else the spare queue with spare time,
else the spent queue if it has units;
every other thread names none and is linked into none,
and the queues hold exactly the threads that name them.
While a thread runs it is its turn.
The thread the kernel is running is one it could run:
it is a live object and it is ready,
though it may have lost its units during its turn, which it finishes.
A preempted thread stays ready and joins its queue,
so it runs again while it keeps its units or its spare time.

**Units and accounts.**
Each of the `TIME_UNITS` units names the one thread bound to it, or none.
Every account holds at most `ACCOUNT_TICKS` ticks' gain for each unit, and so nothing without units,
and was last counted no later than the count;
a thread has time exactly while its account holds a tick.
The thread whose turn it is has its account counted up to the count,
since every tick of its turn is charged to it as it passes,
and under tracing, where the clock is the tick count, it owes nothing from before the tick.
A thread with units that waits without time reaches a tick no earlier than the release,
and the release lies ahead of the count.

**Interrupts.**
Every `Irq` names a line there is: the log's, the controller's or a timer line.
Its notification is a leaf of the derivation tree,
bound through a capability with the right to signal,
and an `Irq` without one is disarmed.
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
The nearest deadline the timer is set for lies ahead of the count
and no later than the deadline of any `Irq` armed on a timer line.

**The timer.**
While a thread runs, every tick the timer lets pass would hand the processor back to it:
nobody is on the run queue, and it has time,
or may run on spare time with nobody on the spare queue,
and none of those ticks is the release, the nearest deadline,
or, unless it may go on alone on spare time, the tick at which its account leaves it less than a tick.
The tick the timer was last set for lies ahead of the count and no later than that first tick,
unless a change since marked it stale.

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

**Authority only flows.**
A call leaves no capability its caller could not have made.
Every node it fills or changes is covered by a capability
the caller's table held before the call:
the same object, or a range within its range, with no more rights,
where an Untyped covers the frames it makes,
and a thread's units are covered as a range of units, with the rights it was bound with,
and its watch as a capability to its notification, with the rights it was set with,
each only on a thread the caller held a capability to with the right to control it.
Or it names an object the call built,
in a pool the caller could allocate from,
bound to what the caller could write,
and a pool on memory the caller held an Untyped to, with read and write,
and a clock covers the counter's frame, read only.
So a thread earns units only through a capability to them with `RIGHT_W`,
and runs on spare time only through one with `RIGHT_X` besides.
This is goal 2 across a call.
It measures a copy against all the caller held, not against its source,
so a copy wider than its source but within another capability of the caller passes;
the derivation invariant checks it only against the parent it shares with its source.

**Memory crosses zeroed.**
Every object that leaves the pools holds nothing but zeros,
whether its pool went or a destroy stopped half way,
and every object holds nothing of its memory from before the pool took it.
No byte of an object reaches a process when its pool is destroyed,
and no byte a process wrote becomes part of an object when its memory becomes a pool.
The rest of a pool's memory comes back as it went in; see "Zeroing goes with the object".

**The caller keeps what it runs on.**
No call begins to destroy the pool its thread, its process or its process's table lives in.
A destroy stopped half way leaves the pool's threads running,
so this is what keeps a thread from making a call it cannot return from.

**A tick charges.**
Under tracing, where the clock is the tick count,
a tick costs the thread whose turn it is a whole tick while its account holds one,
and earns it its units, as every tick does;
a call that moves no time costs it nothing.

**A move keeps a node's place.**
A move that succeeds leaves its source empty
and its destination holding the same capability,
below the same parent, or a root where the source was one,
and above the same children, in their order.
The tree's shape alone does not say so:
a copy beside the source and a delete of it leave a whole tree too.

These five relate the state before a call to the state after it,
so the self-check, which sees one state, cannot check them.
`host/history.c` checks them around every call of the host build, the untraced prologue's too,
and around every fault, which must change no more than a call that moves no time,
but a move's place only around a traced call,
since it follows the tree's links and only the self-check vets them.
It does not know what memory held before a call,
so an object handed out unzeroed shows only by what it holds:
the host's RAM starts out holding a pattern and keeps the objects of the input before,
and a new table's stale slots are capabilities its maker held nothing to cover.

**Exit to user mode.**
Every `mret` enters user mode,
at the running thread's saved program counter with the registers of its own frame,
with the PMP holding its process's image,
and with no reservation, which `trap_return` breaks.
The kernel writes `mstatus.MPP` once, to user mode, before the first `mret`;
a trap from user mode sets it to user mode again,
and a trap from machine mode halts.
`trap_handler` refuses a frame that is not the running thread's
and hands back the running thread's,
and under tracing the self-check compares the PMP CSRs with the running process's image.
The rest lives in `start.S`, which the host build does not have,
so only the demo and the replay under QEMU run it,
and no check says what it does wrong; see `TODO.md`.

## Verification

**Host fuzzing** (`make host-test`, `make fuzz`).
The kernel's logic compiles natively with a shim for `kputc`,
the PMP CSRs and halting, and physical addresses indexing a RAM buffer;
an address outside it breaks "Memory safety" and is reported as any invariant is.
So does an access inside it to anything but an object or the log:
the host poisons the rest of RAM for ASan, and the pools move the poison as objects come and go.
ASan thus sees the kernel reach a pool's free space, a destroyed pool, memory no pool was made of,
or the slot past a table's last, which begins in the table's padding,
but not one object reach into the next, right behind it as on the target; see `TODO.md`.
Where each input runs as if alone, ASan goes on after the report, so that the inputs after it still run,
and the input ends as the call returns.
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
Around every call `host/history.c` compares the state before with the state after,
for the properties that relate them; see "Properties".
Since no system call takes a pointer,
the registers are the whole attack surface.
The mutator in `host/mutator.c` works on whole records:
it inserts, deletes, swaps and moves them,
sets one field to a value in its range,
and splices two inputs on record boundaries,
so that a handoff between the driver's threads is one mutation, not a guess per byte.
The inputs replayed are checked in under `tests/seeds` and `tests/corpus`.
`make mutants` and `make qemu-replay` replay them in one harness process,
each input from RAM as at power-up and ending at its first report, as if it ran alone,
since starting the harness costs more than most inputs do.
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
`git apply` uses line numbers only to break a tie,
and `make mutants` lets it drop the context to one line,
so a hunk that only moved keeps its old numbers
unless its lines with one line of context match in several places.
A refresh thus rewrites only the patches whose text changed.
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
hands its place over to a successor, which destroys the pool the root task lived in,
has a thread of its own fault on a region it took away and hears it through the thread's watch,
maps the region again and resumes the thread, whose load goes through then, and halts;
`tests/run.sh` checks that the logger carried the transcript out
before the halt did, by where the halt's line falls in it.

`make escape` boots the escape-attempt suite,
one root task per scenario, each attempting a single breach of its confinement.
A fault stops the thread that makes it, and a scenario's root task has one,
so the machine stops with nothing left to run, and a scenario tests one thing;
`tests/escape.sh` checks that the fault is the one expected
and that the scenario did not reach the line it prints only when the escape works.
It executes from the no-execute data region and jumps into the kernel, mcause 1,
reads a machine-mode CSR from user mode and runs `mret` there, mcause 2,
loads a misaligned word across the end of the data region, mcause 5,
stores one from a region it may write into one it may only read, mcause 7,
stores into the kernel, mcause 7 too,
and loads the word just past a frame of the smallest region's size, mcause 5,
which a kernel that took the grain for finer than it is lets through.
A core that raises misaligned exceptions instead, as RP2350's does, reports 4 and 6 for the misaligned two.
The user linker script says where the kernel starts, for these scenarios alone.
The fuzzer's whole attack surface is a system call's registers,
so this suite is what runs real instructions on the real core
to check that PMP and the privilege boundary confine a process;
it runs on the target alone, like `make test`, never on the host.

The replay driver carries the log to the UART after every record,
by polling and with no system call, so the transcripts stay the same on both builds,
and the host writes the same mark into the header after every event;
the halt writes out what the last record left.
`make qemu-replay` boots the replay driver once per corpus input
with the input placed in RAM by QEMU's loader.
The driver runs two threads that take records from one cursor,
so a record that blocks one of them leaves the kernel
something else to run and the blocking paths are replayed too.
A third shares the second's process, lives in the first's pool, and starts stopped,
so a record can resume it when two threads should wait at once,
and a destroy of the second's pool leaves it stopped without a process.
All three earn no units and run on spare time, so they take turns as one queue
and passing a record round visits every runnable thread,
until a record binds one of them to units, and so to time the others do not have.
A record that names a thread is passed to it with `OP_DEBUG_TICK`
by the protocol in `include/rvuos/replay.h`,
which the host runs from the kernel's state;
the passing is system calls, so both transcripts carry it.
The second thread has a pool, a process and a table of its own,
so every switch between them reloads the PMP under the self-check's eye,
and memory of its own, an Untyped derived from a quarter of the free RAM, as the first thread has half of it,
so a pool it makes lies below the free RAM the first thread holds,
a record on the first thread can destroy both at once with a revoke,
and the cascade is replayed like any other path.
Each table holds the block the other thread's pool was made of,
so a record on the first thread destroys the second thread's pool,
one on the second destroys the boot pool and the first thread with it,
and either is refused its own.
The state both builds start from is in `include/rvuos/replay.h`
rather than written out twice.
`OP_DEBUG_TRACE` makes the kernel print one line per call
and run the self-check after it, reading the PMP CSRs back.
`tests/differential.py` requires the host and QEMU transcripts
to match line for line,
and fails an input whose transcript ends in an invariant report or a kernel panic,
even where the two agree.
The host's transcript takes in what `host/history.c` reports,
and one that crashes the host fails.
The host has no instruction fetch to fault,
so it makes a driver thread fault as soon as it runs
without its code or its data mapped with the needed rights,
or without the UART or the log when it drains the log, as it does after each record it performs,
and every driver thread that runs once the records are done fault on the breakpoint the driver ends with;
that is the one place where the host models rather than executes.
A fault stops its thread alone, so on both builds the next thread goes on with the records,
and one resumed where it faulted goes on too;
the transcripts compare the line of each fault, but not its frame, whose addresses the host does not know.

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
and moves no time: timer lines fire only through `OP_DEBUG_TICK`,
and so do accounts get charged and fill.
`OP_DEBUG_TRACE` fills every account,
so the host, which boots with them full, starts from the same accounts as QEMU,
where the driver ran untraced for a while before and a tick may have come.
The clock that charges a turn is then the tick count alone, on both builds:
a turn with time pays a tick at each `OP_DEBUG_TICK` and nothing at a wait.
The fill clears what the running thread owed from within the tick,
and the host stands its counter half way into the tick while untraced, so that there is something to clear.
The charge at a change of turn is checked by the demo in `user/init.c`,
by a thread held to an eighth that sleeps across every tick;
see `TODO.md` for what nothing checks.
The interrupt is still taken and acknowledged on QEMU,
so the replay exercises the interrupt entry and return,
but a switch happens only when a thread waits
or when a record asks for the tick through `OP_DEBUG_TICK`.
A traced run in which every thread waits, or waits for an account to fill, therefore stops
even with a timer line armed, since no record is left to fire it,
and the host build, which has no clock, agrees.
A tick pending in a revoke stops it on QEMU as anywhere,
but a stopped call is not traced, only the attempt that finishes,
so where the tick lands leaves the transcript alone,
and the host may stop every such call without a line of its own.
The stall in `wfi` for an armed timer line is checked by the demo in `user/init.c`,
and so are periods that keep pace with the clock,
bounded on both sides, which a stall that miscounted the ticks it skipped would break;
the host never stalls, so nothing else checks the deferral.
The ticks the timer lets pass while a thread runs are checked by the demo as well,
by a thread that spins alone and counts the gaps a trap leaves between two reads of the counter.
The host sets no timer and counts no ticks at a trap,
but the self-check holds the tick the kernel would set the timer for to "The timer" in "Properties",
so a tick let pass that ends the turn, drains an account or fires a line shows on the host too;
and the host does after every call what a trap does on its way back,
so a change that brings that tick forward without marking it stale shows there as well.
Under tracing no trap counts ticks, and the timer is left on every tick,
which the traced interrupt only moves on.
The demo also holds a spinning thread to an eighth of the processor with nothing else to run,
so the kernel stalls for its account to reach a tick, and then lets it run on spare time;
the host replays accounts being spent and filled, but never that stall.
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
that is the only check that reaches its timer, matrix and USB console,
and the user-mode CSRs the kernel sets back.
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
   the ESP32-C6 is the first real board and RP2350 the second; see "Boards".
   QEMU implements 16 PMP entries;
   `PMP_MAX_ENTRIES` caps how many the kernel uses,
   so the design can be exercised with a budget of 8 or fewer.
   The ESP32-C6 is a single RV32IMAC core with user mode, PMP and 512 KiB of SRAM,
   and it runs the demo root task from RAM, loaded by its ROM.
   Booting from flash is not done yet;
   what it needs from Espressif's second-stage bootloader,
   and whether execute-in-place goes through a cache the kernel must control,
   are listed in `TODO.md`.
   RP2350 runs from RAM too, loaded by its bootrom.
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
   Decided, by the derivation tree, and since decision 14 by the Untyped:
   a pool's memory goes back to the Untyped it was retyped from,
   so to the lender's, never to a borrower's.
   A lender that wants it back from a living borrower
   revokes below its own Untyped, which destroys the borrower's pools.

8. **TOR or NAPOT.**
   Decided: NAPOT only; see "Physical Memory Protection".
   Before, a region was any range on the grain, written as TOR entries.
   That saved RAM to rounding,
   but a region cost one or two entries depending on its neighbours,
   two ranges could overlap in part,
   and cores without TOR, RP2350 among them, were out.
   RP2350's PMP is no longer in the way,
   but its hardwired entries leave every peripheral to user mode,
   and its `mtval` reads zero; see "Boards".

9. **Scheduling policy beyond units.**
   Decided in part: units of time earned by one thread each, an account per thread they fill,
   and spare time only by `RIGHT_X`; see "Scheduling" and open decision 18.
   Working default for the rest: no priorities;
   units promise a thread their part of the processor, give or take its account,
   and a thread with time a turn within `TIME_UNITS` ticks, and no more.
   The earlier plan was fixed priorities on `Thread`,
   set by whoever holds the capability and capped by the setter's own,
   round-robin within a priority, no priority inheritance;
   with units they could be a right of the `Time` capability a thread is bound through, as spare time is.
   The alternative is a scheduler in userspace
   that decides which threads are runnable at all.
   It stops a thread by taking back the units it lent it and starts it by binding it again,
   and has a tick of its own in a timer line, the shape every device interrupt takes.
   What it cannot do is choose between two runnable threads
   for less than a system call per switch.
   A period per thread, as seL4's scheduling contexts have, would bound latency more tightly,
   at the price of a queue of refills sorted by time.
   Decide when a workload needs one thread to run before another,
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
    but what its ROM overwrites on the way back in is not measured yet;
    bytes one run wrote at `0x40838000` were still there after the reset and the next load.

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
    Decided: seL4's split; see "Kernel pools and revocation".
    Before, one `Region` type could be installed and could become a pool,
    and overlap checks over every pool and every process kept the two apart,
    which goal 4 rules out;
    a peer holding a copy of a region could also make it inert by pooling it.
    The questions the proposal left are decided so:
    - deleting the Pool capability a retype returned neither refuses nor destroys,
      since the retype's own node lies in the pool's descriptor;
    - a revoke below an Untyped refuses when the caller or its table lives
      in the memory it would destroy, a test by address;
    - a frame carries the rights of the Untyped it was made of;
    - a stopped destroy keeps its progress in the pool:
      the objects go newest first and the used mark with them.

    The price is seL4's: the root task retypes before it installs anything.
    The watermark a retype took each block past, which seL4 has too, went for halves; see decision 20.
    Besides, a grant made through a dying table now outlives it,
    and revoking below a line capability no longer takes an `Irq` bound from it;
    see "Interrupts".

15. **What a process's table slot could do.**
    Working default: the slot is filled once, at allocation, and its rights mean nothing.
    - **Replacing the table.**
      An operation on a `Process` could refill the slot;
      every thread would see the new table from its next call.
      With flat slot numbers a full table then grows by a table's worth of calls:
      a larger table, every capability moved over with `OP_CAP_MOVE`,
      which a copy could not do, since it leaves behind what was derived from its source,
      and the slot refilled.
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
    Decided: `IRQ_SET_PERIOD` counts a timer line's next deadline from its last one,
    and the tick keeps pace with the counter; see "Time".
    Before, a periodic task drifted by each wake's lateness,
    and each tick was set a period after its handling, a few percent slow under QEMU.

17. **CPU time per thread.**
    Working default: none; the clock gives the machine's time, which includes other threads' slices.
    Only the kernel sees a switch, so only it can count,
    and it reads the counter at every change of turn already, to charge the turn;
    what is left is adding it to the thread leaving, eight bytes per thread.
    Reading a thread's time would be an operation on the clock that names a thread,
    because a thread that can read its own time has a clock.
    A read of the counter by system call could join it,
    for a board whose counter no region can show, or to spare a region slot.
    Decide with the first program that needs it.

18. **What a thread's account counts, and when it has time.**
    Working default: the counts a turn ran, time again at a tick, and spare time free.
    The account counts in parts of a count, charged at each change of turn and at each tick,
    but a turn ends on the tick grid, which it never outruns, since it began with a tick in the account;
    under tracing the clock is the tick count, so the host, whose clock is the ticks the records move, decides as QEMU does.
    Time again at a tick, a turn's worth, bounds how long a thread with units waits for its account:
    held to an eighth, a thread runs a tick in every eight.
    The shares had time again only once their account was full,
    which kept a capped share's idle time in one stretch,
    13 ticks of running and about a hundred idle at an eighth,
    where a tick in every eight wakes an otherwise idle core 125 times a second.
    Spare time costs nothing, so an account never goes below zero.
    Before, a tick cost whoever it found running a whole tick, so a thread that waited just before every tick paid nothing.
    The alternatives are time while the account holds anything, with turns that end mid-tick where it drains,
    which would wake the core several times a tick for a thread held to its units;
    a higher threshold, per thread, that trades latency for idle stretches;
    and spare time charged as a debt.
    Decide when a board's sleep states, or a workload's latency, make the difference measurable.

19. **A right to destroy a pool.**
    Decided: there is none; the holder of a pool's Untyped destroys it, and a Pool capability only allocates.
    So lending a pool lends allocation, and lending an Untyped lends the power to destroy what is made of it.
    Before, `OP_POOL_DESTROY` destroyed a pool through a Pool capability,
    so a borrower could destroy every process living in the pool, the root task in the boot pool among them.
    Since decision 20 a pool is the whole of its Untyped, so the operation did what that revoke does.
    The price is a slot per pool for a program that gives pools back one by one,
    and the boot pool's block as a boot capability of its own, `BOOT_CAP_POOL_RAM`.

20. **Halves instead of a watermark.**
    Decided: an Untyped makes one thing of the whole of its memory, a frame, a pool, its two halves
    or a derived Untyped, and nothing while any of those is left;
    see "Kernel pools and revocation".
    Before, a retype took the next block of a given size past a watermark,
    so the kernel chose where each block lay, alignment left gaps,
    and a block given back waited for everything else its Untyped had made.
    Now the program chooses by the half it takes, and a block given back is free again at once.
    The price is the program's: its free memory is capabilities, one a block, in a table of fixed size,
    and a small block out of a large one is a split per halving.
    Still open: no operation joins two free halves whose parent was deleted;
    they join only when their grandparent is free again.
    Decide when a program's free list wants it.

21. **What a fault tells.**
    Working default: the watch's bits say which thread faulted, and the kernel's log says why, to whoever reads it;
    no call returns the cause; see "Faults".
    A watcher that restarts or ends the thread needs no more.
    One that maps regions on demand knows what it withheld but not which address was reached for,
    and one that emulates an instruction the core lacks needs `mepc` to find it and to step past it.
    An operation on a stopped `Thread` returning `mcause`, `mepc` and `mtval`, which its frame keeps already,
    would serve both, if it can tell a thread that faulted from one that never ran.
    Decide with the first watcher that needs the cause.

22. **User-mode CSRs nothing shuts.**
    Working default: on the ESP32-C6 the kernel sets them back at every change of process; see "Boards".
    So a program cannot keep the counter or the dedicated GPIO across another process's turn;
    saving and restoring them with the process would, at some forty bytes a process.
    The counter counts the kernel's cycles too,
    so a process that starts it still sees how long the kernel took over an interrupt while no other process ran.
    The dedicated GPIO reaches a pad only through the GPIO matrix,
    and a pad routed to it is every process's whatever the kernel sets back;
    no process holds the matrix today, so the first GPIO driver decides who may route one.
    Decide with the first program that wants the counter or the dedicated GPIO kept.
