# TODO

Roadmap first, then smaller items.
Each roadmap step ends in something that runs under QEMU and has a test.
Design decisions behind these items live in `DESIGN.md`.

## Roadmap

1. Done: boot in machine mode, trap vector, UART output.
2. Done: one user-mode process behind PMP, a system call, a caught fault.
3. Done: pools, capability tables, object allocation from user memory,
   installing and removing regions.
4. Done: two processes, `Notification` objects,
   shared memory between processes, and the first scheduling decision.
   The kernel keeps no queues and a thread runs until it waits.
5. Done: the timer tick and round-robin preemption.
   The pool walk that finds a runnable thread
   continues from the running one, and that is the whole policy.
   Priorities on `Thread` were part of this step
   and are now open decision 9 in `DESIGN.md`,
   whose working default is that the kernel has no further policy.
   Still open from that step:
   the `A` extension in userspace,
   for what two threads that can now preempt each other
   do in shared memory.
6. Done: `Irq` objects and a userspace UART driver.
   An interrupt is a signal on the notification bound to the `Irq`,
   which has the `Timer`'s shape, and the stall in `wfi` covers both.
   Lines are handed out as `IrqLine` capabilities, objectless like regions.
   Still open from that step:
   the driver in `user/init.c` transmits only,
   because `make test` feeds the UART nothing to receive,
   and open decision 11 in `DESIGN.md`, sharing a line.
   Done since: the kernel has no console.
   Its output is a log in its memory with an interrupt line of its own,
   and the driver in `user/init.c` is a logger thread that carries it out;
   `DESIGN.md`, "The kernel log".
   Still open from that step: open decision 12, the log across a reset.
7. Done: pool destroy, with the capability table sweep in `DESIGN.md`,
   "Kernel pools and revocation",
   and the pool tree, so that a destroy takes the pools
   created from within the destroyed one.
   Done since: the derivation tree, `DESIGN.md`, "The derivation tree",
   with `OP_CAP_DERIVE` and `OP_CAP_REVOKE`,
   which decided open decision 7,
   what a destroy does to region capabilities for the range.
   Still open from that step:
   the history-based "rights only narrow" invariant.
8. First real board, ESP32-C6.
   Done since: the demo root task runs from RAM, loaded by the ROM over USB,
   and passes the QEMU transcript check, `make test BOARD=esp32c6`;
   `DESIGN.md`, "Boards".
   The PMA unit is all zero after the ROM and so in the way of nothing.
   Still open:
   - `make BOARD=esp32c6 test` with NAPOT regions and the block layout,
     which halved the root task's data region there; it has run under QEMU only;
   - boot from flash, and with it whether the ROM can load the image
     straight from offset 0 or Espressif's second-stage bootloader must run first,
     and whether execute-in-place goes through a cache the kernel must control;
   - erratum DIG-694 on misaligned accesses across PMP regions;
   - what the ROM overwrites in RAM on a reset, for open decision 12;
   - the escape-attempt suite below, on the board as well as under QEMU,
     since the board's PMP is what confines a process there.

## Verification

- The self-check sees structure, not semantics.
  That a signal wakes a thread waiting on *that* notification,
  and hands it the bits that were set,
  is checked only by the demo in `user/init.c`.
- The tick preempts nobody while tracing is on;
  `OP_DEBUG_TICK` replays its decision,
  and only the interrupt landing between two instructions
  is checked by the demo in `user/init.c` alone.
- `OP_DEBUG_IRQ` replays what a device interrupt does to an `Irq`;
  the controller, the claim and the completion
  are checked by the demo in `user/init.c` alone.
- The log's line is replayed through the trace itself,
  `tests/seeds/log-signals-untaken` and `log-wakes-reader`;
  a reader falling a whole ring behind, and the logger's byte-per-interrupt path,
  are checked by the demo in `user/init.c` alone,
  and the loss only by reading its code.
  A record that writes garbage into the log's `taken` is beyond the fuzzer,
  since no record writes memory; the clamp in `klog.c` is checked by reading it.
  That the log cannot become a pool is beyond it too:
  the replay driver maps the log, so the overlap check refuses first,
  and a process that never mapped it exists only in `user/init.c`'s children.
