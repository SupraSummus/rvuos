# rvuos user manual

This manual is for a developer who wants to write programs for rvuos
or port it to a board.
It says what the kernel is, what it promises,
and how to talk to it.
For why the kernel is the way it is, see `DESIGN.md`.

Contents:

1. [What rvuos is](#1-what-rvuos-is)
2. [Features at a glance](#2-features-at-a-glance)
3. [Hardware requirements and targets](manual/targets.md#3-hardware-requirements-and-targets)
    - [Boards](manual/targets.md#boards)
      - [QEMU `virt`, RV32](manual/targets.md#qemu-virt-rv32)
      - [ESP32-C6](manual/targets.md#esp32-c6)
      - [RP2350](manual/targets.md#rp2350)
      - [mps2-an385](manual/targets.md#mps2-an385)
4. [Building and running](manual/targets.md#4-building-and-running)
    - [Reading the transcript](manual/targets.md#reading-the-transcript)
5. [The programming model](manual/programming-model.md#5-the-programming-model)
    - [5.1 Processes, threads and regions](manual/programming-model.md#51-processes-threads-and-regions)
    - [5.2 Capabilities and the table](manual/programming-model.md#52-capabilities-and-the-table)
    - [5.3 Objects](manual/programming-model.md#53-objects)
    - [5.4 Memory: Untyped, frames and installing](manual/programming-model.md#54-memory-untyped-frames-and-installing)
    - [5.5 Pools](manual/programming-model.md#55-pools)
    - [5.6 Threads](manual/programming-model.md#56-threads)
    - [5.7 Notifications](manual/programming-model.md#57-notifications)
    - [5.8 Time](manual/programming-model.md#58-time)
    - [5.9 Interrupts](manual/programming-model.md#59-interrupts)
    - [5.10 The kernel log](manual/programming-model.md#510-the-kernel-log)
    - [5.11 Scheduling](manual/programming-model.md#511-scheduling)
6. [System call reference](manual/system-calls.md#6-system-call-reference)
    - [6.1 Invocation ABI](manual/system-calls.md#61-invocation-abi)
    - [6.2 Status codes](manual/system-calls.md#62-status-codes)
    - [6.3 Operations on `Debug`](manual/system-calls.md#63-operations-on-debug)
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
    - [6.14 Operation codes in numeric order](manual/system-calls.md#614-operation-codes-in-numeric-order)
7. [What the root task starts with](manual/system-calls.md#7-what-the-root-task-starts-with)
8. [Writing a program](manual/writing-programs.md#8-writing-a-program)
    - [8.1 Toolchain and layout](manual/writing-programs.md#81-toolchain-and-layout)
    - [8.2 Building a second process](manual/writing-programs.md#82-building-a-second-process)
    - [8.3 Writing a driver](manual/writing-programs.md#83-writing-a-driver)
9. [Debugging and testing interfaces](manual/writing-programs.md#9-debugging-and-testing-interfaces)
10. [Limits](manual/limits.md#10-limits)
11. [What is not there yet](manual/limits.md#11-what-is-not-there-yet)

Chapters 1 and 2 are in this file, the others in `manual/`.

## 1. What rvuos is

rvuos is a capability-based microkernel
for RISC-V microcontrollers that have no memory management unit,
and for ARMv7-M and ARMv8-M ones with an MPU.
It isolates processes with Physical Memory Protection (PMP)
instead of address translation,
so it needs no supervisor mode
and runs on cores that have only machine mode and user mode.
The kernel runs in machine mode and every program runs in user mode.
On ARM the MPU does what PMP does, the kernel runs in handler mode
and every program in unprivileged thread mode;
the objects, the operations and the programs written against `user/rvuos.h` are the same on both,
and where this manual says PMP, machine mode or `ecall`, ARM has its own in their place.

Three goals shape everything, in priority order.

1. **Isolation without an MMU.**
   A process reaches only the resources it was given:
   the memory installed in its region slots, with the rights installed,
   and nothing else.
   Two processes share memory only when someone who holds
   a capability to that memory installs it in both.
   The kernel's own memory is reachable by nobody.
2. **Capability-based authority.**
   Every system call names a capability in the caller's own table,
   and the kernel hands nothing out by a global name:
   there is no way to look up another process, thread or kernel object,
   and a device's registers reach a process only as a frame
   installed in one of its slots.
   A process that holds no capability to a thing cannot act on it,
   however it came to know the thing exists.
3. **The kernel never allocates.**
   The kernel has no heap.
   Every kernel object lives in memory that userspace explicitly
   handed to the kernel for that purpose,
   so a process pays for its kernel objects with its own RAM.

What rvuos is not:

- It is not a POSIX system.
  There is no `fork`, no file descriptor, no signal, no libc.
- It has no virtual memory.
  Every address a program sees is a physical address.
- It has no drivers in the kernel, and no console.
  Drivers run in user mode behind interrupt capabilities,
  and the kernel's own output goes into a log ring that a user program drains.
- It loads no code into the kernel at run time.
  Everything that runs in machine mode is in this repository.
- It is single-core for now.
- It is not binary compatible with seL4, L4 or F9,
  though it borrows from seL4's object model.

## 2. Features at a glance

- **PMP isolation.**
  Each process has a fixed number of region slots, eight today,
  set by the kernel and not by the board.
  A region is installed with read, write and execute rights
  and the kernel rebuilds the process's PMP image on every change.
  A region is a naturally aligned power-of-two block and costs one PMP entry,
  so the core's entry count bounds how many slots can be filled; see section 5.4.
- **Capabilities with rights.**
  A capability names a kernel object, a memory range, a range of interrupt lines
  or a range of the processor's units of time,
  together with rights bits.
  Copying a capability can only narrow its rights.
- **Untyped memory, frames and pools instead of a kernel heap.**
  A process makes its untyped memory into frames it can map and pools the kernel writes,
  never both over the same bytes,
  and every kernel object it creates is allocated from a pool it holds.
- **Revocation by memory.**
  Revoking below an Untyped destroys every pool made of it,
  however far it was lent on:
  every object in it and every capability anywhere that named one of those objects,
  and the memory comes back to the Untyped.
  A pool capability allocates and nothing more.
- **A root task like any other process.**
  The root task starts with the capabilities to the whole machine and nothing else:
  it lives in memory it holds an Untyped to, and the kernel names it nowhere after boot.
  So it can move everything it holds to a successor, which destroys it and takes its place,
  as a bootloader chains to the next stage; see section 7.
- **Plain binaries.**
  A program depends on the register ABI
  and on what its creator hands it:
  a program counter, a stack pointer, installed regions and filled slots.
  There is no executable format, no header and no runtime library.
- **Notifications as the only blocking primitive.**
  A notification is a word of sticky bits.
  Data moves through shared memory; the notification says when.
- **A fault stops a thread, not the machine.**
  A thread that faults stops where it faulted, and a notification its creator chose hears it;
  resumed, it runs the faulting instruction again.
- **Processor time by capability.**
  The processor is `TIME_UNITS` units of time, handed out as capabilities like memory is,
  and a thread runs on the units it is bound to, each earned by one thread at a time,
  or on spare time, which nobody earned, if its capability allows.
  Its account fills at its units' rate and its turns cost it the time they ran, on the machine's counter,
  so a process that makes more threads or children gets no more of the processor than its units,
  and a thread held to its units gets no more, however idle the processor is otherwise.
- **Timers as signals.**
  A timer line is an interrupt line the tick raises once a delay has passed.
  Sleeping is arming a timer line and waiting;
  a period counts from the last deadline, so a periodic task does not drift.
- **A clock as a capability.**
  A process given the `Clock` reads the machine's counter with loads through a read-only region;
  one given neither a clock nor a timer line has no clock to read.
- **Interrupts as signals.**
  An `Irq` binds a hardware line to a notification.
  Lines are handed out as capabilities like memory is.
- **A kernel log in place of a console.**
  The kernel writes into a ring in its own memory
  that the root task maps like a device and drains on an interrupt line.
- **Heavy verification.**
  An in-kernel self-check, host fuzzing under sanitizers,
  differential replay between the host build and QEMU,
  and a mutant suite; see section 9.
