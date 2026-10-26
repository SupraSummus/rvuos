# rvuos user manual: writing and debugging a program

This file holds chapters 8 and 9 of the user manual.
[`MANUAL.md`](../MANUAL.md) lists all chapters,
and explains the words these chapters use.

## 8. Writing a program

### 8.1 Toolchain and layout

A user program is a flat binary.
It is linked against `user/user.ld.S`, with the board's addresses (see "Boards").
The Makefile embeds it into the kernel image.
The current linker script sets these constraints:

- Code and read-only data live in the read-execute region.
  On QEMU, this region is at `0x80100000`.
- Data and the stack live in the read-write region.
  On QEMU, this region is at `0x80110000`.
  Initialised data lies at the base of this region,
  and is loaded right behind the read-only data.
- `user/arch/<arch>/start.S` copies the initialised data into place, zeroes `.bss`, and calls `main`.
  If `main` returns, it halts with code 1 through `BOOT_CAP_HALT`.
- A child runs this code without the data region.
  So code that a child runs keeps no global (section 8.4).

The compile flags are:

- on RISC-V: `-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib`,
- on ARMv7-M: `--target=thumbv7m-none-eabi -mcpu=cortex-m3 -mfloat-abi=soft -ffreestanding -nostdlib`,
- on ARMv8-M: the flags of ARMv7-M,
  with `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33+nodsp+nofp` in place of the first two.

There is no libc.
`user/rvuos.h` has a wrapper for every operation.
`user/lib/` has what a program of several processes needs besides these wrappers (section 8.4).

| Wrapper | Operation |
|---|---|
| `rv_invoke(op, cap, a1, a2, a3)` | any |
| `rv_cap_copy(table, dst, src, rights)` | `OP_CAP_COPY` |
| `rv_cap_derive(table, dst, src, rights)` | `OP_CAP_DERIVE` |
| `rv_cap_move(table, dst, src)` | `OP_CAP_MOVE` |
| `rv_cap_delete(table, slot)` | `OP_CAP_DELETE` |
| `rv_cap_revoke(table, slot)` | `OP_CAP_REVOKE` |
| `rv_cap_info(cap, &type, &rights, words)` | `OP_CAP_INFO`, on any capability |
| `rv_frame_info(cap, &base, &size)` | `OP_CAP_INFO` of a frame; `KERR_WRONG_TYPE` for anything else |
| `rv_frame_min_size(cap, &min)` | `OP_CAP_INFO` of a frame, reading `a4` |
| `rv_frame_carve(cap, offset, size, dst)` | `OP_FRAME_CARVE` |
| `rv_untyped_info(cap, &base, &size, &made)` | `OP_CAP_INFO` of an Untyped; `made` says whether `a4` is not 0 |
| `rv_retype(cap, type, dst, &base)` | `OP_UNTYPED_RETYPE` |
| `rv_split(cap, lower, upper)` | `OP_UNTYPED_SPLIT` |
| `rv_pool_alloc(pool, type, dst, arg)` | `OP_POOL_ALLOC` |
| `rv_pool_info(pool, type, slots, &bytes, &left)` | `OP_POOL_INFO` |
| `rv_process_alloc(pool, dst, table, regions)` | `OP_POOL_ALLOC` of a process with `regions` region slots |
| `rv_process_regions(process, &regions, &most)` | `OP_CAP_INFO` of a process: its number of region slots, and the most a process may have |
| `rv_process_install(process, region, frame, rights)` | `OP_PROCESS_INSTALL` |
| `rv_process_uninstall(process, region)` | `OP_PROCESS_UNINSTALL` |
| `rv_process_mount(process, index, table)` | `OP_PROCESS_MOUNT` |
| `rv_process_unmount(process, index)` | `OP_PROCESS_UNMOUNT` |
| `rv_thread_configure(thread, pc, sp, arg)` | `OP_THREAD_CONFIGURE` |
| `rv_thread_resume(thread)` | `OP_THREAD_RESUME` |
| `rv_thread_stop(thread, &was)` | `OP_THREAD_STOP` |
| `rv_thread_watch(thread, ntfn, bits)` | `OP_THREAD_WATCH` |
| `rv_thread_fault(thread, &cause, &pc, &addr, &status)` | `OP_THREAD_FAULT` |
| `rv_thread_read_reg(thread, reg, &value)` | `OP_THREAD_READ_REG` |
| `rv_thread_write_reg(thread, reg, value)` | `OP_THREAD_WRITE_REG` |
| `rv_signal(cap, bits)` | `OP_NOTIFY_SIGNAL` |
| `rv_wait(cap, &bits)` | `OP_NOTIFY_WAIT` |
| `rv_notify_carve(cap, bits, dst)` | `OP_NOTIFY_CARVE` |
| `rv_irq_carve(lines, offset, count, dst)` | `OP_IRQ_CARVE` |
| `rv_irq_bind(line, pool, ntfn, dst)` | `OP_IRQ_BIND` |
| `rv_irq_set(cap, bits)` | `OP_IRQ_SET` |
| `rv_timer_set(cap, bits, us)` | `OP_IRQ_SET` on a timer line, with the delay |
| `rv_timer_period(cap, bits, us, &skipped)` | `OP_IRQ_SET` on a timer line, with `IRQ_SET_PERIOD` |
| `rv_clock_read(cap, &now, &hz)` | `OP_CLOCK_READ` |
| `rv_clock_watchdog(cap, us)` | `OP_CLOCK_WATCHDOG` |
| `rv_time_carve(time, offset, count, dst)` | `OP_TIME_CARVE` |
| `rv_time_bind(time, thread, offset, count)` | `OP_TIME_BIND` |
| `rv_print(cap, s, n)` | `OP_PRINT`, 12 bytes at most |
| `rv_write(cap, s, n)`, `rv_putc(cap, c)`, `rv_puts(cap, s)`, `rv_put_hex(cap, v)` | `OP_PRINT`, one call for each 12 bytes |
| `rv_halt(cap, code)` | `OP_HALT` |
| `rv_test_trace(cap)`, `rv_test_tick(cap)` | `OP_TEST_TRACE`, `OP_TEST_TICK` |
| `rv_test_irq(cap, line)`, `rv_test_preempt(cap, n)` | `OP_TEST_IRQ`, `OP_TEST_PREEMPT` |

