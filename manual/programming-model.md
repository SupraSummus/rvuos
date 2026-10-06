# rvuos user manual: the programming model

Chapter 5 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 5. The programming model

### 5.1 Processes, threads and regions

A **process** is a protection domain:
a capability table, `PROCESS_REGION_SLOTS` region slots, eight today,
and the threads that run in it.
A **thread** is an execution context inside a process:
a register frame and a state.
Threads of one process share its regions and its capability table.

A **frame** is a physical address range with maximum rights,
and a **region** is a frame installed into one of a process's slots,
which gives that process access to the range with the rights chosen at install time.
Regions are all a process can see of memory.
An **Untyped** is memory that has not become anything yet:
it cannot be installed, and it is made into a frame, a pool or its two halves.
The kernel does not know what a code segment, a stack or a heap is;
whoever builds a process decides its layout.

### 5.2 Capabilities and the table

A process names everything by **slot index** into its capability table.
A slot is empty or holds one capability:
a type, a set of rights, and two words the kernel interprets by type.
Userspace never sees a slot's contents,
but it can ask a frame what it covers with `OP_FRAME_INFO`,
and an Untyped with `OP_UNTYPED_INFO`.

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

| Bit | Value | Frame, Untyped | Other types |
|---|---|---|---|
| `RIGHT_R` | 1 | read | `Notification`: wait |
| `RIGHT_W` | 2 | write | control the object: allocate, install, configure, signal, set, bind, copy or move into a table; `Debug`: write into the log |
| `RIGHT_X` | 4 | execute | `Time`: the threads bound through it run on spare time; `Debug`: halt and drive the machine |

Every operation that produces a capability
puts it into a slot of the caller's own table,
named by an argument, and that slot must be empty.
A process needs no capability to receive into its own table.
The `CapTable` capability exists for writing into a table:
a parent fills a child's table with `OP_CAP_COPY`, `OP_CAP_DERIVE` or `OP_CAP_MOVE` before starting it,
and a process that holds a capability to its own table
can copy and move within it, clear its own slots, and revoke below them.

Every capability remembers what it was derived from,
and that is what `OP_CAP_REVOKE` follows.
The tree grows in these ways:

| Operation | The new capability hangs |
|---|---|
| `OP_CAP_DERIVE` | below the source |
| `OP_CAP_COPY` | beside the source, under the source's parent; a copy of a root is a root; an Untyped is never copied |
| `OP_CAP_MOVE` | where the source hung, which it leaves empty, with what hung below the source below it |
| `OP_FRAME_CARVE`, `OP_IRQ_CARVE`, `OP_TIME_CARVE`, `OP_CLOCK_FRAME` | below the invoked capability |
| `OP_UNTYPED_RETYPE` of a frame, `OP_UNTYPED_SPLIT` | below the invoked Untyped |
| `OP_UNTYPED_RETYPE` of a pool | below the pool's own node, which hangs below the invoked Untyped |
| `OP_PROCESS_INSTALL` | the installed region hangs below the frame |
| `OP_POOL_ALLOC` | below the invoked `KernelPool` capability |
| `OP_POOL_ALLOC` of a `Process` | and the process's hold on its table below the `CapTable` capability |
| `OP_POOL_ALLOC` of a `Thread` | and the thread's hold on its process below the `Process` capability |
| `OP_IRQ_BIND` | below the `KernelPool` capability; the line goes with what was derived from it |
| `OP_IRQ_BIND` | and the `Irq`'s hold on its notification below the `Notification` capability |
| `OP_TIME_BIND` | the thread's hold on its units hangs below the invoked `Time` capability |
| boot | nowhere, but for the root task's frames and the boot pool's block, below `BOOT_CAP_ROOT_RAM`, the boot pool's own node, below that block, and the capabilities to the boot pool and its objects, below that node |

