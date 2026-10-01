# rvuos design

This document records the architecture of rvuos
and the reasoning behind each decision.
Code that disagrees with it is a bug in one of the two.
Work items are in `TODO.md`.

The first four chapters are in this file, and the others in `design/`.
Code and documents cite a section by its title, as in `DESIGN.md`, "Scheduling",
and the contents say which file holds it.

Contents:

- [Goals](#goals)
- [Programs are plain binaries](#programs-are-plain-binaries)
- [Non-goals](#non-goals)
- [Prior art and how rvuos differs](#prior-art-and-how-rvuos-differs)
- [Hardware model](design/hardware.md#hardware-model)
  - [Privilege modes](design/hardware.md#privilege-modes)
  - [Physical Memory Protection](design/hardware.md#physical-memory-protection)
  - [Timer](design/hardware.md#timer)
  - [Interrupt controller](design/hardware.md#interrupt-controller)
  - [One address space](design/hardware.md#one-address-space)
- [Kernel objects and capabilities](design/objects.md#kernel-objects-and-capabilities)
  - [Capabilities](design/objects.md#capabilities)
    - [Invocation ABI](design/objects.md#invocation-abi)
    - [Slot format](design/objects.md#slot-format)
  - [Object types](design/objects.md#object-types)
  - [Kernel pools and revocation](design/objects.md#kernel-pools-and-revocation)
  - [The derivation tree](design/objects.md#the-derivation-tree)
  - [A process's table](design/objects.md#a-processs-table)
  - [Region slots](design/objects.md#region-slots)
  - [Threads](design/objects.md#threads)
  - [Faults](design/objects.md#faults)
- [Communication and synchronisation](design/objects.md#communication-and-synchronisation)
- [Time](design/time.md#time)
- [Scheduling](design/time.md#scheduling)
- [Interrupts](design/io.md#interrupts)
- [The kernel log](design/io.md#the-kernel-log)
- [Boot](design/boards.md#boot)
  - [The root task is its capabilities](design/boards.md#the-root-task-is-its-capabilities)
- [Boards](design/boards.md#boards)
- [Architectures](design/boards.md#architectures)
- [Bounded work](design/bounds.md#bounded-work)
- [Bounded stack](design/bounds.md#bounded-stack)
- [Properties](design/verification.md#properties)
- [Verification](design/verification.md#verification)
- [Open decisions](design/decisions.md#open-decisions)

## Goals

rvuos is a microkernel for RISC-V microcontrollers,
and for ARMv7-M and ARMv8-M ones with an MPU through the same programming model; see "Architectures".
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
or with no header at all and the right `ecall` or `svc` sequences,
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
  so what a second core would need of its own is gathered in `struct core`; see open decision 24.
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
