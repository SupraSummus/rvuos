# rvuos user manual: system calls and the root task

Chapters 6 and 7 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

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

On ARM a call is `svc #0`, with `r0` to `r6` for `a0` to `a6` and `r12` for `a7`,
which leaves `r7` to Thumb code as its frame pointer;
the core saves `r0` to `r3`, `r12`, `lr`, `pc` and `xpsr` on the thread's stack as it traps,
so a thread's `sp` must leave 32 bytes below it that the thread may write,
or the thread faults, and one whose stacking faulted loses those registers.

No operation takes a pointer into user memory,
and no operation moves data between processes.

The C wrapper in `user/rvuos.h`, the same on both, with the registers from `user/arch/<arch>/call.h`:

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
so invoking an `Irq` operation on a `Frame` is `KERR_WRONG_TYPE`,
not `KERR_INVALID_ARG`.

A call whose work grows with the derivation tree or with a pool can be interrupted and made again:
`OP_CAP_REVOKE`, `OP_CAP_DELETE` of a root with capabilities below it,
and `OP_IRQ_BIND`.
When the tick or a device interrupt comes due while such a call works,
the kernel stops it between two capabilities or two objects
and resumes the thread at its `ecall`, or its `svc`, with every register as it was,
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
| 5 | `KERR_NO_MEMORY` | the pool has no room for the object, or the Untyped has made something already |
| 6 | `KERR_SLOT_IN_USE` | the destination slot already holds a capability |
| 7 | `KERR_OVERLAP` | a region overlaps another installed in the same process, or on the ESP32-C6 touches one as `OP_PROCESS_INSTALL` says, or the line is already bound |
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
on the ESP32-C6 the core parks after printing it,
and RP2350 prints it and reboots into BOOTSEL.
Does not return.

**`OP_DEBUG_TRACE` (10).**
Turns tracing and self-checking on; see section 9.
Every thread's account is filled, section 5.11.
Cannot be turned off again,
and once tracing is on the call changes nothing.

**`OP_DEBUG_TICK` (17).**
Does what the timer tick does, on request:
time moves by one tick, charged to the caller if it had time as its turn began, every due timer line fires,
every thread whose account reached a tick has time again,
the caller's turn ends as section 5.11 says,
and the caller stays ready.
Works while tracing is on, unlike the tick itself.

**`OP_DEBUG_IRQ` (22).**
`a1` = a line.
Does what a device interrupt on that line does:
the `Irq` armed on it masks the line and signals its bits.
`KERR_STATE` when nothing is armed on the line;
`KERR_INVALID_ARG` for a line the controller does not have, line 0 included.

**`OP_DEBUG_PREEMPT` (32).**
`a1` = n.
The n-th place from now at which a restartable call could stop between two steps, it stops,
whether or not an interrupt is pending, and `OP_DEBUG_TICK` is done there:
time moves by one tick, and the caller's turn ends with the caller ready at its `ecall`,
so another thread may run before the caller makes its call again.
Every such place counts, in whichever thread's call;
a call that finishes first leaves the rest of n to the next one.
Zero disarms it, and a second call counts n anew.
Works while tracing is on, when the tick stops no call.

**`OP_DEBUG_FRAME` (36).**
`a1` = base, `a2` = size, `a3` = destination slot in the caller's table.
Makes a frame over hardware the board lists, such as a device's registers, which no boot capability grants;
section 3 says what each board lists, and QEMU's lists nothing.
The range must be a block, as for `OP_FRAME_CARVE`, within one of the board's ranges (`KERR_INVALID_ARG`),
and gets that range's rights.
The new frame hangs below the invoked capability.
A device may be a bus master, so a `Debug` capability reaches whatever its board lists:
give it only to a program that owns the machine, and give a driver the frame, not the capability.

### 6.4 Operations on `CapTable`

All need `RIGHT_W` on the table.

**`OP_CAP_COPY` (3).**
`a1` = destination slot in the invoked table,
`a2` = source slot in the caller's table,
`a3` = rights mask ANDed into the copy.
The destination must be empty (`KERR_SLOT_IN_USE`).
The source may be any type, frames and lines included, but an Untyped (`KERR_WRONG_TYPE`).
The copy hangs beside the source in the derivation tree, section 5.2.

