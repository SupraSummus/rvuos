# TODO

Open work only; an item leaves this file in the commit that finishes it.

## ESP32-C6

- Boot from flash, and with it whether the ROM can load the image
  straight from offset 0 or Espressif's second-stage bootloader must run first,
  and whether execute-in-place goes through a cache the kernel must control.
- Erratum DIG-694 on misaligned accesses across PMP regions.
- What the ROM overwrites in RAM on a reset, for open decision 12.
- Try user mode on the rest of the user-level CSR space, here and on QEMU;
  only the CSRs the TRM lists and `0x800` to `0x802` were tried.
  `utval`, `0x043`, reads zero from user mode but was not written, so the kernel does not set it back.
- The counter rate is measured over one tick at boot,
  so it is only as exact as the polling loop in `timer_init`; a longer measurement would do better.
- A device interrupt wakes its driver but does not run it;
  the driver waits for its turn like a thread a timer line woke.
  Measure that latency on the board; it belongs to open decision 9.
- Feed the replay corpus to the board.
  Something has to put each input where `BOOT_CAP_INPUT` points,
  below the ROM's buffers or over USB once the kernel runs,
  and the replay driver's second stack has to follow the board's data region.

## Verification

- `fuzz-work` counts the loop annotations' claims only where the corpus reaches.
  The host's `irq_claim` never returns a line,
  so nothing counts the claims on the way from an interrupt through `sched_claim_interrupts`,
  its `IRQ_LINES` bound among them.
- That an object holds nothing from before its pool took the memory
  shows only where the garbage is a capability or makes a call fail;
  `host/history.c` does not know what the memory held before the call,
  and no record writes memory, so what a process leaves in memory it turns into a pool
  is stood for only by the pattern the host's RAM starts out holding.
- `tools/loop-bounds.py` knows clang's jump tables by their shape;
  any other jump through a register may make up a loop, which fails the link, never passes it,
  as a table whose base clang keeps on the stack does.
- The link checks read what clang makes, and a cloud session's clang is not the local one, 22:
  a tree that linked in the cloud once failed `loop-bounds` locally.
  Pin one clang in `.claude/hooks/session-start.sh`, or run the checks under both.
- That the count of armed sources leaves the log's line out is checked by reading `irq_set_bits`.
  Under tracing each call's line reaches the log before the self-check runs,
  so an `Irq` armed on the log's line has always signalled by then.
- The exit to user mode is checked by nothing but runs under QEMU;
  `start.S` is not in the host build.
  QEMU breaks a reservation on every trap by itself,
  so nothing there would show the `sc.w` in `trap_return` missing.
  The trap path is small enough to prove against the Sail model of RISC-V.
- The PMP image is checked against `pmp_napot_range`, the kernel's own reading of NAPOT,
  so a misreading the encoder and the decoder share passes the self-check.
  A decoder written from the specification alone, or the Sail model, would catch it.
- ASan sees the kernel reach RAM outside its objects, but not one object reach into the next,
  which lies right behind it as on the target.
  Such a write shows only if the self-check or `host/history.c` sees what it damaged.
  A red zone between objects in the host build alone would show it,
  but would change which allocations fit, and with them the transcripts the host and QEMU compare.
- The self-check sees structure, not semantics.
  That a signal wakes a thread waiting on *that* notification,
  and hands it the bits that were set,
  is checked only by the demo in `user/init.c`,
  and so is that a fault signals the watch of the thread that faulted and stops it where it faulted;
  the host and QEMU run the same kernel code, so their transcripts agree on a fault that signals nothing.
- The log's line is replayed through the trace itself,
  `tests/seeds/log-signals-untaken` and `log-wakes-reader`;
  a reader falling a whole ring behind, and the logger's byte-per-interrupt path,
  are checked by the demo in `user/init.c` alone,
  and the loss only by reading its code.
  A record that writes garbage into the log's `taken` is beyond the fuzzer,
  since no record writes memory; the clamp in `klog.c` is checked by reading it.
- `KERR_STATE` for a thread destroying the pool its table or its process lies in, but not the thread,
  is checked only for the two together:
  the driver's third thread lies apart from its process, but the process's table lies with it,
  and the demo's root task lives with both.
