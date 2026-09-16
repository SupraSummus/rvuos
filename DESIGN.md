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
but coarsens revocation to whole memory pools,
which removes the derivation tree entirely.
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
  and the only user memory the kernel touches on a process's behalf
  is its IPC buffer,
  which is a region the kernel already knows.

### One address space

There is a single physical address space shared by everything.
Processes are linked at fixed addresses chosen by whoever builds the image,
or built as position-independent code.
RISC-V code is naturally PC-relative,
so position independence costs little,
and the root task can place processes wherever memory is free.

Code normally executes in place from flash.
Data, stacks, and IPC buffers live in SRAM.
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

A slot is twelve bytes: type, rights, a sixteen-bit generation,
and two words.
For object capabilities the first word is the object's address
and the generation must equal the object's current generation.
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
| `Thread` | An execution context inside a process: registers, priority, state, IPC buffer. |
| `Endpoint` | A synchronous rendezvous point for IPC. |
| `Notification` | An asynchronous word of signal bits. |
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
Destroying a pool destroys every object inside it
and invalidates every capability that pointed at any of them.
This is the deliberate simplification relative to seL4:
there is no per-object derivation tree,
so a pool is the unit of trust.
A process that wants to revoke one object cheaply
allocates it from its own small pool.

Capabilities must not dangle after a destroy.
Each object carries a generation counter
and each capability records the generation it was created against,
so a stale capability can be recognised when used.
How destroy uses that is open decision 5:
once the pool's memory is user RAM again,
a user can forge a header with the right generation,
so either destroy sweeps every table for capabilities into the pool,
or the memory stays quarantined until such a sweep.

Objects reference each other in two ways,
and destroy must cope with both.
A capability carries a generation and is checked on every use.
A structural pointer, such as a process to its table
or a thread to its process, is not checked on use,
so a structural parent must live in the same pool as its child
or outlive it;
the kernel will enforce the same-pool rule at allocation
once processes and threads can be allocated.
Threads queued on an endpoint or notification
are unlinked when either side is destroyed.
Pool destroy is not implemented yet.

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

## IPC

Not implemented yet; roadmap step 4 in `TODO.md`.
IPC follows seL4's synchronous rendezvous model.

- A thread `call`s an endpoint and blocks until a receiver takes the message
  and replies.
- A thread `recv`s on an endpoint and blocks until a sender arrives.
- The receiver gets a one-shot reply capability
  so it can answer exactly that caller.
- Messages consist of a small number of message registers
  passed in `a1` through `a6`,
  plus an optional tail in the sender's IPC buffer
  which the kernel copies into the receiver's IPC buffer.
- Up to a small fixed number of capabilities can travel with a message.
  The kernel copies them into the receiver's table.

Large payloads do not travel through IPC.
The sender grants a `Region` and sends the capability,
and the receiver installs it into a slot.

`Notification` objects provide the asynchronous path.
A `signal` sets bits and never blocks.
A `wait` blocks until any bit is set and returns and clears them all.
Interrupts arrive as signals on the notification bound to an `Irq`.

## Scheduling

Not implemented yet; roadmap step 5.
Fixed-priority preemptive scheduling with round-robin within a priority.
The machine timer provides the tick.
A thread's priority is set by whoever holds its `Thread` capability
with the control right,
and cannot exceed the setter's own priority.

Priority inversion through IPC is handled the simple way for now:
no priority inheritance.
A server that must not be blocked by low-priority clients
runs at a higher priority than all of them.
Scheduling contexts in the seL4 MCS style are a possible later addition
and the `Thread` object leaves room for one.

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
2. discovers the PMP entry count,
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
a region within the granted memory
with rights no greater than the root task received for it,
or a live kernel object of the capability's own type
with a matching generation.

**Structural soundness.**
Every pool's descriptor sits at its base,
its objects tile the space from the descriptor to the used mark exactly,
each object records the pool it lies in,
and pools are pairwise disjoint.
A process and its table, and a thread and its process,
lie in the same pool.

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
libFuzzer feeds system call records into the root thread's registers
and the self-check runs after every call, under ASan and UBSan.
Since no system call takes a pointer,
the registers are the whole attack surface.
The minimised corpus is checked in under `tests/corpus`.
`make mutants` plants three bugs in the kernel one at a time
and requires the replay to catch each.

**QEMU** (`make test`, `make qemu-replay`).
`make test` boots the real kernel and checks the root task's transcript,
which ends in a deliberate fault.
`make qemu-replay` boots the replay driver once per corpus input
with the input placed in RAM by QEMU's loader.
`OP_DEBUG_TRACE` makes the kernel print one line per call
and run the self-check after it, reading the PMP CSRs back.
`tests/differential.py` requires the host and QEMU transcripts
to match line for line.
The host has no instruction fetch to fault,
so it declares the root task dead
when its code or data is no longer mapped with the needed rights;
that is the one place where the host models rather than executes.

**Hardware.**
QEMU's PMP may differ from a real core in granularity or Smepmp behaviour.
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
   a single RV32IMAC core with user mode, PMP and about 512 KB of SRAM,
   pending the datasheet checks listed in `TODO.md`.
   Other candidate: RP2350 on its Hazard3 cores.
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
5. **Pool destroy and stale capabilities.**
   Working default: none yet, destroy does not exist.
   Options: sweep every table on destroy and clear capabilities
   into the pool, which makes generations redundant;
   or keep destroyed memory quarantined until a sweep.
   Decide before roadmap step 7; it may remove `generation` from slots.
