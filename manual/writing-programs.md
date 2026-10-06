# rvuos user manual: writing and debugging a program

Chapters 8 and 9 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 8. Writing a program

### 8.1 Toolchain and layout

A user program is a flat binary linked against `user/user.ld.S`,
with the board's addresses, see "Boards",
and embedded into the kernel image by the Makefile.
Constraints of the current linker script:

- code and read-only data live in the read-execute region, at `0x80100000` on QEMU,
- data and the stack live in the read-write region, at `0x80110000` on QEMU,
  initialised data at its base and loaded right behind the read-only data,
- `user/arch/<arch>/start.S` copies initialised data over, zeroes `.bss`, calls `main`,
  and halts with code 1 through `BOOT_CAP_DEBUG` if `main` returns,
- a child runs this code without the data region, so the code a child runs keeps no global, section 8.4.

Compile flags are `-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib`,
or `--target=thumbv7m-none-eabi -mcpu=cortex-m3 -mfloat-abi=soft -ffreestanding -nostdlib` on ARMv7-M,
with `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33+nodsp+nofp` in place of the first two on ARMv8-M.
There is no libc; `user/rvuos.h` has a wrapper for every operation,
and `user/lib/` what a program of several processes needs beside them, section 8.4:

| Wrapper | Operation |
|---|---|
| `rv_invoke(op, cap, a1, a2, a3)` | any |
| `rv_cap_copy(table, dst, src, rights)` | `OP_CAP_COPY` |
| `rv_cap_derive(table, dst, src, rights)` | `OP_CAP_DERIVE` |
| `rv_cap_move(table, dst, src)` | `OP_CAP_MOVE` |
| `rv_cap_delete(table, slot)` | `OP_CAP_DELETE` |
| `rv_cap_revoke(table, slot)` | `OP_CAP_REVOKE` |
| `rv_frame_info(cap, &base, &size)` | `OP_FRAME_INFO` |
| `rv_frame_min_size(cap, &min)` | `OP_FRAME_INFO`, reading `a4` |
| `rv_frame_carve(cap, offset, size, dst)` | `OP_FRAME_CARVE` |
| `rv_untyped_info(cap, &base, &size, &made)` | `OP_UNTYPED_INFO` |
| `rv_retype(cap, type, dst, &base)` | `OP_UNTYPED_RETYPE` |
| `rv_split(cap, lower, upper)` | `OP_UNTYPED_SPLIT` |
| `rv_pool_alloc(pool, type, dst, arg)` | `OP_POOL_ALLOC` |
| `rv_process_install(process, region, frame, rights)` | `OP_PROCESS_INSTALL` |
| `rv_process_uninstall(process, region)` | `OP_PROCESS_UNINSTALL` |
| `rv_thread_configure(thread, pc, sp, arg)` | `OP_THREAD_CONFIGURE` |
| `rv_thread_resume(thread)` | `OP_THREAD_RESUME` |
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
| `rv_clock_read(cap, &now, &hz, &counter)` | `OP_CLOCK_READ` |
| `rv_clock_frame(cap, dst)` | `OP_CLOCK_FRAME` |
| `rv_counter_low(counter)` | no call: a load from the counter's frame |
| `rv_clock_watchdog(cap, us)` | `OP_CLOCK_WATCHDOG` |
| `rv_time_carve(time, offset, count, dst)` | `OP_TIME_CARVE` |
| `rv_time_bind(time, thread, offset, count)` | `OP_TIME_BIND` |
| `rv_debug_write(cap, s, n)` | `OP_DEBUG_WRITE`, 12 bytes at most |
| `rv_write(cap, s, n)`, `rv_putc(cap, c)`, `rv_puts(cap, s)`, `rv_put_hex(cap, v)` | `OP_DEBUG_WRITE`, a call per 12 bytes |
| `rv_halt(cap, code)` | `OP_DEBUG_HALT` |
| `rv_debug_trace(cap)`, `rv_debug_tick(cap)` | `OP_DEBUG_TRACE`, `OP_DEBUG_TICK` |
| `rv_debug_irq(cap, line)`, `rv_debug_preempt(cap, n)` | `OP_DEBUG_IRQ`, `OP_DEBUG_PREEMPT` |

To replace the demo, edit `user/init.c` or add a program to `USER_PROGRAMS` in the Makefile;
each program becomes its own kernel image.
A program of several processes lies in a directory of its own, section 8.4.