`OP_CAP_REVOKE` on a slot clears everything below it, in every table and every process:
derived capabilities, their copies, what was derived from those,
every region installed from any of them,
and every thread's units bound through any of them;
a thread made through any of them loses its process and stops, section 5.6,
and an `Irq` bound through any of them loses its notification and is disarmed, section 5.9.
Below an Untyped it destroys every pool made of it, section 5.5.
The slot itself stays.
Revoking a copy takes nothing from the original, and the other way round;
to take both back, revoke below what they were both derived from.
So derive to lend, and copy to keep a second handle to what you hold.
A copy cannot take back what was derived from its source, since that hangs below the source and not below the copy;
to hand over a capability together with that power, move it.
An Untyped can only be derived, and only while nothing was made of it:
the derived one is the whole of it, and the source makes nothing until that one is gone.

`OP_CAP_DELETE` clears one slot and hands what hung below it to the slot's parent,
so deleting your own copy of something you lent does not take it back.
Destroying a pool clears every slot in the tables that pool held the same way,
so what a process handed out through its table outlives the process,
below whatever the process derived it from.

Copying and deriving both apply a rights mask, so rights only ever narrow.
A move keeps the rights as they were, since what hangs below may hold all of them.

Slot 0 of the root task's table is left empty on purpose,
so that an uninitialised index fails.
Programs are encouraged to keep the same convention.

### 5.3 Objects

| Type | Constant | Object behind it | Created by |
|---|---|---|---|
| `Untyped` | `CAP_UNTYPED` | none: the slot holds the block | boot, `OP_UNTYPED_SPLIT` |
| `Frame` | `CAP_FRAME` | none: the slot holds base and size | boot, `OP_FRAME_CARVE`, `OP_UNTYPED_RETYPE` |
| `KernelPool` | `CAP_POOL` | the pool's descriptor, at its base | `OP_UNTYPED_RETYPE` |
| `CapTable` | `CAP_CAPTABLE` | a table of `n` slots | `OP_POOL_ALLOC` |
| `Process` | `CAP_PROCESS` | a capability to its table, region slots, a PMP image | `OP_POOL_ALLOC` |
| `Thread` | `CAP_THREAD` | a register frame and a state | `OP_POOL_ALLOC` |
| `Notification` | `CAP_NOTIFICATION` | one word of sticky bits | `OP_POOL_ALLOC` |
| `Mutex` | `CAP_MUTEX` | its holder and its waiters | `OP_POOL_ALLOC` |
| `IrqLine` | `CAP_IRQ_LINE` | none: the slot holds the first line and a count | boot, `OP_IRQ_CARVE` |
| `Irq` | `CAP_IRQ` | one line bound to a notification, with a deadline on a timer line | `OP_IRQ_BIND` |
| `Time` | `CAP_TIME` | none: the slot holds the first unit and a count | boot, `OP_TIME_CARVE` |
| `Debug` | `CAP_DEBUG` | none | boot |
| `Clock` | `CAP_CLOCK` | none: there is one counter | boot |

`Untyped`, `Frame`, `IrqLine`, `Time`, `Debug` and `Clock` capabilities have no kernel object behind them.
Carving a frame, a line range or a range of units, retyping an Untyped into a frame and splitting one,
are pure table operations
that touch no kernel memory,
which is why the root task can hand out memory, lines and time
before it has created a single pool.

### 5.4 Memory: Untyped, frames and installing

Every frame and every Untyped is a **block**:
its size is a power of two and its base a multiple of its size,
which is the range one PMP entry in NAPOT mode describes.
The smallest block is eight bytes, or the PMP's grain if that is coarser;
`OP_FRAME_INFO` returns it in `a4`.
A range of any other size is rounded up or made of several blocks,
one region slot each.

An Untyped is **free** while nothing made of it is left, and **made** while something is;
`OP_UNTYPED_INFO` says which in `a4`.
A free Untyped makes one thing of the whole of its memory:
`OP_UNTYPED_SPLIT` its lower and its upper half, two Untypeds,
`OP_UNTYPED_RETYPE` a frame or a pool,
and `OP_CAP_DERIVE` an Untyped of all of it, for lending, section 6.4.
A made one makes nothing (`KERR_NO_MEMORY`),
so what one Untyped makes never overlaps,
and a frame never overlaps a pool.
Once what it made is gone, whether deleted, destroyed or revoked, it is free again, the whole of it.

