# rvuos user manual

This manual is for a developer who wants to write programs for rvuos
or port it to a board.
It says what the kernel is, what it promises,
and how to talk to it.
For why the kernel is the way it is, see `DESIGN.md`.

Contents:

1. What rvuos is
2. Features at a glance
3. Hardware requirements and targets
4. Building and running
5. The programming model
6. System call reference
7. What the root task starts with
8. Writing a program
9. Debugging and testing interfaces
10. Limits
11. What is not there yet

## 1. What rvuos is

rvuos is a capability-based microkernel
for RISC-V microcontrollers that have no memory management unit.
It isolates processes with Physical Memory Protection (PMP)
instead of address translation,
so it needs no supervisor mode
and runs on cores that have only machine mode and user mode.
The kernel runs in machine mode and every program runs in user mode.

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
   and a device's registers reach a process only as a region
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
  A capability names a kernel object, a memory range or a range of interrupt lines,
  together with rights bits.
  Copying a capability can only narrow its rights.
- **Pools instead of a kernel heap.**
  A process turns a region of its memory into a kernel pool
  and every kernel object it creates is allocated from a pool it holds.
- **Revocation by pool.**
  Destroying a pool destroys every object in it,
  every pool created from within it,
  and every capability anywhere that named one of those objects.
  Destroying the pool a process lives in therefore also takes
  every pool its threads made out of memory they were lent,
  and the lender's capabilities to that memory work again.
- **Plain binaries.**
  A program depends on the register ABI
  and on what its creator hands it:
  a program counter, a stack pointer, installed regions and filled slots.
  There is no executable format, no header and no runtime library.
- **Notifications as the only blocking primitive.**
  A notification is a word of sticky bits.
  Data moves through shared memory; the notification says when.
- **Preemptive round-robin scheduling.**
  A machine timer tick takes the processor from a running thread
  and hands it to the ready thread that has waited longest.
- **Timers as signals.**
  A timer line is an interrupt line the tick raises once a delay has passed.
  Sleeping is arming a timer line and waiting.
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

## 3. Hardware requirements and targets

The kernel needs a RISC-V core with:

- machine mode and user mode (supervisor mode is not used even when present),
- PMP with NAPOT addressing mode and at least four entries,
  which is the kernel's own floor: it refuses to boot with fewer,
- the `A` extension in user mode if programs want atomics in shared memory,
- a machine timer (`mtime` and `mtimecmp`),
- an interrupt controller in front of the machine external interrupt.

Cores without PMP cannot run rvuos.
TOR is not needed; `DESIGN.md`, open decision 8, records why rvuos uses NAPOT alone.

The build is `rv32imac`, `ilp32`, compiled with clang and linked with lld.

### Boards

Two boards are supported, chosen with `make BOARD=<board>`:
`qemu`, QEMU `virt` for RV32, the default,
and `esp32c6`, an Espressif ESP32-C6.
Everything board-specific lives in `kernel/board/<board>/`,
`board.h`, `board.c`, `irq.c`, `timer.c` and `halt.c`,
and in `user/board/<board>/console.h`,
which drives the device behind `BOOT_CAP_UART` for the demo and the replay driver.
The linker scripts take their addresses from `board.h` through `kernel/layout.h`.
Porting to a board means providing those.
A program learns every address it needs from its region capabilities;
only the line numbers of its devices are the board's to know.

#### QEMU `virt`, RV32

Memory map:

| Range | Size | What |
|---|---|---|
| `0x0200BFF8` | 8 B | the CLINT's `mtime`, 10 MHz, read only through `BOOT_CAP_CLOCK` |
| `0x10000000` | 256 B | 16550 UART registers, granted to the root task |
| `0x80000000` to `0x800FF000` | just under 1 MiB | kernel code, data, stack and the boot pool |
| `0x800FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x80100000` | 64 KiB | root task code, read and execute |
| `0x80200000` | 64 KiB | root task data and stack, read and write |
| `0x80210000` | 64 KiB | replay input placed by QEMU's loader, read only |
| `0x80400000` | 4 MiB | free RAM, granted to the root task with all rights |

Every range granted to the root task is a block, see section 5.4,
so what lies between the blocks is used by nothing.

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log (no controller behind it) |
| 1 to 95 | the PLIC's sources; the UART is line 10 |

The kernel's tick on this board is 1 kHz, `TIMER_HZ` in `kernel/timer.h`,
so one tick is one millisecond.
QEMU's PMP has sixteen entries and a four-byte grain.

#### ESP32-C6

The chip's ROM loads the image into SRAM over its USB port and enters it;
nothing is written to flash, and booting from flash is not supported yet.
The same port carries the console.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20001C08` | 8 B | the CLINT's `UTIME`, a read-only copy of `mtime` at the CPU clock, through `BOOT_CAP_CLOCK` |
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x40800000` to `0x4081F000` | just under 128 KiB | kernel code, data, stack and the boot pool |
| `0x4081F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x40820000` | 64 KiB | root task code, read and execute |
| `0x40830000` | 32 KiB | root task data and stack, read and write |
| `0x40838000` | 4 KiB | input region, read only; nothing fills it yet |
| `0x40840000` | 256 KiB | free RAM, granted to the root task with all rights |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; the Wi-Fi MAC's source 0 cannot be bound |
| 1 to 76 | the interrupt matrix's sources, numbered as in Espressif's `soc/interrupts.h`; the USB Serial/JTAG controller is line 48 |

A device's interrupt reaches its `Irq` only while the device itself has it enabled,
in the USB Serial/JTAG controller's case in its `INT_ENA` register.
The tick is 1 kHz here too, measured against the chip's 16 MHz system timer at boot.
The PMP has sixteen entries and a four-byte grain.
The console is the controller's CDC-ACM port:
bytes written to its FIFO leave as one USB packet when `WR_DONE` is written,
and only while a host has the port open.

## 4. Building and running

Requirements: clang and lld with RISC-V support, llvm-objcopy,
GNU make and `qemu-system-riscv32`.
No separate cross toolchain is needed.
The host build needs clang's sanitizer and libFuzzer runtimes.
The ESP32-C6 needs Espressif's `esptool`, version 5, as a command and as a Python module.