To replace the demo, edit `user/init/`,
or add a program to `USER_PROGRAMS` in the Makefile.
Each program becomes its own kernel image.
A program of several processes lies in a directory of its own (section 8.4).

### 8.2 Building a second process

Today only one program is embedded.
A second process runs code from the same code region, with its own data and stack.
Loading a separate binary is a job for userspace, and the root task does not do it yet
(`DESIGN.md`, open decision 3).

The steps below follow what `user/init/root.c` does, though some of its names differ.
In step 4 the demo copies instead of deriving.
This works there only because step 7 destroys the child's table along with the pool.
The library of section 8.4 does them in one step,
and hands out the slots, regions and bits that they name.

1. **Take memory** out of `BOOT_CAP_FREE_RAM`:

   - a frame to share,
   - a frame for the child's data and stack,
   - a pool for the child's kernel objects.

   The demo keeps no allocator.
   `take_untyped` splits what is left of the free RAM.
   It keeps the lower half as the block, and the upper half as what is left.
   So the demo's blocks halve one after another (section 5.4).
   It deletes the Untypeds in between.
   So every block lies directly below the free RAM in the derivation tree,
   and three working slots are enough for all of the blocks.
   `take_frame` makes the block a frame, and deletes the block's Untyped,
   because revoking below the frame takes the frame back.
   The pool's Untyped stays,
   because revoking below it is what destroys the pool.

   ```c
   take_frame(SLOT_SHARED, &shared_base, &shared_size);
   take_frame(SLOT_CHILD_DATA, &child_data_base, &child_data_size);
   take_untyped(SLOT_POOL_MEMORY, &pool_base, &pool_size);
   rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base);
   ```

2. **Allocate** the child's objects from the pool, in this order:

   - the table,
   - the process that uses the table, with a region slot for each region it will have,
   - a thread in that process,
   - two notifications.

   ```c
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS);
   rv_process_alloc(SLOT_POOL, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE, CHILD_REGION_SLOTS);
   rv_thread_alloc(SLOT_POOL, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS, 1);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0);
   ```

3. **Install regions** into the child: code, data, and the shared chunk.

   ```c
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 1, SLOT_CHILD_DATA, RIGHT_R | RIGHT_W);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 2, SLOT_SHARED, RIGHT_R | RIGHT_W);
   ```