Where a block lies is the program's to choose, by the half it takes;
the kernel keeps no allocator.
To get a small block out of a large Untyped, halve it down,
and keep the halves not used yet: they are the program's free memory.
A half the program does not want to keep a slot for may be deleted;
what was made of it then hangs below its parent,
which is free again once all of that is gone.
A frame and an Untyped carry the rights of the Untyped they were made of.
`OP_FRAME_CARVE` hands out a block within a frame.
Programs that lay memory out with fixed offsets
should check the smallest size once and fail early if the layout does not fit.

Rules for installing a frame into a process:

- The rights installed must be a non-empty subset of the frame's rights.
- Write without read is refused,
  because PMP reserves that encoding and hardware may do anything with it.
- On ARM execute without read is refused too, since the MPU fetches only what a thread may read.
- Regions installed in one process may not overlap each other.
- The install takes effect at once, even for the running process.

PMP budget.
Every installed region is one NAPOT entry, so `n` regions cost `n` entries.
An install beyond the core's entries fails with `KERR_LIMIT`,
which with eight region slots happens only on a core with fewer entries for regions,
such as RP2350's Hazard3, with seven.

An access must lie entirely within one PMP entry.
Two regions that touch share a boundary
that no single load, store or instruction fetch may cross,
so a process's creator lays it out so that nothing straddles one.

Sharing costs no copy: installing the same range into two processes
gives both access, and no byte passes through the kernel.
What it does cost is a region slot and a PMP entry in each process.

### 5.5 Pools

A process hands memory to the kernel with `OP_UNTYPED_RETYPE` of a `CAP_POOL`,
which makes the whole of the Untyped a pool.
The Untyped must carry both read and write rights,
and be at least `POOL_MIN_SIZE`, 64 bytes.
The kernel places the pool's descriptor at its base
and returns a `KernelPool` capability with all rights.
The memory belongs to the kernel from then on;
no frame covers it, since what one Untyped makes never overlaps.
The kernel zeroes each object as it allocates it,
so what the process left in the memory never becomes part of an object.

Objects are bump-allocated from a pool, eight-byte aligned,
and never freed individually.
Sizes a developer needs for planning, as the kernel rounds them:

| Object | Bytes (16 PMP entries) |
|---|---|
| pool descriptor | 56 |
| `CapTable` with `n` slots | 12 + 24 × n, rounded up to 8 |
| `Process` | 312 (272 with `PMP_MAX_ENTRIES=8`) |
| `Thread` | 224 |
| `Notification` | 16 |
| `Irq` | 48 |

A minimal child process, table of 10 slots, process, thread and two notifications,
costs 880 bytes including the descriptor.

**Objects in different pools.**
A process's table, a thread's process and an `Irq`'s notification may each lie in any pool,
sections 5.2, 5.6 and 5.9:
each is held by a capability that a destroy of its pool clears.

**Capabilities to a pool.**
Every capability to an object hangs below the `KernelPool` capability it was allocated through,
and every `KernelPool` capability below the pool's own node,
which the kernel keeps in the pool and no process can name.
So revoking below a `KernelPool` capability takes what was allocated through it,
and deleting the last `KernelPool` capability leaves the pool standing,
reachable through nothing, until the Untyped it was made of is revoked.

**Destroying a pool** is revoking below the Untyped it was made of, or below one above that;
no operation on the pool does it, so a `KernelPool` capability only allocates.
A pool is the whole of its Untyped, so keep the Untyped of a pool you mean to give back on its own.
The destroy:

- destroys every object in it,
- clears every capability, in every table of every process,
  that names the pool or one of its objects,
- clears every slot of the pool's own tables as `OP_CAP_DELETE` does,
  so what was derived from those slots goes to their parents,
