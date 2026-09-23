# rvuos

A capability-based microkernel for RISC-V microcontrollers.

The kernel isolates processes using RISC-V Physical Memory Protection
instead of a memory management unit,
so it runs on cores that have only machine mode and user mode.
It follows the seL4 principle that the kernel never allocates memory:
userspace hands memory to the kernel,
and the kernel builds its objects inside that memory.

See `MANUAL.md` for what the kernel offers a program
and the system call reference,
`DESIGN.md` for the architecture,
and `CLAUDE.md` for repository conventions.

## Building and running

Requirements: clang and lld with RISC-V support,
llvm-objcopy, GNU make, and qemu-system-riscv32.
No separate cross toolchain is needed.

```
make            # build/qemu/kernel-init.elf and build/qemu/kernel-fuzzdrv.elf
make run        # boot under QEMU virt, RV32
make test       # boot under QEMU and check the transcript
make host-test  # replay the fuzz corpus on the host build with invariants
make fuzz       # fuzz the system call surface for FUZZ_TIME seconds
make qemu-replay # replay the corpus on QEMU and compare traces with the host
make check      # test, host-test and qemu-replay
make mutants    # plant the bugs of tests/mutants/ and see which checks catch them
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.

The host build needs clang with the sanitizer and libFuzzer runtimes,
on Debian and Ubuntu the `libclang-rt-<version>-dev` package.
See `DESIGN.md`, "Properties" and "Verification",
for what the tests check and why.

An ESP32-C6 connected over USB runs the same demo from RAM,
loaded by the chip's ROM, with nothing written to flash;
this needs Espressif's `esptool`:

```
make BOARD=esp32c6 run   # load the demo and print its console
make BOARD=esp32c6 test  # the same, and check the transcript
```

## Status

The kernel boots in machine mode on QEMU `virt` and on the ESP32-C6
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

Device interrupts have the same shape: an `Irq` object,
bound to a notification, signals it when its line fires
and masks the line until the driver arms it again.
Interrupt lines are handed out as capabilities like memory is;
`DESIGN.md`, "Interrupts", says how.

The kernel has no console.
What it prints goes into a log in its own memory,
which the root task maps like a device's registers,
and the log has an interrupt line like a device has,
high while the log holds bytes the reader has not taken.
The root task's logger thread waits on that line and on the UART's,
and carries the log out one byte per transmitter interrupt;
`DESIGN.md`, "The kernel log", says why.

No driver runs in the kernel; the roadmap in `TODO.md` says what comes next.