4. **Fill the child's table** with exactly the authority it should have.
   Narrow the rights as you go.
   Derive each capability rather than copy it,
   so that revoking below your slot takes it back (section 5.2).
   A copy of `BOOT_CAP_PRINT` would be a root,
   and nothing takes a root back while the child's table lives.

   ```c
   rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_PRINT,  BOOT_CAP_PRINT, RIGHT_W);
   rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED,    RIGHT_R | RIGHT_W);
   rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_UP,     SLOT_UP,        RIGHT_W);
   rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_DOWN,   SLOT_DOWN,      RIGHT_R);
   ```

5. **Configure, bind and start** the thread.

   - The stack grows down from the top of the child's data frame.
   - The last argument, zero here, is what `child_main` gets as its first argument.
   - The thread earns half of the first core:
     units carved out of the boot grant.

   At boot, the root task's thread earns every unit of the first core.
   So the root task keeps the first half, and leaves the rest.
   If you bound the child through `BOOT_CAP_TIME` with no units, the child would get spare time only.
   It would then run only while the root task does not want the processor.

   ```c
   rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
             child_data_base + child_data_size, 0);
   rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS / 2, TIME_UNITS / 2, SLOT_CHILD_TIME);
   rv_invoke(OP_TIME_BIND, SLOT_CHILD_TIME, SLOT_CHILD_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0);
   ```

   Revoking below `SLOT_CHILD_TIME` stops the child's thread.
   Binding it again starts the thread again.
   The demo gives the child no watch.
   Section 5.6 says how a parent hears that its child faulted.

6. **Talk** through the shared region and the two notifications.
   The child writes a word, and signals `SLOT_UP`.
   The parent waits, answers, and signals `SLOT_DOWN`.

7. **Tear down** by revoking below the pool's Untyped.
   This destroys the pool:

   - The child's thread, process, table and notifications go with the pool.
   - Every capability to them is cleared, in every table.
   - The Untyped is free again, and can be retyped into something else.

   ```c
   rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_POOL_MEMORY, 0, 0);
   ```

The child must not touch any global variable,
because it shares no data region with the parent.
Everything it needs is on its stack, or behind a capability in its table.

### 8.3 Writing a driver

A driver needs a frame for the device's registers, and one interrupt line.

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

Arming the line again after servicing the device is the acknowledgement.
For a timeout, give a timer line a second bit on the same notification.

A wait takes every bit.
So a wait nested inside the loop may take a bit that the loop services,
or a bit that the loop arms a line on.
That source then falls silent.
`lib/wait.h` prevents this:

- It keeps such bits for the wait that asks for them.
- It arms a periodic timer line again, whichever wait took the line's bit.

### 8.4 The library for programs

`user/lib/` holds what a program of several processes needs besides the system calls.
It was taken out of code that the Wi-Fi system (section 8.5) first wrote for itself.
It is a convention between a parent and the children that the parent builds.
It is not part of the kernel.
So a program may use part of it, or none of it
(`DESIGN.md`, "Programs are plain binaries").
Each header says how to use its calls.

| Header | What it gives |
|---|---|
| `lib/self.h` | What a process hands out of its own: slots, regions, bits of its inbox, timers, units of time, memory halved out of Untypeds, and room for its children's data. It also moves the process to a larger table. |
| `lib/child.h` | A child, which its parent builds, starts, hears, checks and takes down. Frames mapped into the child and taken back. What the child may build of its own. The child's own calls: its log, its state, its answer, its sleep, and its account of what it builds. |
| `lib/chan.h` | A channel between two children: a frame with two rings of packets, and a bit in each direction. A hub: a server's channels to many clients. |
| `lib/bulk.h` | Bulk data that a lender lends to its borrowers where the data lies. A connection's buffers lie in the lender's memory. Some are lent to read, and the borrower maps them read only. Others are lent to fill. References to the buffers pass through two rings. |
| `lib/lock.h` | A lock over memory that several children share: a word taken with an atomic operation, and a notification to wait on while the lock is held. A lock whose word names its holder, so that a taker that waits lends the holder its time (section 5.12). |
| `lib/event.h` | An event that one thread waits for, until a deadline at most. Other threads set it from memory they share with the waiter. It is a word set with an atomic operation, and a notification and a timer line of the waiter's. |
| `lib/wait.h` | Waits on one notification that lose nothing to a nested wait. Bits are kept for the wait that asks for them, and periodic timer lines are armed again. |
| `lib/seqlock.h` | A snapshot that one child publishes, and others copy out whole, from memory they may hold read only. It is a version number and two copies, so neither side calls the kernel or waits for the other. |
| `lib/ring.h`, `lib/log.h` | The rings that a channel is made of. The kernel's log: written through `Print`, and read the way the root task reads it. |
| `lib/say.h`, `lib/libc.h` | Text with values in it, and the memory functions that the compiler calls. |