- wakes every thread waiting on a notification in the pool
  with `KERR_INVALID_CAP` and no bits,
- stops every thread, in whatever pool, that ran in a process in the pool,
- disarms every `Irq`, in whatever pool, bound to a notification in the pool,
  and masks its line, which stays bound,
- masks the interrupt line of every `Irq` in the pool, which frees the line,
- zeroes the memory its objects took and leaves the rest as it was,
  so clear memory before it becomes a pool if whoever destroys the pool should not read it,
- gives the memory back to the Untyped the pool was made of.

The revoke fails with `KERR_STATE`
when the calling thread, its process or the process's table lies in the Untyped's memory,
so a thread cannot destroy what it runs on
nor the table it names capabilities in,
and when the table it names the Untyped in does,
since the capability the call is made through would go half way through the destroy of that table's pool.
The boot pool holds the root task, which therefore cannot destroy it,
but any other thread holding `BOOT_CAP_POOL_RAM`, the block the boot pool was made of, can,
and the root task goes with it, section 7.
A destroy is restartable: it may stop for an interrupt and go on when the call is made again,
and a pool being destroyed allocates nothing, `KERR_STATE`.

A lender that wants memory back from a living borrower
lends it as an Untyped derived from its own
and revokes below its own, section 5.2.
That takes the borrower's Untyped, the halves, the frames it made and their mappings,
and destroys every pool the borrower made of it,
wherever the borrower passed it on and whatever it deleted in between,
and leaves the lender's Untyped free, the whole of it.

### 5.6 Threads

A thread is **stopped**, **ready** or **waiting**.

- A new thread is stopped.
- `OP_THREAD_CONFIGURE` sets a stopped thread's program counter, its stack pointer,
  and its argument, which it finds in `a0`, `r0` on ARM, as a function finds its first.
  The other registers start at zero.
  The kernel validates none of them.
- `OP_THREAD_RESUME` makes a stopped thread ready.
  It gets the processor once it is bound to units, or to spare time, and its turn comes, section 5.11.
- A thread waiting on a notification is waiting;
  a signal makes it ready again.
- A thread that faults is stopped where it faulted; see **Faults** below.

A thread holds its process by a capability derived from the `Process` capability it was allocated with,
as a process holds its table, section 5.2, so the thread may lie in any pool.
Revoking below that capability, or destroying the pool the process lies in,
takes the process from the thread, and the thread stops for good:
`OP_THREAD_RESUME` refuses it with `KERR_STATE`.

Apart from its state, a thread is bound to units of time or it is not.
A new thread is not, and a thread that is not keeps its state and does not run:
`OP_TIME_BIND` binds it, and revoking below the capability it was bound through unbinds it.
That is how a started thread is stopped and started again.
No thread exits: a thread that has nothing left to do waits forever
on a notification nobody signals, or loses its units or its process,
or stops itself with a breakpoint, which is a fault its watch hears, below.

**Faults.**
A thread that faults, by an access fault, an illegal instruction, a misaligned access or a breakpoint,
is stopped where it faulted, with its registers as they were and its program counter at the instruction,
and the processor goes to the next thread; the machine goes on.
Nothing else changes: the process, the thread's units and the process's other threads are as they were.
The kernel writes `user fault` and the cause into its log, section 4.
`OP_THREAD_WATCH` gives a thread a **watch**, a notification and bits that its faults signal,
so whoever waits there hears of the fault as of any other signal;
one wait can carry a fault's bit next to a device's or a timer's,
and a watcher of many threads gives each a bit.
The thread holds the notification by a capability derived from the one named, as it holds its process,
so revoking below that capability, or destroying the notification's pool, clears the watch.
A thread without a watch stops at a fault all the same, and nobody hears.
What happens next is the watcher's to decide, with the operations there are:
`OP_THREAD_RESUME` runs the faulting instruction again, after installing the region it reached for, say;
`OP_THREAD_CONFIGURE` first starts it afresh;
taking its process ends it, and taking its units, which a stopped thread still earns, frees them.
The bits say which thread faulted, and `OP_THREAD_FAULT` says why while it stays stopped there.
A watcher that emulates the instruction writes the thread's registers with `OP_THREAD_WRITE_REG`,
the program counter past the instruction among them, and resumes it; section 6.8.

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

