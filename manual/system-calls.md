# rvuos user manual: system calls and the root task

This file holds chapters 6 and 7 of the user manual.
[`MANUAL.md`](../MANUAL.md) lists all chapters,
and explains the words these chapters use.

## 6. System call reference

### 6.1 Invocation ABI

The numbers in this section are copied from `include/rvuos/abi.h`.
That file is the binding contract.

Every system call is an `ecall`, with these registers:

| Register | On `ecall` | On return |
|---|---|---|
| `a7` | operation code, one of `OP_*` | unchanged |
| `a0` | slot of the invoked capability | status, one of `KERR_*`; zero on success |
| `a1` to `a3` | arguments | results, where the operation documents them; otherwise unchanged |
| `a4` | reserved | a result where documented; otherwise unchanged |
| `a5`, `a6` | reserved | unchanged |

On ARM, a call is `svc #0`.
Registers `r0` to `r6` stand for `a0` to `a6`, and `r12` stands for `a7`.
This leaves `r7` to Thumb code as its frame pointer.
When the core traps, it saves `r0` to `r3`, `r12`, `lr`, `pc` and `xpsr` on the thread's stack.
So the 32 bytes just under a thread's `sp` must be memory that the thread may write.
Otherwise the thread faults,
and a thread whose stacking faulted loses those registers.

No operation takes a pointer into user memory,
and no operation moves data between processes.

The C wrapper is in `user/rvuos.h`.
It is the same on both architectures,
and it takes the registers from `user/arch/<arch>/call.h`:

```c
uint32_t rv_invoke(uint32_t op, uint32_t cap, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t rv_invoke6(uint32_t op, uint32_t cap, uint32_t a1, uint32_t a2, uint32_t a3,
                    uint32_t a4, uint32_t a5, uint32_t a6);
```

`rv_invoke6` also sets `a4` to `a6`, as the replay driver does with the values of a record.
`rv_invoke` leaves `a4` to `a6` as they were.

**Slot numbers.**
Every slot that an argument names, `a0` included, is a slot of the caller's tables,
unless this chapter says that it is in the invoked table.
Such a slot is named by a slot number, `SLOT_IN(table slot, index)` (section 5.2).
A number less than 1024 is the index in the first table.
A slot of the invoked table, such as the destination of `OP_CAP_COPY`, is named by its index in that table.

**The order of the checks.**
The kernel checks a call in this order:

1. The slot resolves (`KERR_INVALID_CAP`).
2. If the operation is `OP_CAP_INFO`, it checks its own arguments next (section 6.16).
   Every type takes `OP_CAP_INFO`, with no right,
   so the later checks do not apply to it.
3. The capability's type takes the operation (`KERR_WRONG_TYPE`).
4. The capability carries the right that the operation needs, as each operation's entry says (`KERR_NO_RIGHTS`).
5. Every argument among `a1` to `a3` that the operation does not read is zero (`KERR_INVALID_ARG`).
   So is every bit of a rights mask above `RIGHT_ALL`.
   This lets a later kernel give such an argument a meaning
   without changing what an existing program does.
6. The operation's own checks.

Operation codes are one flat numbering across all types.
So invoking an `Irq` operation on a `Frame` fails with `KERR_WRONG_TYPE`, not `KERR_INVALID_ARG`,
whatever the frame's rights.
A code that is no operation of any type also fails with `KERR_WRONG_TYPE`:
0, 11, 26, 27, and `OP_COUNT` and above (section 6.17).

**Which arguments an operation reads.**
Each operation's entry says which arguments it reads.
For three operations, this depends on the values of their own arguments:

- `OP_POOL_ALLOC` of a notification does not read `a3`.
- `OP_THREAD_WATCH` with no bits does not read `a1`.
- `OP_IRQ_SET` with no bits, or on a line that is not a timer line, does not read `a2` or `a3`.

Only these operations read `a4`:
`OP_POOL_ALLOC` of a process or a thread, and `OP_TIME_BIND`.
No operation reads `a5` or `a6`.
Where `a4`, `a5` and `a6` are not read, they are not checked.

**Calls that are made again.**
The work of some calls grows with the derivation tree or with a pool.
These calls can be interrupted and made again:

- `OP_CAP_REVOKE`,
- `OP_CAP_DELETE` of a root with capabilities below it,
- `OP_IRQ_BIND`, `OP_ROUTE_BIND` and `OP_DMA_BIND`.

The timer or a device interrupt may come due while such a call works.
The kernel then stops the call between two capabilities or two objects.
It resumes the thread at its `ecall` (or its `svc`), with every register as it was.
So the thread makes the same call again when it next runs,
and the call goes on from what it had already cleared.
A program sees no difference except in time.
The exception is another thread that changes the same capabilities in between.
The call made again checks everything again, and it may fail.
What it already revoked stays revoked.

### 6.2 Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `KERR_OK` | success |
| 1 | `KERR_INVALID_CAP` | the slot is out of range or empty, or its object was destroyed, or the slot number points into a table slot that holds no table; also every call of a process whose tables were all taken |
| 2 | `KERR_WRONG_TYPE` | the capability's type does not accept this operation |
| 3 | `KERR_NO_RIGHTS` | the capability lacks a right that the operation needs |
| 4 | `KERR_INVALID_ARG` | an argument is out of range, or breaks a rule that this chapter states |
| 5 | `KERR_NO_MEMORY` | the pool has no room for the object, or the Untyped has made something already |
| 6 | `KERR_SLOT_IN_USE` | the destination slot already holds a capability, or the table slot already holds a table |
| 7 | `KERR_OVERLAP` | a region overlaps another region installed in the same process; on the ESP32-C6, also a region that touches another one, as `OP_PROCESS_INSTALL` says; or the line or channel is already bound |
| 8 | `KERR_LIMIT` | a fixed kernel limit was hit, such as a thread's places |
| 9 | `KERR_STATE` | the object is not in a state that allows this |

### 6.3 Operations on `Print`, `Halt` and `Test`

These are three types.
None of them has an object behind it, and each of them needs `RIGHT_W`:

- `Print` writes into the log.
- `Halt` halts the machine.
- `Test` drives the machine as a test does.

An operation of one of the other two types fails with `KERR_WRONG_TYPE`.
So a child given `Print` writes into the log and can do nothing else.
A supervisor given `Halt` can halt the machine, but cannot drive it.

**`OP_PRINT` (1)**, on `Print`.
Appends bytes to the kernel log (section 5.10).

- `a1`, `a2`, `a3`: up to `PRINT_BYTES` (12) bytes, the lowest byte of `a1` first.
  The bytes end at the first zero byte.

`rv_write` in `user/rvuos.h` writes a longer text, with one call for each 12 bytes.

**`OP_HALT` (2)**, on `Halt`.
Stops the machine.
It does not return.

- `a1`: the exit code.

What happens to the exit code depends on the board:

- On QEMU, the emulator exits with that code.
- On the ESP32-C6, the kernel prints the code, and then the core parks.
- On RP2350, the kernel prints the code, and then the chip reboots into BOOTSEL.

**`OP_TEST_TRACE` (10)**, on `Test`.
Turns tracing and self-checking on (section 9).
From then on, the clock starts from where the counter was,
and only `OP_TEST_TICK` moves it.
Every virtual time is set to the clock (section 5.11).
Tracing cannot be turned off again.
Once tracing is on, the call changes nothing.

Errors:

- `KERR_WRONG_TYPE`: the kernel was built without the self-check (section 9).