**Nothing in the library keeps a global.**
Every process runs the code of the one image,
but only the root task runs it with its data.
So each call works on the struct that it is given.
The link checks the library's object files and the children's object files with `tools/no-globals.py`.
A child keeps its state on its stack, in the local variables of its main function,
which never returns.

**A child** starts at its entry, with `a0` pointing at its page.
The page lies at the base of the child's data, which is one frame.
The child's stack is at the top of that frame.
The parent has the data installed too,
and writes the rest of the page before the child starts.

Every child starts with these:

- Its table starts with `CHILD_INBOX`, `CHILD_PARENT`, `CHILD_TIMER`, `CHILD_SELF` and `CHILD_LOG`.
- Its process has `CHILD_REGIONS` region slots, which is eight,
  or the most a process may have, where that is fewer.
  Code and data are in the first two.
- Its inbox starts with `CHILD_BIT_TIMER` and `CHILD_BIT_PARENT`.

Beyond those, the parent hands out the child's slots, regions and bits,
and writes into the page which ones it handed out.
So the type of a child's page is the only thing that the two must agree on.
What the parent gives with `child_give`, it derives from a slot of its own.
So revoking below that slot takes it back.

**A child's time.**
`child_start` names the core that the child runs on, and the units of that core that it earns.
With no units, the child gets only that core's spare time.
The child's thread has a place on each core that its parent has units of.
So `child_spare` can give the child another core's spare time as well.

**A child's signals.**
A child writes its state into the page,
and signals its parent through `CHILD_PARENT`.
`CHILD_PARENT` is the parent's inbox, carved to the child's own bit (section 5.7).
So the parent knows who signalled.
The child signals every bit, `NOTIFY_ALL_BITS`, because it does not know which bit is its own.

**Checking a child.**
A parent can check every so often, with `child_check`, whether the child's loop still comes round.
It asks through the page.
The child answers each time round its loop, with `child_answer`.
A check finds a child that did not answer the last check:
a child that spins, or a child that waits for something that never comes.
What to do about it is the parent's choice:
take the child down and build it again, or halt.

**A child's text** goes into the kernel's log through `CHILD_LOG`, a `Print` capability (section 6.3).
It appears in order with the kernel's lines,
among them the lines about the child's own faults.
The child cannot halt the machine.
`say` in `lib/say.h` formats the text,
and `log_out` in `lib/log.h` writes it, one call for each 12 bytes.

**What a child costs.**
A child costs its parent six slots,
and a seventh if it builds threads of its own (see below).
`child_free` takes back everything that was made for the child.

**A child's data** is a block of its own,
which its parent installs in a region used for that child alone.
But once the parent has asked for room for its children's data with `self_room`,
each child's data is carved from that room instead.
The parent installs the room once.
So the parent spends one region on the pages of all its children, not one region for each child.

**A child of its own image** runs code that its parent did not link, with `child_code`.
A frame of the other image goes into the child's code region,
and the child starts at an entry in that image.
Such an image keeps globals, in memory that its parent maps for them,
because no other process runs its code.

**A child that builds threads of its own**
gets what it builds them from from its parent, with `child_give_own`.
A driver for a library written for an operating system with tasks is an example of such a child.
The parent gives it:

- a pool,
- a few timer lines, carved from the last of its parent's,
- units of a core,
- a capability to its own table,
- the last slots of that table, which the parent no longer hands out.

The child makes a `struct self` of them with `child_self`,
and builds through `lib/self.h` as a root task does.
But it hands out no memory, no regions and no bits of its inbox, which stay its parent's,
and it builds no children.
It is also given its parent's inbox, carved to the child's own fault bit,
and watches its threads with it.
So a fault of any of its threads reaches its parent as the child's own fault.
`child_free` takes all of it back with the rest, including the pool and the units.

Such a child may outgrow its table.
`self_migrate` moves the child to a larger table from its pool,
and every capability keeps its number (section 5.2).
No other thread of the child may make a call during the move.
After the move, what the parent gives the child through `child_give` goes into the old table,
which the child no longer names.