Besides its rights, a `Notification` capability carries the bits it may signal:
every bit for the one `OP_POOL_ALLOC` returns, and fewer once `OP_NOTIFY_CARVE` has made a capability of some of them,
below the one carved, as a frame is carved from a frame.
A signal sets only the bits of `a1` its capability may signal,
and naming none of them is refused with `KERR_NO_RIGHTS`;
copying keeps the bits, as it keeps or narrows rights.
An `Irq` signals only the bits the capability it was bound with may, and a thread's watch only those of the one it was set with.

So a server with many clients gives each a capability carved to the client's own bit,
and the bits a wait returns say who signalled, whatever a client names.
A client need not be told its bit: naming every bit, `0xffffffff`, signals all it may.

No data passes through the kernel.
Two processes exchange bytes through a region installed in both
and use notifications to say when.
The build targets `rv32imac`, so the `A` extension is there in user mode,
as `ldrex` and `strex` are on ARMv7-M,
and a lock or a ring buffer in shared memory is userspace's to build,
as `lib/lock.h` and `lib/ring.h` do, section 8.4;
what userspace cannot build is "stop me until someone says otherwise",
and a notification is exactly that.
A lock the kernel knows the holder of is a mutex, section 5.12.

A round trip is therefore four system calls:
signal, wait on one side; wait, signal on the other.

A trap breaks the thread's reservation,
so an `sc.w` fails whenever a system call, the tick or an interrupt came between it and its `lr.w`;
a loop around the pair tries again, and never succeeds with a system call inside it.

**A signal orders memory.**
What a thread stored before `OP_NOTIFY_SIGNAL`,
a thread sees once `OP_NOTIFY_WAIT` returns it that signal's bits, on any core:
the signal is a release, and the wait an acquire.
So a buffer written and then signalled needs no fence,
as long as its reader reads it after such a wait and not because memory said it was ready;
memory read with no call between, as a ring's counters are, still needs one on each side.

**A server with many clients** waits on one notification;
each client holds it with `RIGHT_W` only, carved to a bit of its own,
and shares a region with the server.
One wake can carry several clients' work,
and the bits it returns say whose, since a client cannot set another's bit;
it cannot forge another's data or consume another's wake either.
The server need not spend a region on each client:
the clients' regions may all be carved from one frame the server installs once,
as the hub of `lib/chan.h` does, section 8.4.

### 5.8 Time

There is no sleep call and no yield.
Time reaches userspace as one more interrupt, on a timer line.

The machine has `TIMER_LINES` timer lines, 16,
and the root task receives all of them at boot in `BOOT_CAP_TIMER_LINES`,
apart from the controller's lines.
It carves them and hands them out as it does any other line, section 5.9,
so who holds how many timers is its choice.
A timer line is bound with `OP_IRQ_BIND` like a device's,
into an `Irq` in a pool, bound to a notification in any pool.
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
and waits for its turn,
and for its account to reach a tick if it has spent it and has no spare time, section 5.11.

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

A timer line fires once, so a periodic task arms it again each time it wakes.
With `IRQ_SET_PERIOD` in `a3` the delay is a period counted from the line's last deadline,
or from its bind, not from the call, so the task keeps its period whatever each wake costs:
the line fires at the first tick after the call
that lies a whole number of periods from that deadline.
The call returns in `a1` how many such ticks had already passed,
the periods a task that woke late has skipped;
it can do their work at once, count them as overruns, or ignore them.
Setting it again before the line fired leaves the deadline where it was.
The period is rounded up to whole ticks and may not be zero.
Ticks keep pace with the clock's counter, so periods do too.

