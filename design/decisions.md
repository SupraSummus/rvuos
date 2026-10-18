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
   The Wi-Fi system's network process is the first such server, with two clients in processes of their own:
   a round trip through a client costs it 37 to 52 microseconds on RP2350 at 150 MHz,
   against 4 to 7 milliseconds for the radio's,
   and a channel for each client, from a hub, answers each asker on its own channel,
   so neither cost has shown yet; see `user/wifi/NOTES.md`.
   A client of several threads asking at once, or a server whose round trips are its time, would show them.
   `make bench` measures the round trip alone:
   about 4,500 cycles between two processes on RP2350's Hazard3 and 5,900 on the ESP32-C6,
   of which the four calls take 2,000 and 2,200,
   and loading the other process's regions about 400 cycles a switch on Hazard3 and 1,000 on the ESP32-C6,
   while it wrote every entry the core has; it writes those the two images use now, not yet measured on either.
   `make lab` shows what an endpoint would carry besides the message, `user/lab/NOTES.md`:
   a client that waits lends its time to nobody, so it goes to spare time while it waits, not to its server,
   and a server cannot tell a client that pays for its asks from one that does not.
   A client that names its server's thread as it waits lends it its time, decision 27,
   and beside a process that spins is answered as fast as alone, where one that lends nothing waits about a tick at the median;
   the server runs on the time of the client that lent it longest, though, not of the one whose ask it serves.
   An `Endpoint` object would not replace notifications:
   interrupts need those either way.

6. **Badged notification capabilities.**
   Decided: a `Notification` capability carries the bits it may signal, in the second word of its slot,
   and `OP_NOTIFY_CARVE` makes one that may signal fewer, below it, as a frame is carved;
   see "Communication and synchronisation".
   A signal sets only the bits it names that its capability may,
   and an `Irq` or a watch only those of the capability it was bound or set with,
   so a client given its own bit cannot speak for another, whatever it names.
   Before, the signalling thread named the bits, and a client's identity to a server was a convention between the two.
   The bits are a mask rather than seL4's badge, which the signal sets whatever it names:
   a mask of one bit is a badge, since naming every bit signals just that one,
   and a capability that may signal every bit still names which.
   `OP_CAP_DERIVE` with the mask in `a4` was the other way to hand out fewer bits;
   a replay record carried three arguments then, and a carve is how every other capability names less than its source.

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
   Decided: earliest deadline first on the run queue, round-robin on spare time;
   decision 39 says what a deadline is and what it promises.
   Units of time are earned by one thread's place each, an account per place they fill,
   and spare time only by `RIGHT_X`; see "Scheduling" and open decision 18.
   Before, the oldest thread on the run queue had the next turn, and turns went round on the tick.
   Beside a process that spins, a round trip between two processes waited for the spinner's turns,
   about 263,000 cycles on RP2350 where the round alone costs about 4,500, `make bench`'s `contended-worst`;
   `make lab` showed the same for an ask with work behind it,
   and that twice the units for the client or for the server moved its latency by less than a fiftieth,
   since a server woken by an ask waited on the run queue behind the spinner's turn.
   What priority inheritance does for priorities, a waiter that names whom it waits for does for accounts, decision 27:
   it lends that thread its time, the oldest lender's, and on through lenders with no time of their own, decision 39.
   Weighed and left, beside what decision 39 weighs:
   - **Fixed priorities on `Thread`**, the first plan:
     set by whoever holds the capability and capped by the setter's own,
     round-robin within a priority, no priority inheritance.
   - **A scheduler in userspace** that decides which threads are runnable at all.
     It stops a thread by taking back the units it lent it and starts it by binding it again,
     or with `OP_THREAD_STOP` and `OP_THREAD_RESUME`, which also take a waiting thread off its notification and back to its wait, decision 32,
     and has a tick of its own in a timer line, the shape every device interrupt takes.
     What it cannot do is choose between two runnable threads
     for less than a system call per switch.
     It stays open to a program over the deadlines, as it was over the turns.
   - **A period per thread**, as seL4's scheduling contexts have,
     at the price of a queue of refills sorted by time, which decision 38 weighs.
   - **Priorities that preempt on wake**, proposed before decision 39.
     A thread with time at a higher level than its core's running thread
     takes the processor as the trap that made it ready ends, a device's interrupt among them,
     and from another core through the interrupt that core already sends.
     That is the rule for a turn on spare time, with two levels made `PRIORITIES`:
     a run queue per level and a word of the queues that hold a place, so the next thread is a count of leading zeros away,
     and turns on the tick within a level.
     It takes besides:
     - **A budget.**
       A thread has its level only while its account holds a tick,
       so a spinner at the top takes at most its units' part and its account,
       and what an account may hold, `ACCOUNT_TICKS`, becomes what a level may suffer from those above; decision 18.
     - **A right.**
       The level is a right of the `Time` capability a thread is bound through, a range a carve narrows,
       as seL4's maximum controlled priority is.
     - **Lent time at its payer's level**, so a lock's holder and a server run at their waiter's.
       The payer is still the oldest lender, not the highest;
       a ring of lenders per level would find the highest without a walk,
       but a chain through a holder that waits in turn stays userspace's.
     - **Wakes by level.**
       A signal wakes the payer of the signalling core's turn, or else the oldest waiter;
       the highest would cost each notification a pair of links per level, or a walk.
     - **A short kernel step.**
       The longest step the kernel takes without looking for an interrupt is the floor of every wait;
       zeroing a table takes longer than a tick at 10 kHz, and a pool destroy stops only for a pending interrupt, `TODO.md`.
     - **Deadlines in counts.**
       Timer lines fire on the tick grid, so a periodic thread at the top still starts up to a tick late.
     - **Signals that preempt.**
       A signal to a waiter of a higher level takes the processor from the signaller,
       as only a signal from a turn on spare time, or one waking the turn's payer, does,
       so a signal and a wait in one call, `TODO.md`, would then save switches as well as traps.
     It would promise the highest ready level a start within the kernel's longest step of the trap that made it ready,
     and a lower level a response-time analysis over the units and accounts above it.
     A level is a promise relative to every thread above it,
     where a deadline from the account is a promise of each thread's own; decision 39.