**A channel** is connected by the parent into two children, running or not.
Each child holds the other's inbox, carved to the other's bit for the channel.
Each end keeps in its page where its rings lie, and their shape.
The other child cannot write that page.
So a peer can spoil a packet, but it cannot steer the other's writes outside the ring.
But a peer can make a ring seem as full as it likes.
So an end that serves a peer it does not trust takes a ring's worth of packets for each wake,
rather than taking packets until the ring is empty.

**A hub** is a server's side of channels to many clients.
It is one frame that holds a channel for each client.
The server installs the frame once, in one region.
Each client is given its own channel, carved from the frame, and sees no other client's channel.

The parent connects a client to an end of the hub with `chan_hub_connect`.
The server and the client may be running or not.
When the parent takes the client down, it closes the end with `chan_hub_close`.
This takes the channel from the client, takes the client's inbox from the server, and tells the server.
Then:

1. The server, told, sees with `chan_changed` that an end was closed or connected.
2. The server forgets what it kept for the end's last client, and says so with `chan_seen`.
3. Only then is the end idle (`chan_hub_idle`), and another client may be connected to it.

The new client's channel starts zeroed, as every channel does.
So nothing is left to read of what the last client sent or was sent.

**Bulk data** is lent rather than copied (`lib/bulk.h`),
once messages grow past a few hundred bytes,
or when a device reads or writes them by itself.
In this library, "lend" and "borrower" are about buffers, not about time as in section 5.12.

The lender is a driver or a server:
the side that its borrowers trust, and that outlives them.
It holds a connection's buffers in its own memory.
Its parent sets up the connections:

- `bulk_new` makes a lender's connections, in one frame that the lender installs once.
- `bulk_connect` connects a borrower to a connection.
  The lender and the borrower may be running or not.
  `bulk_connect` zeroes the connection,
  and installs the connection's buffers lent to read in the borrower, read only.
  It installs the rings and the buffers lent to fill so that the borrower can write them too,
  and lends the borrower every buffer lent to fill.

Then both sides do the same four things:

- `bulk_take` gives a buffer to fill.
  For the lender, this is one of its own buffers that no one holds.
  For a borrower, it is a buffer that it was lent to fill.
- `bulk_hand` hands that buffer over, with its bytes.
  The lender lends it to read.
  A borrower gives it back filled.
- `bulk_next` gives the next buffer that the other side handed over with bytes.
  It reads in rounds, and `bulk_turn` starts a round.
  For one wake, it gives no more than the other side's ring held as the round began.
  So a peer that keeps writing references cannot keep this side from its other work.
  `bulk_more` says to come back for the rest.
- `bulk_put` gives that buffer back, once its bytes are used.
  The lender gives a buffer back to be filled again.
  A borrower gives back a buffer it has read.

A buffer given back empty is room.
The library takes in room as `bulk_next` passes it.
Room wakes the other side only if that side asked for room, by finding none to take.
A buffer handed over with bytes does wake the other side.

The borrower can still write a buffer after it gives it back filled.
So the lender reads once, into its own memory, the part of that buffer that it decides on.
The lender takes back only what it lent and has not had back,
and it counts what it refused.
So a borrower can lose only what it was lent.

The parent closes a connection with `bulk_close` when it takes the borrower down.
Then:

1. The lender, told, sees the close with `bulk_changed`.
2. Once nothing of the lender's touches those buffers, and above all its device does not,
   the lender says with `bulk_seen` that it let go of all of them.
3. Only then is the connection idle (`bulk_idle`), ready for another borrower.

`DESIGN.md`, "Bulk data", says which side lends, why the rights are what they are,
and where a copy is the price to pay.

**Which to use.**

- A message, such as a question or its answer: a channel.
- State that one child publishes to many: a seqlock.
- A stream of buffers, which are large, or which a device fills and empties: a bulk connection.
- Memory handed over for good: `child_unmap` and `child_map` (below).

**A lock** over memory that children share is a word in that memory, and a notification.
Each child holds the notification with `RIGHT_R` and `RIGHT_W`.
The parent gives each child both the word and the notification:
it writes the word's address and the notification's slot into each child's page.
The lock makes no system call while nobody waits.
No child can make another child write past the word.
But any child can keep the lock, or break it.
So the lock is for children that trust each other.