```
make             # build/qemu/kernel-init.elf and build/qemu/kernel-fuzzdrv.elf
make run         # boot the demo root task under QEMU
make test        # boot under QEMU and check the transcript
make host-test   # replay the fuzz corpus on the host build with invariants on
make fuzz        # fuzz the system call surface for FUZZ_TIME seconds
make qemu-replay # replay the corpus on QEMU and compare with the host
make mutants     # plant each bug under tests/mutants/ and require the checks to catch it
make check       # test, host-test and qemu-replay; run before committing
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.
Images go under `build/<board>/`, the host build under `build/host/`;
a smaller budget adds `-pmp<n>` to both.

On the ESP32-C6, connected over USB:

```
make BOARD=esp32c6                    # build/esp32c6/kernel-init.bin
make BOARD=esp32c6 run                # load it into RAM and print the console until the halt
make BOARD=esp32c6 test               # the same, and check the transcript
make BOARD=esp32c6 PORT=/dev/ttyACM1 run
```

Opening the port resets the chip,
so `tools/esp32c6-run.py` loads the image and reads the console on one connection.
Only the demo is built for the board; the replay driver is QEMU's for now.

The kernel image embeds one user program, the root task.
Two images are built, differing only in that program:
`kernel-init.elf` carries the demo of `user/init.c`
and `kernel-fuzzdrv.elf` carries the replay driver of `user/fuzzdrv.c`.

### Reading the transcript

The kernel has no console.
Everything the kernel prints, and everything a program writes with `OP_DEBUG_PUTC`,
goes into the kernel log.
Two things carry that log to the board's UART:

- the root task's logger thread, while the machine runs,
- the halt, which writes out every byte no reader has taken,
  under the line `rvuos: halting, the log follows`.

So a line that appears before that marker was carried out from user mode,
and a line after it was salvaged by the halt.

The kernel prints three lines at boot:

```
rvuos: machine mode up
rvuos: pmp entries 0x00000010 grain 0x00000004
rvuos: entering user mode
```

On QEMU a halt exits the emulator with a status code.
The ESP32-C6 parks its core instead, after the line `rvuos: halted with code <n>`,
and `tools/esp32c6-run.py` exits with that code:

| Code | Meaning |
|---|---|
| 0 | `OP_DEBUG_HALT` with code 0 |
| 1 | kernel panic |
| 3 | the self-check found an invariant violated |
| 4 | a user fault: access fault, illegal instruction, misaligned access, breakpoint |
| 5 | no runnable thread and nothing armed that could make one |
| 6 | tracing is on and a thread the host build cannot follow was about to run |
| other | what the root task passed to `OP_DEBUG_HALT` |

A user fault prints `user fault` followed by `mcause`, `mepc` and `mtval`.
Any fault stops the whole machine, even when another thread could run;
see section 11.

## 5. The programming model

### 5.1 Processes, threads and regions

A **process** is a protection domain:
a capability table, `PROCESS_REGION_SLOTS` region slots, eight today,
and the threads that run in it.
A **thread** is an execution context inside a process:
a register frame and a state.
Threads of one process share its regions and its capability table.

A **region** is a physical address range with maximum rights.
Installing a region into one of a process's slots
gives that process access to the range with the rights chosen at install time.
Regions are all a process can see of memory.
The kernel does not know what a code segment, a stack or a heap is;
whoever builds a process decides its layout.

### 5.2 Capabilities and the table

A process names everything by **slot index** into its capability table.
A slot is empty or holds one capability:
a type, a set of rights, and two words the kernel interprets by type.
Userspace never sees a slot's contents,
but it can ask a region what it covers with `OP_REGION_INFO`.

Every system call is an invocation on the capability in one slot.
The kernel resolves the slot,
checks the capability's type and rights against the operation,
and only then looks at the arguments;
section 6.1 gives the exact order.
An empty slot or an index out of range fails with `KERR_INVALID_CAP`.

A process holds its table by a capability of its own,
derived from the `CapTable` capability it was allocated with.
The table may lie in any pool,
and several processes may be allocated with one table, which they then share.
Revoking below that capability, or destroying the pool the table lies in,
takes the table from the process,
and from then on every call its threads make fails with `KERR_INVALID_CAP`.
The table itself stays as long as its pool does,
and so does everything in it.

Rights are three bits:

| Bit | Value | Region | Other types |
|---|---|---|---|
| `RIGHT_R` | 1 | read | `Notification`: wait |
| `RIGHT_W` | 2 | write | control the object: allocate, install, configure, signal, set, copy into a table |
| `RIGHT_X` | 4 | execute | unused |

Every operation that produces a capability
puts it into a slot of the caller's own table,
named by an argument, and that slot must be empty.
A process needs no capability to receive into its own table.
The `CapTable` capability exists for writing into a table:
a parent fills a child's table with `OP_CAP_COPY` or `OP_CAP_DERIVE` before starting it,
and a process that holds a capability to its own table
can copy within it, clear its own slots, and revoke below them.

Every capability remembers what it was derived from,
and that is what `OP_CAP_REVOKE` follows.
The tree grows in these ways:

| Operation | The new capability hangs |
|---|---|
| `OP_CAP_DERIVE` | below the source |
| `OP_CAP_COPY` | beside the source, under the source's parent; a copy of a root is a root |
| `OP_REGION_CARVE`, `OP_IRQ_CARVE`, `OP_CLOCK_REGION` | below the invoked capability |
| `OP_PROCESS_INSTALL` | the installed region hangs below the `Region` capability |
| `OP_POOL_ALLOC` of a `Process` | the process's hold on its table hangs below the `CapTable` capability |
| `OP_REGION_TO_POOL`, `OP_IRQ_BIND` | where the consumed capability hung |
| `OP_POOL_DESTROY` | where the pool capability hung, below the region the pool was made of |
| `OP_POOL_ALLOC`, boot | nowhere: a new object starts a tree of its own |

`OP_CAP_REVOKE` on a slot clears everything below it, in every table and every process:
derived capabilities, their copies, what was derived from those,
and every region installed from any of them.
The slot itself stays.
Revoking a copy takes nothing from the original, and the other way round;
to take both back, revoke below what they were both derived from.
So derive to lend, and copy to keep a second handle to what you hold.

`OP_CAP_DELETE` clears one slot and hands what hung below it to the slot's parent,
so deleting your own copy of something you lent does not take it back.
Destroying a pool revokes every slot in the tables that pool held:
what a process handed out through its table dies with the process.

Copying and deriving both apply a rights mask, so rights only ever narrow.

Slot 0 of the root task's table is left empty on purpose,
so that an uninitialised index fails.
Programs are encouraged to keep the same convention.

### 5.3 Objects

| Type | Constant | Object behind it | Created by |
|---|---|---|---|
| `Region` | `CAP_REGION` | none: the slot holds base and size | boot, `OP_REGION_CARVE`, `OP_POOL_DESTROY` |
| `KernelPool` | `CAP_POOL` | the pool's descriptor, at its base | `OP_REGION_TO_POOL` |
| `CapTable` | `CAP_CAPTABLE` | a table of `n` slots | `OP_POOL_ALLOC` |
| `Process` | `CAP_PROCESS` | a capability to its table, region slots, a PMP image | `OP_POOL_ALLOC` |
| `Thread` | `CAP_THREAD` | a register frame and a state | `OP_POOL_ALLOC` |
| `Notification` | `CAP_NOTIFICATION` | one word of sticky bits | `OP_POOL_ALLOC` |
| `IrqLine` | `CAP_IRQ_LINE` | none: the slot holds the first line and a count | boot, `OP_IRQ_CARVE` |
| `Irq` | `CAP_IRQ` | one line bound to a notification, with a deadline on a timer line | `OP_IRQ_BIND` |
| `Debug` | `CAP_DEBUG` | none | boot |
| `Clock` | `CAP_CLOCK` | none: there is one counter | boot |

`Region`, `IrqLine`, `Debug` and `Clock` capabilities have no kernel object behind them.
Carving a region or a line range is a pure table operation
that touches no kernel memory,
which is why the root task can hand out memory and lines
before it has created a single pool.

### 5.4 Memory: regions, carving and installing

Every region is a **block**:
its size is a power of two and its base a multiple of its size,
which is the range one PMP entry in NAPOT mode describes.
The smallest block is eight bytes, or the PMP's grain if that is coarser;
`OP_REGION_INFO` returns it in `a4`.
`OP_REGION_CARVE` hands out a block within a block.
A range of any other size is rounded up or made of several blocks,
one region slot each.
Programs that lay memory out with fixed offsets
should check the smallest size once and fail early if the layout does not fit.

Rules for installing a region into a process:

- The rights installed must be a non-empty subset of the region's rights.
- Write without read is refused,
  because PMP reserves that encoding and hardware may do anything with it.
- Regions installed in one process may not overlap each other.
- A region may not overlap a kernel pool.
  The mirror rule holds too: a range installed anywhere cannot become a pool.
- The install takes effect at once, even for the running process.

PMP budget.
Every installed region is one NAPOT entry, so `n` regions cost `n` entries.
An install beyond the core's entries fails with `KERR_LIMIT`,
which with eight region slots happens only on a core with fewer than eight.

An access must lie entirely within one PMP entry.
Two regions that touch share a boundary
that no single load, store or instruction fetch may cross,
so a process's creator lays it out so that nothing straddles one.

Sharing costs no copy: installing the same range into two processes
gives both access, and no byte passes through the kernel.
What it does cost is a region slot and a PMP entry in each process.

### 5.5 Pools

A process hands memory to the kernel with `OP_REGION_TO_POOL`.
The range must:

- carry both read and write rights,
- have its base and size aligned to eight bytes and be at least 64 bytes,
  the kernel's object alignment and its smallest pool,
- lie within RAM, not on a device and not on the kernel log,
- overlap no existing pool and no region installed in any process.

The kernel places the pool's descriptor at its base,
clears the region capability that was invoked,
and returns a `KernelPool` capability with all rights.
Copies of the region capability held elsewhere survive but are inert:
installing them fails the overlap check while the pool exists.

Objects are bump-allocated from a pool, eight-byte aligned,
and never freed individually.
Sizes a developer needs for planning, as the kernel rounds them:

| Object | Bytes (16 PMP entries) |
|---|---|
| pool descriptor | 32 |
| `CapTable` with `n` slots | 12 + 24 × n, rounded up to 8 |
| `Process` | 312 (272 with `PMP_MAX_ENTRIES=8`) |
| `Thread` | 176 |
| `Notification` | 16 |
| `Irq` | 24 |

A minimal child process, table of 10 slots, process, thread and two notifications,
costs 808 bytes including the descriptor.

**The same-pool rule.**
The kernel follows the link from a thread to its process,
and from an `Irq` to its notification,
without checking that the target still exists.
The target must therefore never be destroyed before the object that points at it,
and since destruction happens per pool, the two must lie in one pool:

- a `Thread` is allocated from the pool its `Process` lies in,
- an `Irq` from the pool its `Notification` lies in.

A process's table is not bound by the rule, section 5.2.

An allocation that breaks the rule fails with `KERR_INVALID_ARG`.

**The pool tree.**
A new pool's parent is the pool the calling thread lives in.
The boot pool is the root and can never be destroyed.

**Destroying a pool** with `OP_POOL_DESTROY`:

- destroys every object in it and every pool below it in the tree,
- clears every capability, in every table of every process,
  that names one of those objects,
- wakes every thread waiting on a notification in those pools
  with `KERR_INVALID_CAP` and no bits,
- masks the interrupt line of every `Irq` in those pools,
- zeroes the memory its objects took and leaves the rest as it was,
  so clear a region before it becomes a pool if whoever destroys the pool should not read it,
- returns a `Region` capability to the invoked pool's memory,
  with the rights the region had when it became a pool.

The call fails with `KERR_STATE`
when the calling thread lives in the pool or in one below it,
or its process's table does,
so a thread cannot destroy the pool it runs from
nor the table the memory is to come back to.
The memory of the pools below comes back through no new capability;
region capabilities for it that survived simply work again,
which is how a lender gets lent memory back.
Those are the lender's own capability and any copies made beside it:
what was derived from the region went when the region became a pool.

A lender that wants memory back from a living borrower
revokes below its own `Region` capability instead, section 5.2.
That takes the borrower's derived capability, its mappings,
and any pool capability the borrower made of it;
the pool itself stays, unreachable, until the pool above it is destroyed.
Kernel memory comes back only by destroying pools.

### 5.6 Threads

A thread is **stopped**, **ready** or **waiting**.

- A new thread is stopped.
- `OP_THREAD_CONFIGURE` sets a stopped thread's program counter and stack pointer.
  The other registers start at zero.
  The kernel validates neither value.
- `OP_THREAD_RESUME` makes a stopped thread ready.
  It gets the processor when its turn in the round comes,
  which is after the running thread waits or the tick preempts it.
- A thread waiting on a notification is waiting;
  a signal makes it ready again.

Once a thread runs, nothing short of destroying its pool stops it,
and no thread exits: a thread that has nothing left to do waits forever
on a notification nobody signals.
A thread that faults stops the machine; see section 11.

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

No data passes through the kernel.
Two processes exchange bytes through a region installed in both
and use notifications to say when.
The build targets `rv32imac`, so the `A` extension is there in user mode
and a lock or a ring buffer in shared memory is userspace's to build;
what userspace cannot build is "stop me until someone says otherwise",
and a notification is exactly that.

A round trip is therefore four system calls:
signal, wait on one side; wait, signal on the other.

**A server with many clients** waits on one notification;
each client holds it with `RIGHT_W` only, owns a bit,
and shares a region with the server.
One wake can carry several clients' work.
Which client owns which bit is a convention the kernel does not enforce,
so a client can waste the server a look but cannot forge data
or consume another client's wake.

### 5.8 Time

There is no sleep call and no yield.
Time reaches userspace as one more interrupt, on a timer line.

The machine has `TIMER_LINES` timer lines, 16,
and the root task receives all of them at boot in `BOOT_CAP_TIMER_LINES`,
apart from the controller's lines.
It carves them and hands them out as it does any other line, section 5.9,
so who holds how many timers is its choice.
A timer line is bound with `OP_IRQ_BIND` like a device's,
into an `Irq` in a pool, bound to a notification in that same pool.
`OP_IRQ_SET` arms it with a set of bits and, in `a2`, a delay in microseconds;
when the delay has passed the line fires: the `Irq` signals those bits and disarms.
Setting an armed timer line moves its deadline and replaces its bits;
setting bits to zero cancels it.
A program that needs more timers than it holds keeps its own deadlines
and arms one line for the nearest.

The contract is a **lower bound**.
The kernel fires at the first tick that surely lies past the delay:
the delay rounded up to whole ticks, plus one.
With a millisecond tick, as on both boards,
a delay of zero fires at the next tick,
and a delay of 10 000 µs fires on the eleventh tick after the call,
between 10 and 11 ms later.
No upper bound is promised, because the woken thread is ready, not running,
and waits its turn in the round.

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

A timer line fires once, so a periodic task arms it again each time it wakes,
and drifts by what each wake costs, unless it holds the clock below
and arms each delay as its next deadline less the time now;
`DESIGN.md`, open decision 16.

**The clock.**
`BOOT_CAP_CLOCK` names the machine's counter, 64 bits counting up from boot.
`OP_CLOCK_INFO` returns its rate in hertz and its address;
`OP_CLOCK_REGION` derives a read-only `Region` holding it, which a creator installs like any region.
Reading the counter is then a few loads and no system call:

```c
uint32_t hz, counter;
rv_clock_info(CLOCK, &hz, &counter);
rv_invoke(OP_CLOCK_REGION, CLOCK, COUNTER_REGION, 0, 0);
rv_invoke(OP_PROCESS_INSTALL, PROCESS, 5, COUNTER_REGION, RIGHT_R);