- The replay driver has two threads, so a record's actor is one of two.
  More of them, each with its process and pool as the second has,
  would give the round more candidates and the pool tree more branches;
  the prologue in `include/rvuos/replay.h` and `host_boot` grow with them.
- Escape-attempt suite under QEMU:
  one user program per scenario, expected outcome a specific fault.
  Execute from data, jump into the kernel, `csrr` and `mret` from user mode,
  misaligned access, stack into kernel memory.
- CBMC on the overlap checks, `OP_REGION_CARVE` and the NAPOT encoding.
- Feed the replay corpus to the ESP32-C6.
  Something has to put each input where `BOOT_CAP_INPUT` points,
  below the ROM's buffers or over USB once the kernel runs,
  and the replay driver's second stack has to follow the board's data region.
- One layout header consumed by C, the linker scripts and
  `tests/differential.py`.
  C and the linker scripts share `kernel/layout.h` and the board's `board.h` now;
  the replay driver's second stack in `rvuos/replay.h`
  and `tests/differential.py` still carry QEMU's addresses of their own.
- A pool whose every capability was revoked stays until the pool above it goes,
  and its memory is inert to every region capability meanwhile.
  Nothing today tells a lender that the borrower's pool is the reason
  a revoked region cannot be pooled again;
  `KERR_OVERLAP` is all it sees.
  A revoke that destroys the pools whose only capabilities it took
  would be the seL4 answer; decide when a lender that keeps its borrower alive exists.
- The seeds under `tests/seeds` are binary and were written by hand.
  Moving `BOOT_CAP_LOG` in took a one-off script that knew which argument
  of which operation is a slot; a generator in the repository,
  one line per record with the names from `rvuos/abi.h`,
  would make the seeds readable and the next renumbering a rebuild.

## Code

- The replay driver drains the log by polling after each record,
  so the host models it with one store into the header per event.
  A logger thread in the driver would make traced calls the host would have to follow.
- A device interrupt wakes its driver but does not run it;
  the driver waits its turn in the round like a thread the timer woke.
  Measure that latency on the first board; it belongs to open decision 9.
- `pmp_init` stops counting at the first hardwired entry.
  A core with writable entries above a hardwired one loses them;
  have the image skip such entries if one turns up.
- `selfcheck.c` is always compiled in; make it a build-time option
  before any board with little RAM.
- The root task is granted one block of free RAM,
  so what lies between the blocks is unused:
  about 2.8 MiB on QEMU, where it does not matter, and 28 KiB on the ESP32-C6.
  Grant the rest as further blocks, or lay the board out afresh,
  when a board's RAM gets tight.
- `PROCESS_REGION_SLOTS` is fixed at 8;
  size it per process from the PMP budget when process creation exists.
- The replay reaches one level of the pool tree below the boot pool:
  the driver's second thread lives there and pools it creates hang from it.
  A thread living two levels down would be one the records configured,
  which the host cannot follow,
  so `KERR_STATE` for a thread destroying a pool above its own
  is checked only by the demo in `user/init.c`.
- The same-pool rule for a process and its table, and a thread and its process,
  could loosen to "the same pool or one above it",
  which the pool tree makes safe: a parent pool outlives its children.
  A thread could then live in a pool of its own and be revoked alone.
- A signal and a wait in one system call, the shape of seL4's `ReplyRecv`.
  Today it saves one trap per round trip and no context switch,
  because a signal does not take the processor away from the signaller.
  Should a woken thread ever preempt the signaller,
  which open decision 9 in `DESIGN.md` leaves out,
  it saves switches as well; that is when to add it.
- A user fault stops the machine, even when another thread could run.
  Give a thread's creator somewhere to hear about it:
  a notification the kernel signals is the cheapest candidate,
  since it needs no new object.
- Userspace has no clock: `mcounteren` is left clear, so `rdtime` traps.
  Setting its `TM` bit costs nothing and lets a periodic task
  compute absolute deadlines without drift,
  but the unit of `time` is the board's,
  so a program that reads it is no longer a plain binary.
  Decide with the first periodic driver.
- A thread can be started but not stopped again.
  `OP_THREAD_SUSPEND` waits for a reason to exist,
  and a userspace scheduler, open decision 9 in `DESIGN.md`, would be one;
  a thread waiting on a notification cannot be taken off it today.