**Memory handed from one child to another** is `child_unmap` from the first child,
and `child_map` into the second.
Their parent makes both calls.
The kernel then promises that the first child no longer reaches the memory.
The first child faults if it touches the memory again.
The second child is told after the first child told its parent that it was done.
The second child then reads what the first wrote, with no fence (section 5.7).

**What the library does not do yet.**
`TODO.md` gives the reasons.

- A channel between two peers is connected only once.
  Only a hub's ends are connected again.
- Halved memory is joined again only all at once:
  when no free block will do and every block is back.
  So each free block holds a slot.

**The library's test.**
`make lib-test` boots `user/libtest/` on any board.
Its root task does the following, in this order:

- It checks the memory functions against simple byte-by-byte versions.
  It checks the text that `say` writes, including for a conversion that `say` does not know.
- It reads bytes back out of free memory.
- It has two children send each other packets through rings that fill.
- It builds a child, hears it fault, and takes it down; then it does this again.
- It gives a child a capability, and takes the capability back by revoking below its own slot.
- It checks a child that answers and a child that spins, and finds the second.
- Where there is a second core, it starts a child there,
  and also gives that child the first core's spare time.
- In room for children's data, it builds a server with a hub, and a client on each end.
  Twice, it takes one client down and connects another in its place.
  Each new client finds its channel zeroed.
- It has three children add to a number they share, under a lock.
  Each child sleeps now and then while it holds the lock.
  It does this again under a lock that names its holder.
- It waits for an event that a child sets, and each set ends a wait.
  It also waits for an event that nothing sets, and the event's deadline ends that wait.
- Through a waiter, it waits for one bit while a periodic line ticks.
  It finds every tick counted, and a bit it did not ask for kept.
- It has a thread publish a snapshot through a seqlock while a child reads it.
  The thread runs on the second core, where there is one.
  The root task stops the thread in the middle of a write.
- It hands a buffer between two children and back.
  Then it takes the buffer from the last child,
  which must fault when it stores to the buffer.
- It has a lender lend buffers to two borrowers, to fill and to read.
  The lender must refuse what a forger hands it,
  and zero a connection for its next borrower.
  A borrower that stores into a buffer lent to read must fault.
  The lender must serve one borrower beside another that makes its ring seem to hold a billion references.
- It lets a child build a thread of its own.
  The thread sleeps on its own timer and faults,
  and the root task hears the fault as the child's.
  The root task takes the child down.
  It does all of this twice.
- It lets a child move its table to a larger one, from a pool it gave the child.
  Every capability keeps its number.
  It does this twice.

Throughout, it checks that everything it handed out comes back,
and that a child built the second time left the same behind as the first.

### 8.5 A program of several processes: the Wi-Fi system

`user/wifi/` is a program of several files and six processes on a Pico 2 W.
It is built on the library.
The six processes are:

- a root task, which builds the system,
- a driver for the CYW43439, the Pico 2 W's Wi-Fi chip,
- a network process with an IP stack,
- three clients of the network process: the echo, the clock and the logger.

`wifi.h` says what the processes share: the types of their pages.
`system.h` is the root task's half of the network process and its clients.
The ESP32-C6's root task shares `system.h`.

**The link.**
The driver and the network process trade Ethernet frames through the link,
which the driver lends (`lib/bulk.h`):

- the frames that the driver received, lent to read,
- buffers to fill with the frames to send.

**Sockets.**
The network process lends each client a connection.
Through it, the client uses datagram sockets (`sock.h`):

- what the client asks for goes in buffers lent to fill,
- the answers and the datagrams come in buffers lent to read.

A client binds ports.
A port stays the client's until the client closes it, or is gone.
The client sends from its ports, and receives what is sent to them.

**The clients.**

- The echo sends back every datagram sent to UDP port 7.
- The clock asks the network's DNS server for a server of pool.ntp.org,
  and asks that server for the time by SNTP.
  It tells the time in answer to any datagram sent to UDP port 13.
- The logger carries the kernel's log to a host that asks on UDP port 7070 (section 5.10).

**Faults and checks.**
A datagram that reads `fault` makes the echo store where it has no region, as a broken client would.
A datagram that reads `hang` makes the echo spin.
The root task checks every child each second.
In either case, it takes the echo down and builds it again,
and in the meantime the network process gives back the echo's port.
After each check, the root task feeds the watchdog (section 5.8).

`make BOARD=rp2350 wifi` builds and runs the system,
follows its log, and checks it from the host (section 4).