**`OP_TEST_TICK` (17)**, on `Test`.
Does what the timer does, when a program calls it: it moves the clock one step on.

- The clock moves on by one millisecond of the counter's counts.
  This time is charged to the caller, as if the caller had run it (section 5.11).
- Every timer line that is due fires.
- Every thread whose virtual time has come has time again.
- The caller's turn ends, as section 5.11 says.
- The caller stays ready.

The call works while tracing is on.
The timer itself does not do this while tracing is on.

Errors:

- `KERR_STATE`: tracing is off.
  A step that no time brought would make every deadline fire early.

**`OP_TEST_IRQ` (22)**, on `Test`.
Does what a device interrupt on a line does:
the `Irq` armed on that line masks the line and signals its bits.

- `a1`: the line.

Errors:

- `KERR_STATE`: tracing is off, or nothing is armed on the line.
- `KERR_INVALID_ARG`: the controller does not have this line.
  Line 0 is one of these.

**`OP_TEST_PREEMPT` (32)**, on `Test`.
Makes a restartable call stop, as if an interrupt had come (section 6.1).

- `a1`: n.

The kernel counts the points at which a restartable call could stop between two steps.
At the n-th such point from now, the call stops,
whether or not an interrupt is pending.
The kernel then does what `OP_TEST_TICK` does:

- The clock moves one step.
- The caller's turn ends.
- The caller stays ready at its `ecall`.
  So another thread may run before the caller makes its call again.

Every such point counts, in the call of any thread.
If a call finishes first, the rest of n carries over to the next call.
n = 0 disarms the count.
A second call starts counting n again from the beginning.

The call works while tracing is on.
While tracing is on, the timer stops no call.

Errors:

- `KERR_STATE`: tracing is off, as for `OP_TEST_TICK`.

### 6.4 Operations on `CapTable`

All operations on a table need `RIGHT_W` on the table.

**`OP_CAP_COPY` (3).**
Copies a capability from the caller's tables into the invoked table.

- `a1`: the destination slot, in the invoked table.
- `a2`: the source slot, in the caller's tables.
- `a3`: a rights mask.
  The copy has the source's rights ANDed with this mask.

The source may be of any type, frames and lines included, except an Untyped.
The copy goes next to the source in the derivation tree (section 5.2).

Errors:

- `KERR_SLOT_IN_USE`: the destination is not empty.
- `KERR_WRONG_TYPE`: the source is an Untyped.

**`OP_CAP_DERIVE` (24).**
Derives a capability from one in the caller's tables, into the invoked table.
The arguments and the errors are those of `OP_CAP_COPY`.
The new capability goes below the source.

An Untyped can be derived, but only while it is free (section 5.4).
Otherwise the call fails with `KERR_NO_MEMORY`.
The derived Untyped covers the whole of the source.
The source makes nothing while the derived one is left.
So memory can be lent:
whoever lends it keeps the source,
and a revoke below the source takes back whatever was made of the derived one.

**`OP_CAP_MOVE` (18).**
Moves a capability from the caller's tables into the invoked table.

- `a1`: the destination slot, in the invoked table.
- `a2`: the source slot, in the caller's tables.
  It must hold a capability.

Errors:

- `KERR_INVALID_CAP`: the source slot is empty.
- `KERR_SLOT_IN_USE`: the destination is not empty.
  So a slot cannot move onto itself.

The capability goes to the destination with its rights,
and the source slot is left empty.
The capability keeps its position in the derivation tree (section 5.2):

- It is below what the source was below.
- What was below the source is now below it.

So a revoke through it takes what a revoke through the source would have taken.
A revoke that would have taken the source takes it.

A capability of any type can move.
An Untyped moves together with what was made of it.
The `CapTable` capability that the call is made through can move too.

**`OP_CAP_DELETE` (4).**
Clears a slot.

- `a1`: the slot, in the invoked table.

If the slot has a parent, what was below the cleared capability is now below that parent.
If the slot is a root, each capability that was directly below it becomes a root,
and the call may be made again (section 6.1).

Suppose the delete of an Untyped that is a root stops part way.
Until that Untyped is gone, it makes nothing (`KERR_NO_MEMORY`).
This holds even once nothing is left below it,
because the capabilities that already became roots lie in its memory.

Clearing an empty slot succeeds.
Deleting a frame does not uninstall it anywhere.
Deleting a `KernelPool` capability does not destroy the pool.

**`OP_CAP_REVOKE` (23).**
Clears everything below a slot (section 5.2), and leaves the slot itself.

- `a1`: the slot, in the invoked table.
  It must hold a capability.

The revoke has these effects:

- Regions installed from capabilities below the slot are uninstalled.
  Threads of those processes lose access at once.
- Threads made through a `Process` capability below the slot stop (section 5.6).
- `Irq`s bound through a `Notification` capability below the slot are disarmed (section 5.9).
- Below an Untyped, every pool made of it is destroyed (section 5.5).

A revoke may reach one of these:
the caller's own process,
the table that the call's `a0` lies in,
or the capability that the call is made through.
The revoke then ends there and returns `KERR_OK`.
The rest below the slot stays, for another call to revoke.

The call may be made again (section 6.1).

Errors:

- `KERR_INVALID_CAP`: the slot is empty.
- `KERR_STATE`: the slot is an Untyped, and one of these lies in its memory:
  the calling thread, its process, a table that the process mounts, or the invoked table.

### 6.5 Operations on `Frame` and `Untyped`

`OP_CAP_INFO` tells what a frame or an Untyped covers (section 6.16).

**`OP_FRAME_CARVE` (5).**
Makes a smaller frame from the invoked one.
Needs no right.

- `a1`: the offset from the frame's base.
- `a2`: the size.
- `a3`: the destination slot, in the caller's tables.

The new frame has the same rights, and it goes below the invoked one.
The parent capability is unchanged.

Errors:

- `KERR_INVALID_ARG`: one of these rules is broken:
  - The size is a power of two, and no smaller than the smallest region.
  - The offset is a multiple of the size.
  - The sub-range lies within the frame.

  These rules make the new frame a block.

**`OP_UNTYPED_RETYPE` (6).**
Makes the whole of the Untyped into a frame or a pool.

- `a1`: the type, `CAP_FRAME` or `CAP_POOL`.
- `a2`: the destination slot, in the caller's tables.

Returns:

- `a1`: the base of the memory.

A frame needs no right, and it carries the rights of the invoked Untyped.
A pool needs `RIGHT_R` and `RIGHT_W` on the Untyped,
and an Untyped no smaller than `POOL_MIN_SIZE`.
The new capability goes below the invoked one.
For a pool, the new capability goes below the pool's own node.

Errors:

- `KERR_INVALID_ARG`: `a1` is another type,
  or `a1` is `CAP_POOL` and the Untyped is smaller than `POOL_MIN_SIZE`.
- `KERR_NO_RIGHTS`: `a1` is `CAP_POOL`, and the Untyped lacks `RIGHT_R` or `RIGHT_W`.
- `KERR_NO_MEMORY`: something made of the Untyped is still left.

**`OP_UNTYPED_SPLIT` (30).**
Makes the two halves of the Untyped.
Each half is an Untyped with the invoked one's rights, and it goes below the invoked one.
Needs no right.

- `a1`: the destination slot for the lower half.
- `a2`: the destination slot for the upper half.

Both slots are in the caller's tables.

Errors:

- `KERR_INVALID_ARG`: a half would be smaller than the smallest region, or the two slots are the same.
- `KERR_NO_MEMORY`: something made of the Untyped is still left.

### 6.6 Operations on `KernelPool`

A pool can do two things:

- It allocates objects.
  This needs `RIGHT_W` on the pool.
- It tells what an allocation would take of it.
  This needs no right.

A pool is destroyed by revoking below its Untyped (section 5.5).

**`OP_POOL_ALLOC` (7).**
Allocates an object from the pool.

- `a1`: the object type.
- `a2`: the destination slot.
- `a3`: depends on the type, as the table shows.
- `a4`: for a process or a thread only, as the table shows.

| `a1` | `a3` | Rule |
|---|---|---|
| `CAP_CAPTABLE` | the number of slots, 1 to `CAPTABLE_MAX_SLOTS` (1024) | |
| `CAP_PROCESS` | the slot of the `CapTable` capability that the process will use, with `RIGHT_W` | `a4` is the number of its region slots: 1 to the most that a process may have, which `OP_CAP_INFO` on any process tells; otherwise `KERR_INVALID_ARG` (section 5.4). The table may lie in any pool. The process holds the table in its table slot 0, and this hold is below that capability. |
| `CAP_THREAD` | the slot of the `Process` capability that the thread will run in, with `RIGHT_W` | `a4` is the number of its places, the cores whose time it may hold at once: 1 to the number of the machine's cores; otherwise `KERR_INVALID_ARG` (section 5.11). The process may lie in any pool. The thread's hold on the process is below that capability. The thread starts stopped. |
| `CAP_NOTIFICATION` | unused | |

`Irq` objects are not allocated here; they come from `OP_IRQ_BIND`.
The new capability carries all rights,
and it goes below the invoked `KernelPool` capability.

Errors:

- `KERR_INVALID_ARG`: `a1` is any other type, or `a3` or `a4` is out of range, as the table says.
- `KERR_NO_MEMORY`: the pool is full.
- `KERR_STATE`: the pool is being destroyed.

**`OP_POOL_INFO` (42).**
Tells how many bytes of the pool an object would take.
Needs no right.

- `a1`: the type. It is one of these:
  - `CAP_CAPTABLE`, `CAP_PROCESS`, `CAP_THREAD` or `CAP_NOTIFICATION`, as `OP_POOL_ALLOC` makes them,
  - `CAP_IRQ`, as `OP_IRQ_BIND` makes one in a pool,
  - `CAP_ROUTE`, as `OP_ROUTE_BIND` makes one,
  - `CAP_DMA`, as `OP_DMA_BIND` makes one,
  - `CAP_POOL`, for the descriptor at the base of a pool.
- `a2`: 0.
- `a3`: depends on the type:
  - for `CAP_CAPTABLE`, the number of slots, 1 to 1024,
  - for `CAP_PROCESS`, the number of region slots, 1 to the most a process may have,
  - for `CAP_THREAD`, the number of places, 1 to the number of the machine's cores,
  - for every other type, 0.

Returns:

- `a1`: the bytes that the object takes, as the pool counts them.
  This is a multiple of 16.
- `a2`: the bytes left in the pool: its room, as `OP_CAP_INFO` tells it.
  This is zero while the pool is being destroyed.
- `a3` and `a4` are unchanged.

Errors:

- `KERR_INVALID_ARG`: any other value of the arguments.
  The call then writes nothing.

An allocation of this type that passes its other checks
fails with `KERR_NO_MEMORY` exactly when `a1` is more than `a2`.
The exception is when something else allocates from the pool in between:
on another core,
or while a bind has stopped and waits to be made again.
A new pool of n bytes has n bytes left, less the bytes of its descriptor.
The sizes belong to the kernel build, and depend on its architecture and its board.
They may change from one kernel to the next (section 5.5).

### 6.7 Operations on `Process`

All four operations need `RIGHT_W` on the process.

**`OP_PROCESS_INSTALL` (8).**
Installs a frame as a region of the process.

- `a1`: the region slot index.
  It must be less than the number of the process's region slots.
- `a2`: the slot of a frame, in the caller's tables.
- `a3`: the rights to install.

Errors:

- `KERR_INVALID_ARG`: `a1` is not less than the number of region slots.
- `KERR_NO_RIGHTS`: the rights are empty, or are not a subset of the frame's rights.
- `KERR_INVALID_ARG`: the rights are write without read.
  On ARM, execute without read is also refused this way.
- `KERR_SLOT_IN_USE`: the region slot is occupied.
- `KERR_OVERLAP`: the region overlaps another region in this process.
  On the ESP32-C6, this is also the error for a writable region
  that ends where a readable but not writable region begins.

A process has no more region slots than the core has PMP entries for them.
So every slot can be filled at once.
The installed region goes below the frame in the derivation tree.
So `OP_CAP_REVOKE` on the frame's slot, or on any slot above it, uninstalls the region.

**`OP_PROCESS_UNINSTALL` (9).**
Clears a region slot and rebuilds the PMP image.
Threads of the process lose access at once.
Clearing an empty slot succeeds.

- `a1`: the region slot index.

**`OP_PROCESS_MOUNT` (39).**
Mounts a table in one of the process's table slots.

- `a1`: the table slot index.
  It must be less than `PROCESS_TABLES`.
- `a2`: the slot of a `CapTable` capability with `RIGHT_W`, in the caller's tables.

Errors:

- `KERR_INVALID_ARG`: `a1` is not less than `PROCESS_TABLES`.
- `KERR_NO_RIGHTS`: the `CapTable` capability lacks `RIGHT_W`.
- `KERR_SLOT_IN_USE`: the table slot already holds a table.

The process's threads then name the table's slots `SLOT_IN(a1, index)` (section 5.2).
The process's hold on the table is below that capability in the derivation tree.
So `OP_CAP_REVOKE` on its slot, or on any slot above it, unmounts the table.
The same holds for the table that the process was allocated with, which is in table slot 0.
One table may be mounted in several processes,
and in several table slots of one process.
In the second case, two slot numbers name each of its slots.

**`OP_PROCESS_UNMOUNT` (40).**
Clears a table slot.

- `a1`: the table slot index.
  It must be less than `PROCESS_TABLES`.

Errors:

- `KERR_INVALID_ARG`: `a1` is not less than `PROCESS_TABLES`.

The table stays as long as its pool does.
From the next call on, the slot numbers that pointed into the table slot name nothing,
and fail with `KERR_INVALID_CAP`.
Clearing an empty table slot succeeds.
With `OP_PROCESS_MOUNT` and `OP_CAP_MOVE`, a process moves to another table (section 5.2).

### 6.8 Operations on `Thread`

The rights that these operations need:

- All of them except `OP_THREAD_OVERRUNS` need `RIGHT_W` on the thread.
- `OP_THREAD_OVERRUNS` needs `RIGHT_R`.
  `RIGHT_R` allows nothing else.
- `RIGHT_X` on a thread allows none of these operations.
  It allows lending the thread time, with `OP_NOTIFY_LEND`.

`OP_THREAD_CONFIGURE`, `OP_THREAD_RESUME`, `OP_THREAD_READ_REG` and `OP_THREAD_WRITE_REG` also need a stopped thread.
Otherwise they fail with `KERR_STATE`.
`OP_THREAD_STOP` stops any thread.

**`OP_THREAD_CONFIGURE` (12).**
Sets where a stopped thread will start.

