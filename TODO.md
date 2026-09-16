# TODO

Roadmap first, then smaller items.
Each roadmap step ends in something that runs under QEMU and has a test.
Design decisions behind these items live in `DESIGN.md`.

## Roadmap

1. Done: boot in machine mode, trap vector, UART output.
2. Done: one user-mode process behind PMP, a system call, a caught fault.
3. Done: pools, capability tables, object allocation from user memory,
   installing and removing regions.
4. Two processes, endpoints, `call` and `recv`,
   capability transfer in messages.
   Needs the first scheduler decision; write it into `DESIGN.md` first.
5. Notifications, the timer tick, preemptive scheduling.
6. `Irq` objects and a userspace UART driver.
7. Pool destroy, after open decision 5 in `DESIGN.md`,
   with the history-based "rights only narrow" invariant.
8. First real board, ESP32-C6 if it passes these datasheet checks:
   PMP entry count and modes,
   how the Physical Memory Attribute unit interacts with PMP,
   whether execute-in-place from flash goes through a cache
   the kernel must control,
   how much of Espressif's second-stage bootloader must run first.

## Verification

- Escape-attempt suite under QEMU:
  one user program per scenario, expected outcome a specific fault.
  Execute from data, jump into the kernel, `csrr` and `mret` from user mode,
  misaligned access, stack into kernel memory.
- CBMC on `rebuild_pmp` and the overlap checks.
- Feed the replay corpus to a board over UART once there is one.
- One layout header consumed by C, the linker scripts and
  `tests/differential.py`; today the addresses live in five places.

## Code

- Before granting device ranges: `REGION_TO_POOL` zeroes the range,
  which on MMIO would write device registers from machine mode.
  Mark RAM grants poolable, or check against a RAM list.
- Probe PMP granularity G in `pmp_init` and reject installs
  not aligned to it; 4 bytes is assumed everywhere today.
- `selfcheck.c` is always compiled in; make it a build-time option
  before any board with little RAM.
- `PROCESS_REGION_SLOTS` is fixed at 8;
  size it per process from the PMP budget when process creation exists.
- The invariant "current process's CSRs match its image"
  only covers the running process; with a scheduler,
  check it on every switch in debug builds.