10. **Yield.**
    Working default: none.
    `OP_TEST_TICK` is the tick on request,
    so a yield would be that under a name that invites spinning,
    and a spinning thread is always runnable and keeps the machine from `wfi`,
    while a waiting thread says what it waits for.
    The cases that ask for one have other answers:
    a lock whose holder was preempted is a wait on a notification the unlocker signals, as in `user/lib/lock.h`,
    after a short spin on `lr`/`sc` only where the holder may run on another core,
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
    Decided: the kernel turns the APM filters off at boot,
    and a driver granted a device that is a bus master is trusted with all RAM.
    The filters, as the ROM leaves them, refuse the CPU in user mode every peripheral,
    and PMP alone confines a process; see "Boards".
    PMP says nothing about DMA:
    a peripheral that is a bus master reads and writes where its driver points it.
    The ESP32-C6 lists its modem whole, its two MACs among them,
    so whoever holds the modem's frame reaches any RAM, the kernel's too,
    and the root task gives it only to a driver it trusts as it trusts itself.

    The APM units could confine the MACs, and were measured to on the board:
    with the CPU's user mode in REE0, given everything by a region of each unit, since PMP confines it,
    the modem in REE1, given read and write over the driver's block of RAM alone,
    and every other master in REE2, given nothing,
    the Wi-Fi system passed its every check, and the units refused nothing of its traffic.
    The CPU's user mode always takes one of the three REE modes,
    so the chip can confine bus masters only to one set of windows they all share, not each driver to its own,
    and the windows would take a block of RAM the board names, granted as a frame of its own,
    or an operation that binds a frame to a master, which deleting the frame undoes.
    Trust is simpler than either.
    A driver's connections of `user/lib/bulk.h` rest on it: the driver hands its device only buffers its end names,
    and lets go of a closed connection only once its device no longer uses them; see "Bulk data".

14. **Memory authority: `Untyped` and `Frame`.**
    Decided: seL4's split; see "Kernel pools and revocation".
    Before, one `Region` type could be installed and could become a pool,
    and overlap checks over every pool and every process kept the two apart,
    which goal 4 rules out;
    a peer holding a copy of a region could also make it inert by pooling it.
    The questions the proposal left are decided so:
    - deleting the Pool capability a retype returned neither refuses nor destroys,
      since the retype's own node lies in the pool's descriptor;
    - a revoke below an Untyped refuses when the caller or a table its process mounts lives
      in the memory it would destroy, a test by address;
    - a frame carries the rights of the Untyped it was made of;
    - a stopped destroy keeps its progress in the pool:
      the objects go newest first and the used mark with them.

    The price is seL4's: the root task retypes before it installs anything.
    The watermark a retype took each block past, which seL4 has too, went for halves; see decision 20.
    Besides, a grant made through a dying table now outlives it,
    and revoking below a line capability no longer takes an `Irq` bound from it;
    see "Interrupts".

15. **A process's tables.**
    Decided: two table slots in the `Process`, `OP_PROCESS_MOUNT` and `OP_PROCESS_UNMOUNT`,
    and slot numbers `SLOT_IN(table slot, index)`; see "A process's tables".
    Before, one slot was filled at allocation for good,
    so a process that outgrew its table, or lived in memory its table was to leave, needed a successor.
    - **Replacing the table**, one slot refilled, keeps numbers flat,
      but the replacement is named through capabilities in the old table,
      so those to the process and to the new table are copied rather than moved and stay behind,
      and what has moved is out of reach until it.
    - **Nested tables**, a number read from the top bits with a guard at each level as in seL4,
      grow without a move, but cost each lookup a walk as deep as the tree,
      and a rule for whether a number that ends at a table means the slot or the table, which seL4 passes a depth for.
    - **More table slots**, four, would let a process keep a table of its own beside a shared one and still move either.
      Each costs every process sixteen bytes and nothing asks for it yet;
      the bits above the index leave room for them without changing what a number means.

    Open: a table slot carries the rights of the capability it was filled from, which has `RIGHT_W`, and they mean nothing more;
    one without `RIGHT_W` could seal a table that a group of processes shares against all of them.
    Open: `RIGHT_W` on a table lets its holder use what the table holds, by making a process of it or mounting it,
    so a table others may only put capabilities into would need a right of its own.

16. **Drift of a periodic timer.**
    Decided: `IRQ_SET_PERIOD` counts a timer line's next deadline from its last one,
    and the tick keeps pace with the counter; see "Time".
    Before, a periodic task drifted by each wake's lateness,
    and each tick was set a period after its handling, a few percent slow under QEMU.

