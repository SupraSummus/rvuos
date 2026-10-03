# rvuos design: open decisions

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Open decisions

These are recorded here so they are not implicit in the code.
Each has a working default the code follows
until the maintainer decides otherwise.

1. **Target hardware.**
   Decision: QEMU `virt` for RV32 stays the development target,
   using only machine and user mode,
   the ESP32-C6 is the first real board and RP2350 the second; see "Boards".
   QEMU implements 16 PMP entries;
   `PMP_MAX_ENTRIES` caps how many the kernel uses,
   so the design can be exercised with a budget of 8 or fewer.
   The ESP32-C6 is a single RV32IMAC core with user mode, PMP and 512 KiB of SRAM,
   and it runs the demo root task from RAM, loaded by its ROM.
   Booting from flash is not done yet;
   what it needs from Espressif's second-stage bootloader,
   and whether execute-in-place goes through a cache the kernel must control,
   are listed in `TODO.md`.
   RP2350 runs from RAM too, loaded by its bootrom.
   Cores without PMP, GD32VF103 among them, cannot run rvuos.
   QEMU's mps2-an385, a Cortex-M3, is the development target of ARMv7-M,
   and its mps2-an521, two Cortex-M33s, that of ARMv8-M; see open decision 23.
   QEMU `virt` runs two harts too, `make CORES=2`, and RP2350 both cores of either kind; see open decision 24.

2. **Implementation language.**
   Working default: C, compiled with clang for `riscv32-unknown-elf`,
   `-march=rv32imac -mabi=ilp32`, or for `thumbv7m-none-eabi` on ARM, linked with lld,
   with a thin assembly entry and no libc.
   Rust was considered and rejected for now:
   the kernel is small, mostly CSR and fixed-layout work
   that would be `unsafe` anyway,
   and C keeps the toolchain and the userspace story simple.
   Discipline replaces the type system where it matters:
   `-Wall -Wextra -Werror`, no pointer arguments in system calls,
   and every capability check in one place.
3. **Position-independent processes versus fixed link addresses.**
   Working default: fixed addresses chosen by the image builder,
   because it needs no runtime relocation.
   Revisit when the root task starts loading processes dynamically.
4. **Kernel PMP entries.**
   The kernel does not lock entries and runs in machine mode,
   so it needs no PMP entries for itself.
   Working default: reserve none for the kernel,
   and one for the fence on a core whose hardwired entries grant user mode a right, as RP2350's do;
   see "Physical Memory Protection".
   Revisit if Smepmp support is added
   to protect the kernel from its own stray pointers.
5. **Synchronous endpoints.**
   Working default: none.
   Shared memory and notifications carry everything,
   for the reasons in "Communication and synchronisation".
   Revisit when a real server with many clients exists
   and the four system calls per round trip,
   or the missing guarantee that one answer reaches one asker,
   can be measured rather than guessed.
   An `Endpoint` object would not replace notifications:
   interrupts need those either way.

6. **Badged notification capabilities.**
   Working default: the signalling thread names the bits,
   so a client's identity to a server is a convention
   between the two of them and not something the kernel enforces.
   The alternative is seL4's: the bits a capability may set
   live in the capability, and copying it can only narrow them,
   exactly as rights narrow.
   That makes a client unable to speak for another
   and fits in the second word of an object capability, which is unused.
   It needs a way to set the mask when a capability is handed out;
   `OP_CAP_DERIVE` is the natural place, with the mask in `a4`,
   which the dispatcher already carries.
   Decide before a server with mutually distrusting clients exists.

7. **What a pool destroy gives back, and to whom.**
   Decided, by the derivation tree, and since decision 14 by the Untyped:
   a pool's memory goes back to the Untyped it was retyped from,
   so to the lender's, never to a borrower's.
   A lender that wants it back from a living borrower
   revokes below its own Untyped, which destroys the borrower's pools.

8. **TOR or NAPOT.**
   Decided: NAPOT only; see "Physical Memory Protection".
   Before, a region was any range on the grain, written as TOR entries.
   That saved RAM to rounding,
   but a region cost one or two entries depending on its neighbours,
   two ranges could overlap in part,
   and cores without TOR, RP2350 among them, were out.
   RP2350's PMP is no longer in the way,
   its hardwired entries cost the fence, one entry,
   and its `mtval` reads zero; see "Boards".