- `a1`: the program counter.
- `a2`: the stack pointer.
- `a3`: the thread's argument.
  The thread finds it in `a0` (`r0` on ARM), where a function finds its first argument.

Every other register starts at zero, whatever the thread's last run left.
The kernel checks none of the values.
On ARM, the kernel takes the program counter as a branch does:
bit 0 is set for Thumb, as in a function pointer.
A program counter without bit 0 starts the thread in a fault.

**`OP_THREAD_RESUME` (13).**
Makes the thread ready.
It runs once it is bound to units, or to spare time.

- A thread that faulted runs the faulting instruction again,
  unless `OP_THREAD_WRITE_REG` moved its program counter.
- A thread stopped while it waited makes its wait again.

Errors:

- `KERR_STATE`: the thread is not stopped,
  or the thread's process was taken (section 5.6).

**`OP_THREAD_STOP` (16).**
Stops the thread, whatever it is doing.

- `a1`, `a2` and `a3`: 0.

Returns:

- `a1`: what the thread was doing:
  `THREAD_WAS_STOPPED` (0), `THREAD_WAS_READY` (1) or `THREAD_WAS_WAITING` (2).

Errors:

- `KERR_INVALID_ARG`: `a1`, `a2` or `a3` is not 0.

What the stop does depends on what the thread was doing:

- **A stopped thread** is left as it is.
  If it had a fault to tell, it keeps it.
- **A ready thread** stops where it stands.
  If another core runs the thread, the thread stops once that core has trapped,
  and the call waits for that.
  If the thread was making a call there, that call is not made,
  and the thread's pc stays at its `ecall` or `svc`.
- **A waiting thread** leaves its notification,
  and it leaves the borrower that it lent its time to.
  It stands at its call:
  `OP_THREAD_READ_REG` reads the pc of its `ecall` or `svc`,
  the operation in `a7` (`r12` on ARM), and the slot in `a0`.
  A resume makes the call again, and the call checks everything again.
  Instead, a watcher may answer the wait:
  it writes `a0`, `a1`, and a pc past the call.

On ARM, the core may fail to stack the thread's registers when it traps for the stop.
The thread has then lost those registers (section 5.6).
The stop leaves the thread stopped where it faulted,
and `OP_THREAD_FAULT` tells the stacking fault.

The caller may stop itself, through a capability to its own thread.
Once resumed, it goes on past the call.

The thread keeps its units, its virtual time, its process, its watch and its lenders.
The stop signals nothing, not even the watch (section 5.6).

**`OP_THREAD_WATCH` (31).**
Sets the thread's watch: the notification and bits that the thread's faults signal.

- `a1`: the slot of a `Notification` capability.
  The capability needs `RIGHT_W`.
  The notification may lie in any pool.
- `a2`: the bits that the thread's faults signal there.
  Only the bits that the capability may signal are kept.

Errors:

- `KERR_NO_RIGHTS`: `a2` is not zero, but none of its bits is kept.
- `KERR_INVALID_ARG`: the kept bits are not an aligned block,
  as the bits of a capability must be (section 5.7).

The new watch replaces the watch that the thread had.
`a2` = 0 clears the watch, and then `a1` is not read.
See section 5.6.

**`OP_THREAD_FAULT` (33).**
Tells what stopped a thread that faulted.

Returns:

- `a1`: the cause.
- `a2`: the program counter.
- `a3`: the address.
- `a4`: the status.

| | RISC-V | ARM |
|---|---|---|
| `a1`, the cause | `mcause` | the exception number |
| `a2`, the program counter | `mepc` | the pc, with bit 0 set for Thumb |
| `a3`, the address | `mtval`: on an access fault, the address that the thread tried to reach; always zero on RP2350 | `MMFAR` or `BFAR` when `CFSR` says it is valid; `SFAR` for a SecureFault; otherwise zero |
| `a4`, the status | zero | `CFSR` |

The program counter is where a resume goes on, as `OP_THREAD_READ_REG` reads it.

A `bkpt` instruction arrives in a different way on different cores:

- Under QEMU, on the Cortex-M3 and on the Cortex-M33,
  it arrives as a HardFault, exception 3, with a status of zero.
- On RP2350's Cortex-M33, it arrives as a DebugMonitor exception, 12.

A status with `MSTKERR` or `STKERR` says that the core could not stack the thread's registers.
Then `r0` to `r3`, `r12`, `lr`, the pc and `xpsr` hold what the thread's last trap left.

Errors:

- `KERR_STATE`: the thread is not stopped where it faulted.
  A resume or a configure since the fault, or no fault at all, leaves nothing to tell.

**`OP_THREAD_OVERRUNS` (47).**
Tells how many requests the thread ran out.

- `a1`, `a2` and `a3`: 0.

Returns:

- `a1`: the number of the thread's requests that it ran to their end without waiting (section 5.11).
  The number covers every core whose time the thread holds.
  It is modulo 2^32, so a reader uses the difference between two reads.

Errors:

- `KERR_INVALID_ARG`: `a1`, `a2` or `a3` is not 0.

A thread whose units are enough for its work runs no request out,
whatever the other threads do.
A request runs out when the kernel charges the turn past the request's end.
The kernel charges the turn at the timer, if another thread wants the core.
Otherwise it charges the turn at the thread's next wait, or at its next change of turn.
A request that runs on lent time counts as the borrower's.

**`OP_THREAD_READ_REG` (34).**
Reads one register of a stopped thread.

- `a1`: the register.
  It is one of these:
  - on RISC-V, `x0` to `x31`; `x0` reads zero,
  - on ARM, `r0` to `r14`,
  - `THREAD_REG_PC` (32), for the program counter.
    On ARM, the program counter carries bit 0 for Thumb, as `OP_THREAD_CONFIGURE` takes it.

Returns:

- `a1`: the value of the register.

Errors:

- `KERR_INVALID_ARG`: the number names no register.

**`OP_THREAD_WRITE_REG` (35).**
Writes one register of a stopped thread.

- `a1`: the register, as for `OP_THREAD_READ_REG`.
  A write to `x0` changes nothing.
- `a2`: the value.

The kernel takes a written pc as `OP_THREAD_CONFIGURE` takes it.
On ARM, a written pc also leaves an IT block, and keeps the flags.
A thread stopped where it faulted stays so,
and `OP_THREAD_FAULT` tells of the fault until the resume.
While tracing is on, the thread can no longer run after this call,
as after `OP_THREAD_CONFIGURE` (section 9).

Errors:

- `KERR_INVALID_ARG`: the number names no register.

### 6.9 Operations on `Notification`

**`OP_NOTIFY_SIGNAL` (14).**
Sets bits in the notification.
Needs `RIGHT_W`.

- `a1`: the bits to set.
  It must not be zero.
  Only the bits that the capability may signal are set.

Errors:

- `KERR_INVALID_ARG`: `a1` is zero.
- `KERR_NO_RIGHTS`: `a1` has none of the bits that the capability may signal.

The call never waits.
If a thread waits on the notification, it takes the bits and wakes.
Which thread wakes:

- The thread whose time pays for the caller's turn, if that thread waits there.
  It then has the rest of the turn,
  and the caller goes to the back of its queue (section 5.12).
- Otherwise, today, the thread that has waited longest.

**`OP_NOTIFY_WAIT` (15).**
Waits until some bit is set.
Needs `RIGHT_R`.