**`OP_CAP_DERIVE` (24).**
The arguments and errors of `OP_CAP_COPY`.
The new capability hangs below the source.
An Untyped can be derived, and only while it is free (`KERR_NO_MEMORY`), section 5.4:
the derived one is the whole of it, and the source makes nothing while that one is left.
So memory is lent: the lender keeps the source,
and a revoke below it takes back whatever the borrower made of the derived one.

**`OP_CAP_MOVE` (18).**
`a1` = destination slot in the invoked table,
`a2` = source slot in the caller's table, which must be filled (`KERR_INVALID_CAP`).
The destination must be empty (`KERR_SLOT_IN_USE`), so a slot does not move onto itself.
The capability goes to the destination with its rights, and the source is left empty.
It keeps its place in the derivation tree, section 5.2:
it hangs below what the source hung below, and what hung below the source hangs below it,
so a revoke through it takes what one through the source would have,
and a revoke that would have taken the source takes it.
Any type moves, an Untyped with what was made of it,
and so does the `CapTable` capability the call is made through.

**`OP_CAP_DELETE` (4).**
`a1` = slot in the invoked table.
Clears it; what hung below it now hangs below the slot's parent,
or, when the slot is a root, each becomes a root, and the call may be made again, section 6.1.
Clearing an empty slot succeeds.
Deleting a frame does not uninstall it anywhere,
and deleting a `KernelPool` capability does not destroy the pool.

**`OP_CAP_REVOKE` (23).**
`a1` = slot in the invoked table, which must be filled (`KERR_INVALID_CAP`).
Clears everything below it, section 5.2, and leaves the slot.
Regions installed from capabilities below it are uninstalled,
and threads of those processes lose access at once;
threads made through a `Process` capability below it stop, section 5.6,
and `Irq`s bound through a `Notification` capability below it are disarmed, section 5.9.
A revoke that takes the caller's own process, its process's table,
or the capability the call is made through ends there,
returns `KERR_OK`, and leaves the rest below the slot for another call to revoke.
Below an Untyped, every pool made of it is destroyed, section 5.5;
`KERR_STATE` if the calling thread, its process, the process's table or the invoked table
lies in the Untyped's memory.
The call may be made again, section 6.1.

### 6.5 Operations on `Frame` and `Untyped`

**`OP_FRAME_INFO` (11).**
No right needed.
Returns `a1` = base, `a2` = size, `a3` = rights,
`a4` = the size of the smallest region in bytes, a power of two of at least 8.

**`OP_FRAME_CARVE` (5).**
No right needed.
`a1` = offset from the frame's base, `a2` = size, `a3` = destination slot in the caller's table.
Produces a frame with the same rights, hanging below the invoked one.
The size must be a power of two no smaller than the smallest region,
the offset a multiple of the size,
and the sub-range within the frame (`KERR_INVALID_ARG`),
so the new frame is a block.
The parent capability is unchanged.

**`OP_UNTYPED_INFO` (27).**
No right needed.
Returns `a1` = base, `a2` = size, `a3` = rights,
`a4` = 1 while something made of it is left, 0 while it is free, section 5.4.

**`OP_UNTYPED_RETYPE` (6).**
`a1` = the type, `CAP_FRAME` or `CAP_POOL` (`KERR_INVALID_ARG`),
`a2` = destination slot in the caller's table.
Makes the whole of the Untyped into it, and returns `a1` = its base.
A pool needs `RIGHT_R` and `RIGHT_W` on the Untyped (`KERR_NO_RIGHTS`)
and an Untyped no smaller than `POOL_MIN_SIZE` (`KERR_INVALID_ARG`);
a frame needs no right and carries the invoked one's.
`KERR_NO_MEMORY` while something made of the Untyped is left.
The new capability hangs below the invoked one, a pool's below the pool's own node.