17. **CPU time and wakes per thread.**
    Working default: none; the clock gives the machine's time, which includes other threads' slices.
    Only the kernel sees a switch, so only it can count,
    and it reads the counter at every change of turn already, to charge the turn;
    what is left is adding it to the thread leaving, eight bytes per thread.
    Reading a thread's time would be an operation on the clock that names a thread,
    because a thread that can read its own time has a clock.
    A read of the counter by system call could join it,
    for a board whose counter no region can show, or to spare a region slot.
    - **Turns and wakes.**
      Beside the time, the kernel could count each thread's turns and the waits that parked it, four bytes each,
      on paths that change the thread already.
      A mechanism's cost shows in the wakes it takes for its work:
      `lib/bulk.h`'s first lenders woke half again as often as a channel's ends for the same frames,
      which only a count the Wi-Fi driver kept of itself found.
      The counts are not time, so a page of a thread's description could tell them, `OP_CAP_INFO`, decision 33,
      to a holder with `RIGHT_W`, as a stopped thread's registers are, since they tell when the thread ran.
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
    `OP_THREAD_STOP` makes any thread stopped, so a live thread's registers are read by a stop, a read and a resume, decision 32.
    Before, only the log said why, which serves a watcher that restarts or ends the thread,
    but not one that maps regions on demand, which needs the address,
    nor one that emulates an instruction, which needs the program counter.
    They came from a tracer, not in the tree, that runs Espressif's Wi-Fi PHY library in a process that maps no device
    and carries out each register access it faults at.

22. **User-mode CSRs nothing shuts.**
    Decided for the ESP32-C6's performance counter: a process keeps it, in twelve bytes of the process,
    as its threads keep their registers, and finds it stopped at zero until it starts it; see "Boards".
    The ROM's `ets_delay_us` counts on it, and Espressif's Wi-Fi libraries call it,
    so a counter set back whenever another process ran would never end a delay that process cut into.
    A process holds one copy, which is enough on the chip's single core.
    Kept so, the counter stops while another process runs,
    so a delay lasts at least as long as asked, and a process times its own turns and nobody else's.
    It counts the kernel's cycles too,
    so a process that starts it still sees how long the kernel took over an interrupt while no other process ran.
    Working default for the rest: the kernel sets them back at every change of process,
    so a program cannot keep the dedicated GPIO across another process's turn.
    The dedicated GPIO reaches a pad only through the GPIO matrix,
    and a pad routed to it is every process's whatever the kernel sets back;
    the board grants the matrix, so whoever holds it decides whether a pad is so shared,
    and the Wi-Fi system's root task, which sets the XIAO's RF switch, routes none.
    Decide with the first program that wants the dedicated GPIO kept.

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
      The Cortex-M33 has one, which the kernel shuts, so a floating-point instruction faults its thread;
      the nRF52840's Cortex-M4 has one too, which reset leaves shut and the kernel leaves so.
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
      and interrupts it when it idles, runs on with its timer set past the next tick, or runs a turn on spare time a thread with time now waits for.
    - **The units.** `TIME_UNITS` of each core, so that which core a thread runs on is held as its units are;
      a program learns how many cores there are from `BOOT_CAP_TIME`'s count, which its description tells.
    - **A controller of each core's own**, as ARM's NVIC and RP2350's Hazard3's are: the first core's alone enables a device line,
      and a call on another that arms or disarms one leaves the change for the first to make as it next takes the lock,
      interrupting it for that; the line the cores interrupt each other on is the kernel's, and no `Irq` binds it.
    - **A thread on several cores.**
      A thread is made with places, from one to `CORES`, decision 35, each holding the time of one core; see "Cores".
      A bind fills the place for its units' core, or an empty one, `KERR_LIMIT` with none:
      the program names the core, by its units, and never a place.
      A place empties only as a revoke above its units takes them;
      a bind of none without `RIGHT_X` keeps it, bound to nothing, so that its thread runs there on lent time alone, decision 27.
      A ready thread waits on the queues of every core it holds time of, and a turn on one takes it off the others'.
      Units of a core promise their part of it to a thread that holds time of several only while it runs on none of the others.
      Weighed and left: a queue of spare time for the whole machine, any idle core taking a thread on spare time,
      which is a policy in the kernel placing threads; here which cores a thread may run on is authority, held as units are.

    Open:
    - **Device lines.** The controller forwards every line to the first core,
      so a driver on another costs an interrupt between the cores for each of its interrupts.
      A line could go to the core its `Irq`'s waiter runs on, which the bind or the arm would choose;
      a thread that holds time of several cores has no one core, so the arm's would be the one to take.
    Decide it with the first board or workload that needs it.
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
26. **The watchdog in the kernel.**
    Working default: the kernel keeps a deadline on its own tick, which `OP_CLOCK_WATCHDOG` feeds, and halts at it;
    see "The watchdog".
    A watchdog is a device, every other of which is a driver's, and userspace could do the same:
    a supervisor with a timer line halts the machine through `Halt` when nobody fed it in time,
    and a driver feeds the board's own watchdog through a frame, which catches a kernel that stops, since no process runs then.
    RP2350's would be listed as its first 32 bytes, which hold the count and stop short of the boot vector.
    That costs the boot's BOOTSEL watchdog shared with a driver, and a halt that waits for the supervisor's turn.
    Decide with the first system meant to run unattended.
    The Wi-Fi system, with `run=0`, is the first:
    its root task checks every child each second and deals with one that did not answer itself,
    so the watchdog it then feeds watches only the root task and the kernel.
    Under the alternative the root task would be the supervisor already, and feed the chip's watchdog through a frame.