- `a1`: the span.
  The wake starts a request, and the span says how soon that request is due.
  It is a number of counts of the counter, past the thread's virtual time,
  on every core whose time the thread holds.
  It is a power of two, up to 2^`SPAN_MAX`, which is 2^29.
  Zero keeps the span that the thread had, from its binding or from its last wait.
  A span given here holds until another one is given (section 5.11).

Returns:

- `a1`: the bits.
  The call clears them.

Errors:

- `KERR_INVALID_ARG`: the span is not zero and not a power of two up to 2^29.
- `KERR_INVALID_CAP`, with no bits: the notification's pool was destroyed while the thread waited.

A stop leaves the waiting thread at this call,
and a resume makes the call again (see `OP_THREAD_STOP`).

**`OP_NOTIFY_LEND` (38).**
Waits as `OP_NOTIFY_WAIT` does, and lends the caller's time to another thread while it waits.
Needs `RIGHT_R`.

- `a1`: the slot of a `Thread` capability with `RIGHT_X`: the borrower.
- `a2`: the span, as the `a1` of `OP_NOTIFY_WAIT`.
  The borrower's requests on the caller's time are also due within this span.

If some bit is set already, the call returns at once and lends nothing.
While the caller waits:

- A borrower without time of its own on a core runs there
  on the units of the thread that has lent it time longest,
  if that thread also holds the time of that core.
- Suppose the caller is that thread, and its own time pays for its current turn.
  If the borrower waits for a turn, the caller hands it the rest of the turn (section 5.12).

Errors:

- `KERR_INVALID_ARG`: the borrower is the caller itself, or the span is not valid, as for `OP_NOTIFY_WAIT`.
- `KERR_INVALID_CAP`, with no bits: the notification's pool, or the borrower's pool, was destroyed while the caller waited.

A stop leaves the waiting thread at this call, and it then lends nothing.
A resume makes the call again (see `OP_THREAD_STOP`).

**`OP_NOTIFY_CARVE` (36).**
Makes a capability to the same notification that may signal fewer bits.
Needs no right.

- `a1`: the bits.
- `a2`: the destination slot.

The new capability has the same rights.
It may signal those bits of `a1` that the invoked capability may signal.
It goes below the invoked capability in the derivation tree.
Like `OP_FRAME_CARVE`, this is a table operation (section 5.7).

Errors:

- `KERR_INVALID_ARG`: no bit is left, or the bits left are not an aligned block.
  An aligned block is 1, 2, 4, 8, 16 or 32 bits, starting at a multiple of their number.

### 6.10 Operations on `IrqLine`

**`OP_IRQ_CARVE` (19).**
Makes a smaller range of lines.
Needs no right.

- `a1`: the offset from the first line.
- `a2`: the number of lines.
- `a3`: the destination slot.

This is a table operation, like `OP_FRAME_CARVE`.

**`OP_IRQ_BIND` (20).**
Binds the one line that the capability names to a notification, as an `Irq` object.
Needs `RIGHT_W`.
The capability must name exactly one line.

- `a1`: the slot of the `KernelPool` capability to allocate the `Irq` from, with `RIGHT_W`.
- `a2`: the slot of the `Notification` capability that the `Irq` signals, with `RIGHT_W`.
  The notification may lie in any pool.
  The `Irq`'s hold on it is below that capability (section 5.9).
  The `Irq` signals only the bits that this capability may signal.
- `a3`: the destination slot for the `Irq` capability.
  It may be the invoked slot.

The invoked slot is cleared, together with everything below it.
The `Irq` capability goes below the `KernelPool` capability.
The new `Irq` is masked until `OP_IRQ_SET` arms it.
A timer line binds in the same way as a device's line.

Errors:

- `KERR_INVALID_ARG`: the capability does not name exactly one line.
- `KERR_OVERLAP`: an `Irq` is already bound to the line,
  or the line is the one that the kernel keeps for its cores (section 5.9).
- `KERR_NO_MEMORY`: the pool has no room for the `Irq`.
- `KERR_STATE`: the pool is being destroyed.

All these errors are checked before anything is revoked.
The revoke may make the call again (section 6.1).

### 6.11 Operations on `Irq`

**`OP_IRQ_SET` (21).**
Arms or masks the `Irq`'s line.
Needs `RIGHT_W`.

- `a1`: the bits that the next interrupt signals.
  Only the bits that the `Irq` may signal are kept.
  A non-zero `a1` unmasks the line.
  `a1` = 0 masks the line.
  It does not take back a signal that already happened.
  With `a1` = 0, `a2` and `a3` are not read.
- `a2`: on a timer line, the delay in microseconds.
  The kernel rounds it up to whole counts of the counter.
  The line fires at the first count that lies that many counts past the call (section 5.8).
  On other lines, `a2` is unused.
- `a3`: on a timer line, 0 or `IRQ_SET_PERIOD`.
  On other lines, `a3` is unused.
  With `IRQ_SET_PERIOD`, `a2` is a period, counted from the line's last deadline (section 5.8).

Returns, with `IRQ_SET_PERIOD`:

- `a1`: the number of periods skipped.

Errors:

- `KERR_NO_RIGHTS`: `a1` is not zero, but none of its bits is kept.
- `KERR_INVALID_ARG`: `a3` has any other bit, or the period is zero.
- `KERR_STATE`: the `Irq`'s notification was taken (section 5.9), so the `Irq` has nothing to signal.

On the log's line, arming the line while there are bytes not yet taken signals at once.
A delay longer than 32 bits of microseconds, about 71 minutes, needs several calls.

### 6.12 Operations on `Clock`

Reading the time needs `RIGHT_R`.
Feeding the watchdog needs `RIGHT_W`.
Without the right, a call fails with `KERR_NO_RIGHTS`.

**`OP_CLOCK_READ` (25).**
Reads the time.

Returns:

- `a1`: the low word of the time.
- `a2`: the high word of the time.
- `a3`: the counter's rate in hertz.
  The rate does not change while the machine runs.

The time is the number of the counter's counts since boot.
It is 64 bits on every board, whatever the width of the board's counter.
Under tracing, it is the record's clock, which only `OP_TEST_TICK` moves (section 9).

**`OP_CLOCK_WATCHDOG` (37).**
Arms the machine's watchdog, or moves its deadline nearer or further.

- `a1`: a number of microseconds.
  It must be more than zero, and at most `WATCHDOG_US_MAX`, ten seconds.

Errors:

- `KERR_INVALID_ARG`: `a1` is zero, or more than `WATCHDOG_US_MAX`.

The machine halts with code 7, unless the call comes again within `a1` microseconds.
The deadline is the first count that lies at least that many microseconds past the call,
as for a timer line (section 5.8).
Nothing disarms the watchdog.
There is one watchdog,
and every capability to the clock with `RIGHT_W` feeds the same one.
Until the first call arms it, nothing halts the machine for lack of feeding.
The exception is RP2350's own watchdog (section 3).

### 6.13 Operations on `Time`

**`OP_TIME_CARVE` (28).**
Makes a smaller range of units.
Needs no right.

- `a1`: the offset from the first unit.
- `a2`: the number of units.
- `a3`: the destination slot.

This is a table operation, like `OP_IRQ_CARVE`.

**`OP_TIME_BIND` (29).**
Binds one of a thread's places to units of the capability.
Needs `RIGHT_W`.

- `a1`: the slot of the `Thread` capability, with `RIGHT_W`.
- `a2`: the first unit, as an offset from the capability's first unit.
  It must not lie past the capability's last unit, even when `a3` is zero.
- `a3`: how many units; zero for none.
  They must not go past the capability's last unit.