**`OP_UNTYPED_SPLIT` (30).**
No right needed.
`a1` = destination slot for the lower half, `a2` = for the upper half, two slots in the caller's table.
Makes the two halves of the Untyped, each an Untyped with the invoked one's rights, hanging below it.
A half must be no smaller than the smallest region, and the two slots must differ (`KERR_INVALID_ARG`).
`KERR_NO_MEMORY` while something made of the Untyped is left.

### 6.6 Operations on `KernelPool`

A pool has one operation, which needs `RIGHT_W` on it;
it is destroyed by revoking below its Untyped, section 5.5.

**`OP_POOL_ALLOC` (7).**
`a1` = object type, `a2` = destination slot, `a3` = type-specific:

| `a1` | `a3` | Rule |
|---|---|---|
| `CAP_CAPTABLE` | number of slots, 1 to `CAPTABLE_MAX_SLOTS` (1024) | |
| `CAP_PROCESS` | slot of the `CapTable` capability the process will use, with `RIGHT_W` | the table may lie in any pool; the process's hold on it hangs below that capability |
| `CAP_THREAD` | slot of the `Process` capability the thread will run in, with `RIGHT_W` | the process may lie in any pool; the thread's hold on it hangs below that capability; the thread starts stopped |
| `CAP_NOTIFICATION` | unused | |

Any other type is `KERR_INVALID_ARG`; `Irq` objects come from `OP_IRQ_BIND`.
The new capability carries all rights and hangs below the invoked `KernelPool` capability.
`KERR_NO_MEMORY` when the pool is full,
`KERR_STATE` while the pool is being destroyed.

### 6.7 Operations on `Process`

Both need `RIGHT_W` on the process.

**`OP_PROCESS_INSTALL` (8).**
`a1` = region slot index, below `PROCESS_REGION_SLOTS`,
`a2` = slot of a frame in the caller's table,
`a3` = rights to install.
The rights must be a non-empty subset of the frame's (`KERR_NO_RIGHTS`);
write without read is `KERR_INVALID_ARG`, and so on ARM is execute without read;
an occupied region slot is `KERR_SLOT_IN_USE`;
overlap with another region in this process is `KERR_OVERLAP`,
and so, on the ESP32-C6, is a writable region ending where a readable but not writable one begins;
too few PMP entries is `KERR_LIMIT`.
The installed region hangs below the frame in the derivation tree,
so `OP_CAP_REVOKE` on that capability's slot, or on any slot above it, uninstalls it.

**`OP_PROCESS_UNINSTALL` (9).**
`a1` = region slot index.
Clears it and rebuilds the PMP image.
Threads of the process lose access at once.
Clearing an empty slot succeeds.

### 6.8 Operations on `Thread`

All need `RIGHT_W` on the thread,
and `OP_THREAD_CONFIGURE`, `OP_THREAD_RESUME`, `OP_THREAD_READ_REG` and `OP_THREAD_WRITE_REG` a stopped thread (`KERR_STATE`).

**`OP_THREAD_CONFIGURE` (12).**
`a1` = program counter, `a2` = stack pointer.
Neither is checked.
On ARM the program counter is taken as a branch takes it, with bit 0 set for Thumb as a function pointer has it;
one without bit 0 starts the thread in a fault.

**`OP_THREAD_RESUME` (13).**
Makes the thread ready.
It runs once it is bound to units, or to spare time.
A thread that faulted runs the faulting instruction again, unless `OP_THREAD_WRITE_REG` moved its program counter.
`KERR_STATE` too once the thread's process was taken, section 5.6.

**`OP_THREAD_WATCH` (31).**
`a1` = slot of a `Notification` capability, which needs `RIGHT_W`, and may lie in any pool;
`a2` = the bits the thread's faults signal there.
Replaces the watch the thread had.
`a2` = 0 clears the watch and ignores `a1`.
See section 5.6.

**`OP_THREAD_FAULT` (33).**
What stopped a thread that faulted:
`a1` = the cause, `a2` = the program counter, `a3` = the address, `a4` = the status.

| | RISC-V | ARM |
|---|---|---|
| `a1`, the cause | `mcause` | the exception number |
| `a2`, the program counter | `mepc` | the pc, with bit 0 set for Thumb |
| `a3`, the address | `mtval`: on an access fault the address reached for, on RP2350 always zero | `MMFAR` or `BFAR` while `CFSR` says it is valid, `SFAR` for a SecureFault, else zero |
| `a4`, the status | zero | `CFSR` |