27. **Lent time, and no mutex in the kernel.**
    Working default, an experiment: a waiter lends a thread it names its time, `OP_NOTIFY_LEND`,
    through a capability to it with `RIGHT_X`, and a signal that wakes the lender hands it its turn back;
    see "Communication and synchronisation" and "Scheduling".
    Before, a `Mutex` the kernel knew the holder of, handed to its oldest waiter, whose waiters could lend the holder their time,
    and before that a lock was userspace's alone, `user/lib/lock.h`.
    In `make lab` a thread with a quarter of the core waits for a lock held by one with a thirty-second, `user/lab/NOTES.md`:
    beside a process that spins, lending cuts the wait from about three ticks at p90 to a fraction of one,
    on the mutex and on the lock of `user/lib/lock.h` that names its holder alike,
    and the mutex without lending made a convoy.
    The lock costs a hold nobody else wants no call, where the mutex cost two,
    and lending costs every switch a few per cent; see `make bench`.
    The same wait lends a server its client's time, decision 5.
    What went with the mutex: a hand-over in order to waiters that lend nothing, which beside a busy process is the convoy,
    and the mutexes a destroyed holder gave its oldest waiters, which were not told; a lender is told now.
    Open: lending across cores, by running the borrower on its lender's core;
    borrowing from more than one lender at once, or through a borrower that waits or lends in turn, walks that goal 4 rules out;
    whether a server should run on the time of the client it serves rather than of its oldest lender, decision 5;
    whether time is a thing of its own, an account a thread's place on a core holds rather than one the place is, decision 35,
    which a lender could hand over whole, as seL4 donates its scheduling contexts,
    and with it whether a thread may hold two times of one core, its own and one lent, decision 24;
    and a lock fair to takers that lend nothing,
    which userspace can build with a bit that asks the giver to hand the lock on, as Linux's mutex has.
    Decide with the first workload that shares a lock between threads of unequal time, or a server between clients that pay unequally.
28. **What the ESP32-C6's Wi-Fi driver takes from Espressif's libraries.**
    Working default: the driver's own MAC and bring-up are written from what `libpp.a` and `libnet80211.a` do to the device --
    the registers, the values and their order, read by disassembly and by the trace window, see `user/wifi/esp32c6/NOTES.md` --
    and none of their code is copied;
    `libphy.a` stays, linked as Espressif ships it.
    Those libraries are esp32-wifi-lib's and esp-phy-lib's, whose READMEs give the binaries ESP-IDF's license,
    the Apache License 2.0, as its "Object" form.
    The station runs on hostap, under the BSD license, and its SAE on Mbed TLS, under Apache 2.0 or the GPL at the taker's choice;
    this repository's own license is MIT, `LICENSE`.
    esp32c6-open-mac, which sends a beacon from the C6, states no license, so nothing is taken from it.
    Open: what the Apache License asks of a driver written from an Object form's behaviour --
    nothing, as for facts about a device, or a notice --
    and what an image that links `libphy.a`, and the libraries until they go, must carry beside it.
    Decide before the Wi-Fi system's image is handed to anyone.

29. **How tightly a slot packs.**
    Working default: sixteen bytes, three links of 28 bits, every node on sixteen bytes; see "Slot format".
    It costs every object an alignment of sixteen bytes,
    and a `Notification` capability's bits are an aligned block of 1, 2, 4, 8, 16 or 32, not any set;
    a client's own bit is one, and so is every bit.
    Slots take most of a child's objects as `user/lib/` builds them, and an empty slot takes as much as a filled one.
    - **Twenty-four bytes**, a word for each link, as the slot was before:
      any set of bits for a capability, at half as much again for every slot.
    - **Sixteen bytes in a window**, sixteen-bit links counted in eight-byte steps from the base of a 512 KiB window
      every pool lies in, in which QEMU and MPS2 would keep 256 KiB of free RAM, not 4 and 2 MiB;
      a prototype is on the branch `claude/elegant-planck-pjp8ch-slot16`.
      A window ties every pool to where a board's RAM lies, and memory elsewhere could only ever be frames.
    - **Two links**, seL4's list in pre-order, which reads a node's parent off the words of the two:
      only an allocation can then make a node that has children, so a carve, or what a holder derived from what it was given,
      can be taken back only with everything else below the allocation it came from;
      an experiment of it is SupraSummus/rvuos#113.
    - **A rank** in the bits alignment leaves, telling a derivation's parent from its siblings where the words cannot,
      keeps two links but caps a chain of derivations at fifteen, which a program can exhaust,
      and a delete would walk what lay below the node to lower it.

    A smaller slot frees memory only in pools sized to what they hold; see `TODO.md`.
    `OP_POOL_INFO` tells what each object takes as the pool counts it,
    so a program that sizes its pools by asking survives another slot unchanged; `user/lib/` does not ask yet.
    Decide once the system calls settle.

30. **How the IEEE 802.15.4 nodes form a mesh.**
    Working default: RPL, RFC 6550, over 6LoWPAN: a tree towards a root, the node with the most memory,
    each other node keeping only its parents, and no loop at any moment if a node's rank never rises within a version.
    It suits nodes that stay where they are, links that fail now and then, and traffic to and from a border router.
    - **Thread's MLE**, which Matter and the border routers of Apple and Google speak,
      but whose specification comes under a licence for internal use that grants no right to implement it,
      its patents free only to members who certify.
    - **Babel**, RFC 8966, for traffic between any two nodes, at a route to every node kept on each.
    Open: whether working with Thread's devices is worth its licence.
    Decide before the mesh's first routing code.

