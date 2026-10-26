# rvuos user manual

This manual is for developers who want to write programs for rvuos,
or port it to a new board.
It describes what the kernel is, what it promises,
and how a program talks to it.
`DESIGN.md` explains why the kernel is built this way.

Contents:

1. [What rvuos is](#1-what-rvuos-is)
    - [Words this manual uses](#words-this-manual-uses)
2. [Features at a glance](#2-features-at-a-glance)
3. [Hardware requirements and targets](manual/targets.md#3-hardware-requirements-and-targets)
    - [Boards](manual/targets.md#boards)
      - [QEMU `virt`, RV32](manual/targets.md#qemu-virt-rv32)
      - [ESP32-C6](manual/targets.md#esp32-c6)
      - [ESP32-C5](manual/targets.md#esp32-c5)
      - [RP2350](manual/targets.md#rp2350)
      - [nRF52840](manual/targets.md#nrf52840)
      - [mps2-an385](manual/targets.md#mps2-an385)
      - [mps2-an521](manual/targets.md#mps2-an521)
4. [Building and running](manual/targets.md#4-building-and-running)
    - [Reading the transcript](manual/targets.md#reading-the-transcript)
5. [The programming model](manual/programming-model.md#5-the-programming-model)
    - [5.1 Processes, threads and regions](manual/programming-model.md#51-processes-threads-and-regions)
    - [5.2 Capabilities and the tables](manual/programming-model.md#52-capabilities-and-the-tables)
    - [5.3 Objects](manual/programming-model.md#53-objects)
    - [5.4 Memory: Untyped, frames and installing](manual/programming-model.md#54-memory-untyped-frames-and-installing)
    - [5.5 Pools](manual/programming-model.md#55-pools)
    - [5.6 Threads](manual/programming-model.md#56-threads)
    - [5.7 Notifications](manual/programming-model.md#57-notifications)
    - [5.8 Time](manual/programming-model.md#58-time)
    - [5.9 Interrupts](manual/programming-model.md#59-interrupts)
    - [5.10 The kernel log](manual/programming-model.md#510-the-kernel-log)
    - [5.11 Scheduling](manual/programming-model.md#511-scheduling)
    - [5.12 Lent time](manual/programming-model.md#512-lent-time)
    - [5.13 Routes](manual/programming-model.md#513-routes)
    - [5.14 DMA](manual/programming-model.md#514-dma)
6. [System call reference](manual/system-calls.md#6-system-call-reference)
    - [6.1 Invocation ABI](manual/system-calls.md#61-invocation-abi)
    - [6.2 Status codes](manual/system-calls.md#62-status-codes)
    - [6.3 Operations on `Print`, `Halt` and `Test`](manual/system-calls.md#63-operations-on-print-halt-and-test)
    - [6.4 Operations on `CapTable`](manual/system-calls.md#64-operations-on-captable)
    - [6.5 Operations on `Frame` and `Untyped`](manual/system-calls.md#65-operations-on-frame-and-untyped)
    - [6.6 Operations on `KernelPool`](manual/system-calls.md#66-operations-on-kernelpool)
    - [6.7 Operations on `Process`](manual/system-calls.md#67-operations-on-process)
    - [6.8 Operations on `Thread`](manual/system-calls.md#68-operations-on-thread)
    - [6.9 Operations on `Notification`](manual/system-calls.md#69-operations-on-notification)
    - [6.10 Operations on `IrqLine`](manual/system-calls.md#610-operations-on-irqline)
    - [6.11 Operations on `Irq`](manual/system-calls.md#611-operations-on-irq)
    - [6.12 Operations on `Clock`](manual/system-calls.md#612-operations-on-clock)
    - [6.13 Operations on `Time`](manual/system-calls.md#613-operations-on-time)
    - [6.14 Operations on `RouteLine` and `Route`](manual/system-calls.md#614-operations-on-routeline-and-route)
    - [6.15 Operations on `DmaLine` and `Dma`](manual/system-calls.md#615-operations-on-dmaline-and-dma)
    - [6.16 Describing a capability](manual/system-calls.md#616-describing-a-capability)
    - [6.17 Operation codes in numeric order](manual/system-calls.md#617-operation-codes-in-numeric-order)
7. [What the root task starts with](manual/system-calls.md#7-what-the-root-task-starts-with)
8. [Writing a program](manual/writing-programs.md#8-writing-a-program)
    - [8.1 Toolchain and layout](manual/writing-programs.md#81-toolchain-and-layout)
    - [8.2 Building a second process](manual/writing-programs.md#82-building-a-second-process)
    - [8.3 Writing a driver](manual/writing-programs.md#83-writing-a-driver)
    - [8.4 The library for programs](manual/writing-programs.md#84-the-library-for-programs)
    - [8.5 A program of several processes: the Wi-Fi system](manual/writing-programs.md#85-a-program-of-several-processes-the-wi-fi-system)
9. [Debugging and testing interfaces](manual/writing-programs.md#9-debugging-and-testing-interfaces)
10. [Limits](manual/limits.md#10-limits)
11. [What is not there yet](manual/limits.md#11-what-is-not-there-yet)

Chapters 1 and 2 are in this file.
The other chapters are in `manual/`.

## 1. What rvuos is

rvuos is a capability-based microkernel.
It runs on RISC-V microcontrollers that have no memory management unit (MMU).
It also runs on ARMv7-M and ARMv8-M microcontrollers that have a memory protection unit (MPU).

On RISC-V, rvuos keeps processes apart with Physical Memory Protection (PMP).
It does not translate addresses.
So it does not need supervisor mode,
and it runs on cores that have only machine mode and user mode.
The kernel runs in machine mode.
Every program runs in user mode.

On ARM, the MPU does the work of PMP.
The kernel runs in handler mode,
and every program runs in unprivileged thread mode.
The objects and the operations are the same on both architectures,
and so are the programs written against `user/rvuos.h`.
Where this manual says PMP, machine mode or `ecall`,
ARM uses its own equivalent.

Three goals shape the kernel.
The most important one comes first.

1. **Isolation without an MMU.**
   A process can reach only the resources it was given.
   For memory, these are the regions installed in its region slots,
   with the rights they were installed with.
   It can reach nothing else.
   Two processes share memory only when someone installs that memory in both,
   and that someone must hold a capability to the memory.
   No process can reach the kernel's own memory.
2. **Authority through capabilities.**
   Every system call names a capability in the caller's own tables.
   The kernel gives nothing out by a global name:
   a process cannot look up another process, thread or kernel object.
   A process reaches a device's registers only as a frame installed in one of its region slots.
   A process that holds no capability to a thing cannot act on it,
   even if it knows that the thing exists.
3. **The kernel never allocates memory.**
   The kernel has no heap.
   Every kernel object lives in memory that a user program gave to the kernel for that purpose.
   So a process pays for its kernel objects with its own RAM.

What rvuos is not:

- It is not a POSIX system.
  There is no `fork`, no file descriptor, no signal and no libc.
- It has no virtual memory.
  Every address that a program sees is a physical address.
- It has no drivers in the kernel, and no console.
  Drivers run in user mode, and get their interrupts through capabilities.
  The kernel writes its own output into a log ring,
  and a user program reads it out.
- It does not load code into the kernel at run time.
  All code that runs in machine mode is in this repository.
- It uses at most two cores.
  Built with `CORES=2`, it uses two under QEMU,
  on the two harts of `virt` or the two Cortex-M33 cores of mps2-an521,
  and on RP2350, with either kind of core.
  Every other board uses one core; see section 5.11.
- It is not binary compatible with seL4, L4 or F9,
  but it borrows from seL4's object model.

### Words this manual uses

This manual uses some common words with a narrow meaning.
The sections named here explain each one in full.

| Word | Meaning | Section |
|---|---|---|
| capability | a reference to something the kernel controls, together with rights; a program can act on a thing only through a capability to it | 5.2 |
| slot, slot number | a place in a capability table that holds one capability or nothing; a program names a capability by the number of its slot | 5.2 |
| table, table slot | a capability table (`CapTable`) holds slots; a process holds its tables in its table slots | 5.2 |
| derive, copy, move | three ways to make a capability from another one | 5.2 |
| below | under another capability in the derivation tree, directly or further down | 5.2 |
| root | a capability that is below nothing | 5.2 |
| revoke | clear everything below a capability | 5.2 |
| process | a protection domain: its tables, its region slots and its threads | 5.1 |
| thread | a set of registers and a state, which runs inside a process | 5.6 |
| block | a range whose size is a power of two and whose base is a multiple of its size | 5.4 |
| frame | a block of physical memory, or of a device's registers, with rights | 5.4 |
| region, region slot | a region is a frame installed in a process, so that the process can access it; it lies in one of the process's region slots, and uses one PMP entry | 5.4 |
| Untyped | memory that has not become anything yet; it can be split, or made into a frame or a pool | 5.4 |
| pool | memory given to the kernel; kernel objects are allocated from pools | 5.5 |
| object | something the kernel keeps in a pool: a pool's descriptor, a table, a process, a thread, a notification, an `Irq`, a `Route` or a `Dma` | 5.3 |
| notification | a 32-bit word of bits; threads signal it and wait on it | 5.7 |
| watch | a notification and bits that a thread's faults signal | 5.6 |
| line | an interrupt input; a timer line is one that the timer raises | 5.8, 5.9 |
| `Irq` | an object that connects one line to a notification | 5.9 |
| arm | allow an `Irq` or a `Route` to fire | 5.9, 5.13 |
| count | one step of the machine's counter | 5.8 |
| unit | a sixty-fourth of one core's time | 5.11 |
| place | the part of a thread that holds the time of one core | 5.11 |
| earn | a thread bound to a unit earns it; one thread at a time earns a unit | 5.11 |
| spare time | the time of the units that nobody earns | 5.11 |
| virtual time | how far a thread's units have paid for its running on one core, in counts of the clock; each place has its own | 5.11 |
| span | a length of time, a power of two of counts; a request is due one span after it starts | 5.11 |
| request | the thread's units' share of one span; the kernel runs the request that is due first | 5.11 |
| turn | one stretch that a thread runs on a core, until it waits or is preempted | 5.11 |
| lend, borrower | a waiting thread can lend its time to another thread, the borrower | 5.12 |
| trap | an entry into the kernel: a system call, an interrupt or a fault | 5.7 |
| root task | the first process, which starts with capabilities to the whole machine | 7 |

## 2. Features at a glance

- **PMP isolation.**
  Each process has a number of region slots.
  Its creator chooses the number:
  at least one, and at most the number of PMP entries the core has for regions.
  `OP_CAP_INFO` on a process tells its number of region slots, and the largest number allowed.
  A process pays for the region slots it has,
  and all of them can be filled at the same time.
  A region is installed with read, write and execute rights.
  The kernel rebuilds the process's PMP settings after every change.
  A region is a naturally aligned block whose size is a power of two,
  and it uses one PMP entry; see section 5.4.
- **Capabilities with rights.**
  A capability names one of these things, together with rights bits:
  a kernel object, a range of memory, a range of interrupt lines,
  or a range of the processor's units of time.
  When a capability is copied, its rights stay the same or become fewer, never more.
- **Untyped memory, frames and pools instead of a kernel heap.**
  A process turns its Untyped memory into frames and pools.
  It can map a frame, and the kernel writes a pool.
  The same bytes never become both.
  Every kernel object that a process creates is allocated from a pool that the process holds.
- **Taking memory back by revoking it.**
  Revoking below an Untyped destroys every pool made from it,
  however many times the memory was lent on.
  This destroys every object in those pools,
  and every capability, anywhere, that named one of those objects.
  The memory then returns to the Untyped.
  A capability to a pool can only allocate.
- **A root task like any other process.**
  The root task is the first process.
  At start it holds capabilities to the whole machine, and nothing else.
  It lives in memory that it holds an Untyped for.
  After boot, the kernel keeps no reference to it, so it is like any other process.
  So the root task can give everything it holds to a successor.
  The successor then destroys the root task and takes its place,
  as a bootloader starts the next stage; see section 7.
- **Plain binaries.**
  A program depends only on the register ABI and on what its creator gives it:
  a program counter, a stack pointer, installed regions and filled slots.
  There is no executable format, no header and no runtime library.
- **Notifications are the only way to wait.**
  A notification is a word of sticky bits.
  Data moves through shared memory, and the notification says when it is ready.
  The notification also orders memory:
  the thread whose wait returns the bits sees everything that the signalling thread stored before the signal.
  A thread that waits for another thread can lend that thread its time,
  for example to a lock's holder or to a server.
  So a thread with little time does not keep a thread with much time waiting for long.
- **A fault stops a thread, not the machine.**
  A thread that faults stops at the faulting instruction,
  and a notification that its creator chose hears about it.
  When the thread is resumed, it runs the faulting instruction again.
- **Processor time through capabilities.**
  Each core has `TIME_UNITS` units of time.
  They are handed out as capabilities, like memory.
  A thread runs on the units it is bound to, on the core that they belong to.
  Only one thread at a time earns a unit.
  If its capability allows it, a thread can also run on spare time on that same core:
  the time that nobody earned.
  Each turn moves the thread's virtual time forward,
  by the time the turn ran on the machine's counter, at the rate of the thread's units.
  Waiting does not undo any of it.
  So a process does not get more of the processor by making more threads or children;
  it gets what its units give.
  A thread held to its units gets no more than that,
  even when the processor is otherwise idle.
- **Timers are signals.**
  A timer line is an interrupt line that the timer raises after a delay.
  To sleep, a thread arms a timer line and waits.
  A period counts from the last deadline, so a periodic task does not drift.
- **The clock is a capability.**
  A process that holds the `Clock` capability reads the machine's counter with `OP_CLOCK_READ`.
  On some boards, a process can also read the counter with plain loads, through a read-only frame.
  A process that holds no clock, no counter frame and no timer line cannot read the time.
- **Interrupts are signals.**
  An `Irq` connects a hardware interrupt line to a notification.
  Interrupt lines are handed out as capabilities, like memory.
- **A kernel log instead of a console.**
  The kernel writes into a ring in its own memory.
  The root task maps the ring like a device,
  and reads it out when the ring's interrupt line fires.
- **Heavy verification.**
  The kernel is checked in four ways:
  a self-check inside the kernel,
  fuzzing on the host under sanitizers,
  a comparison of the host build with QEMU on the same inputs (differential replay),
  and a set of planted bugs (mutants) that the checks must catch; see section 9.