9. **Scheduling policy beyond units.**
   Decided in part: units of time earned by one thread each, an account per thread they fill,
   and spare time only by `RIGHT_X`; see "Scheduling" and open decision 18.
   Working default for the rest: no priorities;
   units promise a thread their part of the processor, give or take its account,
   and a thread with time a turn within `TIME_UNITS` ticks, and no more.
   The earlier plan was fixed priorities on `Thread`,
   set by whoever holds the capability and capped by the setter's own,
   round-robin within a priority, no priority inheritance;
   with units they could be a right of the `Time` capability a thread is bound through, as spare time is.
   The alternative is a scheduler in userspace
   that decides which threads are runnable at all.
   It stops a thread by taking back the units it lent it and starts it by binding it again,
   and has a tick of its own in a timer line, the shape every device interrupt takes.
   What it cannot do is choose between two runnable threads
   for less than a system call per switch.
   A period per thread, as seL4's scheduling contexts have, would bound latency more tightly,
   at the price of a queue of refills sorted by time.
   Decide when a workload needs one thread to run before another,
   and taking turns measurably fails it.

10. **Yield.**
    Working default: none.
    `OP_DEBUG_TICK` is the tick on request,
    so a yield would be that under a name that invites spinning,
    and a spinning thread is always runnable and keeps the machine from `wfi`,
    while a waiting thread says what it waits for.
    The cases that ask for one have other answers:
    a lock whose holder was preempted is a short spin on `lr`/`sc`
    and then a wait on a notification the unlocker signals,
    and polling a device ends with an `Irq`.
    A sleep bounded from above, "no longer than", was considered as the same operation;
    it would have to round the delay down and end in a spin of up to one tick.
    Revisit when a workload measures the cost of a slice burnt
    behind a preempted lock holder.

11. **Sharing an interrupt line.**
    Working default: one `Irq` per line, and a second bind fails.
    Boards wire several devices to one line,
    and then either one driver serves them all
    or several `Irq`s hang on the line,
    every one is signalled when it fires,
    and the line stays masked until every one is armed again,
    which is a count the kernel would keep per line.
    The `IrqLine` capability would then be copied rather than carved,
    and `KERR_OVERLAP` on a bind would go.
    Signalling every `Irq` on the line is a step each,
    and goal 4 allows it only if their number per line has a bound.
    Decide when a board with a shared line is worth that count.

12. **The log across a reset.**
    Working default: the kernel resets the log's header at boot
    and leaves the ring's bytes alone,
    so on a board whose boot does not clear RAM
    the previous run's last words are there to be read,
    but nothing says how many of them are valid or where they end.
    A logger that reports a crash after a reset needs the old head,
    which means a header the kernel does not reset,
    a marker that tells a cold boot from a warm one,
    and a checksum against RAM that came up as noise.
    The kernel could also keep the old ring as it is
    and start a fresh one behind it, at twice the memory.
    Decide with the first board, whose reset behaviour decides what survives.
    The ESP32-C6 parks on a halt and keeps RAM,
    but what its ROM overwrites on the way back in is not measured yet;
    bytes one run wrote at `0x40838000` were still there after the reset and the next load.

13. **Access permission management on the ESP32-C6.**
    Working default: the kernel turns the APM filters off at boot,
    because they refuse the CPU in user mode every peripheral,
    and PMP alone confines a process; see "Boards".
    PMP says nothing about DMA:
    a peripheral that is a bus master reads and writes where its driver points it,
    so a driver granted such a device can reach any RAM.
    The APM units are what could confine a master,
    by region and by security mode,
    which would make them the kernel's to program when a DMA driver asks.
    Decide when the first driver of a DMA-capable peripheral exists.

14. **Memory authority: `Untyped` and `Frame`.**
    Decided: seL4's split; see "Kernel pools and revocation".
    Before, one `Region` type could be installed and could become a pool,
    and overlap checks over every pool and every process kept the two apart,
    which goal 4 rules out;
    a peer holding a copy of a region could also make it inert by pooling it.
    The questions the proposal left are decided so:
    - deleting the Pool capability a retype returned neither refuses nor destroys,
      since the retype's own node lies in the pool's descriptor;
    - a revoke below an Untyped refuses when the caller or its table lives
      in the memory it would destroy, a test by address;
    - a frame carries the rights of the Untyped it was made of;
    - a stopped destroy keeps its progress in the pool:
      the objects go newest first and the used mark with them.

    The price is seL4's: the root task retypes before it installs anything.
    The watermark a retype took each block past, which seL4 has too, went for halves; see decision 20.
    Besides, a grant made through a dying table now outlives it,
    and revoking below a line capability no longer takes an `Irq` bound from it;
    see "Interrupts".

15. **What a process's table slot could do.**
    Working default: the slot is filled once, at allocation, and its rights mean nothing.
    - **Replacing the table.**
      An operation on a `Process` could refill the slot;
      every thread would see the new table from its next call.
      With flat slot numbers a full table then grows by a table's worth of calls:
      a larger table, every capability moved over with `OP_CAP_MOVE`,
      which a copy could not do, since it leaves behind what was derived from its source,
      and the slot refilled.
    - **Nested tables.**
      A slot number could be a path, read from the most significant bit:
      each `CapTable` capability skips a guard, kept in its unused second word,
      then takes the bits that index its table.
      A table grows under a new two-slot root, the old table in the first slot
      with a guard one bit shorter, so every old name keeps its meaning.
      At least one bit per level bounds a lookup at 32 steps.
      Open: whether a name that ends at a slot holding a table means the slot
      or descends into the table; seL4 passes a depth.
    - **Rights.**
      A slot without `RIGHT_W` could seal the table against its own process.

    Decide with the first program whose creator cannot size its table in advance.