31. **A stable ABI.**
    Working default: none; the ABI changes with the kernel, and the seeds and the corpus are written anew with it.
    The model holds; two things it lacked were added without breaking anything:
    a stop for a thread, `OP_THREAD_STOP`, decision 32, and introspection, `OP_CAP_INFO` and `OP_POOL_INFO`, decision 33.
    The first change that broke something had dispatch check every operation alike, from one table, `op_shapes`, in the manual's order:
    the slot, the type that takes the operation, the right it needs, and zero in every argument of `a1` to `a3` it does not read
    and every bit of a rights mask past `RIGHT_ALL`, else `KERR_INVALID_ARG`,
    so that an argument unread today may mean something later without changing what a program that left it does;
    a foreign operation on a capability without its right is `KERR_WRONG_TYPE` since, where it was `KERR_NO_RIGHTS`.
    The next made `Debug` three types, `Print`, `Halt` and `Test`,
    so a child that prints holds nothing that halts, and a supervisor that halts, decision 26, nothing that drives the machine.
    The next took the two older descriptions, `OP_FRAME_INFO` and `OP_UNTYPED_INFO`,
    whose wrappers read `OP_CAP_INFO` since, so a program built against them changed only by being built again;
    their codes, 11 and 27, are no operation's.
    The next had the replay's record carry all six arguments the registers do, twenty-eight bytes,
    and the fuzzer's inputs keep 128 records at most, its `-max_len` 3584;
    `a4` to `a6` stay unread and unchecked, `TODO.md` says why.
    The rest below would break a binary built against today's ABI, and each goes in a change of its own:
    the seeds name what they call, `tools/seed.py`,
    so a change rebuilds them and rewrites only those that call an operation whose arguments it changes.
    - **What the test format chose.**
      A replay record carried three arguments, so no operation takes a fourth:
      `OP_IRQ_BIND` makes its object through the `IrqLine` it consumes rather than through the pool,
      `OP_TIME_BIND`'s offset names the core of a bind to no units,
      `OP_POOL_ALLOC`'s `a3` means another thing for each type,
      and routing a device line to a core, decision 24, has no argument left.
      The watchdog went onto the clock, the boot table keeps a test input and a console on every board,
      and execute-only stays open, decision 23, each because the alternative would have renumbered the seeds and the corpus.
      The record carries six now, and the seeds are text, so each may be decided on its merits.
    - **The numbering.**
      Operation codes are flat, in the order the operations came,
      and `Test`'s four operations lie among the rest.
      Codes grouped by type, with the tests' in a range of their own that a stable program never calls,
      would keep `KERR_WRONG_TYPE` and say which calls the ABI promises;
      the describing calls, 41 and 42, came last, and every type takes the first.
    - **Where a new capability goes.**
      `a1` for `OP_CAP_COPY`, `OP_CAP_DERIVE`, `OP_CAP_MOVE`, `OP_CLOCK_FRAME` and `OP_UNTYPED_SPLIT`,
      `a2` for `OP_UNTYPED_RETYPE`, `OP_POOL_ALLOC` and `OP_NOTIFY_CARVE`,
      and `a3` for the other carves and `OP_IRQ_BIND`; one place would do.
    - **The errors.**
      A signal, an arm or a watch that names none of the bits its capability may is `KERR_NO_RIGHTS`, and such a carve `KERR_INVALID_ARG`;
      an Untyped that has made something already is `KERR_NO_MEMORY`, where it is a state;
      `KERR_OVERLAP` is a region, a line already bound and a unit another thread earns;
      and a revoke that ends early because it took the caller's own table returns `KERR_OK`, done or not.
    - **What a halt does.**
      Nothing resets the machine, and what a halt does is the board's.
    - **`Test` on every board.**
      The boot grants it on every board, so any root task may move time and fire lines no device raised.
      Granting it only to the test programs would close that, and refusing its calls untraced, `TODO.md`, most of it.
    - **The boot table.**
      `BOOT_CAP_INPUT` is the test's input on every board.
    - **An `Irq` holds its line by no node**, `TODO.md`, so a revoke takes back a bound line no other way a lender takes back what it lent.
    - **What a fault tells is the architecture's.**
      `OP_THREAD_FAULT` returns `mcause` or the exception number,
      and a breakpoint, which ends a thread kept from its watch, as a stop of itself ends one that holds its own thread,
      arrives as a HardFault under QEMU and a DebugMonitor exception on RP2350;
      a class beside the raw values, an access by load, store or fetch, an illegal or misaligned instruction, a breakpoint, a stacking,
      would serve a watcher on every board.
    - **Copy is the name of the grant its giver cannot revoke.**
      A copy stands beside its source, and a copy of a root is a root,
      so the operation named as the plain one hands out what only a common parent takes back;
      a parent nearly always wants a derivation.
    - **A process's own table asks for a capability to delete from.**
      What makes a capability writes into the caller's tables with no capability,
      but deleting, revoking, copying or deriving there takes a `CapTable` capability with `RIGHT_W` to the caller's own table,
      which also makes a process of the table or mounts it, decision 15;
      a child that only tidies its own slots is given more than that.
    - **The rights a table does not use.**
      Decision 15's two open questions, a sealed table and one others may only put into, would each take a right,
      and `RIGHT_R` and `RIGHT_X` on a `CapTable` mean nothing yet;
      describing another process's slots through a table, decision 33, could give the first its meaning.
    - **What lending promises.**
      `OP_NOTIFY_LEND` is an experiment, decision 27, and its text states the scheduler's steps;
      a stable ABI would promise that the borrower may run on the lender's time while it waits, and leave the rest to policy,
      as it leaves which waiter a signal wakes.
    - **What a stop says.**
      `OP_THREAD_STOP` returns the state it found, `THREAD_WAS_*`, which commits the ABI to the three states;
      a supervisor needs it to know whether it may answer a wait, and the host build to know a thread stands at its call again,
      which it could tell from the pc instead, decision 32.
    - **What an object is made with.**
      `OP_POOL_ALLOC` of a `Thread` took its places, in `a4`, and a bind fills a thread's place for a core rather than move the thread,
      decision 24; of a `Process` it took its region slots, in `a4` too, decision 35;
      each a change that broke what a program built before did.
    - **A version.**
      The root task's thread starts with `a0` zero, which could carry the ABI's version.
    Each is decided on its own merits, and made when it is.
