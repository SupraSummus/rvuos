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
   A thread runs until it waits.
5. Done: the timer tick and round-robin preemption.
   Ready threads take turns in the order they became ready,
   and that is the whole policy.
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
9. Bounded work, goal 4 in `DESIGN.md`,
   whose table lists every walk this removes.
   ABI changes come last.
   Done: the line table, the installed region's slot index, the wait queue,
   the run queue, the count of armed sources for the stall in `wfi`,
   zeroing each object as it is allocated and on destroy only what was,
   the check that every loop a trap runs says what bounds it,
   `tools/loop-bounds.py`, a predecessor link per slot,
   revoke, delete below a root and the conversions preempted and restarted,
   and the timer lines, which the tick looks at instead of every pool;
   `DESIGN.md`, "Bounded work".
   - The pool destroy preempted too, once open decision 14 says what its walk is.
   - `Untyped` and `Frame`, open decision 14, once its open questions are decided.

## Verification

- `fuzz-work` counts the loop annotations' claims only where the corpus reaches.
  The host's `irq_claim` never returns a line,
  so nothing counts the `IRQ_LINES` bound of `sched_claim_interrupts`.
- That a destroy zeroes its objects is checked only by reading `pool_clear`:
  no harness reads the memory a process gets back.
- `tools/loop-bounds.py` knows clang's jump tables by their shape;
  any other jump through a register may make up a loop, which fails the link, never passes it.
- That the count of armed sources leaves the log's line out is checked by reading `irq_set_bits`.
  Under tracing each call's line reaches the log before the self-check runs,
  so an `Irq` armed on the log's line has always signalled by then.
- No record writes memory, so what a process leaves in memory it turns into a pool
  is stood for only by the pattern the host's RAM starts out holding.
- The exit to user mode is checked by nothing but runs under QEMU;
  `start.S` is not in the host build.
  The trap path is small enough to prove against the Sail model of RISC-V.
- The PMP image is checked against `pmp_napot_range`, the kernel's own reading of NAPOT,
  so a misreading the encoder and the decoder share passes the self-check.
  A decoder written from the specification alone, or the Sail model, would catch it.
- The host's RAM is one buffer, so ASan sees only accesses that leave it.
  A write from one object into its neighbour, or into a pool's free space,
  shows only if the self-check or `host/history.c` sees what it damaged.
  Poisoning each pool's free space, with `pool_alloc` and the zeroing unpoisoning what they write,
  would make ASan see it.
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
- The replay driver has three threads, and the third shares the second's process and pool.
  More of them, each with its process and pool as the second has,
  would give the pool tree more branches;
  the prologue in `include/rvuos/replay.h` and `host_boot` grow with them.
- No harness reaches `KERR_LIMIT` in `process_install`.
  Since regions are NAPOT blocks, each costs one PMP entry,
  and a process has eight region slots,
  so a budget of eight, `fuzz-pmp8`'s, is never short:
  the eighth install finds seven regions installed.
  The replay driver's processes hold four regions each once set up,
  so a harness with a budget of six would reach the limit on the third install
  and still run the prologue.
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
  would be the seL4 answer, and open decision 14 in `DESIGN.md` is that answer.
- The host stops every preemptible call after one step, and makes it again,
  but has no second thread run in between,
  since under tracing an interrupt switches nothing.
  On QEMU a call stops, and the interrupt is taken on the way back, only when a tick happens to land in it.
  A restart that finds its slots changed by another thread
  is checked by reading `syscall.c`.
  A call that stops without putting its thread back on the `ecall`
  the host takes as finished, and no host check sees it;
  `make qemu-replay` does, by the trace line the host then lacks.
- The seeds under `tests/seeds` are binary and were written by hand.
  Moving `BOOT_CAP_LOG` in, and `BOOT_CAP_TIMER_LINES` and `BOOT_CAP_CLOCK` after it,
  each took a one-off script that knew which argument of which operation is a slot,
  run over the corpus too the last two times.
  A generator in the repository, one line per record with the names from `rvuos/abi.h`,
  would make the seeds readable and the next renumbering a rebuild;
  replay slots that start a few above `BOOT_CAP_COUNT` would spare the next one.

## Code

- The replay driver drains the log by polling after each record,
  so the host models it with one store into the header per event.
  A logger thread in the driver would make traced calls the host would have to follow.
- A device interrupt wakes its driver but does not run it;
  the driver waits its turn in the round like a thread a timer line woke.
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
  `KERR_STATE` for a thread destroying the pool its table lies in
  is checked by nothing:
  the driver's threads keep their tables in their own pools,
  and the demo gives its child no table elsewhere.
- The same-pool rule for a thread and its process,
  and for an `Irq` and its notification,
  could go the way a process's table went:
  hold the target by a capability slot the sweep clears,
  and give clearing it the effect it needs,
  as clearing an installed region rebuilds the PMP image.
  A thread whose process is taken leaves its queue and stops;
  an `Irq` whose notification is taken disarms and masks its line.
  A thread could then live in a pool of its own and be revoked alone,
  and no structural pointer between objects would be left.
  Each costs sixteen bytes per object and one test on use.
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
- The clock is untried on the ESP32-C6:
  `make BOARD=esp32c6 test` has to print "root: clock ok", which needs user mode to read `UTIME`.
- The ESP32-C6's counter rate is measured over one tick at boot,
  so it is only as exact as the polling loop in `timer_init`; a longer measurement would do better.
- The ESP32-C6 resets with `mideleg` at `0x111`, delegating interrupts 0, 4 and 8 to user mode.
  Nothing raises them today, but the kernel should clear it at boot.
- The ESP32-C6's GPIO CSRs, `0x803` to `0x805`, lie in the user-mode CSR range.
  If user mode can reach them, every process drives eight pads past PMP; check on the chip.
- A thread can be started but not stopped again.
  `OP_THREAD_SUSPEND` waits for a reason to exist,
  and a userspace scheduler, open decision 9 in `DESIGN.md`, would be one;
  a thread waiting on a notification cannot be taken off it today.
- `object_first`/`object_next` collapsed the pool walk everywhere
  except the tiling check in `selfcheck.c`, which verifies
  the very link the flat walk crosses pools by.
  Leave that one nested.