uint64_t start = rv_counter_read(counter);
work();
uint64_t ticks = rv_counter_read(counter) - start;   /* ticks / hz seconds */
```

The rate is the board's, 10 MHz on QEMU and the CPU clock on the ESP32-C6,
so a program takes it from `OP_CLOCK_INFO`.
The time is the machine's, not the thread's: it includes other threads' slices.
Revoking below a `Clock` capability uninstalls every region derived through it.
`rdtime` traps on every board.

### 5.9 Interrupts

A driver holds an `IrqLine` capability naming one line.
The root task receives every line of the controller at boot, with the log's,
and every timer line in a second capability,
and carves single lines out with `OP_IRQ_CARVE`,
as it carves regions out of memory.

`OP_IRQ_BIND` turns a one-line capability into an `Irq` object
in a pool of the driver's choosing,
bound to a notification in that same pool.
The invoked slot is consumed.
A line is bound at most once; a second bind fails with `KERR_OVERLAP`
until the first `Irq`'s pool is destroyed.

An `Irq` on a device's line works like one on a timer line:

- `OP_IRQ_SET` with bits **arms** it, which unmasks the line at the controller.
- When the line fires, the kernel masks the line,
  disarms the `Irq` and signals the bits.
- The driver services the device and arms the `Irq` again,
  which is the acknowledgement: only the driver lifts the mask.
- `OP_IRQ_SET` with zero bits masks the line by hand.

An `Irq` is armed exactly while its line is unmasked,
so a level that stays high costs one trap and not a storm.
A device interrupt wakes its driver but does not run it;
the driver waits its turn in the round like a thread a timer line woke.

Sharing a line between drivers is not supported;
`DESIGN.md`, open decision 11.

### 5.10 The kernel log

The kernel writes every byte it prints, `OP_DEBUG_PUTC` included,
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
The line cannot be fired by `OP_DEBUG_IRQ` either.

`user/init.c` shows the reader:
a logger thread that waits on one notification with the log's bit and the UART's,
and carries the ring out one byte per transmitter interrupt.

### 5.11 Scheduling

- Ready threads wait for the processor in a queue.
  A thread that becomes ready, whether resumed, woken or preempted,
  joins the back of it,
  so threads take turns in the order they became ready.
- The machine timer ticks at `TIMER_HZ`, 1 kHz on both boards.
  A thread runs until it waits or until a tick takes the processor from it.
- A system call is never interrupted:
  machine mode runs with interrupts off from the trap to the return.
- When nothing is runnable and an `Irq` is armed on a timer line or a device's line,
  the kernel stalls in `wfi` until the tick or the interrupt arrives.
  When nothing is armed either, it prints `no runnable thread` and halts with code 5.

There are no priorities and no yield.
A spinning thread cannot starve the others, because the tick preempts it,
but it burns its slice every round and keeps the machine out of `wfi`;
a thread that waits costs nothing until it is signalled.

## 6. System call reference

### 6.1 Invocation ABI

The numbers in this section are copied from `include/rvuos/abi.h`,
which is the binding contract.

Every system call is an `ecall` with:

| Register | On `ecall` | On return |
|---|---|---|
| `a7` | operation code, one of `OP_*` | unchanged |
| `a0` | slot of the invoked capability | status, one of `KERR_*`, zero on success |
| `a1` to `a3` | arguments | results, where the operation documents them; otherwise unchanged |
| `a4` | reserved | a result where documented, otherwise unchanged |
| `a5`, `a6` | reserved | unchanged |

No operation takes a pointer into user memory,
and no operation moves data between processes.

The C wrapper in `user/rvuos.h`:

```c
uint32_t rv_invoke(uint32_t op, uint32_t cap, uint32_t a1, uint32_t a2, uint32_t a3);
```

The kernel checks in this order:
the slot resolves (`KERR_INVALID_CAP`),
the capability type carries `RIGHT_W` where the type needs it for every operation
(`KERR_NO_RIGHTS`),
the type accepts the operation (`KERR_WRONG_TYPE`),
the operation's own right and arguments.
Operation codes are a single flat numbering across all types,
so invoking an `Irq` operation on a `Region` is `KERR_WRONG_TYPE`,
not `KERR_INVALID_ARG`.

A call whose work grows with the derivation tree can be interrupted and made again:
`OP_CAP_REVOKE`, `OP_CAP_DELETE` of a root with capabilities below it,
`OP_REGION_TO_POOL` and `OP_IRQ_BIND`.
When the tick or a device interrupt comes due while such a call works,
the kernel stops it between two capabilities
and resumes the thread at its `ecall` with every register as it was,
so the thread makes the same call again when it next runs,
and the call goes on from what it had already cleared.
A program sees no difference but time,
unless another thread changes the same capabilities in between:
the call made again checks everything afresh
and may fail, with what it already revoked staying revoked.

### 6.2 Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `KERR_OK` | success |
| 1 | `KERR_INVALID_CAP` | slot out of range, empty, or its object was destroyed; every call of a process whose table was taken |
| 2 | `KERR_WRONG_TYPE` | the capability's type does not accept this operation |
| 3 | `KERR_NO_RIGHTS` | the capability lacks a right the operation needs |
| 4 | `KERR_INVALID_ARG` | an argument is out of range or breaks a rule stated below |
| 5 | `KERR_NO_MEMORY` | the pool has no room for the object |
| 6 | `KERR_SLOT_IN_USE` | the destination slot already holds a capability |
| 7 | `KERR_OVERLAP` | a region overlaps a pool or an installed region, or the line is already bound |
| 8 | `KERR_LIMIT` | a fixed kernel limit was hit, such as the PMP entry count |
| 9 | `KERR_STATE` | the object is not in a state that allows this |

### 6.3 Operations on `Debug`

The `Debug` capability needs no particular right.

**`OP_DEBUG_PUTC` (1).**
`a1` = the byte.
Appends it to the kernel log.

**`OP_DEBUG_HALT` (2).**
`a1` = exit code.
Stops the machine; on QEMU the emulator exits with that code,
and on the ESP32-C6 the core parks after printing it.
Does not return.

**`OP_DEBUG_TRACE` (10).**
Turns tracing and self-checking on; see section 9.
Cannot be turned off again.

**`OP_DEBUG_TICK` (17).**
Does what the timer tick does, on request:
time moves by one tick, every due timer line fires,
the processor goes to the next runnable thread in the round,
and the caller stays ready.
Works while tracing is on, unlike the tick itself.

**`OP_DEBUG_IRQ` (22).**
`a1` = a line.
Does what a device interrupt on that line does:
the `Irq` armed on it masks the line and signals its bits.
`KERR_STATE` when nothing is armed on the line;
`KERR_INVALID_ARG` for a line the controller does not have, line 0 included.

### 6.4 Operations on `CapTable`

All need `RIGHT_W` on the table.

**`OP_CAP_COPY` (3).**
`a1` = destination slot in the invoked table,
`a2` = source slot in the caller's table,
`a3` = rights mask ANDed into the copy.
The destination must be empty (`KERR_SLOT_IN_USE`).
The source may be any type, regions and lines included.
The copy hangs beside the source in the derivation tree, section 5.2.

**`OP_CAP_DERIVE` (24).**
The arguments and errors of `OP_CAP_COPY`.
The new capability hangs below the source.

**`OP_CAP_DELETE` (4).**
`a1` = slot in the invoked table.
Clears it; what hung below it now hangs below the slot's parent,
or, when the slot is a root, each becomes a root, and the call may be made again, section 6.1.
Clearing an empty slot succeeds.
Deleting a `Region` capability does not uninstall the region anywhere.

**`OP_CAP_REVOKE` (23).**
`a1` = slot in the invoked table, which must be filled (`KERR_INVALID_CAP`).
Clears everything below it, section 5.2, and leaves the slot.
Regions installed from capabilities below it are uninstalled,
and threads of those processes lose access at once.
The call may be made again, section 6.1.

### 6.5 Operations on `Region`

**`OP_REGION_INFO` (11).**
No right needed.
Returns `a1` = base, `a2` = size, `a3` = rights,
`a4` = the size of the smallest region in bytes, a power of two of at least 8.

**`OP_REGION_CARVE` (5).**
No right needed.
`a1` = offset from the region's base, `a2` = size, `a3` = destination slot in the caller's table.
Produces a region with the same rights, hanging below the invoked one.
The size must be a power of two no smaller than the smallest region,
the offset a multiple of the size,
and the sub-range within the region (`KERR_INVALID_ARG`),
so the new region is a block.
The parent capability is unchanged.

**`OP_REGION_TO_POOL` (6).**
Needs `RIGHT_R` and `RIGHT_W`.
`a1` = destination slot for the `KernelPool` capability,
which may be the invoked slot.
The invoked slot is revoked with everything below it,
and the `KernelPool` capability takes its place in the tree.
The requirements of section 5.5 apply:
alignment and minimum size, RAM only, not the log (`KERR_INVALID_ARG`),
no overlap with a pool or an installed region (`KERR_OVERLAP`).
They are checked before anything is revoked,
and the revoke may make the call again, section 6.1.
The new pool's parent is the pool the calling thread lives in.

### 6.6 Operations on `KernelPool`

Both need `RIGHT_W` on the pool.

**`OP_POOL_ALLOC` (7).**
`a1` = object type, `a2` = destination slot, `a3` = type-specific:

| `a1` | `a3` | Rule |
|---|---|---|
| `CAP_CAPTABLE` | number of slots, 1 to `CAPTABLE_MAX_SLOTS` (1024) | |
| `CAP_PROCESS` | slot of the `CapTable` capability the process will use, with `RIGHT_W` | the table may lie in any pool; the process's hold on it hangs below that capability |
| `CAP_THREAD` | slot of the `Process` capability the thread will run in, with `RIGHT_W` | the process must lie in this pool; the thread starts stopped |
| `CAP_NOTIFICATION` | unused | |

Any other type is `KERR_INVALID_ARG`; `Irq` objects come from `OP_IRQ_BIND`.
The new capability carries all rights.
`KERR_NO_MEMORY` when the pool is full.

**`OP_POOL_DESTROY` (16).**
`a1` = destination slot for the `Region` capability to the pool's memory,
which may be the invoked slot.
Does everything section 5.5 lists.
The `Region` capability hangs where the pool capability hung,
below the region the pool was made of;
it is a root when that region's slot went with the destroy.
`KERR_STATE` if the calling thread or its process's table lives in the pool or below it.

### 6.7 Operations on `Process`

Both need `RIGHT_W` on the process.

**`OP_PROCESS_INSTALL` (8).**
`a1` = region slot index, below `PROCESS_REGION_SLOTS`,
`a2` = slot of a `Region` capability in the caller's table,
`a3` = rights to install.
The rights must be a non-empty subset of the region's (`KERR_NO_RIGHTS`);
write without read is `KERR_INVALID_ARG`;
an occupied region slot is `KERR_SLOT_IN_USE`;
overlap with a pool or with another region in this process is `KERR_OVERLAP`;
too few PMP entries is `KERR_LIMIT`.
The installed region hangs below the `Region` capability in the derivation tree,
so `OP_CAP_REVOKE` on that capability's slot, or on any slot above it, uninstalls it.

**`OP_PROCESS_UNINSTALL` (9).**
`a1` = region slot index.
Clears it and rebuilds the PMP image.
Threads of the process lose access at once.
Clearing an empty slot succeeds.

### 6.8 Operations on `Thread`

Both need `RIGHT_W` on the thread and a stopped thread (`KERR_STATE`).

**`OP_THREAD_CONFIGURE` (12).**
`a1` = program counter, `a2` = stack pointer.
Neither is checked.

**`OP_THREAD_RESUME` (13).**
Makes the thread ready.

### 6.9 Operations on `Notification`

**`OP_NOTIFY_SIGNAL` (14).**
Needs `RIGHT_W`.
`a1` = bits to set, non-zero (`KERR_INVALID_ARG`).
Never blocks.

**`OP_NOTIFY_WAIT` (15).**
Needs `RIGHT_R`.
Blocks until some bit is set, then returns `a1` = the bits and clears them.
Returns `KERR_INVALID_CAP` with no bits
if the notification's pool is destroyed while the thread waits.

### 6.10 Operations on `IrqLine`

**`OP_IRQ_CARVE` (19).**
No right needed.
`a1` = offset from the first line, `a2` = count, `a3` = destination slot.
A table operation like `OP_REGION_CARVE`.

**`OP_IRQ_BIND` (20).**
Needs `RIGHT_W`, and the capability must name exactly one line (`KERR_INVALID_ARG`).
`a1` = slot of the `KernelPool` capability to allocate the `Irq` from, with `RIGHT_W`,
`a2` = slot of the `Notification` capability the `Irq` signals, with `RIGHT_W`,
which must lie in that pool,
`a3` = destination slot for the `Irq` capability, which may be the invoked slot.
The invoked slot is cleared with everything below it.
`KERR_OVERLAP` if an `Irq` is already bound to the line,
`KERR_NO_MEMORY` if the pool has no room for it,
both checked before anything is revoked,
and the revoke may make the call again, section 6.1.
The new `Irq` is masked until `OP_IRQ_SET` arms it.
A timer line binds the same way as a device's.

### 6.11 Operations on `Irq`

**`OP_IRQ_SET` (21).**
Needs `RIGHT_W`.
`a1` = bits the next interrupt signals; unmasks the line.
`a1` = 0 masks the line; it takes back no signal that already happened.
On the log's line, arming while bytes are untaken signals at once.
On a timer line, `a2` = the delay in microseconds, and the line fires
at the first tick that surely lies past it, section 5.8;
elsewhere `a2` is unused.
A delay longer than 32 bits of microseconds, about 71 minutes, is several calls.

### 6.12 Operations on `Clock`

The `Clock` capability needs no particular right.

**`OP_CLOCK_INFO` (25).**
Returns `a1` = the counter's rate in hertz, `a2` = the address of its low word;
the high word is at `a2 + 4`.
The rate does not change while the machine runs.

**`OP_CLOCK_REGION` (26).**
`a1` = destination slot in the caller's table.
Produces a `Region` capability, read only, to the smallest region that holds the counter,
hanging below the invoked capability.
On a PMP grain coarser than eight bytes the region holds the registers beside the counter too.

### 6.13 Operation codes in numeric order

| Code | Operation | Type |
|---|---|---|
| 1 | `OP_DEBUG_PUTC` | `Debug` |
| 2 | `OP_DEBUG_HALT` | `Debug` |
| 3 | `OP_CAP_COPY` | `CapTable` |
| 4 | `OP_CAP_DELETE` | `CapTable` |
| 5 | `OP_REGION_CARVE` | `Region` |
| 6 | `OP_REGION_TO_POOL` | `Region` |
| 7 | `OP_POOL_ALLOC` | `KernelPool` |
| 8 | `OP_PROCESS_INSTALL` | `Process` |
| 9 | `OP_PROCESS_UNINSTALL` | `Process` |
| 10 | `OP_DEBUG_TRACE` | `Debug` |
| 11 | `OP_REGION_INFO` | `Region` |
| 12 | `OP_THREAD_CONFIGURE` | `Thread` |
| 13 | `OP_THREAD_RESUME` | `Thread` |
| 14 | `OP_NOTIFY_SIGNAL` | `Notification` |
| 15 | `OP_NOTIFY_WAIT` | `Notification` |
| 16 | `OP_POOL_DESTROY` | `KernelPool` |
| 17 | `OP_DEBUG_TICK` | `Debug` |
| 19 | `OP_IRQ_CARVE` | `IrqLine` |
| 20 | `OP_IRQ_BIND` | `IrqLine` |
| 21 | `OP_IRQ_SET` | `Irq` |
| 22 | `OP_DEBUG_IRQ` | `Debug` |
| 23 | `OP_CAP_REVOKE` | `CapTable` |
| 24 | `OP_CAP_DERIVE` | `CapTable` |
| 25 | `OP_CLOCK_INFO` | `Clock` |
| 26 | `OP_CLOCK_REGION` | `Clock` |

`OP_COUNT` is 27, one above the highest code.

## 7. What the root task starts with

The image in RAM contains the kernel followed by the root task.
The kernel builds the root process by hand in the **boot pool**,
4 KiB reserved by the linker,
and drops into user mode with:

- the program counter at the start of the code region, `0x80100000` on QEMU,
- the stack pointer at the top of the data region, `0x80210000` on QEMU,
- region slot 0: the code region, read and execute,
- region slot 1: the data region, read and write,
- a capability table of 64 slots, filled as below.

| Slot | Constant | Capability | Rights |
|---|---|---|---|
| 0 | `BOOT_CAP_NULL` | empty on purpose | |
| 1 | `BOOT_CAP_CAPTABLE` | the root task's own table | all |
| 2 | `BOOT_CAP_PROCESS` | the root task's own process | all |
| 3 | `BOOT_CAP_THREAD` | the root task's first thread | all |
| 4 | `BOOT_CAP_POOL` | the boot pool | all |
| 5 | `BOOT_CAP_DEBUG` | `Debug` | all |
| 6 | `BOOT_CAP_CODE` | `Region`: the root task's code | read, execute |
| 7 | `BOOT_CAP_DATA` | `Region`: the root task's data and stack | read, write |
| 8 | `BOOT_CAP_FREE_RAM` | `Region`: the block of RAM the board sets aside for the root task | all |
| 9 | `BOOT_CAP_INPUT` | `Region`: test input the loader placed in RAM | read |
| 10 | `BOOT_CAP_IRQ_LINES` | `IrqLine`: line 0 (the log) and every controller line | write |
| 11 | `BOOT_CAP_UART` | `Region`: the board's console registers, a 16550 on QEMU and the USB Serial/JTAG controller on the ESP32-C6 | read, write |
| 12 | `BOOT_CAP_LOG` | `Region`: the kernel log's header and ring | read, write |
| 13 | `BOOT_CAP_TIMER_LINES` | `IrqLine`: every timer line, `TIMER_LINES` of them | write |
| 14 | `BOOT_CAP_CLOCK` | `Clock`: the machine's counter | all |

`BOOT_CAP_COUNT` is 15; a root task puts its own slots from there upwards.

The root task's own table, process and thread take about 1.7 KiB of the boot pool,
so roughly 2.3 KiB remain for objects the root task allocates from `BOOT_CAP_POOL`.
Anything larger goes into a pool the root task makes out of free RAM.

Device ranges are granted read and write, never execute,
the counter behind `BOOT_CAP_CLOCK` read only,
and can never become a pool.
The log region can be installed but never pooled either.

Which capability sits in which slot is a convention between loader and program,
not something the kernel enforces.
The `BOOT_CAP_*` slots are the kernel's own instance of that convention
for the one program it starts itself.

## 8. Writing a program

### 8.1 Toolchain and layout

A user program is a flat binary linked against `user/user.ld.S`,
with the board's addresses, see "Boards",
and embedded into the kernel image by the Makefile.
Constraints of the current linker script:

- code and read-only data live in the read-execute region, at `0x80100000` on QEMU,
- zero-initialised data and the stack live in the read-write region, at `0x80200000` on QEMU,
- **initialised data is forbidden**: nobody would copy it into the data region,
  and the link fails if `.data` is not empty,
- `user/start.S` zeroes `.bss`, calls `main`,
  and halts with code 1 through `BOOT_CAP_DEBUG` if `main` returns.

Compile flags are `-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib`.
There is no libc; `user/rvuos.h` provides the system call wrappers:

| Wrapper | Operation |
|---|---|
| `rv_invoke(op, cap, a1, a2, a3)` | any |
| `rv_region_info(cap, &base, &size)` | `OP_REGION_INFO` |
| `rv_region_min_size(cap, &min)` | `OP_REGION_INFO`, reading `a4` |
| `rv_signal(cap, bits)` | `OP_NOTIFY_SIGNAL` |
| `rv_wait(cap, &bits)` | `OP_NOTIFY_WAIT` |
| `rv_timer_set(cap, bits, us)` | `OP_IRQ_SET` on a timer line, with the delay |
| `rv_irq_set(cap, bits)` | `OP_IRQ_SET` |
| `rv_putc(cap, c)`, `rv_puts(cap, s)` | `OP_DEBUG_PUTC` |
| `rv_halt(cap, code)` | `OP_DEBUG_HALT` |

To replace the demo, edit `user/init.c` or add a program to `USER_PROGRAMS` in the Makefile;
each program becomes its own kernel image.

### 8.2 Building a second process

Today one program is embedded, and a second process runs code
from the same code region with its own data and stack.
Loading a separate binary is a userspace job the root task does not do yet;
`DESIGN.md`, open decision 3.
The steps below are what `user/init.c` does.

1. **Carve memory** out of `BOOT_CAP_FREE_RAM`:
   one chunk for the child's kernel objects, one for its data and stack,
   one to share,
   each a 4 KiB block at an offset that is a multiple of 4 KiB.

   ```c
   rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, POOL_OFFSET, CHUNK, SLOT_POOL_REGION);
   rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, DATA_OFFSET, CHUNK, SLOT_CHILD_DATA);
   rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, SHARED_OFFSET, CHUNK, SLOT_SHARED);
   ```

2. **Make a pool** and allocate the child's objects from it,
   table first, then the process that uses it, then a thread in it.

   ```c
   rv_invoke(OP_REGION_TO_POOL, SLOT_POOL_REGION, SLOT_POOL, 0, 0);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_PROCESS, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0);
   ```

3. **Install regions** into the child: code, data, and the shared chunk.

   ```c
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 1, SLOT_CHILD_DATA, RIGHT_R | RIGHT_W);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 2, SLOT_SHARED, RIGHT_R | RIGHT_W);
   ```

4. **Fill the child's table** with exactly the authority it should have,
   narrowing rights as you go.

   ```c
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG,  BOOT_CAP_DEBUG, RIGHT_ALL);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED,    RIGHT_R | RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP,     SLOT_UP,        RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN,   SLOT_DOWN,      RIGHT_R);
   ```

5. **Configure and start** the thread.
   The stack grows down from the top of the child's data chunk.

   ```c
   rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
             free_base + DATA_OFFSET + CHUNK, 0);
   rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0);
   ```

6. **Talk** through the shared region and the two notifications.
   The child writes a word, signals `SLOT_UP`; the parent waits, answers, signals `SLOT_DOWN`.

7. **Tear down** by destroying the pool.
   The child's thread, process, table and notifications go with it,
   every capability to them is cleared in every table,
   and the region capability to the memory comes back in the slot you name.

   ```c
   rv_invoke(OP_POOL_DESTROY, SLOT_POOL, SLOT_POOL_REGION, 0, 0);
   ```

The child must touch no global variable, since it shares no data region with the parent:
everything it needs is on its stack or behind a capability in its table.

### 8.3 Writing a driver

A driver needs a region for the device's registers and one interrupt line.

```c
rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W);
rv_invoke(OP_POOL_ALLOC, POOL, CAP_NOTIFICATION, NTFN, 0);
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, UART_IRQ, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, IRQ);