32. **Stopping a thread.**
    Decided: `OP_THREAD_STOP` takes a thread to stopped from whatever it is doing, through a capability to it with `RIGHT_W`,
    with `a1` to `a3` zero, so that they may be given a meaning, and returns the state it found;
    see "Threads" and "Cores".
    A waiting thread leaves its notification and its borrower's ring of lenders and stands at the call it waited in,
    which a resume makes again; the loss of a process stops a waiter the same way, so one rule holds for every stop.
    It keeps its process, watch, units, account and lenders, and nothing is signalled.
    A stop of a thread another core runs marks that core's trap to be dropped, whatever the thread is by then,
    and on ARM finds a frame that trap could not stack, which it reports as the fault it is.
    Weighed and left:
    - **A status instead of the rewind**; "Threads" says why not.
    - **A watch signal on a self-stop.**
      A thread that stops itself could signal its watch, `THREAD_FAULTED` clear, so `OP_THREAD_FAULT`'s `KERR_STATE` told an exit from a fault.
      An exit others should hear is said first by the thread's own means, as `child_report` does,
      and whoever hears it may stop the thread whatever it is doing by then, since a stop of a stopped thread changes nothing;
      a signal would cost a test on the stop and a case for the host's history, and echo a supervisor's own stops on its fault bit.
    - **No result.**
      The host could tell a waiter stopped from its pc, one call back, and a supervisor could read the registers,
      but a supervisor needs to know whether it may answer a wait before it touches one, and the state is no clock.
    - **Ending a paid turn at once.**
      A stopped lender whose time pays for another thread's turn could stop paying as it stops,
      charged up to now, the turn's thread paying for itself from there;
      it would charge another core's turn from this one, against the counter that core counted to,
      and break "whether a turn is paid is decided as it begins and at each tick",
      so it pays to the end of the turn, as a payer another core's signal woke does.
    - **A narrower right.**
      `RIGHT_R` on a thread means nothing yet, and could allow a stop alone,
      so a child could stop itself without the capability that also sets its watch, which `RIGHT_W` is;
      a self-stop needing no capability at all, as a fault needs none, would be ambient authority, against goal 2.
      Today a child kept from its watch ends with a breakpoint, and the library's children hold no capability to their own threads.
      Decide with the first child that should exit cleanly and be kept from its watch.
    - **Waking a stopped borrower's lenders**, a walk only a destroy pays for; see "Communication and synchronisation".
33. **What a description tells.**
    Working default: `OP_CAP_INFO` on any capability, with no right, tells its type and rights and three words by type,
    and `OP_POOL_INFO` on a pool, with no right, what an object of a type takes of it and the room;
    see "Capabilities" and "Kernel pools and revocation", and `include/rvuos/abi.h` for the words.
    Each takes constant time, a load past the slot at most, changes nothing,
    and returns no object's address, no bits a notification holds and not the time.
    The machine's constants come from the capability they belong to:
    the smallest region from a frame, the tick from a clock or a capability to units, the cores from `BOOT_CAP_TIME`'s count,
    and the regions a process may have installed from the process,
    so a program given no clock learns no tick, as one given no units learns no cores.
    The results follow the call's line under tracing, so the replay holds QEMU's answers to the host's.
    - **Pages.**
      `OP_CAP_INFO`'s `a1` is a page, and only page 0 is told, so a later kernel can tell more without a new code:
      a thread's state and core, which go with a stop for a thread, a process's installed regions and mounted tables,
      or the smallest region to the holder of an Untyped alone.
      `OP_POOL_INFO` could be such a page of a pool's description, at the price of arguments that differ by type.
    - **The room without a right.**
      A `KernelPool` capability with no right reads the pool's room, and so what its other holders allocate,
      a channel between them that did not exist before;
      `RIGHT_R` on a pool would close it, at the price of a right for that alone.
    - **What an Untyped made.**
      Its description tells the type of what lies first below it, which once a delete handed things up is one of a mixture,
      and neither the bytes an unnamed pool below it holds nor whether something outside the pool still holds its objects,
      which is the pool's node having no child;
      nor can the Untyped's holder have a capability to that pool again.
    - **Another process's slots.**
      A supervisor or a debugger could describe a slot of a table it holds a capability to, by its index,
      where today a copy goes only into a table, never out of one.
    - **The words left at zero.**
      A thread's, a table's, a process's, and those of `Print`, `Halt` and `Test`;
      what fills them comes with the first program that needs it, or a page.
    Decide as decision 31's changes are.
34. **Copies by the kernel.**
    Decided: none; the kernel moves no data, see "Communication and synchronisation",
    and bulk data is lent where it lies, `user/lib/bulk.h`; see "Bulk data".
    An operation that copies bytes between two frames the caller holds, as Hubris's leases let a server reach a client's memory,
    was weighed: it would let a server take a client's bytes, or give it some, with no region over the client's memory,
    and take them as a snapshot the client cannot change halfway.
    Lending gives both already.
    The lender's memory holds a connection, so a server spends one region on all its borrowers, and a borrower one or two;
    and a server takes a snapshot of what it decides on by copying it into its own memory, at the cost the kernel's copy would have.
    The kernel would have read and written user memory, which it does nowhere,
    held its lock on several cores for as long as a copy takes, which goal 4 would have had to bound,
    and needed the host build and the fuzzer to model the memory it copied.