**Reaching the chip.**
The driver reaches the chip through frames over PIO0's registers,
and over the control registers of four pins.
The root task carves these frames from the frames it is granted over PIO0 and IO_BANK0 (section 3).
The chip's firmware lies in more frames than the driver has regions.
So the driver installs those frames one at a time,
in a region that the root task left free for this.

**Configuration.**
What the system does comes from a file that the loader places in the root task's input region.
The system can scan, join a network, or run an access point.
`make BOARD=rp2350 wifi WIFI_CONFIG=file` names the file.
Its lines are `mode=scan|sta|ap`, `ssid=`, `pass=`, `bssid=`, `channel=` and `run=`.
So a passphrase is in no image, and in no file that git tracks.
`local/`, which git ignores, is where to keep such a file.

**The bridge.**
A build of its own, `make BOARD=rp2350 WIFI_BRIDGE=1`, runs on the Hazard3 only.
It builds no network process, and ends as a bridge:

- at once, with `mode=bridge` or with no mode,
- or once the scan, the join or the access point that the mode asks for is up.

As a bridge, the host reads and writes the chip's backplane through the console, one line of text at a time.
`user/wifi/bridge.h` describes this,
and `user/wifi/tools/cyw43bridge.py` speaks it.

**Security.**
A station joins by WPA3's SAE where the access point offers it,
and by WPA2's passphrase otherwise.
It leaves the network once its run is over.
An access point offers both.
`sae=0` makes either one keep to WPA2.
The chip's firmware runs SAE itself, and the driver gives it the passphrase.

**On an ESP32-C6**, the same network process runs beside a driver of another kind:
`user/wifi/esp32c6/`, over the radio of `user/radio/esp32c6/`.
The driver is a child that runs an image of its own, from the window onto flash.
It is a station of its own, on hostap's supplicant, over the radio's own MAC.
It links none of Espressif's closed Wi-Fi libraries.
An adapter builds the threads the driver runs on, in the shape of an operating system's tasks,
from what its root task gave it (section 8.4).

The ESP32-C6's root task builds the network process and its clients
as the Pico 2 W's root task does, except for the logger.
It needs no logger, because it carries the kernel's log to the console itself,
as the log comes (section 5.10).
There the host may type `end`, which ends the run.

`make BOARD=esp32c6 wifi-esp32c6 WIFI_CONFIG=file` writes the driver into flash,
unless the flash holds it already, and runs it:

- With no `ssid=`, the driver scans.
- With `ssid=` and `pass=`, it joins that network,
  at the access point that `bssid=` names, if it names one.
  It joins by WPA3's SAE where the access point offers it,
  and by WPA2's passphrase otherwise.

On that network, the network process takes an address by DHCP,
and answers ping and UDP port 7777.
Its clients answer on their own ports.
`tools/wifi-run.py` checks them from the host, as it checks the Pico 2 W's, and ends the run.
Otherwise the run lasts `run=` seconds.
`debug=frames,wpa`, `pmf=0` and `sae=0` make the driver tell more, or join in another way;
`user/radio/esp32c6/root.c` describes them.
When a thread of the driver's faults, the log shows the fault with the thread's stack,
and names the functions in it.

A program of several processes lies in a directory of its own, `user/<program>/`,
with its root task in `root.c`.
`PROGRAMS` in `user/build.mk` links each such program with the library.
`user/wifi/NOTES.md` says what writing the Wi-Fi system was like, before the library and after.

## 9. Debugging and testing interfaces

**Tracing.**
`OP_TEST_TRACE` makes the kernel print one line per system call:

```
trace: op=0x00000007 slot=0x00000004 a1=0x00000007 a2=0x0000000d a3=0x00000000 -> 0x00000000
```

In that line, `blocked` takes the place of the status when the call waits.
The kernel also prints these lines:

- `trace: info a1=... a2=...` after a describing call that succeeded, with its results:
  four words for `OP_CAP_INFO`, and two for `OP_POOL_INFO` (section 6.16).
  An `OP_THREAD_OVERRUNS` that succeeded prints this line too,
  with its result in `a1` and 0 in `a2`.
- `trace: wake <bits>` after a call that woke a waiter,
  or after the `user fault` of a thread whose watch woke one.

A call can stop where `OP_TEST_PREEMPT` armed a stop.
Then the call prints `trace: preempt` instead of its line,
followed by what the step of the clock woke.
The call's own line comes once the call is made again and finishes.

