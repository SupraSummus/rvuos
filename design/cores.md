# rvuos design: cores

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Cores

The kernel runs on `CORES` cores, a constant of the build:
one on every board but QEMU `virt`, which `make CORES=2` builds for two harts,
mps2-an521, which `make BOARD=mps2-an521 CORES=2` builds for its two Cortex-M33s,
and RP2350, which `make BOARD=rp2350 CORES=2` builds for both cores of either kind.
What a core has of its own is `struct core` in `kernel/object.h`:
its thread, its turn, its three queues, its release, its nearest deadline, its timer, the call it is in,
and what it tells the others.
The objects, the derivation tree, the units, the tick count, the timer lines and the device lines are the machine's.
With one core there is no lock, and no other core to interrupt or wait for.

**One lock around the kernel.**
A trap takes the kernel's lock as it begins, once the thread's registers are in its frame,
and gives it up as it returns to user mode,
so the kernel's state changes one trap at a time, on whichever core,
as in seL4's multicore build.
Every way out of the kernel, a trap's, the idle's and the first, goes through `trap_return` on either architecture,
which gives the lock up, `core_leave`, once the frame it returns into is whole.
Each trap is still one step on one state,
which the self-check, `host/history.c` and the replay rely on.
It is a ticket lock, so the cores take it in the order they asked for it,
and a core waits at most as long as each core ahead of it holds the lock.
RISC-V takes a ticket with one `amoadd`; ARM has no atomic add,
so it takes one with an exclusive load and store, `arch_ticket_take` in its `kernel/arch/arm/mpu.c`,
which goes round again when another core took a ticket between the two, once a trap at most.
ARM's pair reaches the other cores only on memory the MPU marks Shareable,
and the kernel runs with the MPU off, on the default memory map, which marks no RAM Shareable, as RP2350 showed; see "Boards".
The take therefore borrows region 0 for the 32 bytes of the ticket, Shareable and the kernel's alone,
turns the MPU on for the pair, and gives the region back, which spends none of a process's regions.
A process's regions of RAM are Shareable too, so a thread's pair reaches the other cores,
which the demo checks by adding to one word from both.
QEMU's pair reaches every core whatever the memory, so only silicon tells either missing.
That is bounded by goal 4 but for the walks,
and a walk asks between two steps whether another core waits, as it asks whether an interrupt is pending,
so it stops for the waiting core as for an interrupt and is made again after; see "Bounded work".
A core waits for the lock at most a step of each other core.
A core that stalls gives the lock up while it waits for an interrupt.
On ARM a core that waits on another, for the lock or for a trap to begin, waits in `wfe` until the other signals with `sev`,
as ARM's spin waits do; a hart spins.
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

**A timer line wakes the core that armed it.**
The arm notes in the `Irq` which core made it,
and each core keeps the nearest deadline of the lines armed on it, as it keeps its release,
so an idle core with no line of its own sleeps through the others'.
The arm's core learns of the deadline without an interrupt between the cores,
which the timer lines kept on one core, as the device lines are, would cost at every arm from another.
Whichever core counts a tick puts every core's nearest deadline right again,
which only moves it later, so no core is told;
the core that armed a line another core fired first may then wake for it, a trap that counts nothing.

**A core tells another by interrupting it.**
A core that puts a thread on another core's queue, brings another core's release forward,
or changes the thread whose turn another core has,
marks that core's timer stale, and the other core sets its timer again before it leaves the kernel.
It interrupts the other core for it, with the software interrupt of QEMU `virt`'s CLINT or of RP2350's SIO on Hazard3,
or on the Cortex-M33 on a line of each core's NVIC,
through MHU0 on mps2-an521, the SSE-200's message handling unit, and through SIO's doorbells on RP2350,
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
`core_trapped` says whether it is, and hands the processor on if so, for every architecture's trap and for the host's.
On ARM half the frame lies on the thread's stack until the trap copies it in,
so the trap does that before it clears `in_user`, and a frame a watcher reads, or one that is dropped, is whole.
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
QEMU `virt`'s PLIC takes the first hart's enables from any hart,
but each ARM core has an NVIC that only it reaches, and each of RP2350's Hazard3 cores the CSRs of its controller.
So a call on another core that arms or disarms a line records it in a word of lines the first core is to enable,
and interrupts the first, which makes its controller agree as it next takes the lock, `irq_sync` in `kernel/irq.h`,
and passes over a line it entered on that no `Irq` is armed on any more.
Each other core masks every line of its own controller as it starts, `irq_core_init`.
The line the cores interrupt each other on, `IPI_LINE` in the board's `board.h`, is the kernel's on every core,
and `OP_IRQ_BIND` answers `KERR_OVERLAP` for it, as for a line an `Irq` holds.

**How the cores start.**
The first core boots the kernel with the lock held, and at the end of the boot the board starts the others,
`board_cores_start`, which go on in the architecture's `core_start`.
Every hart of QEMU `virt` enters at the reset vector;
the others take their stacks, each `KERNEL_STACK_SIZE` below the one before,
and wait in `wfi` until the first raises their software interrupt;
then they start their timers, fence their protection units as the first did, and idle.
A hart past `CORES` waits for ever.
mps2-an521's second core waits in the SSE-200's `CPUWAIT` until the first lets it go,
then enters the kernel's vector table at its reset vector, takes its stack, and enters handler mode through PendSV,
as the first leaves the boot for the root task;
there it sets up its own MPU, before it takes a ticket through it, then its NVIC and SysTick, and idles.
RP2350's second core waits in the bootrom until the first launches it through SIO's FIFOs,
handing it the kernel's vector table, its stack and `_start` a word at a time as the datasheet's handshake has it;
from `_start` it goes on as QEMU's harts do on Hazard3, waiting for its software interrupt,
and as mps2-an521's second core on the Cortex-M33.
Each other core sets up what the board has of its own as it starts, `board_core_init`, Hazard3's counters shut among it,
since `board_init` reaches the first core's alone.
On ARM a core knows which it is from a register of the board's, `CORE_ID_ADDR`, as RISC-V reads `mhartid`,
and `start.S` finds by it the core's stack and the frame it saves a trap into.

**What it costs.**
A trap takes and gives the lock, two atomic operations, and stores `in_user` twice;
on ARM it reads which core it is from the board's register a few times too,
and the ticket's borrowed region costs about a dozen accesses to the system control space.
A core that changes what another runs pays an interrupt to it,
and a call that takes what another core runs waits for that core's trap to begin.