35. **A count of the hardware's is a limit, not a size.**
    Decided: what an object holds once for each of a thing the hardware has several of
    is sized by the object's creator, as a `CapTable`'s slots are,
    and the hardware's count bounds what the creator may ask.
    A `Process` is made with the region slots it is to have, from one to what the board's protection unit allows,
    and a `Thread` with the cores it may hold time of at once, from one to `CORES`, decision 24;
    `OP_POOL_ALLOC` answers `KERR_INVALID_ARG` past either, and for none.
    So an object takes what its creator asked for, which goal 3 reads as paying for what one makes,
    a program reckons what its objects take from what it needs of them, the same on every board of its architecture,
    and a board short of what it needs refuses it plainly.
    A process's PMP image has an entry for each of its region slots, so an install never runs out of entries,
    and `OP_CAP_INFO` on any process tells the most a process may have, as a frame's tells the smallest region;
    the root task's has `ROOT_REGION_SLOTS`, eight, or that most where it is fewer, as its table has `ROOT_TABLE_SLOTS`:
    sixteen would not leave the two-core demo room in the 4 KiB boot pool.
    Weighed and left:
    - **Sizes by the board's count**, as a process had them first: `OP_POOL_INFO` tells them, but only as the program runs,
      and every object pays for what the board has rather than for what it uses.
    - **An object for each binding**, an installed region or a thread's time on a core an object of its own:
      sizes the same on every board, paid for as used,
      but every binding then has two ends that may each go, a header and links of its own, and a pointer more on a switch.
      A thing that is handed over whole is an object, and a place something is put in is a slot:
      a region slot is a place, and so is a thread's place on a core,
      while the time in that place may yet become a thing of its own, decision 27.
    What a board keeps of a process besides, the ESP32-C6's CSRs, `BOARD_PROCESS_CSRS`, is still the board's.

36. **What real time takes.**
    Working default: rvuos is not a real-time kernel and does not claim to be.
    A program can be shown to meet its deadlines only from promises stated in time rather than in steps, and checked.
    Kept: bounded work and a bounded stack, checked at every link;
    time as authority, with a budget per thread; faults kept to their thread;
    and a wait below a tick, bounded for each thread by its own units and window, through lent time too: decision 39.
    Missing:
    - **A short kernel step.**
      The kernel takes no interrupt, so its longest step is the floor of every latency;
      `TODO.md` lists those that are too long today.
    - **Deadlines finer than the tick** for timer lines, as decision 39 has them for turns.
    - **Bounds in cycles.**
      The loop bounds and call graph the link checks, with each instruction's cost on an in-order core,
      would bound the longest step per board, checked at the link as the stack is.
    - **The latency measured.**
      `contended-worst` holds one wait to a record;
      a test should hold the worst wait on each board to the bound the kernel states.
    Each promise goes into "Properties" as it is kept,
    checked by the self-check where it is state and by a measure on the board where it is time.
    Decide with the first workload whose deadline is shorter than a tick, or than the turns of its core.

37. **Routes between devices.**
    Decided: `RouteLine` and `Route`, as "Routes" says, the nRF52840's PPI the first router;
    the other boards lend no channel, and QEMU's and the host's router wires nothing.
    The first use is the acknowledgement of `user/thread/`'s link, which `TODO.md` lays out:
    the radio's CRCOK starts a TIMER whose compare starts the radio sending,
    so a frame received wrong is never acknowledged, and the driver need only wake within the frame.
    Weighed:
    - **Priorities alone**, decision 9's proposal, bound the driver's wait by the kernel's longest step, a tick and more today.
    - **The radio's shortcuts alone** send an acknowledgement before the FCS is known, unless the driver wakes to stop it.
    - **The PPI whole to one driver**: the kernel keeps channel 0, and the MPU cannot split the block.
    - **One call to bind and connect** would take six arguments, and a call carries three.
    Open:
    - **The nRF53's and nRF54's DPPI** keeps each device's channel in the device's own registers, which its holder writes,
      so a driver could listen on the kernel's channel; the kernel would keep none there, or refuse such a device.
    - **The ESP32-C6's ETM** numbers its events and tasks; the board would map each number to its device.
    - **RP2350** has no router, but a DMA channel paced by a device's DREQ writes a word to a register;
      the channels are a bus master in one block, which the kernel would hold and confine through the DMA's own MPU.
    - **Groups and forks**, arming several routes at once and a second task per event, as the PPI has.

