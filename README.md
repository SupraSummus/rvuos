# rvuos

A capability-based microkernel for RISC-V microcontrollers.

The kernel isolates processes using RISC-V Physical Memory Protection
instead of a memory management unit,
so it runs on cores that have only machine mode and user mode.
It follows the seL4 principle that the kernel never allocates memory:
userspace hands memory to the kernel,
and the kernel builds its objects inside that memory.

See `DESIGN.md` for the architecture
and `CLAUDE.md` for repository conventions.

## Building and running

Requirements: clang and lld with RISC-V support,
llvm-objcopy, GNU make, and qemu-system-riscv32.
No separate cross toolchain is needed.

```
make            # build/kernel.elf
make run        # boot under QEMU virt, RV32
make test       # boot under QEMU and check the transcript
make host-test  # replay the fuzz corpus on the host build with invariants
make fuzz       # fuzz the system call surface for FUZZ_TIME seconds
make qemu-replay # replay the corpus on QEMU and compare traces with the host
make check      # test, host-test and qemu-replay
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.

The host build needs clang with the sanitizer and libFuzzer runtimes,
on Debian and Ubuntu the `libclang-rt-<version>-dev` package.
See `DESIGN.md`, "Properties" and "Verification",
for what the tests check and why.

## Status

The kernel boots in machine mode on QEMU `virt`
and runs an embedded root task in user mode behind PMP.
The root task holds capabilities to its own objects and to free memory,
carves regions, turns one into a kernel pool,
allocates objects from it,
and installs and removes regions, with the PMP image following each change.
Out of those capabilities it builds a second process,
maps a region into both, starts a thread in it,
and exchanges a word with it through that shared memory,
using notifications to say when.

Kernel objects live in pools carved from user memory,
and the pools form a tree by who created them:
destroying a process's pool takes every pool it made,
so nothing a process built in kernel memory outlives it.

No data passes through the kernel: the only blocking primitive
is a notification, a word of sticky bits,
and `DESIGN.md`, "Communication and synchronisation", says why.

The machine timer ticks and the kernel preempts on it,
taking runnable threads in turn;
that is all the scheduling policy the kernel has,
and `DESIGN.md`, "Scheduling", says why.

There is no sleep: a `Timer` object signals a notification
once a delay has passed, so a thread sleeps by waiting on it,
and a wait with a timeout is the same wait with one more bit;
`DESIGN.md`, "Time", says why.

Device interrupts and a userspace driver
are not implemented yet;
see the roadmap in `TODO.md`.