16. **Drift of a periodic timer.**
    Decided: `IRQ_SET_PERIOD` counts a timer line's next deadline from its last one,
    and the tick keeps pace with the counter; see "Time".
    Before, a periodic task drifted by each wake's lateness,
    and each tick was set a period after its handling, a few percent slow under QEMU.

17. **CPU time per thread.**
    Working default: none; the clock gives the machine's time, which includes other threads' slices.
    Only the kernel sees a switch, so only it can count,
    and it reads the counter at every change of turn already, to charge the turn;
    what is left is adding it to the thread leaving, eight bytes per thread.
    Reading a thread's time would be an operation on the clock that names a thread,
    because a thread that can read its own time has a clock.
    A read of the counter by system call could join it,
    for a board whose counter no region can show, or to spare a region slot.
    Decide with the first program that needs it.

18. **What a thread's account counts, and when it has time.**
    Working default: the counts a turn ran, time again at a tick, and spare time free.
    The account counts in parts of a count, charged at each change of turn and at each tick,
    but a turn ends on the tick grid, which it never outruns, since it began with a tick in the account;
    under tracing the clock is the tick count, so the host, whose clock is the ticks the records move, decides as QEMU does.
    Time again at a tick, a turn's worth, bounds how long a thread with units waits for its account:
    held to an eighth, a thread runs a tick in every eight.
    The shares had time again only once their account was full,
    which kept a capped share's idle time in one stretch,
    13 ticks of running and about a hundred idle at an eighth,
    where a tick in every eight wakes an otherwise idle core 125 times a second.
    Spare time costs nothing, so an account never goes below zero.
    Before, a tick cost whoever it found running a whole tick, so a thread that waited just before every tick paid nothing.
    The alternatives are time while the account holds anything, with turns that end mid-tick where it drains,
    which would wake the core several times a tick for a thread held to its units;
    a higher threshold, per thread, that trades latency for idle stretches;
    and spare time charged as a debt.
    Decide when a board's sleep states, or a workload's latency, make the difference measurable.

19. **A right to destroy a pool.**
    Decided: there is none; the holder of a pool's Untyped destroys it, and a Pool capability only allocates.
    So lending a pool lends allocation, and lending an Untyped lends the power to destroy what is made of it.
    Before, `OP_POOL_DESTROY` destroyed a pool through a Pool capability,
    so a borrower could destroy every process living in the pool, the root task in the boot pool among them.
    Since decision 20 a pool is the whole of its Untyped, so the operation did what that revoke does.
    The price is a slot per pool for a program that gives pools back one by one,
    and the boot pool's block as a boot capability of its own, `BOOT_CAP_POOL_RAM`.

20. **Halves instead of a watermark.**
    Decided: an Untyped makes one thing of the whole of its memory, a frame, a pool, its two halves
    or a derived Untyped, and nothing while any of those is left;
    see "Kernel pools and revocation".
    Before, a retype took the next block of a given size past a watermark,
    so the kernel chose where each block lay, alignment left gaps,
    and a block given back waited for everything else its Untyped had made.
    Now the program chooses by the half it takes, and a block given back is free again at once.
    The price is the program's: its free memory is capabilities, one a block, in a table of fixed size,
    and a small block out of a large one is a split per halving.
    Still open: no operation joins two free halves whose parent was deleted;
    they join only when their grandparent is free again.
    Decide when a program's free list wants it.

21. **What a fault tells.**
    Decided: `OP_THREAD_FAULT` returns what the log says,
    and `OP_THREAD_READ_REG` and `OP_THREAD_WRITE_REG` reach a stopped thread's registers; see "Faults".
    Before, only the log said why, which serves a watcher that restarts or ends the thread,
    but not one that maps regions on demand, which needs the address,
    nor one that emulates an instruction, which needs the program counter.
    They came from a tracer, not in the tree, that runs Espressif's Wi-Fi PHY library in a process that maps no device
    and carries out each register access it faults at.

22. **User-mode CSRs nothing shuts.**
    Working default: on the ESP32-C6 the kernel sets them back at every change of process; see "Boards".
    So a program cannot keep the counter or the dedicated GPIO across another process's turn;
    saving and restoring them with the process would, at some forty bytes a process.
    The counter counts the kernel's cycles too,
    so a process that starts it still sees how long the kernel took over an interrupt while no other process ran.
    The dedicated GPIO reaches a pad only through the GPIO matrix,
    and a pad routed to it is every process's whatever the kernel sets back;
    no process holds the matrix today, so the first GPIO driver decides who may route one.
    Decide with the first program that wants the counter or the dedicated GPIO kept.