```c
uint32_t skipped;
for (;;) {
    rv_timer_period(TIMER, BIT_TIMER, 5000, &skipped);   /* every 5 ms */
    rv_wait(NTFN, &bits);
    /* one period's work, or skipped + 1 of them */
}
```

**The clock.**
`BOOT_CAP_CLOCK` names the machine's time, 64 bits of the counter's counts since boot.
With `RIGHT_R`, `OP_CLOCK_READ` returns the time and the counter's rate in hertz:

```c
uint64_t start, end;
uint32_t hz, counter;
rv_clock_read(CLOCK, &start, &hz, &counter);
work();
rv_clock_read(CLOCK, &end, &hz, &counter);   /* (end - start) / hz seconds */
```

A thread that cannot afford a call for each read, one that measures what the kernel does to it,
reads the counter's low 32 bits with a load instead,
through a read-only frame `OP_CLOCK_FRAME` derives, installed like any frame at the address the read returned.
They wrap, after 71 minutes at a megahertz, so it takes differences of them:

```c
rv_invoke(OP_CLOCK_FRAME, CLOCK, COUNTER_FRAME, 0, 0);
rv_invoke(OP_PROCESS_INSTALL, PROCESS, 5, COUNTER_FRAME, RIGHT_R);

uint32_t at = rv_counter_low(counter);
spin();
uint32_t counts = rv_counter_low(counter) - at;
```

The rate is the board's, 10 MHz on QEMU, the CPU clock on the ESP32-C6 and 1 MHz on RP2350,
so a program takes it from `OP_CLOCK_READ`.
The time is the machine's, not the thread's: it includes other threads' slices.
Revoking below a `Clock` capability uninstalls every region derived through it.
`rdtime` traps on every board.
The ESP32-C6's performance counter is no clock: it is the process's, and stops whenever another process runs, section 3.

**The watchdog.**
With `RIGHT_W`, `OP_CLOCK_WATCHDOG` arms the machine's watchdog, or feeds it:
the machine halts with code 7 unless the call comes again within the microseconds it names, ten seconds at most.
Nothing disarms it, and there is one for the machine, which every clock with `RIGHT_W` feeds.
What it watches is the feeder's to decide: a root task that feeds it only once each child has said it is well
halts a machine whose children hang, where one that feeds it from its own loop halts only a machine where the root task hangs.

```c
rv_clock_watchdog(CLOCK, 2000000);            /* arm: two seconds */
rv_timer_period(TIMER, BIT_FEED, 500000, &skipped);
for (;;) {
    rv_wait(NTFN, &bits);
    if ((bits & BIT_FEED) && children_well()) {
        rv_clock_watchdog(CLOCK, 2000000);
    }
    /* ... */
}
```

### 5.9 Interrupts

A driver holds an `IrqLine` capability naming one line.
The root task receives every line of the controller at boot, with the log's,
and every timer line in a second capability,
and carves single lines out with `OP_IRQ_CARVE`,
as it makes frames of memory.

`OP_IRQ_BIND` turns a one-line capability into an `Irq` object
in a pool of the driver's choosing,
bound to a notification in any pool.
The invoked slot is consumed.
A line is bound at most once; a second bind fails with `KERR_OVERLAP`
until the first `Irq`'s pool is destroyed.
Where each core has a controller of its own and the kernel runs on several,
the line they interrupt each other on is bound to the kernel for good, and binding it fails so too:
line 6 on mps2-an521 and line 26 on RP2350's Cortex-M33, each with both cores.

An `Irq` holds its notification by a capability
derived from the `Notification` capability it was bound with,
as a thread holds its process, section 5.6.
Revoking below that capability, or destroying the pool the notification lies in,
takes the notification from the `Irq`, which is disarmed and its line masked for good:
`OP_IRQ_SET` refuses it with `KERR_STATE`, and the line stays bound until the `Irq`'s own pool goes.

An `Irq` on a device's line works like one on a timer line:

- `OP_IRQ_SET` with bits **arms** it, which unmasks the line at the controller.
- When the line fires, the kernel masks the line,
  disarms the `Irq` and signals the bits.
- The driver services the device and arms the `Irq` again,
  which is the acknowledgement: only the driver lifts the mask.
- `OP_IRQ_SET` with zero bits masks the line by hand.

An `Irq` is armed exactly while its line is unmasked,
so a level that stays high costs one trap and not a storm.
The first core takes every device interrupt, whichever core its driver runs on;
where each core has a controller of its own, as on mps2-an521 and on RP2350 of either kind,
an arm or a disarm made on another core reaches the first core's as that core next enters the kernel,
which the call interrupts it for, so the line may come that much later, and a disarmed one never signals.
A device interrupt wakes its driver but does not run it;
the driver waits for its turn like a thread a timer line woke.

Sharing a line between drivers is not supported;
`DESIGN.md`, open decision 11.

### 5.10 The kernel log

The kernel writes every byte it prints, `OP_DEBUG_WRITE`'s included,
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
`user/wifi/logger.c` is a reader the root task gave the frame to, which carries the ring over the network
and moves `taken` only as far as its host says it has the bytes, so a halt writes out what no host had.

### 5.11 Scheduling

- Each core is divided into `TIME_UNITS` units of time, 64, each a sixty-fourth of it.
  The units of every core are numbered one core after the other:
  the first core's are 0 to 63, the second's 64 to 127, and so on.
  The root task receives them all in `BOOT_CAP_TIME`, its own thread earning every one of the first core's,
  and hands them out with `OP_TIME_CARVE` and `OP_CAP_DERIVE` as it hands out memory.
  A program learns how many cores there are by the units it can carve out of `BOOT_CAP_TIME`:
  a carve of 64 units at 64 succeeds only with a second core.
- A thread runs on the core its units are of, and on no other.
  A bind's units must all be of one core; a bind to none puts the thread on spare time on the core of the first unit named.
  A thread bound to another core's units while it runs finishes its turn where it runs and moves at the next tick.
- `OP_TIME_BIND` binds a thread to some of a capability's units, or to none of them,
  and the thread **earns** them.
  A unit is earned by one thread at a time:
  a bind that names a unit another thread earns fails with `KERR_OVERLAP`,
  so copies of a `Time` capability may be held by many, and each unit still goes to one thread.
  The time of the units nobody earns is spare.
- A thread bound through a capability with `RIGHT_X` also runs on **spare time**:
  turns no thread with time wants, which cost its account nothing.
  The boot grant has `RIGHT_X`.
  A thread bound through a capability without it runs on its account alone,
  at most its units' part of its core and a full account,
  and the core sleeps in `wfi` for the rest if nothing else wants it.
  A thread bound to no units runs on spare time alone, or not at all without `RIGHT_X`.
- Each thread has an **account** of time,
  which gains a sixty-fourth of a tick for each unit the thread earns every tick
  and holds at most 100 ticks' worth of them, a tenth of a second.
  A turn the thread began with time costs the account the time it ran, counted on the machine's counter,
  whether the tick ends it or the thread waits;
  a turn on spare time costs nothing.
  A thread has **time** while its account holds at least a tick.
  A new thread's account is empty, and the root task's thread's starts full.
  Bound to other units, a thread keeps what its account held, up to what the new units hold;
  unbound, it loses it.
- Ready threads wait for the processor in three queues, which each core has.
  A thread with time waits on the run queue;
  one without time waits on the spare queue if it may run on spare time,
  and for its account to reach a tick if not.
  A thread that becomes ready, whether resumed, woken, bound or preempted,
  joins the back of its queue.
- A thread waiting on a mutex with `MUTEX_LEND` lends the holder its account, section 5.12:
  a holder without time waits on the run queue while its lender has time, and the turns it takes so cost the lender's account.