The program counter is where a resume goes on, as `OP_THREAD_READ_REG` reads it.
Under QEMU, on the Cortex-M3 and the Cortex-M33 alike, a `bkpt` arrives as a HardFault, exception 3, with a status of zero,
and on RP2350's Cortex-M33 as a DebugMonitor exception, 12.
A status with `MSTKERR` or `STKERR` says the core could not stack the thread's registers,
so `r0` to `r3`, `r12`, `lr`, the pc and `xpsr` hold what its last trap left.
`KERR_STATE` unless the thread is stopped where it faulted:
a resume or a configure since, or no fault at all, leaves nothing to tell.

**`OP_THREAD_READ_REG` (34).**
`a1` = the register: `x0` to `x31` on RISC-V, where `x0` reads zero, `r0` to `r14` on ARM,
or `THREAD_REG_PC` (32) for the program counter, which on ARM carries bit 0 for Thumb, as `OP_THREAD_CONFIGURE` takes it.
Returns `a1` = its value.
`KERR_INVALID_ARG` for a number that names no register.

**`OP_THREAD_WRITE_REG` (35).**
`a1` = the register, as for `OP_THREAD_READ_REG`; a write to `x0` changes nothing.
`a2` = the value.
A pc written is taken as `OP_THREAD_CONFIGURE` takes it, and on ARM leaves an IT block, keeping the flags.
The thread stays stopped where it faulted, and `OP_THREAD_FAULT` tells of it until the resume.
While tracing, the thread can no longer run, as after `OP_THREAD_CONFIGURE`; section 9.
`KERR_INVALID_ARG` for a number that names no register.

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
A table operation like `OP_FRAME_CARVE`.

**`OP_IRQ_BIND` (20).**
Needs `RIGHT_W`, and the capability must name exactly one line (`KERR_INVALID_ARG`).
`a1` = slot of the `KernelPool` capability to allocate the `Irq` from, with `RIGHT_W`,
`a2` = slot of the `Notification` capability the `Irq` signals, with `RIGHT_W`,
which may lie in any pool, and the `Irq`'s hold on it hangs below that capability, section 5.9,
`a3` = destination slot for the `Irq` capability, which may be the invoked slot.
The invoked slot is cleared with everything below it,
and the `Irq` capability hangs below the `KernelPool` capability.
`KERR_OVERLAP` if an `Irq` is already bound to the line,
or the line is the one the kernel keeps for its cores, section 5.9,
`KERR_NO_MEMORY` if the pool has no room for it,
`KERR_STATE` while the pool is being destroyed,
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
On a timer line, `a3` = 0 or `IRQ_SET_PERIOD`, and unused elsewhere.
With `IRQ_SET_PERIOD`, `a2` is a period counted from the line's last deadline, section 5.8,
and the call returns `a1` = the periods skipped.
`KERR_INVALID_ARG` for any other bit of `a3`, or a period of zero.
With `a1` = 0, `a2` and `a3` are ignored.
`KERR_STATE` once the `Irq`'s notification was taken, section 5.9: it has nothing to signal.

### 6.12 Operations on `Clock`

The `Clock` capability needs no particular right.

**`OP_CLOCK_INFO` (25).**
Returns `a1` = the counter's rate in hertz, `a2` = the address of its low word;
the high word is at `a2 + 4`.
The rate does not change while the machine runs.

**`OP_CLOCK_FRAME` (26).**
`a1` = destination slot in the caller's table.
Produces a frame, read only, the smallest block that holds the counter,
hanging below the invoked capability.
On a PMP grain coarser than eight bytes the frame holds the registers beside the counter too.

### 6.13 Operations on `Time`

**`OP_TIME_CARVE` (28).**
No right needed.
`a1` = offset from the first unit, `a2` = count, `a3` = destination slot.
A table operation like `OP_IRQ_CARVE`.

