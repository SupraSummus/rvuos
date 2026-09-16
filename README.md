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
and runs one embedded root task in user mode behind PMP.
The root task holds capabilities to its own objects and to free memory,
carves regions, turns one into a kernel pool,
allocates a capability table from it,
and installs and removes regions in its own process,
with the PMP image following each change.
Processes beyond the root task, threads, IPC, and interrupts
are not implemented yet;
see the roadmap in `DESIGN.md`.