- `a4`: the span: how soon each of the thread's requests on the core is due,
  in counts of the counter past the thread's virtual time.
  The span holds until a wait gives another one.
  It is one of these (section 5.11):
  - zero, for the default span: the longest power of two of counts that fits in a millisecond,
  - a power of two, up to 2^`SPAN_MAX`, which is 2^29.

Errors:

- `KERR_INVALID_ARG`: `a2` lies past the last unit, `a3` goes past the last unit, or `a4` is not a valid span.
- `KERR_INVALID_ARG`: the units are not all of one core.
- `KERR_LIMIT`: every place of the thread holds the time of another core.
- `KERR_OVERLAP`: another thread earns one of the units.

**Which place the bind fills.**
The bind is of one core: the core of the units.
With no units, it is the core of the first unit.
The bind fills the thread's place that holds that core's time,
or else an empty place (section 5.11).
The thread's places on other cores stay as they are.

**What the place earns.**
The place leaves the units that it earned, whoever bound it to them,
and it earns the new units.
With `RIGHT_X` on the invoked capability, the thread also runs on that core's spare time.
A bind with no units and without `RIGHT_X` still makes the place hold the core's time.
The thread then runs there on lent time only (section 5.12).

**Virtual time.**
The units that the place leaves keep its virtual time.
The place takes the later of its own virtual time and the virtual time of the new units.
So a bind never cancels the time that a thread ran ahead of its units.
The thread's request starts again from the beginning.
A ready thread joins the back of the queue that its virtual time puts it on (section 5.11).

**Taking the units back.**
The thread's hold on the units is below the invoked capability.
So revoking below that capability empties the place.
A thread whose places are all empty keeps its state,
and does not run until it is bound again.

A thread may be running on a core when its place there is emptied or bound again.
The caller itself is one example.
The thread goes on running while it has time or spare time.
Otherwise it gives up the processor when the trap ends.

### 6.14 Operations on `RouteLine` and `Route`

A `Route` connects an event of one device to a task of another device.
The router then triggers the task each time the event comes,
with no thread in between (section 5.13).

**`OP_ROUTE_CARVE` (43).**
Makes a smaller range of channels.
Needs no right.

- `a1`: the offset from the first channel.
- `a2`: the number of channels.
- `a3`: the destination slot.

This is a table operation, like `OP_IRQ_CARVE`.

**`OP_ROUTE_BIND` (44).**
Binds the one channel that the capability names, as a `Route` object.
Needs `RIGHT_W`.
The capability must name exactly one channel.

- `a1`: the slot of the `KernelPool` capability to allocate the `Route` from, with `RIGHT_W`.
- `a2`: the destination slot for the `Route` capability.
  It may be the invoked slot.
- `a3`: 0.

The invoked slot is cleared, together with everything below it.
The `Route` capability goes below the `KernelPool` capability.
The new `Route` connects nothing, and it is disarmed.
Its channel is free again once the `Route`'s pool is destroyed.

Errors:

- `KERR_INVALID_ARG`: the capability does not name exactly one channel.
- `KERR_OVERLAP`: a `Route` is already bound to the channel.
- `KERR_NO_MEMORY`: the pool has no room for the `Route`.
- `KERR_STATE`: the pool is being destroyed.

All these errors are checked before anything is revoked.
The revoke may make the call again (section 6.1).

**`OP_ROUTE_END` (45).**
Connects one end of the `Route`.
Needs `RIGHT_W`.

- `a1`: which end: `ROUTE_EVENT` or `ROUTE_TASK`.
- `a2`: the slot of a `Frame` capability over the device that the end lies in.
  It needs `RIGHT_R` for the event, and `RIGHT_W` for the task.
- `a3`: the end within that device, as the board names it (section 3).

Errors:

- `KERR_INVALID_ARG`: `a1` is neither `ROUTE_EVENT` nor `ROUTE_TASK`.
- `KERR_INVALID_ARG`: the frame does not hold the end.
  On a board without a router, every end fails this way.

The `Route`'s hold on the frame is below the named capability.
So a revoke above the hold takes the end away, and disarms the `Route`.
Connecting an end again replaces it, and also disarms the `Route`.
`OP_ROUTE_SET` arms it again.

**`OP_ROUTE_SET` (46).**
Arms or disarms the `Route`.
Needs `RIGHT_W`.

- `a1`: 1 arms the `Route`; 0 disarms it.
  Arming needs both ends to be connected.

Errors:

- `KERR_STATE`: `a1` is 1, and the `Route` does not have both ends.
- `KERR_INVALID_ARG`: `a1` is neither 0 nor 1.

While the `Route` is armed, the router triggers the task each time the event comes.
An event that came while the `Route` was disarmed triggers nothing.

### 6.15 Operations on `DmaLine` and `Dma`

A `Dma` moves bytes between a range of a frame of memory and a device.
It uses a channel of the board's DMA controller,
and the kernel starts the channel only on a range that it checked (section 5.14).

**`OP_DMA_CARVE` (48).**
Makes a smaller range of channels.
Needs no right.

- `a1`: the offset from the first channel.
- `a2`: the number of channels.
- `a3`: the destination slot.

This is a table operation, like `OP_ROUTE_CARVE`.

**`OP_DMA_BIND` (49).**
Binds the one channel that the capability names, as a `Dma` object.
Needs `RIGHT_W`.
The capability must name exactly one channel.

- `a1`: the slot of the `KernelPool` capability to allocate the `Dma` from, with `RIGHT_W`.
- `a2`: the destination slot for the `Dma` capability.
  It may be the invoked slot.
- `a3`: 0.

The invoked slot is cleared, together with everything below it.
The `Dma` capability goes below the `KernelPool` capability.
The new `Dma` connects nothing, and moves nothing.
Its channel is free again once the `Dma`'s pool is destroyed.

Errors:

- `KERR_INVALID_ARG`: the capability does not name exactly one channel.
- `KERR_OVERLAP`: a `Dma` is already bound to the channel.
- `KERR_NO_MEMORY`: the pool has no room for the `Dma`.
- `KERR_STATE`: the pool is being destroyed.

All these errors are checked before anything is revoked.
The revoke may make the call again (section 6.1).

**`OP_DMA_END` (50).**
Connects one end of the `Dma`.
Needs `RIGHT_W`.

- `a1`: which end: `DMA_SOURCE` or `DMA_SINK`.
- `a2`: the slot of a `Frame` capability.
  It needs `RIGHT_R` for the source, and `RIGHT_W` for the sink.
- `a3`: one of these:
  - 0, for the end that the board puts in memory,
  - for the other end, the device's DMA request number, as the board names it (section 3).
    The frame must then hold the device's registers whole.

Errors:

- `KERR_INVALID_ARG`: `a1` is neither `DMA_SOURCE` nor `DMA_SINK`.
- `KERR_INVALID_ARG`: the frame does not hold the end.
  On a board that lends no channel, every end fails this way.

The `Dma`'s hold on the frame is below the named capability.
So a revoke above the hold takes the end away, and stops the channel first.
Connecting an end again replaces it, and also stops the channel.

**`OP_DMA_START` (51).**
Starts a transfer.
Needs `RIGHT_W`.

- `a1`: the offset into the frame of the memory end.
- `a2`: the length.
- `a3`: 0.

The rules for the range:

- The offset and the length are whole numbers of the board's grains.
- The length is at least one grain, and at most the board's longest transfer (section 3).
- The range lies within the frame.

Errors:

- `KERR_INVALID_ARG`: the range breaks one of these rules.
- `KERR_STATE`: an end is not connected,
  or a transfer may still reach the memory, as `OP_CAP_INFO` tells.

The channel moves the bytes to or from the device,
at the pace at which the device takes or gives them.
It reaches nothing outside the range, whatever the program writes in the meantime.

**`OP_DMA_STOP` (52).**
Stops the channel.
Needs `RIGHT_W`.
Once the call returns, no transfer reaches the memory.

- `a1` to `a3`: 0.

### 6.16 Describing a capability

**`OP_CAP_INFO` (41).**
Describes any capability.
Needs no right.

- `a1`: the page.
  It is 0, the only page so far.
- `a2` and `a3`: 0.

Returns:

- `a1`: the type, one of `CAP_*`, in the low byte, and the rights in the next byte.
  `CAP_INFO_TYPE` and `CAP_INFO_RIGHTS` extract them.
- `a2` to `a4`: what the capability names, by its type, as the table shows.

Errors:

- `KERR_INVALID_ARG`: any other value of the arguments.
  The call then writes nothing.

| Type | `a2` | `a3` | `a4` |
|---|---|---|---|
| `Frame` | base | size | the size of the smallest region in bytes, a power of two of at least 8 |
| `Untyped` | base | size | 0 while nothing lies below it; otherwise what lies first there: `CAP_FRAME`, `CAP_UNTYPED` or `CAP_POOL` (section 5.4) |
| `KernelPool` | size | the room: the bytes that an allocation may still take; 0 while the pool is being destroyed | 1 while the pool is being destroyed, else 0 |
| `CapTable` | its number of slots | 0 | 0 |
| `Process` | its number of region slots; every one of them can be filled at once | the most region slots a process may have on this machine, a PMP entry each; `OP_POOL_ALLOC` holds `a4` to this | 0 |
| `Thread` | its number of places (section 5.11) | the cores whose time its places hold: bit `c` for core `c` | 0 |
| `Notification` | the bits it may signal; never the bits that are set | 0 | 0 |
| `IrqLine` | the first line | the number of lines | 1 for timer lines; 0 for the log's line and the controller's lines |
| `Irq` | its line | the bits it may signal; 0 once its notification was taken; never the bits it is armed with | 1 on a timer line, else 0 |
| `Time` | the first unit | the number of units | 0 |
| `RouteLine` | the first channel | the number of channels | 0 |
| `Route` | its channel | the ends connected: bit `ROUTE_EVENT` and bit `ROUTE_TASK` | 1 while armed, else 0 |
| `DmaLine` | the first channel | the number of channels | 0 |
| `Dma` | its channel | the ends connected: bit `DMA_SOURCE` and bit `DMA_SINK` | 1 while a transfer may still reach the memory, else 0 |
| `Clock` | the counter's rate in hertz, as `OP_CLOCK_READ` gives it | the default span, in counts of the counter (section 5.11) | 0 |
| `Print`, `Halt`, `Test` | 0 | 0 | 0 |

**Describing grants nothing.**
The call changes nothing.
None of its results is something that a call takes as a reference.
It returns only these:

- the capability's own words,
- the constants of the machine that the operations of its type depend on,
- the accounting that its holder needs: a pool's room, and what an Untyped made.

No result is the address of an object behind a capability, or a link of the derivation tree.
But an Untyped's base may be where a pool made of it keeps its descriptor.
No result is the bits that a notification holds, or the time.
Only `RIGHT_R` reads those.

**Notes on the results.**
Unit `u` of a `Time` capability belongs to core `u / TIME_UNITS`.
So `BOOT_CAP_TIME` has `TIME_UNITS` units for each core that the kernel runs (section 5.11).

The default span has two uses (section 5.11):

- It says how soon a request is due when no wait or bind gave a span for it.
- It says how long a turn on spare time lasts while another thread waits for a turn on spare time.

For an Untyped, `a4` shows what lies below it as the call found it.
After a delete has handed things up (section 5.4), what lies below may be a mixture,
and `a4` shows one kind of it.
A pool's room is also as the call found it.
A call on another core may change either of them at once.

A pool that nothing names any more still shows as `CAP_POOL` below its Untyped,
until a revoke below the Untyped destroys it.
The call does not tell whether something outside the pool still holds the pool's objects.

### 6.17 Operation codes in numeric order

| Code | Operation | Type |
|---|---|---|
| 1 | `OP_PRINT` | `Print` |
| 2 | `OP_HALT` | `Halt` |
| 3 | `OP_CAP_COPY` | `CapTable` |
| 4 | `OP_CAP_DELETE` | `CapTable` |
| 5 | `OP_FRAME_CARVE` | `Frame` |
| 6 | `OP_UNTYPED_RETYPE` | `Untyped` |
| 7 | `OP_POOL_ALLOC` | `KernelPool` |
| 8 | `OP_PROCESS_INSTALL` | `Process` |
| 9 | `OP_PROCESS_UNINSTALL` | `Process` |
| 10 | `OP_TEST_TRACE` | `Test` |
| 11 | none; it was `OP_FRAME_INFO` | |
| 12 | `OP_THREAD_CONFIGURE` | `Thread` |
| 13 | `OP_THREAD_RESUME` | `Thread` |
| 14 | `OP_NOTIFY_SIGNAL` | `Notification` |
| 15 | `OP_NOTIFY_WAIT` | `Notification` |
| 16 | `OP_THREAD_STOP` | `Thread` |
| 17 | `OP_TEST_TICK` | `Test` |
| 18 | `OP_CAP_MOVE` | `CapTable` |
| 19 | `OP_IRQ_CARVE` | `IrqLine` |
| 20 | `OP_IRQ_BIND` | `IrqLine` |
| 21 | `OP_IRQ_SET` | `Irq` |
| 22 | `OP_TEST_IRQ` | `Test` |
| 23 | `OP_CAP_REVOKE` | `CapTable` |
| 24 | `OP_CAP_DERIVE` | `CapTable` |
| 25 | `OP_CLOCK_READ` | `Clock` |
| 26 | none; it was `OP_CLOCK_FRAME` | |
| 27 | none; it was `OP_UNTYPED_INFO` | |
| 28 | `OP_TIME_CARVE` | `Time` |
| 29 | `OP_TIME_BIND` | `Time` |
| 30 | `OP_UNTYPED_SPLIT` | `Untyped` |
| 31 | `OP_THREAD_WATCH` | `Thread` |
| 32 | `OP_TEST_PREEMPT` | `Test` |
| 33 | `OP_THREAD_FAULT` | `Thread` |
| 34 | `OP_THREAD_READ_REG` | `Thread` |
| 35 | `OP_THREAD_WRITE_REG` | `Thread` |
| 36 | `OP_NOTIFY_CARVE` | `Notification` |
| 37 | `OP_CLOCK_WATCHDOG` | `Clock` |
| 38 | `OP_NOTIFY_LEND` | `Notification` |
| 39 | `OP_PROCESS_MOUNT` | `Process` |
| 40 | `OP_PROCESS_UNMOUNT` | `Process` |
| 41 | `OP_CAP_INFO` | any |
| 42 | `OP_POOL_INFO` | `KernelPool` |
| 43 | `OP_ROUTE_CARVE` | `RouteLine` |
| 44 | `OP_ROUTE_BIND` | `RouteLine` |
| 45 | `OP_ROUTE_END` | `Route` |
| 46 | `OP_ROUTE_SET` | `Route` |
| 47 | `OP_THREAD_OVERRUNS` | `Thread` |
| 48 | `OP_DMA_CARVE` | `DmaLine` |
| 49 | `OP_DMA_BIND` | `DmaLine` |
| 50 | `OP_DMA_END` | `Dma` |
| 51 | `OP_DMA_START` | `Dma` |
| 52 | `OP_DMA_STOP` | `Dma` |