### 8.2 Building a second process

Today one program is embedded, and a second process runs code
from the same code region with its own data and stack.
Loading a separate binary is a userspace job the root task does not do yet;
`DESIGN.md`, open decision 3.
The steps below are what `user/init.c` does, call by call;
the library of section 8.4 takes them in one, and hands out the slots, regions and bits they name.

1. **Take memory** out of `BOOT_CAP_FREE_RAM`:
   a frame to share, a frame for the child's data and stack,
   and a pool for its kernel objects.
   The demo keeps no allocator: `take_untyped` splits what is left of the free RAM,
   keeps the lower half as the block and the upper as what is left,
   so its blocks halve one after another, section 5.4.
   It deletes the Untypeds in between, so every block hangs right below the free RAM
   and three working slots do for all of them.
   `take_frame` makes the block a frame and deletes its Untyped, since revoking below a frame takes it back;
   the pool's Untyped stays, since revoking below it is what destroys the pool.

   ```c
   take_frame(SLOT_SHARED, &shared_base, &shared_size);
   take_frame(SLOT_CHILD_DATA, &child_data_base, &child_data_size);
   take_untyped(SLOT_POOL_MEMORY, &pool_base, &pool_size);
   rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base);
   ```

2. **Allocate** the child's objects from the pool,
   table first, then the process that uses it, then a thread in it.

   ```c
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

5. **Configure, bind and start** the thread.
   The stack grows down from the top of the child's data frame,
   and the last argument, zero here, is what `child_main` finds as its first.
   The thread earns half the first core, units carved out of the boot grant.
   The root task's thread earns every unit of the first core at boot, so it keeps the first half and leaves the rest;
   binding the child through `BOOT_CAP_TIME` with no units would give it spare time alone,
   which it gets only while the root task does not want the processor.

   ```c
   rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
             child_data_base + child_data_size, 0);
   rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS / 2, TIME_UNITS / 2, SLOT_CHILD_TIME);
   rv_invoke(OP_TIME_BIND, SLOT_CHILD_TIME, SLOT_CHILD_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0);
   ```

   Revoking below `SLOT_CHILD_TIME` stops the child's thread; binding it again starts it.
   The demo gives the child no watch; section 5.6 says how a parent hears its child fault.

6. **Talk** through the shared region and the two notifications.
   The child writes a word, signals `SLOT_UP`; the parent waits, answers, signals `SLOT_DOWN`.

7. **Tear down** by revoking below the pool's Untyped, which destroys the pool.
   The child's thread, process, table and notifications go with it,
   every capability to them is cleared in every table,
   and the Untyped is free again, to be retyped into something else.

   ```c
   rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_POOL_MEMORY, 0, 0);
   ```

The child must touch no global variable, since it shares no data region with the parent:
everything it needs is on its stack or behind a capability in its table.

### 8.3 Writing a driver

A driver needs a frame for the device's registers and one interrupt line.

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

### 8.4 The library for programs

`user/lib/` is what a program of several processes needs beside the system calls,
taken out of what the Wi-Fi system, section 8.5, first wrote for itself.
It is a convention between a parent and the children it builds, not the kernel's,
so a program may take part of it or none; `DESIGN.md`, "Programs are plain binaries".
Each header says how its calls are used.

| Header | What it gives |
|---|---|
| `lib/self.h` | what a process hands out of its own: slots, regions, bits of its inbox, timers, units of time, memory halved out of Untypeds, and room for its children's data |
| `lib/child.h` | a child built, started, heard, checked and taken down by its parent, frames mapped into it and taken back, what it may build of its own, and the child's own calls: its log, its state, its answer, its sleep, its account of what it builds |
| `lib/chan.h` | a channel between two children: a frame of two rings of packets, and a bit each way; and a hub, a server's channels to many clients |
| `lib/lock.h` | a lock over memory several children share: a word taken with an atomic operation, and a notification to wait on while it is held; the kernel's mutex, section 5.12, is the one that knows its holder |
| `lib/seqlock.h` | a snapshot one child publishes and others copy out whole, from memory they may hold read only: a count and two copies, so that neither side calls the kernel or waits for the other |
| `lib/ring.h`, `lib/log.h` | the rings a channel is made of, and the kernel's log: written through `Debug`, and read as the root task reads it |
| `lib/say.h`, `lib/libc.h` | text with values, and the memory functions the compiler calls |

**Nothing in it keeps a global**,
since every process runs the one image's code but only the root task with its data;
each call works on the struct it is handed,
and the link checks the library's objects and the children's with `tools/no-globals.py`.
A child keeps its state on its stack, in its main's frame, which never returns.

**A child** starts at its entry with `a0` at its page,
which lies at the base of its data, one frame, with its stack at the top;
its parent has the data installed too, and writes the rest of the page before the start.
Its table starts with `CHILD_INBOX`, `CHILD_PARENT`, `CHILD_TIMER`, `CHILD_SELF` and `CHILD_LOG`,
its process with code and data in the first two regions, and its inbox with `CHILD_BIT_TIMER` and `CHILD_BIT_PARENT`.
Past those the parent hands out the child's slots, regions and bits, and writes which into the page,
so the type of a child's page is all the two agree on.
A child writes its state into the page,
and signals its parent through `CHILD_PARENT`, its parent's inbox carved to the child's own bit, section 5.7,
so the parent knows who signalled and the child names every bit, `NOTIFY_ALL_BITS`, not knowing which.
A parent that checks every so often, `child_check`, asks through the page whether the child's loop still comes round,
and the child answers each time round it, `child_answer`;
a check finds a child that did not answer the last, one that spins or waits for what never comes,
and what to do about it, take it down and build it again or halt, is the parent's.
Its text goes into the kernel's log through `CHILD_LOG`, a `Debug` capability with `RIGHT_W` alone, section 6.3,
in order with the kernel's lines, among them its own faults, and it cannot halt the machine.
`say` in `lib/say.h` formats it, and `log_out` in `lib/log.h` writes it a call per 12 bytes.
It costs its parent six slots, and `child_free` takes back everything that was made for it.
Its data is a block of its own, which its parent installs in a region of its own,
or, once the parent has asked for room for its children's data with `self_room`, carved from that room,
which the parent installs once: a parent then spends one region on all its children's pages, not one each.

**A child of its own image** runs code its parent did not link, `child_code`:
a frame of the other image goes into the child's code region, and the child starts at an entry in it.
Such an image keeps globals, in memory its parent maps for them, since no other process runs its code.

**A child that builds threads of its own**, as a driver of a library written for an operating system with tasks does,
is given what to build them from by its parent, `child_give_own`:
a pool, a few timer lines carved from the last of its parent's, units of the first core,
a capability to its own table, and the last slots of that table, which the parent hands out no more.
The child makes a `struct self` of them, `child_self`, and builds through `lib/self.h` as a root task does,
but hands out no memory, regions or bits of its inbox, which stay its parent's, and builds no children.
It is given its parent's inbox carved to its own fault bit too, and watches its threads with it,
so a fault of any of them reaches its parent as the child's own.
`child_free` takes back all of it with the rest, the pool and the units among it.

**A channel** is connected by the parent into two children, running or not,
each holding the other's inbox carved to the other's bit for the channel.
Each end keeps where its rings lie and their shape in its page, which the other child cannot write,
so a peer can spoil a packet but not steer the other's writes outside the ring.
It can make a ring seem as full as it likes, though,
so an end that serves a peer it does not trust takes a ring's worth of packets for each wake rather than until the ring is empty.

**A hub** is a server's side of channels to many clients:
one frame of a channel for each, which the server installs once, in one region,
and of which each client is given its own channel, carved, and sees no other's.
The parent connects a client to an end of the hub with `chan_hub_connect`, both running or not,
and closes the end with `chan_hub_close` when it takes the client down,
which takes the channel from the client and the client's inbox from the server, and tells the server.
The server, told, sees with `chan_changed` that an end was closed or connected,
forgets what it kept for the end's last client, and says so with `chan_seen`;
only then is the end idle, `chan_hub_idle`, and may another client be connected to it,
whose rings start empty.

**A lock** over memory children share is a word in that memory and a notification each holds with `RIGHT_R` and `RIGHT_W`,
both from their parent, which writes the word's address and the notification's slot into each page.
It makes no system call while nobody waits.
None can make another write past the word, but any can keep the lock or break it: it is for children that trust each other.

**Memory handed from one child to another**, with the kernel's word that the first no longer reaches it,
is `child_unmap` from the one and `child_map` into the other, by their parent.
The first faults if it touches the memory again,
and the second, told after the first told its parent it was done, reads what it wrote with no fence, section 5.7.

**What it does not do yet**, with the reasons in `TODO.md`:
children run on the first core;
a channel between two peers is connected once, as only a hub's ends are connected again;
and halved memory is never joined again, so each free block holds a slot.

`make lib-test` boots `user/libtest/` on any board.
Its root task reads bytes back out of free memory, has two children send each other packets through rings that fill,
and builds, hears fault and takes down a child twice;
it checks a child that answers and one that spins, and finds the second;
then, in room for children's data, it builds a server with a hub and a client on each end,
and takes one client down and connects another in its place twice;
has three children add to a count they share under a lock, each sleeping now and then while it holds it;
hands a buffer between two children and back, then takes it from the last, which must fault when it stores to it;
and lets a child build a thread of its own, which sleeps on its own timer and faults, which the root task hears as the child's,
and takes it down, twice;
checking that everything handed out comes back and that the second child left the same behind as the first.

### 8.5 A program of several processes: the Wi-Fi system

`user/wifi/` is a program of several files and six processes on a Pico 2 W, built on the library:
a root task that builds the system, a driver for the CYW43439, its Wi-Fi chip, a network process with an IP stack,
and three clients of the network process, the echo, the clock and the logger;
`wifi.h` says what they share, which is the types of their pages,
and `system.h` the root task's half of the network process and its clients, which the ESP32-C6's root task shares.
The driver and the network process trade Ethernet frames through a channel, the link.
Each client has a channel from the network process's hub, through which it uses datagram sockets, `sock.h`:
it binds ports, its own until it closes them or is gone, sends from them and receives what is sent to them.
The echo sends back every datagram to UDP port 7;
the clock asks the network's DNS server for a server of pool.ntp.org, asks that server for the time by SNTP,
and tells the time to any datagram to UDP port 13.
The logger carries the kernel's log to a host that asks on UDP port 7070, section 5.10.
A datagram that reads `fault` makes the echo store where it has no region, as a broken client would,
and one that reads `hang` makes it spin:
the root task, which checks every child each second, takes it down and builds it again,
and the network process gives its port back meanwhile.
After each check the root task feeds the watchdog, section 5.8.
`make BOARD=rp2350 wifi` builds and runs it, follows its log and checks it from the host, section 4.
The driver reaches the chip through frames over PIO0's registers and over the control registers of four pins,
which the root task carves from the frames it is granted over PIO0 and IO_BANK0, section 3,
and it installs the frames that hold the chip's firmware one at a time in a region the root task left it,
since there are more of them than regions.
What the system does, scan, join a network or run an access point, comes from a file the loader places in the root task's input region,
`make BOARD=rp2350 wifi WIFI_CONFIG=file`, with lines `mode=scan|sta|ap`, `ssid=`, `pass=`, `bssid=`, `channel=` and `run=`;
so a passphrase lies in no image and in no file git tracks; `local/`, which git ignores, is the place for one.
A station joins by WPA3's SAE where the access point offers it and by WPA2's passphrase otherwise,
and leaves the network once its run is over;
an access point offers both, and `sae=0` has either keep to WPA2.
The chip's firmware runs SAE itself, and the driver gives it the passphrase.
On an ESP32-C6 the same network process runs beside a driver of another kind, `user/wifi/esp32c6/`:
Espressif's closed Wi-Fi libraries around an adapter, a child that runs an image of its own from the window onto flash,
and builds the threads the libraries want as tasks from what its root task gave it, section 8.4.
Its root task builds the network process and its clients as the Pico 2 W's does, but for the logger,
since it carries the kernel's log to the console itself as the log comes, section 5.10,
where the host may type `end` or `stats`.
`make BOARD=esp32c6 wifi-esp32c6 WIFI_CONFIG=file` writes the driver into flash, unless the flash holds it already, and runs it,
scanning, or with `ssid=` and `pass=`, joining that network, at the access point `bssid=` names if it names one,
by WPA3's SAE where the access point offers it and by WPA2's passphrase otherwise,
where the network process takes an address by DHCP and answers ping and UDP port 7777, and its clients theirs;
`tools/wifi-run.py` checks them from the host as it checks the Pico 2 W's, and ends the run, which otherwise lasts `run=` seconds.
`debug=frames,stats,wpa`, `lib=`, `ax=0`, `pmf=0`, `sae=0` and `ps=1` have the driver tell more or join otherwise,
as `user/wifi/esp32c6/root.c` says, and a thread of the driver's that faults is told with its stack, its functions named.
A program of several processes lies in a directory of its own, `user/<program>/`, its root task in `root.c`,
and the Makefile's `PROGRAMS` links each with the library.
`user/wifi/NOTES.md` says what writing it was like, before the library and after.

## 9. Debugging and testing interfaces

**Tracing.**
`OP_DEBUG_TRACE` makes the kernel print one line per system call,

```
trace: op=0x00000007 slot=0x00000004 a1=0x00000007 a2=0x0000000d a3=0x00000000 -> 0x00000000
```

with `blocked` in place of a status when the call blocked,
and `trace: wake <bits>` after a call that woke a waiter,
or after the `user fault` of a thread whose watch woke one.
A call stopped where `OP_DEBUG_PREEMPT` armed prints `trace: preempt` instead of its line,
followed by what the tick woke,
and its line comes once the call is made again and finishes.
After every traced call the kernel runs the self-check,
the executable form of the properties in `DESIGN.md`,
and halts with code 3 on the first violation.
While tracing is on the tick preempts nobody and moves no time;
only `OP_DEBUG_TICK` does,
so that the host build can reproduce the transcript.
The clock that charges turns is then the tick count alone:
a turn with time pays a whole tick at each `OP_DEBUG_TICK` and nothing at a wait.
A thread whose program counter was set while tracing was on,
or any of whose registers `OP_THREAD_WRITE_REG` wrote then,
cannot be run under tracing; the kernel halts with code 6 instead.

**The self-check** (`kernel/selfcheck.c`) verifies, among other things:
no installed region or frame overlaps a pool,
and what one Untyped made lies within it and overlaps nothing else it made,
every process's PMP image matches its region slots,
every capability names a live object of its own type or an in-bounds frame, Untyped or line range,
and every capability to a pool or an object lies below the pool's own node,
pools tile their memory exactly,
every waiting thread names a live notification,
every ready thread but the running one that may run is on the queue its account says,
and each unit of time is earned by the thread bound to it and by no other,
every `Irq` names a live notification, or none and is disarmed,
every thread's watch names a live notification, through a capability that could signal it,
no `Irq` armed on a timer line is past its deadline,
and the controller forwards exactly the lines an `Irq` is armed on.

**The benchmark.**
`make bench` boots `user/bench/`, whose `root.c` names its measures,
and says what a round of each costs in the core's cycles, or under QEMU, whose clock counts instructions there, in instructions.
On QEMU's boards, where a run repeats exactly, it holds each cost to the board's record in `tests/bench/` within a tenth either way,
and `make bench-refresh` writes the record from a run.
The worst round beside a busy process waits for that process's turns, so the tick sets it, and its cycles grow with the clock.

**The laboratory.**
`make lab` boots `user/lab/`, a made-up system of servers and clients built from the library,
and says, scenario by scenario, how long its clients' asks waited, or its lockers' takes,
on the lock of `lib/lock.h`, on a mutex, and on a mutex whose waiters lend their time;
`user/lab/NOTES.md` says what each scenario stands for and what it showed.

**Replay input.**
`BOOT_CAP_INPUT` maps 64 KiB where QEMU's loader places a file:

```c
struct replay_header { uint32_t magic; uint32_t count; };  /* magic "RVFZ", 0x5a465652 */
struct replay_record { uint8_t op; uint8_t actor; uint16_t slot; uint32_t a1, a2, a3; };
```

Each record is one system call and the thread that makes it:
`actor` 0 is whichever thread runs, 1 the driver's root thread, 2 its second thread,
3 a third that starts stopped until a record resumes it,
and lies in the root thread's pool while it runs in the second's process.
The replay driver in `user/fuzzdrv.c` performs them in order,
the host build in `host/` performs the same records natively under ASan and UBSan,
and `tests/differential.py` requires the two transcripts to match line for line.
A driver thread the records unmapped faults, and the next goes on with the records;
every driver thread ends in a breakpoint, a fault too, once the records are done,
so a transcript ends in `no runnable thread` unless something stopped it first.
Hand-written scenarios live in `tests/seeds`,
what the fuzzer found in `tests/corpus`,
and `tests/mutants/` holds one planted bug per invariant.