- The machine timer ticks at `TIMER_HZ`, 1 kHz on both boards.
  A tick ends the running thread's turn:
  the thread goes to the back of its queue,
  and the thread that has waited longest on the run queue runs,
  or, if the run queue is empty, the one that has waited longest on the spare queue.
  A thread that waits hands the rest of the tick to the next, which pays for no more than it runs of it.
  A tick that would hand the processor back to the thread that had it takes no interrupt:
  the kernel counts and charges it at its next trap all the same,
  so a thread alone on the processor is interrupted only at a timer line's deadline,
  where another thread's account reaches a tick,
  or where its own account drains, if it may not go on on spare time.
- So threads with work and time take equal turns, and then threads on spare time do.
  A thread with work all along gets its units' part of the processor,
  give or take a tenth of a second's worth of the accounts.
  A thread with time waits for its turn at most `TIME_UNITS` ticks, since only a thread with units of its core has time there.
  A thread a process adds earns only units split off the process's own, or runs on spare time:
  it takes nothing another thread earns.
  Spare time goes round by thread, so more threads there get more of it.
- A system call is never interrupted:
  machine mode runs with interrupts off from the trap to the return.
  On several cores one call runs at a time, whichever core makes it, and another core's call waits for it.
- When nothing is runnable and an `Irq` is armed on a timer line or a device's line,
  or a thread waits for its account to reach a tick,
  the kernel stalls in `wfi` until the nearest timer line's deadline, the account that reaches a tick or the interrupt arrives,
  and takes no tick in between.
  On several cores a core with nothing to run waits so while another runs, which wakes it when it gives it a thread.
  When no core runs a thread and there is none of these, it prints `no runnable thread` and halts with code 5,
  watchdog or not.

There are no priorities and no yield.
A spinning thread cannot starve the others, because the tick preempts it,
but it burns its turns and its account, and keeps the machine out of `wfi`
unless it is held to its units;
a thread that waits costs nothing until it is signalled.

A turn is charged by the counter, not by the tick that ends it,
so a thread that waits just before every tick pays for the time it ran all the same.

To hold a process to about a tenth of the processor, the root task gives it six units without `RIGHT_X`.
Its own thread earns every unit at boot, so it leaves the six first,
then carves them and lends them without spare time:

```c
rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, 58);
rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, 58, 6, TIME);
rv_invoke(OP_CAP_DERIVE, CHILD_TABLE, CHILD_TIME, TIME, RIGHT_W);
```

The child binds its threads through `CHILD_TIME` with `OP_TIME_BIND`;
`TIME` itself keeps `RIGHT_X`, so a bind through it would give spare time too.
Revoking below `TIME` takes the child's units back, and its threads stop.

### 5.12 Mutexes

A mutex is held by one thread at a time.
`OP_MUTEX_LOCK` takes it, at once if it is free, and otherwise waits behind the threads already waiting;
`OP_MUTEX_UNLOCK` gives it back to the thread that waited longest, which holds it as its call returns.
Only the holder gives it back, and it may not take it twice: both are refused with `KERR_STATE`.
Each is a system call even when nobody else wants the mutex, where `lib/lock.h` makes none, section 8.4;
what a mutex has is the kernel's knowledge of who holds it.

So a holder that gives it back and wants it again waits behind the others,
and a waiter may lend the holder its time:
with `MUTEX_LEND`, a holder without time of its own, section 5.11, runs on the waiter's account while it waits,
the waiter hands it the rest of its turn at once, and the unlock hands the turn back with the mutex.
A holder with few units then keeps a waiter with many no longer than its work takes.

```c
rv_invoke(OP_POOL_ALLOC, POOL, CAP_MUTEX, MUTEX, 0);
/* in each thread that shares it */
rv_mutex_lock(MUTEX, MUTEX_LEND);
/* ... the work the mutex guards ... */
rv_mutex_unlock(MUTEX);
```

The time goes one step, from the oldest waiter of the first mutex the holder holds, and only within a core.
A holder that faults keeps what it holds, stopped;
one destroyed gives each mutex to its oldest waiter, which is not told.
A mutex keeps nobody from the memory it guards: that is the regions' work.