- The replay driver has three threads, and the third shares the second's process and lies in the first's pool.
  More of them, each with its process, pool and Untyped as the second has,
  would give the derivation below the free RAM more branches;
  the prologue in `include/rvuos/replay.h` and `host_boot` grow with them.
- No harness reaches `KERR_LIMIT` in `process_install`.
  Since regions are NAPOT blocks, each costs one PMP entry,
  and a process has eight region slots,
  so a budget of eight, `fuzz-pmp8`'s, is never short:
  the eighth install finds seven regions installed.
  The replay driver's processes hold four regions each once set up,
  so a harness with a budget of six would reach the limit on the third install
  and still run the prologue.
- Escape-attempt suite: more scenarios.
  `make escape` runs one root task per scenario, each expected to fault a specific way;
  see `user/escape-*.c` and `tests/escape.sh`.
  It has execute-from-data, an instruction access fault,
  and a machine-CSR read from user mode, an illegal instruction, so far.
  Still to add: jump into the kernel and `mret` from user mode,
  and a stack into kernel memory, a store access fault.
  A misaligned access belongs here too,
  but QEMU's `virt` may emulate one rather than fault,
  so decide what it must show before adding it, and mind the ESP32-C6's erratum DIG-694.
  Since a fault stops only its thread, one root task could run every scenario in a thread of its own
  and hear each fault through its watch, one boot for the suite;
  it would read the cause from the log, or through an operation open decision 21 leaves out.
- CBMC on the halves `OP_UNTYPED_SPLIT` makes, `OP_FRAME_CARVE` and the NAPOT encoding.
- One layout header consumed by C, the linker scripts and
  `tests/differential.py`.
  C and the linker scripts share `kernel/layout.h` and the board's `board.h` now;
  the replay driver's second stack in `rvuos/replay.h`
  and `tests/differential.py` still carry QEMU's addresses of their own.
- A preemptible call that takes its caller's own code or data mapping,
  a revoke below the frame or the Untyped they came from,
  and that stops for a tick on QEMU leaves its thread to fault on the `ecall` it would make again,
  while the host, which makes a stopped call again at once, finishes it and traces one line more.
  The corpus has no such input; the fuzzer made one within minutes,
  which `make qemu-replay` fails by that line.
- A second `OP_DEBUG_TRACE` fills every account again,
  which `host/history.c` takes for a traced call that moved no time and charged the thread whose turn it was;
  the fuzzer finds it within minutes.
  Either the fill belongs to the call that turns tracing on, or the check leaves the call out.
- The host stops every preemptible call after one step, and makes it again,
  but has no second thread run in between,
  since under tracing an interrupt switches nothing.
  On QEMU a call stops, and the interrupt is taken on the way back, only when a tick happens to land in it.
  A restart that finds its slots changed by another thread
  is checked by reading `syscall.c`,
  and so are a destroy that another call goes on with
  and an allocation refused because its pool is dying, by reading `pool_destroy` and `op_pool`.
  A call that stops without putting its thread back on the `ecall`
  the host takes as finished, and no host check sees it;
  `make qemu-replay` does, by the trace line the host then lacks.
- The host moves time a tick at a time, and under tracing every turn begins at a tick,
  so what a turn begun within a tick pays at the tick, `turn_next`,
  and the ticks a trap counts at once, `account_after`, run only in the demo, whose bounds are loose.
  A demo thread held to its units that always takes over half way into the tick
  would run its units' part only if it paid for no more than that half.
- Once a record binds a driver thread to units, and so to time the others do not have,
  passing a record round may come back before it visited every runnable thread,
  and a record for a thread that could run is performed by another.
- `make qemu-replay` boots QEMU once per input, about 40 ms each,
  which is now most of what a mutant costs `make mutants`.
  A driver that replays several inputs per boot would need the kernel back to its boot state in between.
- The seeds under `tests/seeds` are binary and were written by hand.
  Renumbering a `BOOT_CAP_*` or `REPLAY_CAP_*` slot takes a one-off script
  that knows which argument of which operation is a slot,
  run over the seeds and the corpus,
  and checked by replaying every seed on the kernels before and after and comparing the statuses;
  an operation whose arguments change leaves the seeds that use it to be written anew.
  A generator in the repository, one line per record with the names from `rvuos/abi.h`,
  would make the seeds readable and the next renumbering a rebuild;
  replay slots that start a few above `BOOT_CAP_COUNT` would spare the next one.
  A seed written before a renumbering and merged after it keeps passing and tests nothing:
  three seeds once did, and only a mutant nothing caught showed it.
