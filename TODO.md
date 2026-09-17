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
5. The timer tick, priorities on `Thread`, preemptive scheduling.
   This is where the pool walk that finds a runnable thread
   has to become something with a policy in it,
   and where userspace needs the `A` extension
   for what it does in shared memory.
6. `Irq` objects and a userspace UART driver.
   An interrupt is a signal on the notification bound to the `Irq`,
   so no new mechanism is needed.
7. Done: pool destroy, with the capability table sweep in `DESIGN.md`,
   "Kernel pools and revocation",
   and the pool tree, so that a destroy takes the pools
   created from within the destroyed one.
   Still open from that step:
   the history-based "rights only narrow" invariant,
   and open decision 7 in `DESIGN.md`,
   what a destroy does to region capabilities for the range.
8. First real board, ESP32-C6 if it passes these datasheet checks:
   the PMA unit, 16 entries of Espressif's own that check machine mode too,
   whether execute-in-place from flash goes through a cache
   the kernel must control,
   how much of Espressif's second-stage bootloader must run first,
   and erratum DIG-694 on misaligned accesses across PMP regions.

## Verification

- The self-check sees structure, not semantics.
  That a signal wakes a thread waiting on *that* notification,
  and hands it the bits that were set,
  is checked only by the demo in `user/init.c`.
- Escape-attempt suite under QEMU:
  one user program per scenario, expected outcome a specific fault.
  Execute from data, jump into the kernel, `csrr` and `mret` from user mode,
  misaligned access, stack into kernel memory.
- CBMC on `rebuild_pmp` and the overlap checks.
- Feed the replay corpus to a board over UART once there is one.
- One layout header consumed by C, the linker scripts and
  `tests/differential.py`; today the addresses live in seven places,
  the newest being the replay driver's second stack in `rvuos/replay.h`,
  which only a static assert ties to the data region it must lie in.

## Code

- Before granting device ranges: `REGION_TO_POOL` zeroes the range,
  which on MMIO would write device registers from machine mode.
  Mark RAM grants poolable, or check against a RAM list.
- `pmp_init` stops counting at the first hardwired entry.
  A core with writable entries above a hardwired one loses them;
  have the image skip such entries if one turns up.
- `selfcheck.c` is always compiled in; make it a build-time option
  before any board with little RAM.
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
  Once step 5 makes a higher-priority thread preempt on the signal,
  it saves switches as well; that is when to add it.
- A user fault stops the machine, even when another thread could run.
  Give a thread's creator somewhere to hear about it:
  a notification the kernel signals is the cheapest candidate,
  since it needs no new object.
- A thread can be started but not stopped again.
  `OP_THREAD_SUSPEND` waits for a reason to exist;
  a thread waiting on a notification cannot be taken off it today.
- `object_first`/`object_next` collapsed the pool walk everywhere
  except the tiling check in `selfcheck.c`, which verifies
  the very link the flat walk crosses pools by.
  Leave that one nested.