23. **A second architecture.**
    Decided: ARMv7-M, with the kernel's objects, operations and demo unchanged, and ARMv8-M's Mainline after it;
    see "Architectures".
    The model needs every region to be a block, which PMP's NAPOT, PMSAv7 and PMSAv8 each take as one entry.
    Open:
    - **Execute only.** ARM refuses a region that may execute but not read, which RISC-V grants;
      refusing it on both would make the ABI one, and change what the seeds and the corpus mean.
    - **The registers a stacking fault loses.** A thread whose sp points where it may not write
      loses r0 to r3, r12, lr, pc and xpsr as it traps, so its watcher can start it afresh but not resume it;
      the status `OP_THREAD_FAULT` returns says so, and `OP_THREAD_READ_REG` reads what the last trap left there.
      A kernel that kept the last frame it gave could resume it where its last trap left it.
    - **What a watcher cannot set.** `OP_THREAD_WRITE_REG` reaches the general registers and the pc, not the flags,
      and a pc written leaves an IT block as a branch does,
      so a watcher cannot emulate an instruction that sets the flags, nor step a thread past one inside an IT block.
      A register for the flags, and a pc write that advances the IT state, would.
    - **The floating point unit.** The Cortex-M3 has none; a core with one stacks its registers lazily,
      and a process switch then owes them a save the kernel does not make.
      The Cortex-M33 has one, which the kernel shuts, so a floating-point instruction faults its thread.
    - **The name.** rvuos says RISC-V, which the kernel no longer is alone.
    Decide each with the first board or program that needs it.

24. **More than one core.**
    Decided: one lock around the kernel, a thread on the core its units are of, and units numbered core by core;
    see "Cores".
    QEMU `virt` runs two harts with `make CORES=2`, which `make smp-test` boots and `fuzz-smp2` models,
    mps2-an521 its two Cortex-M33s with `make BOARD=mps2-an521 CORES=2`, which `make arm-test` boots,
    and RP2350 both cores of either kind with `make BOARD=rp2350 CORES=2`, which only the board's own runs boot;
    every other board runs one.
    Before, the default was one core, with what a second would need gathered in `struct core`.
    The questions it left are decided so:
    - **Exclusion.** A ticket lock, taken as a trap begins and given up as it returns, and while a core stalls;
      a walk stops for a core waiting on it as for an interrupt, so the wait is at most a step of each other core.
      On ARM the ticket's exclusive pair needs memory marked Shareable, which a region borrowed for the take gives it.
    - **A region taken from a process another core runs**, and a thread stopped or destroyed while another core runs it:
      the call interrupts that core and waits until its trap has begun, and the core loads its regions again
      as it takes the lock, or hands its taken thread's turn on.
    - **Wakes.** A core that changes what another runs marks the other's timer stale,
      and interrupts it when it idles or runs on with its timer set past the next tick.
    - **The units.** `TIME_UNITS` of each core, so that which core a thread runs on is held as its units are;
      a program learns how many cores there are by the units it can carve.
    - **A controller of each core's own**, as ARM's NVIC and RP2350's Hazard3's are: the first core's alone enables a device line,
      and a call on another that arms or disarms one leaves the change for the first to make as it next takes the lock,
      interrupting it for that; the line the cores interrupt each other on is the kernel's, and no `Irq` binds it.

    Open:
    - **Device lines.** The controller forwards every line to the first core,
      so a driver on another costs an interrupt between the cores for each of its interrupts.
      A line could go to the core its `Irq`'s waiter runs on, which the bind or the arm would choose.
    - **Spare time across cores.** A thread on spare time runs on its own core alone, however idle another is;
      a queue of spare time for the whole machine would let any idle core take it, at the price of a thread moving with every turn.
    Decide each with the first board or workload that needs it.
25. **Devices for drivers.**
    Decided: the boot grants a `Frame` over each device the board lists, from `BOOT_CAP_DEVICES` up,
    which the root task carves and lends as it does RAM; see "Boards".
    So `BOOT_CAP_COUNT` is the board's, and the root task's table grows with it,
    leaving 46 slots of its own on every board.
    Before, `OP_DEBUG_FRAME` made such frames through the `Debug` capability,
    so whoever handed devices out could halt the machine too.
    It kept the boot table the same on every board for the seeds and the corpus,
    but QEMU lists no devices, so theirs stays the same anyway.
    Rights on `Debug` that split its frames from its halt would have left one capability reaching every listed device,
    where a frame per device lends each on its own.