38. **Work that grows as the logarithm of memory.**
    Working default: none.
    Destruction aside, which is paid for and preempted,
    what a system call, a tick or an interrupt costs does not grow with how many objects exist;
    see "Bounded work".
    Open: a walk as deep as a balanced tree over every object of some kind.
    Their count is bounded by the memory they live in,
    so the tree's depth by the logarithm of the RAM,
    which is a constant of the machine as the PMP entries are.
    It is the door to what the kernel cannot keep in order otherwise:
    things with no bound but memory, sorted by a key with no bound but its width,
    as timers made of pools' memory and sorted by deadline would be, rather than `TIMER_LINES` lines,
    or a queue of refills sorted by time, which decision 9's period per thread needs.
    Ordering by comparison does no better than that logarithm.
    The price:
    - **What one process makes is no longer free to the others.**
      Each doubling of the objects a tick orders costs it a step,
      and a balanced tree as deep as a board's RAM allows would cost, by estimate, about what the cheapest system call does.
    - **The kernel's longest step grows by as much**,
      since a walk down a tree is not one a call can stop and take on again,
      and that step is the floor of every latency, decision 36,
      and of the wait for the kernel's lock on several cores.
    - **The door orders, but it does not bound what falls due together.**
      A tree finds the first timer due in its depth,
      but a tick must fire every timer due, and nothing but memory bounds how many share a deadline;
      the same holds for refills.
      Firing one takes away what arming it made, so that walk is paid for,
      but it needs a way to stop and go on that a tick, unlike a call, does not have yet.
    Before the door, the two ways that keep the bound flat:
    - **A key with few values.**
      Levels of priority, the highest found by counting leading zeros in words of bits,
      first come first served within a level,
      as seL4's 256 priorities are with two levels of such words.
      That suits a choice of one, as the next thread to run is,
      but not timers, which a wheel over a bounded horizon would only gather into the bucket that comes due; see "Time".
    - **A count the kernel fixes, and the order in userspace.**
      A program with more timers than it holds lines keeps its own deadlines and arms one line for the nearest;
      see "Time".
      The tree lies in the memory of the process that needs it, and costs its time alone.
    If the door is taken:
    - **The depth is bounded on every call, not on average**:
      a tree that stays balanced, its links in the objects it orders,
      never a splay tree or a pairing heap, since a tick cannot borrow from the calls after it.
    - **The walk keeps "Bounded stack"**: no recursion, no frame of run-time size.
    - **An annotation in `kernel/work.h` names the depth** against the board's RAM.
      `tools/loop-bounds.py` requires it as it requires every bound,
      but the bound rests on the balance, an invariant, as `i < proc->regions` does, which the link does not check.
    - **The self-check checks the balance**, since the bound rests on it,
      and `fuzz-work` counts the steps, though only of trees as deep as the corpus builds.
    - **Goal 4 and "Bounded work" say so** in the same commit.
    Decide with the first thing the kernel must keep in order across processes
    that neither a key with few values nor a fixed count serves.

39. **Time as deadlines from the accounts, a window, and crossings below the tick.**
    Decided: one mechanism for how much of a core a thread may take and how soon it runs,
    in place of turns taken in order on the tick;
    it is decision 9's order, and the wait below a tick that decision 36 lacked.
    A thread's deadline is the count of the counter at which its account would be full again, plus its window.
    The account stays as decision 18 has it: its units earn it, a turn spends it,
    and it holds a tenth of a second's worth at most.
    The window is none or a power of two of ticks up to 2^15,
    given in `a4` of `OP_TIME_BIND` and kept in the place's spare byte.
    A place's deadline is computed once, as it joins the run queue,
    and kept in a field of its own, in counts of the counter,
    while its `stamp` stays the tick its account was counted to.
    That costs a place 4 bytes, 24 to 28,
    and keeps one time base to a field, which the self-check holds by equalities.
    While the thread waits on the run queue its deadline stands;
    while it runs on u units, its turn's deadline moves 64 / u counts for each count it runs.
    A core runs the ready thread with the earliest deadline.
    A thread made ready with a deadline before the turn's payer's takes the processor as the trap ends.
    When the running turn's deadline crosses the earliest waiting one,
    the timer ends the turn, no sooner than `CROSS_QUANTUM` counts after it was armed:
    a count per board, beside `COUNTER_HZ`, that covers a switch and the way back to user mode,
    4096 instructions at most, summed part by part in "Scheduling",
    so that every turn runs that long and a crossing costs a switch, not a count.
    The units of a core add up to the whole core, so earliest deadline first needs no admission test of its own.
    A thread with u units, an account def short of full and a window w, ready at t0,
    starts within `B + N * CROSS_QUANTUM + ceil((64 - u) * (def + w) / 64)` counts,
    where N is the number of the core's other places with units and B the kernel's longest step,
    and work of c counts finishes within `ceil(64 * c / u)` counts more,
    whatever the other threads do: Zhang's virtual clock, served by Goyal's start times, which EEVDF refines.
    `model/Rvuos/Sched.lean` proves the start bound, from arrival and through a busy period that began before it;
    the finish bound is derived by hand.
    The bound is a rule of capacity, not a count promised to one workload;
    "Scheduling" works it for drivers of 10 us in windows of 100 us.
    Lent time goes on through a lender with no time of its own, `LEND_DEPTH` lenders on,
    so a client pays for its server and for the driver that server waits on.
    Under QEMU an ask beside a spinner is answered within tens of microseconds instead of after the spinner's turns,
    and a switch between threads costs about a fifth more than turns taken in order did;
    the commits that made it give the numbers.
    Weighed and left:
    - **A constant bandwidth server per place**, as Abeni and Buttazzo's and Linux's `SCHED_DEADLINE`:
      a budget per period, refilled at the deadline, was prototyped.
      The period ties how soon a thread runs to how much it may do at once,
      so a short period chops a lock's hold into pieces and a long one answers late.
    - **Priorities**, decision 9's proposal:
      a bound for the highest level alone, each level below subject to every thread above it.
    - **A crossing without a quantum**:
      threads whose deadlines are equal or near would take the core from each other at every count.
    Open:
    - **The kernel's longest step**, B, bounded in cycles at each board's link and made shorter; decision 36 and `TODO.md`.
    - **The earliest deadline without a walk.**
      Finding it walks the run queue, `TIME_UNITS` places at most, twice on a crossing,
      which is most of `CROSS_QUANTUM`.
    - **Time nobody earns** goes by deadline, so by units rather than by thread as it did before;
      `make lab`'s `bulk-hog` waits longer for it.
    - **Timer lines, an account running dry and the watchdog** still keep to the tick.
    - **Lending across cores**, decision 27, and each borrower's lenders kept in the order of their deadlines,
      so that the earliest pays.
    - **Fuel**: counts of a process's own, moved and never created, which every count it runs takes from,
      so that units bound its power and fuel its energy.