- `make mutants` plants with `git apply -C1`,
  which, when the context has changed and one line of it matches in several places,
  silently takes the place nearest the patch's line numbers,
  and a refresh leaves those numbers old wherever they broke no tie.
  A refresh calls such a patch stale;
  planting through the refresh's `carry` would make the two agree.

## Code

- The replay driver drains the log by polling after each record,
  so the host models it with one store into the header per event.
  A logger thread in the driver would make traced calls the host would have to follow.
- The UART driver in `user/init.c` transmits only,
  because `make test` feeds the UART nothing to receive.
- Revoking below a line capability takes the line back only while it is unbound.
  The `Irq` could hold its line as it holds its notification, by a node below the line capability,
  whose clearing disarms the `Irq` and frees the line; `DESIGN.md`, "Interrupts".
- A pool whose every capability was deleted or revoked stays until the Untyped above it is revoked,
  and nothing tells the holder of that Untyped what is left below it:
  `OP_UNTYPED_INFO` says only that something is.
- A signal and a wait in one system call, the shape of seL4's `ReplyRecv`.
  Today it saves one trap per round trip and no context switch,
  because a signal does not take the processor away from the signaller.
  Should a woken thread ever preempt the signaller,
  which open decision 9 in `DESIGN.md` leaves out,
  it saves switches as well; that is when to add it.
- `pmp_init` stops counting at the first hardwired entry.
  A core with writable entries above a hardwired one loses them;
  have the image skip such entries if one turns up.
- `selfcheck.c` is always compiled in; make it a build-time option
  before any board with little RAM.
- The root task is granted one block of free RAM,
  so what lies between the blocks is unused:
  about 2.8 MiB on QEMU, where it does not matter, and 28 KiB on the ESP32-C6.
  So is the root task's own memory past the boot pool, 60 KiB on QEMU and 24 KiB on the ESP32-C6,
  until the root task is gone; open decision 20 in `DESIGN.md`.
  Grant the rest as further blocks, or lay the board out afresh,
  when a board's RAM gets tight.
- `PROCESS_REGION_SLOTS` is fixed at 8, whatever the PMP budget;
  size it per process from the budget.

## Scheduling

- A turn is one tick while another thread could run, so the timer skips ticks only for a thread alone,
  and a turn that begins mid-tick gets only the rest of one.
  A turn of a few ticks, or of what the thread's account holds, would skip them while threads compete too,
  and give turns the length the scheduler chooses: `sched_wake_ticks` would return the turn's end instead of one.
  It changes what `OP_DEBUG_TICK` means to the replay, which passes records with it,
  and what the demo's "root: units ok" measures; decide it with open decision 9.
- Threads with time take equal turns,
  so a thread with more units gets its larger part only by running on
  after the others have spent their accounts.
  A turn of as many ticks as the thread's units weigh would spread that over time;
  decide with the first workload that wants it, and with the turn counted from the switch below.
- A trap reads the counter on entry whenever the timer is set past the next tick,
  which QEMU counts at a sixth of the cheapest system call.
  Counting only where the count or an account is read, the timer lines' calls and a change of turn among them,
  which reads the counter already to charge the turn, would spare it,
  at the price of a rule every such place has to keep.
- Threads cannot spend one account together, and spare time goes round by thread,
  so a group of threads shares a part of the processor only by splitting units, a sixty-fourth at least each.
  An account several threads are bound to would bring the share back; decide with the first workload that wants one.
- An account holds at most a tenth of a second's worth of its units.
  More lets an idle thread bring back a longer burst, which the others then wait out;
  less holds a thread nearer its units over short stretches;
  decide when a workload measures the difference.
- A thread is stopped by revoking its units and started again by binding it,
  which is what a userspace scheduler, open decision 9 in `DESIGN.md`, needs.
  A thread waiting on a notification stays on it meanwhile, and cannot be taken off it today.