**`OP_TIME_BIND` (29).**
Needs `RIGHT_W`.
`a1` = slot of the `Thread` capability, with `RIGHT_W`,
`a2` = the first unit, as an offset from the capability's first
(`KERR_INVALID_ARG` past the last, even for none),
`a3` = how many units (`KERR_INVALID_ARG` past the last), zero for none.
The units must all be of one core, `KERR_INVALID_ARG` otherwise,
and the thread runs on that core, or with none on the first unit's; section 5.11.
Fails with `KERR_OVERLAP` if another thread earns one of the units.
The thread leaves the units it earned, whoever bound it to them,
and earns these; with `RIGHT_X` on the invoked capability it runs on spare time too.
Its account keeps what it held, up to what the new units hold at most,
and a ready thread joins the back of the queue its account puts it on, section 5.11.
Its hold on the units hangs below the invoked capability,
so revoking below that capability unbinds it:
it keeps its state and does not run until it is bound again, and its account is emptied.
A thread unbound or moved while it runs, the caller itself for one, finishes the turn it had:
only a wait, the tick, a fault, section 5.6, or a revoke that takes its own process, section 6.4, takes the processor from it.

### 6.14 Operation codes in numeric order

| Code | Operation | Type |
|---|---|---|
| 1 | `OP_DEBUG_PUTC` | `Debug` |
| 2 | `OP_DEBUG_HALT` | `Debug` |
| 3 | `OP_CAP_COPY` | `CapTable` |
| 4 | `OP_CAP_DELETE` | `CapTable` |
| 5 | `OP_FRAME_CARVE` | `Frame` |
| 6 | `OP_UNTYPED_RETYPE` | `Untyped` |
| 7 | `OP_POOL_ALLOC` | `KernelPool` |
| 8 | `OP_PROCESS_INSTALL` | `Process` |
| 9 | `OP_PROCESS_UNINSTALL` | `Process` |
| 10 | `OP_DEBUG_TRACE` | `Debug` |
| 11 | `OP_FRAME_INFO` | `Frame` |
| 12 | `OP_THREAD_CONFIGURE` | `Thread` |
| 13 | `OP_THREAD_RESUME` | `Thread` |
| 14 | `OP_NOTIFY_SIGNAL` | `Notification` |
| 15 | `OP_NOTIFY_WAIT` | `Notification` |
| 17 | `OP_DEBUG_TICK` | `Debug` |
| 18 | `OP_CAP_MOVE` | `CapTable` |
| 19 | `OP_IRQ_CARVE` | `IrqLine` |
| 20 | `OP_IRQ_BIND` | `IrqLine` |
| 21 | `OP_IRQ_SET` | `Irq` |
| 22 | `OP_DEBUG_IRQ` | `Debug` |
| 23 | `OP_CAP_REVOKE` | `CapTable` |
| 24 | `OP_CAP_DERIVE` | `CapTable` |
| 25 | `OP_CLOCK_INFO` | `Clock` |
| 26 | `OP_CLOCK_FRAME` | `Clock` |
| 27 | `OP_UNTYPED_INFO` | `Untyped` |
| 28 | `OP_TIME_CARVE` | `Time` |
| 29 | `OP_TIME_BIND` | `Time` |
| 30 | `OP_UNTYPED_SPLIT` | `Untyped` |
| 31 | `OP_THREAD_WATCH` | `Thread` |
| 32 | `OP_DEBUG_PREEMPT` | `Debug` |
| 33 | `OP_THREAD_FAULT` | `Thread` |
| 34 | `OP_THREAD_READ_REG` | `Thread` |
| 35 | `OP_THREAD_WRITE_REG` | `Thread` |
| 36 | `OP_DEBUG_FRAME` | `Debug` |

`OP_COUNT` is 37, one above the highest code; 16 is unused.

## 7. What the root task starts with

The image in RAM contains the kernel followed by the root task.
The kernel builds the root process by hand in the **boot pool**,
4 KiB of the root task's own memory,
and drops into user mode with:

- the program counter at the start of the code region, `0x80100000` on QEMU,
- the stack pointer at the top of the data region, `0x80120000` on QEMU,
- region slot 0: the code region, read and execute,
- region slot 1: the data region, read and write,
- a capability table of `ROOT_TABLE_SLOTS`, 64, filled as below.