for (;;) {
    rv_irq_set(IRQ, BIT_UART);      /* arm: unmask the line */
    rv_wait(NTFN, &bits);           /* the interrupt masked it and signalled */
    /* service the device */
}
```

Arming again after servicing is the acknowledgement.
Give a timer line a second bit on the same notification for a timeout.

## 9. Debugging and testing interfaces

**Tracing.**
`OP_DEBUG_TRACE` makes the kernel print one line per system call,

```
trace: op=0x00000007 slot=0x00000004 a1=0x00000007 a2=0x0000000d a3=0x00000000 -> 0x00000000
```

with `blocked` in place of a status when the call blocked,
and `trace: wake <bits>` after a call that woke a waiter.
After every traced call the kernel runs the self-check,
the executable form of the properties in `DESIGN.md`,
and halts with code 3 on the first violation.
While tracing is on the tick preempts nobody and moves no time;
only `OP_DEBUG_TICK` does,
so that the host build can reproduce the transcript.
A thread whose program counter was set while tracing was on
cannot be run under tracing; the kernel halts with code 6 instead.

**The self-check** (`kernel/selfcheck.c`) verifies, among other things:
no installed region overlaps a pool,
every process's PMP image matches its region slots,
every capability names a live object of its own type or an in-bounds region or line range,
pools tile their memory exactly and form a tree,
every waiting thread names a live notification,
every `Irq` names a notification in its own pool,
no `Irq` armed on a timer line is past its deadline,
and the controller forwards exactly the lines an `Irq` is armed on.

**Replay input.**
`BOOT_CAP_INPUT` maps 64 KiB where QEMU's loader places a file:

```c
struct replay_header { uint32_t magic; uint32_t count; };  /* magic "RVFZ", 0x5a465652 */
struct replay_record { uint8_t op; uint8_t actor; uint16_t slot; uint32_t a1, a2, a3; };
```

Each record is one system call and the thread that makes it:
`actor` 0 is whichever thread runs, 1 the driver's root thread, 2 its second thread,
3 a third that starts stopped until a record resumes it.
The replay driver in `user/fuzzdrv.c` performs them in order,
the host build in `host/` performs the same records natively under ASan and UBSan,
and `tests/differential.py` requires the two transcripts to match line for line.
Hand-written scenarios live in `tests/seeds`,
what the fuzzer found in `tests/corpus`,
and `tests/mutants/` holds one planted bug per invariant.

## 10. Limits

| Limit | Value | Where |
|---|---|---|
| region slots per process | 8 | `PROCESS_REGION_SLOTS` |
| PMP entries the kernel uses | 16, or `PMP_MAX_ENTRIES` | Makefile |
| minimum PMP entries to boot | 4 | `kernel/main.c` |
| slots per `CapTable` | 1 to 1024 | `CAPTABLE_MAX_SLOTS` |
| root task's table | 64 slots | `kernel/boot.c` |
| boot pool | 4 KiB | `kernel/kernel.ld.S` |
| smallest pool | 64 bytes, 8-byte aligned | `kernel/syscall.c` |
| object alignment | 8 bytes | `OBJ_ALIGN` |
| notification bits | 32 | the word size |
| timer lines | 16, on the whole machine | `TIMER_LINES` |
| timer delay per call | 2^32 - 1 µs | `OP_IRQ_SET` |
| tick | 1 ms (`TIMER_HZ` 1000) | `kernel/timer.h` |
| interrupt lines | 96 on QEMU, 77 on the ESP32-C6, line 0 the log's | `IRQ_LINES` |
| smallest region | 8 bytes, or the PMP grain if coarser | `region_min_size` in `kernel/pmp.h` |
| kernel log ring | 4 KiB less a 32-byte header | `KLOG_SIZE` |
| replay records per input | 256 | `REPLAY_MAX_RECORDS` |

## 11. What is not there yet

These are documented gaps, not surprises;
`TODO.md` carries the items and `DESIGN.md` the open decisions.

- **A fault stops the machine.**
  A user fault has nobody to report to,
  so the kernel prints the frame and halts with code 4,
  even when another thread could run.
- **No thread suspend.**
  A started thread can only be stopped by destroying its pool.
- **No priorities and no yield.**
  Round-robin on a tick is the whole policy;
  `DESIGN.md`, open decisions 9 and 10.
- **No badged notifications.**
  A client's identity to a server is a convention, not kernel-enforced;
  open decision 6.
- **No synchronous endpoints.**
  Shared memory and notifications carry everything; open decision 5.
- **One `Irq` per line**; open decision 11.
- **One board**, QEMU `virt`.
  ESP32-C6 is the candidate for real hardware.
- **No loader.**
  The image embeds one program, linked at fixed addresses;
  a second process runs code from the same region.
- **No initialised data** in user programs, until something copies it.
- **The log across a reset** is not yet trustworthy; open decision 12.
