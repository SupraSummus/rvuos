# rvuos design: cores

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Cores

The kernel runs on `CORES` cores, a constant of the build:
one on every board but QEMU `virt`, which `make CORES=2` builds for two harts.
What a core has of its own is `struct core` in `kernel/object.h`:
its thread, its turn, its three queues, its release, its timer, the call it is in,
and what it tells the others.
The objects, the derivation tree, the units, the tick count, the timer lines and the device lines are the machine's.
With one core there is no lock, and no other core to interrupt or wait for.

**One lock around the kernel.**
A trap takes the kernel's lock as it begins, once the thread's registers are in its frame,
and gives it up as it returns to user mode,
so the kernel's state changes one trap at a time, on whichever core,
as in seL4's multicore build.
Each trap is still one step on one state,
which the self-check, `host/history.c` and the replay rely on.
It is a ticket lock, so the cores take it in the order they asked for it,
and a core waits at most as long as each core ahead of it holds the lock.
That is bounded by goal 4 but for the walks,
and a walk asks between two steps whether another core waits, as it asks whether an interrupt is pending,
so it stops for the waiting core as for an interrupt and is made again after; see "Bounded work".
A core waits for the lock at most a step of each other core.
A core that stalls gives the lock up while it waits for an interrupt.
Finer locks would leave the self-check nothing to hold between two steps, and on a microcontroller's two cores buy little.

**A thread runs on the core its units are of.**
`TIME_UNITS` is the units of each core, numbered core by core:
unit `u` is a part of core `u / TIME_UNITS`.
`BOOT_CAP_TIME` holds every core's, and the root thread earns the first core's.
A bind takes units of one core, or none, at the first unit's core, for spare time there,
and the thread waits on that core's queues and runs there and nowhere else.
So which core a thread runs on is authority, held as units are, and no policy in the kernel places a thread.
A thread bound to another core's units during its turn finishes the turn, which then ends at the next tick, and moves:
`t->core` names the core it runs and waits on, and changes as the thread settles while it is no core's turn.
Spare time is each core's own: a thread on spare time runs on its core when nobody there with time wants it.
A program learns how many cores there are by the units it can carve out of `BOOT_CAP_TIME`.

**Time is the machine's, and each core counts up to it.**
The tick count, the deadlines of the timer lines and every account count the machine's ticks,
on the one counter every core reads, on one grid.
A trap counts the ticks the counter passed, as it does on one core,
and with them those other cores counted since this one last did,
charging its turn for all of them as the tick would have.
So a core's turn is charged to the tick the core counted, `ticks` in `struct core`,
which lags while the core runs on without trapping, and which only the core's own traps move.
Whichever core counts a tick fires the timer lines that are due,
and looks at the release of every core whose release it reached, a fixed few units each,
so every core's queues agree with the accounts, whichever core counted.
Every core's timer wakes for the nearest deadline,
so the core that armed it takes the tick, and another may wake for one the first took,
a trap that counts nothing.

**A core tells another by interrupting it.**
A core that puts a thread on another core's queue, brings another core's release forward,
or changes the thread whose turn another core has,
marks that core's timer stale, and the other core sets its timer again before it leaves the kernel.
It interrupts the other core for it, with the software interrupt of QEMU `virt`'s CLINT,
when that core idles, or runs user mode with its timer set past the next tick;
one whose timer comes within a tick, or that runs the kernel, finds the mark itself.
The interrupt is sent once the lock is given up, `interrupt_owed` in `struct core` until then,
so that the other core does not trap only to wait for the lock the sender still holds.

**A core that takes what another runs makes it trap first.**
A core runs user mode with the regions of its thread's process in its protection unit,
which only that core can change, and with its thread's registers out of the kernel's reach until it traps.
So a call that takes a region from a process another core runs,
or stops the thread another core runs, by taking its process or destroying it,
interrupts that core and waits until it has trapped,
before the memory can go to anything else and before a watcher reads the frame.
Each core says without the lock whether it runs user mode, `in_user`,
cleared once a trap has saved the thread's registers, and set as the core leaves the kernel with the lock.
The wait is for a trap to begin, which user mode takes at once,
so the hardware bounds it, and the other core needs no lock for it.
That core then waits for the lock the call holds,
and as it takes the lock loads its regions again, or finds its thread taken and hands the processor on.
A trap whose thread another core stopped or destroyed while the trap waited for the lock is dropped:
a call is not made and its pc stays at it, and a fault is not reported,
since a thread that runs again makes the call, or faults, again.
A thread that only loses its units runs on as it does on one core, to the end of its turn.

**A core with nothing to run idles, outside any trap.**
A trap that leaves nobody's turn on its core returns into `start.S`, which idles in `core_idle`
until a thread has a turn there, then leaves for it as a trap does,
so no trap's call chain outlives the thread it was for.
The machine stops, saying `no runnable thread`, only when no core runs a thread or holds one waiting for a turn,
and nothing could make one runnable, as on one core;
a core with nothing to do while another runs waits for an interrupt.

**The device lines are the first core's.**
The controller forwards every line to the first core alone, which claims them, and the signal reaches a driver on any core
as any signal does, by the interrupt above when the driver's core needs one.
Which core takes a line is open decision 24.

**How the cores start.**
Every hart of QEMU `virt` enters at the reset vector.
The first boots the kernel with the lock held.
The others take their stacks, each `KERNEL_STACK_SIZE` below the one before,
and wait in `wfi` until the first raises their software interrupt at the end of the boot;
then they start their timers, fence their protection units as the first did, and idle.
A hart past `CORES` waits for ever.

**What it costs.**
A trap takes and gives the lock, two atomic operations, and stores `in_user` twice.
A core that changes what another runs pays an interrupt to it,
and a call that takes what another core runs waits for that core's trap to begin.