After every traced call, the kernel runs the self-check:
the executable form of the properties in `DESIGN.md`.
It halts with code 3 on the first violation.
Only the kernel for QEMU `virt` has the self-check and the tracing,
unless the build sets `SELFCHECK=1`.
The kernel for every other board is smaller, and has neither.
It refuses `OP_TEST_TRACE` with `KERR_WRONG_TYPE`.

While tracing is on, the timer preempts nobody and moves no time.
Only `OP_TEST_TICK` does: each step is a millisecond of the counter's counts.
This lets the host build reproduce the transcript.
The clock that charges turns then follows the record alone.
At each `OP_TEST_TICK`, a turn with time pays its units' part of each step it ran.
A turn pays nothing when it waits.

A thread cannot run under tracing in these cases:

- its program counter was set while tracing was on,
- or `OP_THREAD_WRITE_REG` wrote any of its registers while tracing was on.

The kernel halts with code 6 instead.
`OP_THREAD_STOP` and `OP_THREAD_READ_REG` are allowed.
The only program counter that a stop moves is a waiter's:
back to the call it made, which the host build makes again too.
So a traced thread that a stop took off its wait prints its wait's line again, once it is resumed.
A watcher that answers the wait instead writes registers,
and after that the thread cannot run under tracing.

**The self-check** (`kernel/selfcheck.c`) verifies these properties, among others:

- No installed region or frame overlaps a pool.
- What one Untyped made lies within it, and overlaps nothing else that it made.
- Every process's PMP image matches its region slots.
- Every capability names a live object of its own type, or an in-bounds frame, Untyped or line range.
- Every capability to a pool or an object lies below the pool's own node.
- Pools tile their memory exactly.
- Every waiting thread names a live notification.
- Every ready thread that may run, other than the running one, is on the queue that its virtual time says.
- Each unit of time is earned by the thread bound to it, and by no other.
- Every `Irq` names a live notification, or it names none and is disarmed.
- Every thread's watch names a live notification, through a capability that could signal it.
- No `Irq` armed on a timer line is past its deadline.
- The controller forwards exactly the lines that an `Irq` is armed on.

**The benchmark.**
`make bench` boots `user/bench/`, whose `root.c` names its measures.
It says what one round of each measure costs, in the core's cycles.
Under QEMU, the clock counts instructions, so there the cost is in instructions.
On QEMU's boards, a run repeats exactly.
There, `make bench` holds each cost to the board's record in `tests/bench/`, within a tenth either way.
`make bench-refresh` writes the record from a run.
The worst round beside a busy process waits for that process's requests.
So that process's span sets the worst round, and the worst round's cycles grow with the clock.

**The laboratory.**
`make lab` boots `user/lab/`, a made-up system of servers and clients built from the library.
Scenario by scenario, it says one of these:

- how long its clients' asks waited, when they lend their servers their time and when they do not,
- how long its lockers' takes waited on the locks of `lib/lock.h`:
  the lock whose takers lend its holder their time, and the lock whose takers do not.

`user/lab/NOTES.md` says what each scenario stands for, and what it showed.

**Replay input.**
`BOOT_CAP_INPUT` maps 64 KiB, where QEMU's loader places a file:

```c
struct replay_header { uint32_t magic; uint32_t count; };  /* magic "RVFZ", 0x5a465652 */
struct replay_record { uint8_t op; uint8_t actor; uint16_t slot; uint32_t a1, a2, a3; };
```

Each record is one system call, and the thread that makes it.
`actor` names the thread:

- 0 is whichever thread runs.
- 1 is the driver's root thread.
- 2 is its second thread.
- 3 is a third thread, which starts stopped until a record resumes it.
  It lies in the root thread's pool, but runs in the second thread's process.

The replay driver in `user/fuzzdrv.c` performs the records in order.
The host build in `host/` performs the same records natively, under ASan and UBSan.
`tests/differential.py` requires the two transcripts to match line for line.
A driver thread that the records unmapped faults, and the next thread goes on with the records.
Once the records are done, every driver thread ends in a breakpoint, which is a fault too.
So a transcript ends in `no runnable thread`, unless something stopped it first.

- Hand-written scenarios live in `tests/seeds`, as text that `tools/seed.py` turns into records.
- What the fuzzer found lives in `tests/corpus`.
- `tests/mutants/` holds one planted bug per invariant.