`OP_COUNT` is 53, one above the highest code.

## 7. What the root task starts with

The image in RAM contains the kernel, followed by the root task.
The kernel builds the root process by hand in the **boot pool**:
4 KiB of the root task's own memory.
Then it drops into user mode with this:

- The program counter is at the start of the code region, `0x80100000` on QEMU.
- The stack pointer is at the top of the data region, `0x80120000` on QEMU.
- Every other register is zero, `a0` included,
  as `OP_THREAD_CONFIGURE` leaves a thread (section 6.8).
- The process has `ROOT_REGION_SLOTS` region slots: eight,
  or the most that a process may have, where that is fewer.
- Region slot 0 holds the code region, with read and execute.
- Region slot 1 holds the data region, with read and write.
- Table slot 0 holds a capability table of `ROOT_TABLE_SLOTS` slots, filled as follows.
  That is 46 slots more than `BOOT_CAP_COUNT` on every board, so 69 on QEMU.
  Table slot 1 is empty.

| Slot | Constant | Capability | Rights |
|---|---|---|---|
| 0 | `BOOT_CAP_NULL` | empty on purpose | |
| 1 | `BOOT_CAP_CAPTABLE` | the root task's own table | all |
| 2 | `BOOT_CAP_PROCESS` | the root task's own process | all |
| 3 | `BOOT_CAP_THREAD` | the root task's first thread; it has a place on every core, and its first place holds the first core's units | all |
| 4 | `BOOT_CAP_POOL` | the boot pool | all |
| 5 | `BOOT_CAP_PRINT` | `Print` | all |
| 6 | `BOOT_CAP_CODE` | `Frame`: the root task's code; in flash on the ESP32-C6 and the ESP32-C5 | read, execute |
| 7 | `BOOT_CAP_DATA` | `Frame`: the root task's data and stack | read, write |
| 8 | `BOOT_CAP_FREE_RAM` | `Untyped`: the block of RAM that the board sets aside for the root task | all |
| 9 | `BOOT_CAP_INPUT` | `Frame`: test input that the loader placed in RAM; in flash on the ESP32-C6 and the ESP32-C5 | read |
| 10 | `BOOT_CAP_IRQ_LINES` | `IrqLine`: line 0 (the log) and every controller line | write |
| 11 | `BOOT_CAP_UART` | `Frame`: the board's console registers: a 16550 on QEMU, the USB Serial/JTAG controller on the ESP32-C6, the USB controller on RP2350 | read, write |
| 12 | `BOOT_CAP_LOG` | `Frame`: the kernel log's header and ring | read, write |
| 13 | `BOOT_CAP_TIMER_LINES` | `IrqLine`: every timer line, `TIMER_LINES` of them | write |
| 14 | `BOOT_CAP_CLOCK` | `Clock`: the machine's time and its watchdog | all |
| 15 | `BOOT_CAP_TIME` | `Time`: every unit of time, `TIME_UNITS` of each core; the root task's thread earns all of the first core's units | write, execute |
| 16 | `BOOT_CAP_ROOT_RAM` | `Untyped`: the root task's own memory: its code, data and input frames, and the boot pool's block, all made already. On the ESP32-C6 and the ESP32-C5, only its data and the boot pool's block, as the code and the input lie in flash | all |
| 17 | `BOOT_CAP_POOL_RAM` | `Untyped`: the boot pool's block, below `BOOT_CAP_ROOT_RAM`, made into the boot pool | all |
| 18 | `BOOT_CAP_HALT` | `Halt` | all |
| 19 | `BOOT_CAP_TEST` | `Test` | all |
| 20 | `BOOT_CAP_ROUTES` | `RouteLine`: every channel of the board's router that the kernel lends (section 5.13); empty where there is no router | write |
| 21 | `BOOT_CAP_DMA` | `DmaLine`: every channel of the board's DMA controller that the kernel lends (section 5.14); empty where there is none | write |
| 22 on | `BOOT_CAP_DEVICES` | `Frame`s: one over each device that the board lists, in the board's order; on QEMU, the clock's counter (section 3) | as the board sets them: read and write; but read only for an eFuse and a counter, and read and execute for a ROM and a window onto flash |
| after them | `BOOT_CAP_RAM_BLOCKS` | `Untyped`s: one over each further block of RAM that the board sets aside for the root task, in the board's order; there are none except on the ESP32-C6 and the ESP32-C5 (section 3) | all |

`BOOT_CAP_COUNT` is 22 plus `BOOT_DEVICES` plus `BOOT_RAM_BLOCKS`.
These two counts are in the board's `devices.h`, which `rvuos.h` includes.
`BOOT_CAP_COUNT` is:

- 23 on QEMU and the MPS2 boards,
- 32 on the ESP32-C5,
- 28 on the nRF52840,
- 28 on RP2350,
- 34 on the ESP32-C6.

A root task puts its own capabilities in the slots from `BOOT_CAP_COUNT` upwards.

**The boot pool.**
The root task's own table, process and thread take about 1.6 KiB of the boot pool.
So roughly 2.4 KiB remain for objects that the root task allocates from `BOOT_CAP_POOL`.
On RP2350 and the ESP32-C6, a little less remains.
Anything larger goes into a pool that the root task makes out of free RAM.
The capabilities to the boot pool, and to its table, process and thread,
are below the boot pool's own node, not below `BOOT_CAP_POOL`.
So revoking below `BOOT_CAP_POOL` takes only what was allocated through it.

**The root task's memory.**
`BOOT_CAP_ROOT_RAM` is an Untyped over the root task's own memory.
Directly below it are the code, data and input frames,
and the boot pool's block, `BOOT_CAP_POOL_RAM`.
So `BOOT_CAP_ROOT_RAM` makes nothing until those are gone.
The rest of it, past the boot pool, stays unused until then.
The boot pool's own node is below `BOOT_CAP_POOL_RAM`, where a retype would have put it.
So revoking below that block destroys the boot pool,
and leaves the root task's code where it is.

**Handing over to a successor.**
The root task lives in this memory, so it cannot revoke below either Untyped.
Nothing else makes the root task special:
a process that holds its capabilities can do everything that the root task could.
So the root task can hand over its role:

1. The root task moves every capability into a successor's table with `OP_CAP_MOVE`,
   each to the same slot, and waits.
2. The successor stops the root task's thread.
3. The successor revokes below `BOOT_CAP_POOL_RAM`.
   This destroys the boot pool, and the root task with it.

`user/init/successor.c` ends the demo this way.
`DESIGN.md`, "The root task is its capabilities", gives the steps.

**Device ranges.**
Device ranges are frames.
They are granted read and write, or read only where the board says so.
They are never granted execute,
except a ROM and a window onto flash, which are read and execute.
The counter behind `BOOT_CAP_CLOCK` is read only.
No Untyped covers device ranges, so they can never become a pool.
The log cannot become a pool either, because it is a frame too.

Which capability lies in which slot is a convention between the loader and the program.
The kernel does not enforce it.
The `BOOT_CAP_*` slots are the kernel's own instance of that convention,
for the one program that it starts itself.