| Slot | Constant | Capability | Rights |
|---|---|---|---|
| 0 | `BOOT_CAP_NULL` | empty on purpose | |
| 1 | `BOOT_CAP_CAPTABLE` | the root task's own table | all |
| 2 | `BOOT_CAP_PROCESS` | the root task's own process | all |
| 3 | `BOOT_CAP_THREAD` | the root task's first thread | all |
| 4 | `BOOT_CAP_POOL` | the boot pool | all |
| 5 | `BOOT_CAP_DEBUG` | `Debug` | all |
| 6 | `BOOT_CAP_CODE` | `Frame`: the root task's code | read, execute |
| 7 | `BOOT_CAP_DATA` | `Frame`: the root task's data and stack | read, write |
| 8 | `BOOT_CAP_FREE_RAM` | `Untyped`: the block of RAM the board sets aside for the root task | all |
| 9 | `BOOT_CAP_INPUT` | `Frame`: test input the loader placed in RAM | read |
| 10 | `BOOT_CAP_IRQ_LINES` | `IrqLine`: line 0 (the log) and every controller line | write |
| 11 | `BOOT_CAP_UART` | `Frame`: the board's console registers, a 16550 on QEMU, the USB Serial/JTAG controller on the ESP32-C6, a block of RAM on RP2350 | read, write |
| 12 | `BOOT_CAP_LOG` | `Frame`: the kernel log's header and ring | read, write |
| 13 | `BOOT_CAP_TIMER_LINES` | `IrqLine`: every timer line, `TIMER_LINES` of them | write |
| 14 | `BOOT_CAP_CLOCK` | `Clock`: the machine's counter | all |
| 15 | `BOOT_CAP_TIME` | `Time`: every unit of time, `TIME_UNITS` of each core, the first core's all earned by the root task's thread | write, execute |
| 16 | `BOOT_CAP_ROOT_RAM` | `Untyped`: the root task's own memory, its code, data and input frames and the boot pool's block, all made already | all |
| 17 | `BOOT_CAP_POOL_RAM` | `Untyped`: the boot pool's block, below `BOOT_CAP_ROOT_RAM`, made into the boot pool | all |

`BOOT_CAP_COUNT` is 18; a root task puts its own slots from there upwards.

The root task's own table, process and thread take about 2.1 KiB of the boot pool,
so roughly 1.9 KiB remain for objects the root task allocates from `BOOT_CAP_POOL`.
Anything larger goes into a pool the root task makes out of free RAM.
The capabilities to the boot pool, its table, process and thread
hang below the boot pool's own node, not below `BOOT_CAP_POOL`,
so revoking below `BOOT_CAP_POOL` takes only what was allocated through it.

`BOOT_CAP_ROOT_RAM` is an Untyped over the root task's own memory,
with its code, data and input frames and the boot pool's block, `BOOT_CAP_POOL_RAM`, right below it,
so it makes nothing until those are gone, and the rest of it, past the boot pool, waits for them.
The boot pool's own node hangs below `BOOT_CAP_POOL_RAM`, as a retype would have put it,
so revoking below that block destroys the boot pool and leaves the root task's code where it is.
The root task cannot revoke below either, since it lives there, and nothing else sets it apart:
a process holding its capabilities can do all it could.
So the root task can hand its place over:
it moves every capability into a successor's table with `OP_CAP_MOVE`, each to the same slot, and waits,
and the successor stops the root task's thread and revokes below `BOOT_CAP_POOL_RAM`,
which destroys the boot pool, and the root task with it.
`user/init.c` ends its demo this way; `DESIGN.md`, "The root task is its capabilities", gives the steps.

Device ranges are frames, granted read and write, never execute,
the counter behind `BOOT_CAP_CLOCK` read only,
and no Untyped covers them, so they can never become a pool.
Neither can the log, which is a frame too.

Which capability sits in which slot is a convention between loader and program,
not something the kernel enforces.
The `BOOT_CAP_*` slots are the kernel's own instance of that convention
for the one program it starts itself.
