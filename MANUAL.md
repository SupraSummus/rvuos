# rvuos user manual

This manual is for a developer who wants to write programs for rvuos
or port it to a board.
It says what the kernel is, what it promises,
and how to talk to it.
For why the kernel is the way it is, see `DESIGN.md`.

Contents:

1. What rvuos is
2. Features at a glance
3. Hardware requirements and targets
4. Building and running
5. The programming model
6. System call reference
7. What the root task starts with
8. Writing a program
9. Debugging and testing interfaces
10. Limits
11. What is not there yet

## 1. What rvuos is

rvuos is a capability-based microkernel
for RISC-V microcontrollers that have no memory management unit,
and for ARMv7-M and ARMv8-M ones with an MPU.
It isolates processes with Physical Memory Protection (PMP)
instead of address translation,
so it needs no supervisor mode
and runs on cores that have only machine mode and user mode.
The kernel runs in machine mode and every program runs in user mode.
On ARM the MPU does what PMP does, the kernel runs in handler mode
and every program in unprivileged thread mode;
the objects, the operations and the programs written against `user/rvuos.h` are the same on both,
and where this manual says PMP, machine mode or `ecall`, ARM has its own in their place.

Three goals shape everything, in priority order.

1. **Isolation without an MMU.**
   A process reaches only the resources it was given:
   the memory installed in its region slots, with the rights installed,
   and nothing else.
   Two processes share memory only when someone who holds
   a capability to that memory installs it in both.
   The kernel's own memory is reachable by nobody.
2. **Capability-based authority.**
   Every system call names a capability in the caller's own table,
   and the kernel hands nothing out by a global name:
   there is no way to look up another process, thread or kernel object,
   and a device's registers reach a process only as a frame
   installed in one of its slots.
   A process that holds no capability to a thing cannot act on it,
   however it came to know the thing exists.
3. **The kernel never allocates.**
   The kernel has no heap.
   Every kernel object lives in memory that userspace explicitly
   handed to the kernel for that purpose,
   so a process pays for its kernel objects with its own RAM.

What rvuos is not:

- It is not a POSIX system.
  There is no `fork`, no file descriptor, no signal, no libc.
- It has no virtual memory.
  Every address a program sees is a physical address.
- It has no drivers in the kernel, and no console.
  Drivers run in user mode behind interrupt capabilities,
  and the kernel's own output goes into a log ring that a user program drains.
- It loads no code into the kernel at run time.
  Everything that runs in machine mode is in this repository.
- It is single-core for now.
- It is not binary compatible with seL4, L4 or F9,
  though it borrows from seL4's object model.

## 2. Features at a glance

- **PMP isolation.**
  Each process has a fixed number of region slots, eight today,
  set by the kernel and not by the board.
  A region is installed with read, write and execute rights
  and the kernel rebuilds the process's PMP image on every change.
  A region is a naturally aligned power-of-two block and costs one PMP entry,
  so the core's entry count bounds how many slots can be filled; see section 5.4.
- **Capabilities with rights.**
  A capability names a kernel object, a memory range, a range of interrupt lines
  or a range of the processor's units of time,
  together with rights bits.
  Copying a capability can only narrow its rights.
- **Untyped memory, frames and pools instead of a kernel heap.**
  A process makes its untyped memory into frames it can map and pools the kernel writes,
  never both over the same bytes,
  and every kernel object it creates is allocated from a pool it holds.
- **Revocation by memory.**
  Revoking below an Untyped destroys every pool made of it,
  however far it was lent on:
  every object in it and every capability anywhere that named one of those objects,
  and the memory comes back to the Untyped.
  A pool capability allocates and nothing more.
- **A root task like any other process.**
  The root task starts with the capabilities to the whole machine and nothing else:
  it lives in memory it holds an Untyped to, and the kernel names it nowhere after boot.
  So it can move everything it holds to a successor, which destroys it and takes its place,
  as a bootloader chains to the next stage; see section 7.
- **Plain binaries.**
  A program depends on the register ABI
  and on what its creator hands it:
  a program counter, a stack pointer, installed regions and filled slots.
  There is no executable format, no header and no runtime library.
- **Notifications as the only blocking primitive.**
  A notification is a word of sticky bits.
  Data moves through shared memory; the notification says when.
- **A fault stops a thread, not the machine.**
  A thread that faults stops where it faulted, and a notification its creator chose hears it;
  resumed, it runs the faulting instruction again.
- **Processor time by capability.**
  The processor is `TIME_UNITS` units of time, handed out as capabilities like memory is,
  and a thread runs on the units it is bound to, each earned by one thread at a time,
  or on spare time, which nobody earned, if its capability allows.
  Its account fills at its units' rate and its turns cost it the time they ran, on the machine's counter,
  so a process that makes more threads or children gets no more of the processor than its units,
  and a thread held to its units gets no more, however idle the processor is otherwise.
- **Timers as signals.**
  A timer line is an interrupt line the tick raises once a delay has passed.
  Sleeping is arming a timer line and waiting;
  a period counts from the last deadline, so a periodic task does not drift.
- **A clock as a capability.**
  A process given the `Clock` reads the machine's counter with loads through a read-only region;
  one given neither a clock nor a timer line has no clock to read.
- **Interrupts as signals.**
  An `Irq` binds a hardware line to a notification.
  Lines are handed out as capabilities like memory is.
- **A kernel log in place of a console.**
  The kernel writes into a ring in its own memory
  that the root task maps like a device and drains on an interrupt line.
- **Heavy verification.**
  An in-kernel self-check, host fuzzing under sanitizers,
  differential replay between the host build and QEMU,
  and a mutant suite; see section 9.

## 3. Hardware requirements and targets

The kernel needs a RISC-V core with:

- machine mode and user mode (supervisor mode is not used even when present),
- PMP with NAPOT addressing mode and at least four entries,
  which is the kernel's own floor: it refuses to boot with fewer,
- the `A` extension in user mode if programs want atomics in shared memory,
- a machine timer (`mtime` and `mtimecmp`),
- an interrupt controller in front of the machine external interrupt.

Cores without PMP cannot run rvuos.
TOR is not needed; `DESIGN.md`, open decision 8, records why rvuos uses NAPOT alone.

The build is `rv32imac`, `ilp32`, compiled with clang and linked with lld.

On ARM the kernel needs an ARMv7-M or ARMv8-M Mainline core with:

- an MPU with at least four regions, PMSAv7 on ARMv7-M or PMSAv8 on ARMv8-M,
- SysTick and the NVIC, which every such core has,
- a counter of the board's that a process can read as memory, for `BOOT_CAP_CLOCK`.

On ARMv8-M the kernel and every program run in the Secure state,
and no program reaches a coprocessor or the floating-point unit, whose instructions fault.
The build is `thumbv7m-none-eabi` for the Cortex-M3
and `thumbv8m.main-none-eabi` for the Cortex-M33, with no floating point and no DSP extension.

### Boards

Four boards are supported, chosen with `make BOARD=<board>`:
`qemu`, QEMU `virt` for RV32, the default,
`esp32c6`, an Espressif ESP32-C6,
`rp2350`, a Raspberry Pi RP2350 on its RISC-V cores, or with `ARCH=arm` on its Cortex-M33 ones, as on a Pico 2,
and `mps2-an385`, QEMU's model of ARM's MPS2 board with a Cortex-M3.
A board picks its architecture, whose files lie in `kernel/arch/<arch>/` and `user/arch/<arch>/`;
RP2350 has both, and keeps the files that differ between them in `kernel/board/rp2350/<arch>/`.
Everything board-specific lives in `kernel/board/<board>/`,
`board.h`, `board.c`, `irq.c`, `timer.c` and `halt.c`,
with no `irq.c` on ARM, whose controller is the architecture's,
and in `user/board/<board>/console.h`,
which drives the device behind `BOOT_CAP_UART` for the demo and the replay driver.
The linker scripts take their addresses from `board.h` through `kernel/layout.h`.
Porting to a board means providing those.
A program learns every address it needs from its frames and its Untyped;
only the line numbers of its devices are the board's to know.

#### QEMU `virt`, RV32

Memory map:

| Range | Size | What |
|---|---|---|
| `0x0200BFF8` | 8 B | the CLINT's `mtime`, 10 MHz, read only through `BOOT_CAP_CLOCK` |
| `0x10000000` | 256 B | 16550 UART registers, granted to the root task |
| `0x80000000` to `0x800FF000` | just under 1 MiB | kernel code, data and stack |
| `0x800FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x80100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x80100000` | 64 KiB | root task code, read and execute |
| `0x80110000` | 64 KiB | root task data and stack, read and write |
| `0x80120000` | 64 KiB | replay input placed by QEMU's loader, read only |
| `0x80130000` | 4 KiB | the boot pool |
| `0x80400000` | 4 MiB | free RAM, granted to the root task as an Untyped with all rights |

Every range granted to the root task is a block, see section 5.4,
so what lies between the blocks is used by nothing.

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log (no controller behind it) |
| 1 to 95 | the PLIC's sources; the UART is line 10 |

The kernel's tick on this board is 1 kHz, `TIMER_HZ` in `kernel/timer.h`,
so one tick is one millisecond.
QEMU's PMP has sixteen entries and a four-byte grain.

#### ESP32-C6

The chip's ROM loads the image into SRAM over its USB port and enters it;
nothing is written to flash, and booting from flash is not supported yet.
The same port carries the console.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20001C08` | 8 B | the CLINT's `UTIME`, a read-only copy of `mtime` at the CPU clock, through `BOOT_CAP_CLOCK` |
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x40800000` to `0x4081E000` | 120 KiB | kernel code, data and stack |
| `0x4081E000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x4081F000` | 4 KiB | unused, so that the log does not touch the code |
| `0x40820000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x40820000` | 64 KiB | root task code, read and execute |
| `0x40830000` | 32 KiB | root task data and stack, read and write |
| `0x40838000` | 4 KiB | unused, so that the data does not touch the input |
| `0x40839000` | 4 KiB | input region, read only; nothing fills it yet |
| `0x4083A000` | 4 KiB | the boot pool |
| `0x40840000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; the Wi-Fi MAC's source 0 cannot be bound |
| 1 to 76 | the interrupt matrix's sources, numbered as in Espressif's `soc/interrupts.h`; the USB Serial/JTAG controller is line 48 |

A device's interrupt reaches its `Irq` only while the device itself has it enabled,
in the USB Serial/JTAG controller's case in its `INT_ENA` register.
The tick is 1 kHz here too, measured against the chip's 16 MHz system timer at boot.
The PMP has sixteen entries and a four-byte grain.
Keep memory accesses aligned, as clang does unless told otherwise.
A misaligned load followed at once by a store into a region with other rights faults,
and faults again when resumed (Espressif's erratum DIG-694); a `nop` between them avoids it.
A misaligned store would write past a writable region into a read-only one right above it,
which is why `OP_PROCESS_INSTALL` does not let the two touch.
The console is the controller's CDC-ACM port:
bytes written to its FIFO leave as one USB packet when `WR_DONE` is written,
and only while a host has the port open.

User mode can write some of the core's CSRs:
`ustatus`, `uie`, `utvec`, `uepc`, `ucause`,
the performance counter at `0x800` to `0x802` and the dedicated GPIO at `0x803` and `0x805`.
Whenever another process has run, a program finds them set back: `utvec` at 1, the rest at zero, the counter stopped.
No interrupt is delegated to user mode,
and the dedicated GPIO reaches no pad, since no process is granted the GPIO matrix.

#### RP2350

The bootrom's BOOTSEL mode loads the image into SRAM over USB and reboots into it,
on the RISC-V cores or on the Arm ones, whichever the image is for;
nothing is written to flash.
The two have the same layout, lines and console; what differs is said below.
The chip has no serial port on USB while the kernel runs,
so the console is a block of RAM, and the halt sends it to the host over a USB serial port it makes then.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x400B0028` | 8 B | TIMER0's `TIMERAWL`, 1 MHz, read only through `BOOT_CAP_CLOCK`; the word above it is not the high half |
| `0x20000000` to `0x2001F000` | 124 KiB | kernel code, data and stack |
| `0x2001F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x20020000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x20020000` | 64 KiB | root task code, read and execute |
| `0x20030000` | 32 KiB | root task data and stack, read and write |
| `0x20038000` | 4 KiB | input region, read only; nothing fills it yet |
| `0x20039000` | 4 KiB | the boot pool |
| `0x20040000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |
| `0x20080000` | 8 KiB | the console, SRAM8 and SRAM9, granted to the root task |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; TIMER0's IRQ 0 cannot be bound |
| 1 to 51 | the system IRQs, numbered as in the datasheet; nothing raises the console's line, 46 |

The console's first word counts every byte written, and the bytes follow from offset 16;
what does not fit is counted and dropped.
The counter's high half lies below it, so `rv_counter_read` gets a constant high word
and wraps after 71 minutes.
A watchdog reboots the chip into BOOTSEL about seventeen seconds after boot, so no run lasts longer.

On the RISC-V cores the tick is 1 kHz, on the microseconds of the RISC-V platform timer.
The PMP holds seven regions, none smaller than 32 bytes:
the core has eight entries, and the kernel spends one shutting the three it hardwires open to user mode.
A fault reports `mtval` as zero, and a misaligned access raises a misaligned exception.
A program reaches a peripheral only through a frame, as it reaches RAM,
and only where the chip's ACCESSCTRL lets user mode in too: TIMER0, for the clock, is the one opened.

On the Cortex-M33 the tick is 1 kHz too, on TIMER0's counter, the one `BOOT_CAP_CLOCK` names.
The MPU has eight regions, none smaller than 32 bytes, and a fault is reported as on `mps2-an385`.
A program reaches a peripheral only through a frame, as it reaches RAM.

#### mps2-an385

QEMU loads the image into SSRAM1 and resets the Cortex-M3, which finds the kernel's vector table at 0;
the halt leaves QEMU through semihosting.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x40028018` | 8 B | the FPGA's `COUNTER`, 25 MHz, read only through `BOOT_CAP_CLOCK`; the word above it is the prescaler, zero |
| `0x40004000` | 4 KiB | UART0 of the CMSDK, granted to the root task |
| `0x00000000` to `0x000FF000` | just under 1 MiB | kernel code, data and stack, the vector table first |
| `0x000FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x00100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x00100000` | 64 KiB | root task code, read and execute |
| `0x00110000` | 64 KiB | root task data and stack, read and write |
| `0x00120000` | 64 KiB | input region, read only; nothing fills it yet |
| `0x00130000` | 4 KiB | the boot pool |
| `0x00200000` | 2 MiB | free RAM, granted to the root task as an Untyped with all rights |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; UART0's receiver cannot be bound |
| 1 to 31 | the NVIC's lines; UART0's transmitter is line 1 |

The tick is 1 kHz, on the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide, so `rv_counter_read` gets a zero high word and the counter wraps every 171 seconds.
UART0's transmitter holds one byte and raises its line as that byte leaves, latched until the driver clears it.
A fault is reported as `exception`, `cfsr`, `pc` and `addr`, section 4.

## 4. Building and running

Requirements: clang and lld with RISC-V and ARM support, llvm-objcopy,
GNU make, `qemu-system-riscv32` and `qemu-system-arm`,
the latter no newer than 10.2, since QEMU 11 never wakes the ARM demo from its first sleep; see `TODO.md`.
No separate cross toolchain is needed.
The host build needs clang's sanitizer and libFuzzer runtimes.
The ESP32-C6 needs Espressif's `esptool`, version 5, as a command and as a Python module.
RP2350 needs Python's `pyusb` and write access to the chip's USB devices,
which a udev rule such as `SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", TAG+="uaccess"` gives.

```
make             # build/qemu/kernel-init.elf and build/qemu/kernel-fuzzdrv.elf
make run         # boot the demo root task under QEMU
make test        # boot under QEMU and check the transcript
make host-test   # replay the fuzz corpus on the host build with invariants on
make fuzz        # fuzz the system call surface for FUZZ_TIME seconds in FUZZ_JOBS processes
make qemu-replay # replay the corpus on QEMU and compare with the host
make mutants     # plant each bug under tests/mutants/ and require the checks to catch it
make check       # test, escape, host-test, qemu-replay and the ARM test; run before committing
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.
Images go under `build/<board>/`, the host build under `build/host/`;
a smaller budget adds `-pmp<n>` to both.

On the ESP32-C6, connected over USB:

```
make BOARD=esp32c6                    # build/esp32c6/kernel-init.bin
make BOARD=esp32c6 run                # load it into RAM and print the console until the halt
make BOARD=esp32c6 test               # the same, and check the transcript
make BOARD=esp32c6 PORT=/dev/ttyACM1 run
```

Opening the port resets the chip,
so `tools/esp32c6-run.py` loads the image and reads the console on one connection.
Only the demo is built for the board; the replay driver is QEMU's for now.

On RP2350, in BOOTSEL mode, as it is when plugged in with BOOTSEL held and again after each run:

```
make BOARD=rp2350                    # build/rp2350/kernel-init.elf
make BOARD=rp2350 run                # load it into RAM and print the transcript the halt writes
make BOARD=rp2350 test escape        # check the transcripts
make BOARD=rp2350 ARCH=arm test      # the same demo on the Cortex-M33, from build/rp2350-arm/
```

`tools/rp2350-run.py` loads the image through the bootrom's PICOBOOT interface,
waits for the halt's serial port and reads it; closing it reboots the chip into BOOTSEL.

On ARM, under QEMU:

```
make BOARD=mps2-an385                # build/mps2-an385/kernel-init.elf
make BOARD=mps2-an385 run            # boot the demo root task
make BOARD=mps2-an385 test           # the same, and check the transcript
```

Only the demo is built; the replay driver's layout is QEMU virt's, and the escape suite is RISC-V's so far.

The kernel image embeds one user program, the root task.
Two images are built, differing only in that program:
`kernel-init.elf` carries the demo of `user/init.c`
and `kernel-fuzzdrv.elf` carries the replay driver of `user/fuzzdrv.c`.

### Reading the transcript

The kernel has no console.
Everything the kernel prints, and everything a program writes with `OP_DEBUG_PUTC`,
goes into the kernel log.
Two things carry that log to the board's UART:

- the root task's logger thread, while the machine runs,
- the halt, which writes out every byte no reader has taken,
  under the line `rvuos: halting, the log follows`.

So a line that appears before that marker was carried out from user mode,
and a line after it was salvaged by the halt.
On RP2350 both reach the host at the halt, the logger's from its console, in the same order.

The kernel prints three lines at boot:

```
rvuos: machine mode up
rvuos: pmp entries 0x00000010 grain 0x00000004
rvuos: entering user mode
```

On QEMU a halt exits the emulator with a status code.
The ESP32-C6 parks its core instead, after the line `rvuos: halted with code <n>`,
and RP2350 reboots into BOOTSEL once the host has read that line;
the board's runner exits with the code:

| Code | Meaning |
|---|---|
| 0 | `OP_DEBUG_HALT` with code 0 |
| 1 | kernel panic |
| 3 | the self-check found an invariant violated |
| 5 | no runnable thread and nothing armed that could make one |
| 6 | tracing is on and a thread the host build cannot follow was about to run |
| other | what the root task passed to `OP_DEBUG_HALT` |

A user fault, an access fault, an illegal instruction, a misaligned access or a breakpoint,
prints `user fault` followed by `mcause`, `mepc` and `mtval`,
on ARM by the exception number, the configurable fault status `cfsr`, the `pc`,
and the address `MMFAR` or `BFAR` names, zero where neither is valid,
and stops the thread that made it and nothing else, section 5.6;
a program whose only thread faults therefore ends in `no runnable thread`, code 5.

## 5. The programming model

### 5.1 Processes, threads and regions

A **process** is a protection domain:
a capability table, `PROCESS_REGION_SLOTS` region slots, eight today,
and the threads that run in it.
A **thread** is an execution context inside a process:
a register frame and a state.
Threads of one process share its regions and its capability table.

A **frame** is a physical address range with maximum rights,
and a **region** is a frame installed into one of a process's slots,
which gives that process access to the range with the rights chosen at install time.
Regions are all a process can see of memory.
An **Untyped** is memory that has not become anything yet:
it cannot be installed, and it is made into a frame, a pool or its two halves.
The kernel does not know what a code segment, a stack or a heap is;
whoever builds a process decides its layout.

### 5.2 Capabilities and the table

A process names everything by **slot index** into its capability table.
A slot is empty or holds one capability:
a type, a set of rights, and two words the kernel interprets by type.
Userspace never sees a slot's contents,
but it can ask a frame what it covers with `OP_FRAME_INFO`,
and an Untyped with `OP_UNTYPED_INFO`.

Every system call is an invocation on the capability in one slot.
The kernel resolves the slot,
checks the capability's type and rights against the operation,
and only then looks at the arguments;
section 6.1 gives the exact order.
An empty slot or an index out of range fails with `KERR_INVALID_CAP`.

A process holds its table by a capability of its own,
derived from the `CapTable` capability it was allocated with.
The table may lie in any pool,
and several processes may be allocated with one table, which they then share.
Revoking below that capability, or destroying the pool the table lies in,
takes the table from the process,
and from then on every call its threads make fails with `KERR_INVALID_CAP`.
The table itself stays as long as its pool does,
and so does everything in it.

Rights are three bits:

| Bit | Value | Frame, Untyped | Other types |
|---|---|---|---|
| `RIGHT_R` | 1 | read | `Notification`: wait |
| `RIGHT_W` | 2 | write | control the object: allocate, install, configure, signal, set, bind, copy or move into a table |
| `RIGHT_X` | 4 | execute | `Time`: the threads bound through it run on spare time |

Every operation that produces a capability
puts it into a slot of the caller's own table,
named by an argument, and that slot must be empty.
A process needs no capability to receive into its own table.
The `CapTable` capability exists for writing into a table:
a parent fills a child's table with `OP_CAP_COPY`, `OP_CAP_DERIVE` or `OP_CAP_MOVE` before starting it,
and a process that holds a capability to its own table
can copy and move within it, clear its own slots, and revoke below them.

Every capability remembers what it was derived from,
and that is what `OP_CAP_REVOKE` follows.
The tree grows in these ways:

| Operation | The new capability hangs |
|---|---|
| `OP_CAP_DERIVE` | below the source |
| `OP_CAP_COPY` | beside the source, under the source's parent; a copy of a root is a root; an Untyped is never copied |
| `OP_CAP_MOVE` | where the source hung, which it leaves empty, with what hung below the source below it |
| `OP_FRAME_CARVE`, `OP_IRQ_CARVE`, `OP_TIME_CARVE`, `OP_CLOCK_FRAME` | below the invoked capability |
| `OP_UNTYPED_RETYPE` of a frame, `OP_UNTYPED_SPLIT` | below the invoked Untyped |
| `OP_UNTYPED_RETYPE` of a pool | below the pool's own node, which hangs below the invoked Untyped |
| `OP_PROCESS_INSTALL` | the installed region hangs below the frame |
| `OP_POOL_ALLOC` | below the invoked `KernelPool` capability |
| `OP_POOL_ALLOC` of a `Process` | and the process's hold on its table below the `CapTable` capability |
| `OP_POOL_ALLOC` of a `Thread` | and the thread's hold on its process below the `Process` capability |
| `OP_IRQ_BIND` | below the `KernelPool` capability; the line goes with what was derived from it |
| `OP_IRQ_BIND` | and the `Irq`'s hold on its notification below the `Notification` capability |
| `OP_TIME_BIND` | the thread's hold on its units hangs below the invoked `Time` capability |
| boot | nowhere, but for the root task's frames and the boot pool's block, below `BOOT_CAP_ROOT_RAM`, the boot pool's own node, below that block, and the capabilities to the boot pool and its objects, below that node |

`OP_CAP_REVOKE` on a slot clears everything below it, in every table and every process:
derived capabilities, their copies, what was derived from those,
every region installed from any of them,
and every thread's units bound through any of them;
a thread made through any of them loses its process and stops, section 5.6,
and an `Irq` bound through any of them loses its notification and is disarmed, section 5.9.
Below an Untyped it destroys every pool made of it, section 5.5.
The slot itself stays.
Revoking a copy takes nothing from the original, and the other way round;
to take both back, revoke below what they were both derived from.
So derive to lend, and copy to keep a second handle to what you hold.
A copy cannot take back what was derived from its source, since that hangs below the source and not below the copy;
to hand over a capability together with that power, move it.
An Untyped can only be derived, and only while nothing was made of it:
the derived one is the whole of it, and the source makes nothing until that one is gone.

`OP_CAP_DELETE` clears one slot and hands what hung below it to the slot's parent,
so deleting your own copy of something you lent does not take it back.
Destroying a pool clears every slot in the tables that pool held the same way,
so what a process handed out through its table outlives the process,
below whatever the process derived it from.

Copying and deriving both apply a rights mask, so rights only ever narrow.
A move keeps the rights as they were, since what hangs below may hold all of them.

Slot 0 of the root task's table is left empty on purpose,
so that an uninitialised index fails.
Programs are encouraged to keep the same convention.

### 5.3 Objects

| Type | Constant | Object behind it | Created by |
|---|---|---|---|
| `Untyped` | `CAP_UNTYPED` | none: the slot holds the block | boot, `OP_UNTYPED_SPLIT` |
| `Frame` | `CAP_FRAME` | none: the slot holds base and size | boot, `OP_FRAME_CARVE`, `OP_UNTYPED_RETYPE` |
| `KernelPool` | `CAP_POOL` | the pool's descriptor, at its base | `OP_UNTYPED_RETYPE` |
| `CapTable` | `CAP_CAPTABLE` | a table of `n` slots | `OP_POOL_ALLOC` |
| `Process` | `CAP_PROCESS` | a capability to its table, region slots, a PMP image | `OP_POOL_ALLOC` |
| `Thread` | `CAP_THREAD` | a register frame and a state | `OP_POOL_ALLOC` |
| `Notification` | `CAP_NOTIFICATION` | one word of sticky bits | `OP_POOL_ALLOC` |
| `IrqLine` | `CAP_IRQ_LINE` | none: the slot holds the first line and a count | boot, `OP_IRQ_CARVE` |
| `Irq` | `CAP_IRQ` | one line bound to a notification, with a deadline on a timer line | `OP_IRQ_BIND` |
| `Time` | `CAP_TIME` | none: the slot holds the first unit and a count | boot, `OP_TIME_CARVE` |
| `Debug` | `CAP_DEBUG` | none | boot |
| `Clock` | `CAP_CLOCK` | none: there is one counter | boot |

`Untyped`, `Frame`, `IrqLine`, `Time`, `Debug` and `Clock` capabilities have no kernel object behind them.
Carving a frame, a line range or a range of units, retyping an Untyped into a frame and splitting one,
are pure table operations
that touch no kernel memory,
which is why the root task can hand out memory, lines and time
before it has created a single pool.

### 5.4 Memory: Untyped, frames and installing

Every frame and every Untyped is a **block**:
its size is a power of two and its base a multiple of its size,
which is the range one PMP entry in NAPOT mode describes.
The smallest block is eight bytes, or the PMP's grain if that is coarser;
`OP_FRAME_INFO` returns it in `a4`.
A range of any other size is rounded up or made of several blocks,
one region slot each.

An Untyped is **free** while nothing made of it is left, and **made** while something is;
`OP_UNTYPED_INFO` says which in `a4`.
A free Untyped makes one thing of the whole of its memory:
`OP_UNTYPED_SPLIT` its lower and its upper half, two Untypeds,
`OP_UNTYPED_RETYPE` a frame or a pool,
and `OP_CAP_DERIVE` an Untyped of all of it, for lending, section 6.4.
A made one makes nothing (`KERR_NO_MEMORY`),
so what one Untyped makes never overlaps,
and a frame never overlaps a pool.
Once what it made is gone, whether deleted, destroyed or revoked, it is free again, the whole of it.

Where a block lies is the program's to choose, by the half it takes;
the kernel keeps no allocator.
To get a small block out of a large Untyped, halve it down,
and keep the halves not used yet: they are the program's free memory.
A half the program does not want to keep a slot for may be deleted;
what was made of it then hangs below its parent,
which is free again once all of that is gone.
A frame and an Untyped carry the rights of the Untyped they were made of.
`OP_FRAME_CARVE` hands out a block within a frame.
Programs that lay memory out with fixed offsets
should check the smallest size once and fail early if the layout does not fit.

Rules for installing a frame into a process:

- The rights installed must be a non-empty subset of the frame's rights.
- Write without read is refused,
  because PMP reserves that encoding and hardware may do anything with it.
- On ARM execute without read is refused too, since the MPU fetches only what a thread may read.
- Regions installed in one process may not overlap each other.
- The install takes effect at once, even for the running process.

PMP budget.
Every installed region is one NAPOT entry, so `n` regions cost `n` entries.
An install beyond the core's entries fails with `KERR_LIMIT`,
which with eight region slots happens only on a core with fewer entries for regions,
such as RP2350's Hazard3, with seven.

An access must lie entirely within one PMP entry.
Two regions that touch share a boundary
that no single load, store or instruction fetch may cross,
so a process's creator lays it out so that nothing straddles one.

Sharing costs no copy: installing the same range into two processes
gives both access, and no byte passes through the kernel.
What it does cost is a region slot and a PMP entry in each process.

### 5.5 Pools

A process hands memory to the kernel with `OP_UNTYPED_RETYPE` of a `CAP_POOL`,
which makes the whole of the Untyped a pool.
The Untyped must carry both read and write rights,
and be at least `POOL_MIN_SIZE`, 64 bytes.
The kernel places the pool's descriptor at its base
and returns a `KernelPool` capability with all rights.
The memory belongs to the kernel from then on;
no frame covers it, since what one Untyped makes never overlaps.
The kernel zeroes each object as it allocates it,
so what the process left in the memory never becomes part of an object.

Objects are bump-allocated from a pool, eight-byte aligned,
and never freed individually.
Sizes a developer needs for planning, as the kernel rounds them:

| Object | Bytes (16 PMP entries) |
|---|---|
| pool descriptor | 56 |
| `CapTable` with `n` slots | 12 + 24 × n, rounded up to 8 |
| `Process` | 312 (272 with `PMP_MAX_ENTRIES=8`) |
| `Thread` | 224 |
| `Notification` | 16 |
| `Irq` | 48 |

A minimal child process, table of 10 slots, process, thread and two notifications,
costs 880 bytes including the descriptor.

**Objects in different pools.**
A process's table, a thread's process and an `Irq`'s notification may each lie in any pool,
sections 5.2, 5.6 and 5.9:
each is held by a capability that a destroy of its pool clears.

**Capabilities to a pool.**
Every capability to an object hangs below the `KernelPool` capability it was allocated through,
and every `KernelPool` capability below the pool's own node,
which the kernel keeps in the pool and no process can name.
So revoking below a `KernelPool` capability takes what was allocated through it,
and deleting the last `KernelPool` capability leaves the pool standing,
reachable through nothing, until the Untyped it was made of is revoked.

**Destroying a pool** is revoking below the Untyped it was made of, or below one above that;
no operation on the pool does it, so a `KernelPool` capability only allocates.
A pool is the whole of its Untyped, so keep the Untyped of a pool you mean to give back on its own.
The destroy:

- destroys every object in it,
- clears every capability, in every table of every process,
  that names the pool or one of its objects,
- clears every slot of the pool's own tables as `OP_CAP_DELETE` does,
  so what was derived from those slots goes to their parents,
- wakes every thread waiting on a notification in the pool
  with `KERR_INVALID_CAP` and no bits,
- stops every thread, in whatever pool, that ran in a process in the pool,
- disarms every `Irq`, in whatever pool, bound to a notification in the pool,
  and masks its line, which stays bound,
- masks the interrupt line of every `Irq` in the pool, which frees the line,
- zeroes the memory its objects took and leaves the rest as it was,
  so clear memory before it becomes a pool if whoever destroys the pool should not read it,
- gives the memory back to the Untyped the pool was made of.

The revoke fails with `KERR_STATE`
when the calling thread, its process or the process's table lies in the Untyped's memory,
so a thread cannot destroy what it runs on
nor the table it names capabilities in,
and when the table it names the Untyped in does,
since the capability the call is made through would go half way through the destroy of that table's pool.
The boot pool holds the root task, which therefore cannot destroy it,
but any other thread holding `BOOT_CAP_POOL_RAM`, the block the boot pool was made of, can,
and the root task goes with it, section 7.
A destroy is restartable: it may stop for an interrupt and go on when the call is made again,
and a pool being destroyed allocates nothing, `KERR_STATE`.

A lender that wants memory back from a living borrower
lends it as an Untyped derived from its own
and revokes below its own, section 5.2.
That takes the borrower's Untyped, the halves, the frames it made and their mappings,
and destroys every pool the borrower made of it,
wherever the borrower passed it on and whatever it deleted in between,
and leaves the lender's Untyped free, the whole of it.

### 5.6 Threads

A thread is **stopped**, **ready** or **waiting**.

- A new thread is stopped.
- `OP_THREAD_CONFIGURE` sets a stopped thread's program counter and stack pointer.
  The other registers start at zero.
  The kernel validates neither value.
- `OP_THREAD_RESUME` makes a stopped thread ready.
  It gets the processor once it is bound to units, or to spare time, and its turn comes, section 5.11.
- A thread waiting on a notification is waiting;
  a signal makes it ready again.
- A thread that faults is stopped where it faulted; see **Faults** below.

A thread holds its process by a capability derived from the `Process` capability it was allocated with,
as a process holds its table, section 5.2, so the thread may lie in any pool.
Revoking below that capability, or destroying the pool the process lies in,
takes the process from the thread, and the thread stops for good:
`OP_THREAD_RESUME` refuses it with `KERR_STATE`.

Apart from its state, a thread is bound to units of time or it is not.
A new thread is not, and a thread that is not keeps its state and does not run:
`OP_TIME_BIND` binds it, and revoking below the capability it was bound through unbinds it.
That is how a started thread is stopped and started again.
No thread exits: a thread that has nothing left to do waits forever
on a notification nobody signals, or loses its units or its process,
or stops itself with a breakpoint, which is a fault its watch hears, below.

**Faults.**
A thread that faults, by an access fault, an illegal instruction, a misaligned access or a breakpoint,
is stopped where it faulted, with its registers as they were and its program counter at the instruction,
and the processor goes to the next thread; the machine goes on.
Nothing else changes: the process, the thread's units and the process's other threads are as they were.
The kernel writes `user fault` and the cause into its log, section 4.
`OP_THREAD_WATCH` gives a thread a **watch**, a notification and bits that its faults signal,
so whoever waits there hears of the fault as of any other signal;
one wait can carry a fault's bit next to a device's or a timer's,
and a watcher of many threads gives each a bit.
The thread holds the notification by a capability derived from the one named, as it holds its process,
so revoking below that capability, or destroying the notification's pool, clears the watch.
A thread without a watch stops at a fault all the same, and nobody hears.
What happens next is the watcher's to decide, with the operations there are:
`OP_THREAD_RESUME` runs the faulting instruction again, after installing the region it reached for, say;
`OP_THREAD_CONFIGURE` first starts it afresh;
taking its process ends it, and taking its units, which a stopped thread still earns, frees them.
The bits say which thread faulted, and `OP_THREAD_FAULT` says why while it stays stopped there.
A watcher that emulates the instruction writes the thread's registers with `OP_THREAD_WRITE_REG`,
the program counter past the instruction among them, and resumes it; section 6.8.

### 5.7 Notifications

A notification is one 32-bit word of sticky bits.

- `OP_NOTIFY_SIGNAL` ORs bits into the word and never blocks.
  Signalling zero bits is refused.
- `OP_NOTIFY_WAIT` returns every set bit and clears them,
  blocking until at least one is set.
  It never returns zero bits.
- If a thread is waiting when a signal arrives,
  that thread wakes with every bit set so far, and the word is cleared.
- A signal that arrives before a waiter got there is not lost.
- When several threads wait on one notification,
  one of them wakes and takes every bit; which one is not promised.

`RIGHT_W` allows signalling and `RIGHT_R` allows waiting,
so a client can be given the power to announce something
without the power to consume the announcement.

No data passes through the kernel.
Two processes exchange bytes through a region installed in both
and use notifications to say when.
The build targets `rv32imac`, so the `A` extension is there in user mode,
as `ldrex` and `strex` are on ARMv7-M,
and a lock or a ring buffer in shared memory is userspace's to build;
what userspace cannot build is "stop me until someone says otherwise",
and a notification is exactly that.

A round trip is therefore four system calls:
signal, wait on one side; wait, signal on the other.

A trap breaks the thread's reservation,
so an `sc.w` fails whenever a system call, the tick or an interrupt came between it and its `lr.w`;
a loop around the pair tries again, and never succeeds with a system call inside it.

**A server with many clients** waits on one notification;
each client holds it with `RIGHT_W` only, owns a bit,
and shares a region with the server.
One wake can carry several clients' work.
Which client owns which bit is a convention the kernel does not enforce,
so a client can waste the server a look but cannot forge data
or consume another client's wake.

### 5.8 Time

There is no sleep call and no yield.
Time reaches userspace as one more interrupt, on a timer line.

The machine has `TIMER_LINES` timer lines, 16,
and the root task receives all of them at boot in `BOOT_CAP_TIMER_LINES`,
apart from the controller's lines.
It carves them and hands them out as it does any other line, section 5.9,
so who holds how many timers is its choice.
A timer line is bound with `OP_IRQ_BIND` like a device's,
into an `Irq` in a pool, bound to a notification in any pool.
`OP_IRQ_SET` arms it with a set of bits and, in `a2`, a delay in microseconds;
when the delay has passed the line fires: the `Irq` signals those bits and disarms.
Setting an armed timer line moves its deadline and replaces its bits;
setting bits to zero cancels it.
A program that needs more timers than it holds keeps its own deadlines
and arms one line for the nearest.

The contract is a **lower bound**.
The kernel fires at the first tick that surely lies past the delay:
the delay rounded up to whole ticks, plus one.
With a millisecond tick, as on both boards,
a delay of zero fires at the next tick,
and a delay of 10 000 µs fires on the eleventh tick after the call,
between 10 and 11 ms later.
No upper bound is promised, because the woken thread is ready, not running,
and waits for its turn,
and for its account to reach a tick if it has spent it and has no spare time, section 5.11.

Sleeping:

```c
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, TIMER);

rv_timer_set(TIMER, BIT_TIMER, us);   /* OP_IRQ_SET with the delay in a2 */
rv_wait(NTFN, &bits);
```

A wait with a timeout is the same two calls with one more bit:
give the device one bit on the notification and the timer line another,
and look at which bits came back.

A timer line fires once, so a periodic task arms it again each time it wakes.
With `IRQ_SET_PERIOD` in `a3` the delay is a period counted from the line's last deadline,
or from its bind, not from the call, so the task keeps its period whatever each wake costs:
the line fires at the first tick after the call
that lies a whole number of periods from that deadline.
The call returns in `a1` how many such ticks had already passed,
the periods a task that woke late has skipped;
it can do their work at once, count them as overruns, or ignore them.
Setting it again before the line fired leaves the deadline where it was.
The period is rounded up to whole ticks and may not be zero.
Ticks keep pace with the clock's counter, so periods do too.

```c
uint32_t skipped;
for (;;) {
    rv_timer_period(TIMER, BIT_TIMER, 5000, &skipped);   /* every 5 ms */
    rv_wait(NTFN, &bits);
    /* one period's work, or skipped + 1 of them */
}
```

**The clock.**
`BOOT_CAP_CLOCK` names the machine's counter, 64 bits counting up from boot.
`OP_CLOCK_INFO` returns its rate in hertz and its address;
`OP_CLOCK_FRAME` derives a read-only frame holding it, which a creator installs like any frame.
Reading the counter is then a few loads and no system call:

```c
uint32_t hz, counter;
rv_clock_info(CLOCK, &hz, &counter);
rv_invoke(OP_CLOCK_FRAME, CLOCK, COUNTER_FRAME, 0, 0);
rv_invoke(OP_PROCESS_INSTALL, PROCESS, 5, COUNTER_FRAME, RIGHT_R);

uint64_t start = rv_counter_read(counter);
work();
uint64_t ticks = rv_counter_read(counter) - start;   /* ticks / hz seconds */
```

The rate is the board's, 10 MHz on QEMU, the CPU clock on the ESP32-C6 and 1 MHz on RP2350,
so a program takes it from `OP_CLOCK_INFO`.
The time is the machine's, not the thread's: it includes other threads' slices.
Revoking below a `Clock` capability uninstalls every region derived through it.
`rdtime` traps on every board.
The ESP32-C6's performance counter is no clock: the kernel stops it at zero whenever another process runs.

### 5.9 Interrupts

A driver holds an `IrqLine` capability naming one line.
The root task receives every line of the controller at boot, with the log's,
and every timer line in a second capability,
and carves single lines out with `OP_IRQ_CARVE`,
as it makes frames of memory.

`OP_IRQ_BIND` turns a one-line capability into an `Irq` object
in a pool of the driver's choosing,
bound to a notification in any pool.
The invoked slot is consumed.
A line is bound at most once; a second bind fails with `KERR_OVERLAP`
until the first `Irq`'s pool is destroyed.

An `Irq` holds its notification by a capability
derived from the `Notification` capability it was bound with,
as a thread holds its process, section 5.6.
Revoking below that capability, or destroying the pool the notification lies in,
takes the notification from the `Irq`, which is disarmed and its line masked for good:
`OP_IRQ_SET` refuses it with `KERR_STATE`, and the line stays bound until the `Irq`'s own pool goes.

An `Irq` on a device's line works like one on a timer line:

- `OP_IRQ_SET` with bits **arms** it, which unmasks the line at the controller.
- When the line fires, the kernel masks the line,
  disarms the `Irq` and signals the bits.
- The driver services the device and arms the `Irq` again,
  which is the acknowledgement: only the driver lifts the mask.
- `OP_IRQ_SET` with zero bits masks the line by hand.

An `Irq` is armed exactly while its line is unmasked,
so a level that stays high costs one trap and not a storm.
A device interrupt wakes its driver but does not run it;
the driver waits for its turn like a thread a timer line woke.

Sharing a line between drivers is not supported;
`DESIGN.md`, open decision 11.

### 5.10 The kernel log

The kernel writes every byte it prints, `OP_DEBUG_PUTC` included,
into a ring behind a header of `RVUOS_LOG_HEADER` bytes,
and never waits for a reader.
The ring's size is the kernel's `KLOG_SIZE`, 4 KiB less the header today;
the header reports it, so a reader need not assume it.
`BOOT_CAP_LOG` names the header and the ring;
the root task installs it like a device's registers and reads without a system call.

```c
struct rvuos_log {
    uint32_t head;   /* written by the kernel: count of every byte ever written */
    uint32_t size;   /* the ring's size in bytes */
    uint32_t taken;  /* written by the reader: count of bytes it has read */
};
```

Byte `n` lies at offset `RVUOS_LOG_HEADER + n % size`.
Once `head - taken` exceeds `size`, bytes have been overwritten
and the oldest byte still kept is `head - size`;
a reader that finds itself that far behind resets `taken` accordingly.

The log is a device with interrupt line `LOG_IRQ_LINE`, which is 0.
The line is level: high while `head` lies past `taken`.
A reader binds and arms it as it would a UART's,
and writes `taken` to acknowledge.
Arming while bytes are untaken signals at once.
The log's line cannot wake the machine from an idle stall:
only the kernel writes the log, and with no thread runnable
nothing runs that could make it write,
so an idle kernel with only the log's `Irq` armed halts with `no runnable thread`.
The line cannot be fired by `OP_DEBUG_IRQ` either.

`user/init.c` shows the reader:
a logger thread that waits on one notification with the log's bit and the UART's,
and carries the ring out one byte per transmitter interrupt.

### 5.11 Scheduling

- The processor is divided into `TIME_UNITS` units of time, 64 on the whole machine, each a sixty-fourth of it.
  The root task receives them all in `BOOT_CAP_TIME`, its own thread earning every one,
  and hands them out with `OP_TIME_CARVE` and `OP_CAP_DERIVE` as it hands out memory.
- `OP_TIME_BIND` binds a thread to some of a capability's units, or to none of them,
  and the thread **earns** them.
  A unit is earned by one thread at a time:
  a bind that names a unit another thread earns fails with `KERR_OVERLAP`,
  so copies of a `Time` capability may be held by many, and each unit still goes to one thread.
  The time of the units nobody earns is spare.
- A thread bound through a capability with `RIGHT_X` also runs on **spare time**:
  turns no thread with time wants, which cost its account nothing.
  The boot grant has `RIGHT_X`.
  A thread bound through a capability without it runs on its account alone,
  at most its units' part of the processor and a full account,
  and the processor sleeps in `wfi` for the rest if nothing else wants it.
  A thread bound to no units runs on spare time alone, or not at all without `RIGHT_X`.
- Each thread has an **account** of time,
  which gains a sixty-fourth of a tick for each unit the thread earns every tick
  and holds at most 100 ticks' worth of them, a tenth of a second.
  A turn the thread began with time costs the account the time it ran, counted on the machine's counter,
  whether the tick ends it or the thread waits;
  a turn on spare time costs nothing.
  A thread has **time** while its account holds at least a tick.
  A new thread's account is empty, and the root task's thread's starts full.
  Bound to other units, a thread keeps what its account held, up to what the new units hold;
  unbound, it loses it.
- Ready threads wait for the processor in three queues.
  A thread with time waits on the run queue;
  one without time waits on the spare queue if it may run on spare time,
  and for its account to reach a tick if not.
  A thread that becomes ready, whether resumed, woken, bound or preempted,
  joins the back of its queue.
- The machine timer ticks at `TIMER_HZ`, 1 kHz on both boards.
  A tick ends the running thread's turn:
  the thread goes to the back of its queue,
  and the thread that has waited longest on the run queue runs,
  or, if the run queue is empty, the one that has waited longest on the spare queue.
  A thread that waits hands the rest of the tick to the next, which pays for no more than it runs of it.
  A tick that would hand the processor back to the thread that had it takes no interrupt:
  the kernel counts and charges it at its next trap all the same,
  so a thread alone on the processor is interrupted only at a timer line's deadline,
  where another thread's account reaches a tick,
  or where its own account drains, if it may not go on on spare time.
- So threads with work and time take equal turns, and then threads on spare time do.
  A thread with work all along gets its units' part of the processor,
  give or take a tenth of a second's worth of the accounts.
  A thread with time waits for its turn at most `TIME_UNITS` ticks, since only a thread with units has time.
  A thread a process adds earns only units split off the process's own, or runs on spare time:
  it takes nothing another thread earns.
  Spare time goes round by thread, so more threads there get more of it.
- A system call is never interrupted:
  machine mode runs with interrupts off from the trap to the return.
- When nothing is runnable and an `Irq` is armed on a timer line or a device's line,
  or a thread waits for its account to reach a tick,
  the kernel stalls in `wfi` until the nearest timer line's deadline, the account that reaches a tick or the interrupt arrives,
  and takes no tick in between.
  When there is none of these, it prints `no runnable thread` and halts with code 5.

There are no priorities and no yield.
A spinning thread cannot starve the others, because the tick preempts it,
but it burns its turns and its account, and keeps the machine out of `wfi`
unless it is held to its units;
a thread that waits costs nothing until it is signalled.

A turn is charged by the counter, not by the tick that ends it,
so a thread that waits just before every tick pays for the time it ran all the same.

To hold a process to about a tenth of the processor, the root task gives it six units without `RIGHT_X`.
Its own thread earns every unit at boot, so it leaves the six first,
then carves them and lends them without spare time:

```c
rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, 58);
rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, 58, 6, TIME);
rv_invoke(OP_CAP_DERIVE, CHILD_TABLE, CHILD_TIME, TIME, RIGHT_W);
```

The child binds its threads through `CHILD_TIME` with `OP_TIME_BIND`;
`TIME` itself keeps `RIGHT_X`, so a bind through it would give spare time too.
Revoking below `TIME` takes the child's units back, and its threads stop.

## 6. System call reference

### 6.1 Invocation ABI

The numbers in this section are copied from `include/rvuos/abi.h`,
which is the binding contract.

Every system call is an `ecall` with:

| Register | On `ecall` | On return |
|---|---|---|
| `a7` | operation code, one of `OP_*` | unchanged |
| `a0` | slot of the invoked capability | status, one of `KERR_*`, zero on success |
| `a1` to `a3` | arguments | results, where the operation documents them; otherwise unchanged |
| `a4` | reserved | a result where documented, otherwise unchanged |
| `a5`, `a6` | reserved | unchanged |

On ARM a call is `svc #0`, with `r0` to `r6` for `a0` to `a6` and `r12` for `a7`,
which leaves `r7` to Thumb code as its frame pointer;
the core saves `r0` to `r3`, `r12`, `lr`, `pc` and `xpsr` on the thread's stack as it traps,
so a thread's `sp` must leave 32 bytes below it that the thread may write,
or the thread faults, and one whose stacking faulted loses those registers.

No operation takes a pointer into user memory,
and no operation moves data between processes.

The C wrapper in `user/rvuos.h`, the same on both, with the registers from `user/arch/<arch>/call.h`:

```c
uint32_t rv_invoke(uint32_t op, uint32_t cap, uint32_t a1, uint32_t a2, uint32_t a3);
```

The kernel checks in this order:
the slot resolves (`KERR_INVALID_CAP`),
the capability type carries `RIGHT_W` where the type needs it for every operation
(`KERR_NO_RIGHTS`),
the type accepts the operation (`KERR_WRONG_TYPE`),
the operation's own right and arguments.
Operation codes are a single flat numbering across all types,
so invoking an `Irq` operation on a `Frame` is `KERR_WRONG_TYPE`,
not `KERR_INVALID_ARG`.

A call whose work grows with the derivation tree or with a pool can be interrupted and made again:
`OP_CAP_REVOKE`, `OP_CAP_DELETE` of a root with capabilities below it,
and `OP_IRQ_BIND`.
When the tick or a device interrupt comes due while such a call works,
the kernel stops it between two capabilities or two objects
and resumes the thread at its `ecall`, or its `svc`, with every register as it was,
so the thread makes the same call again when it next runs,
and the call goes on from what it had already cleared.
A program sees no difference but time,
unless another thread changes the same capabilities in between:
the call made again checks everything afresh
and may fail, with what it already revoked staying revoked.

### 6.2 Status codes

| Code | Name | Meaning |
|---|---|---|
| 0 | `KERR_OK` | success |
| 1 | `KERR_INVALID_CAP` | slot out of range, empty, or its object was destroyed; every call of a process whose table was taken |
| 2 | `KERR_WRONG_TYPE` | the capability's type does not accept this operation |
| 3 | `KERR_NO_RIGHTS` | the capability lacks a right the operation needs |
| 4 | `KERR_INVALID_ARG` | an argument is out of range or breaks a rule stated below |
| 5 | `KERR_NO_MEMORY` | the pool has no room for the object, or the Untyped has made something already |
| 6 | `KERR_SLOT_IN_USE` | the destination slot already holds a capability |
| 7 | `KERR_OVERLAP` | a region overlaps another installed in the same process, or on the ESP32-C6 touches one as `OP_PROCESS_INSTALL` says, or the line is already bound |
| 8 | `KERR_LIMIT` | a fixed kernel limit was hit, such as the PMP entry count |
| 9 | `KERR_STATE` | the object is not in a state that allows this |

### 6.3 Operations on `Debug`

The `Debug` capability needs no particular right.

**`OP_DEBUG_PUTC` (1).**
`a1` = the byte.
Appends it to the kernel log.

**`OP_DEBUG_HALT` (2).**
`a1` = exit code.
Stops the machine; on QEMU the emulator exits with that code,
on the ESP32-C6 the core parks after printing it,
and RP2350 prints it and reboots into BOOTSEL.
Does not return.

**`OP_DEBUG_TRACE` (10).**
Turns tracing and self-checking on; see section 9.
Every thread's account is filled, section 5.11.
Cannot be turned off again,
and once tracing is on the call changes nothing.

**`OP_DEBUG_TICK` (17).**
Does what the timer tick does, on request:
time moves by one tick, charged to the caller if it had time as its turn began, every due timer line fires,
every thread whose account reached a tick has time again,
the caller's turn ends as section 5.11 says,
and the caller stays ready.
Works while tracing is on, unlike the tick itself.

**`OP_DEBUG_IRQ` (22).**
`a1` = a line.
Does what a device interrupt on that line does:
the `Irq` armed on it masks the line and signals its bits.
`KERR_STATE` when nothing is armed on the line;
`KERR_INVALID_ARG` for a line the controller does not have, line 0 included.

**`OP_DEBUG_PREEMPT` (32).**
`a1` = n.
The n-th place from now at which a restartable call could stop between two steps, it stops,
whether or not an interrupt is pending, and `OP_DEBUG_TICK` is done there:
time moves by one tick, and the caller's turn ends with the caller ready at its `ecall`,
so another thread may run before the caller makes its call again.
Every such place counts, in whichever thread's call;
a call that finishes first leaves the rest of n to the next one.
Zero disarms it, and a second call counts n anew.
Works while tracing is on, when the tick stops no call.

### 6.4 Operations on `CapTable`

All need `RIGHT_W` on the table.

**`OP_CAP_COPY` (3).**
`a1` = destination slot in the invoked table,
`a2` = source slot in the caller's table,
`a3` = rights mask ANDed into the copy.
The destination must be empty (`KERR_SLOT_IN_USE`).
The source may be any type, frames and lines included, but an Untyped (`KERR_WRONG_TYPE`).
The copy hangs beside the source in the derivation tree, section 5.2.

**`OP_CAP_DERIVE` (24).**
The arguments and errors of `OP_CAP_COPY`.
The new capability hangs below the source.
An Untyped can be derived, and only while it is free (`KERR_NO_MEMORY`), section 5.4:
the derived one is the whole of it, and the source makes nothing while that one is left.
So memory is lent: the lender keeps the source,
and a revoke below it takes back whatever the borrower made of the derived one.

**`OP_CAP_MOVE` (18).**
`a1` = destination slot in the invoked table,
`a2` = source slot in the caller's table, which must be filled (`KERR_INVALID_CAP`).
The destination must be empty (`KERR_SLOT_IN_USE`), so a slot does not move onto itself.
The capability goes to the destination with its rights, and the source is left empty.
It keeps its place in the derivation tree, section 5.2:
it hangs below what the source hung below, and what hung below the source hangs below it,
so a revoke through it takes what one through the source would have,
and a revoke that would have taken the source takes it.
Any type moves, an Untyped with what was made of it,
and so does the `CapTable` capability the call is made through.

**`OP_CAP_DELETE` (4).**
`a1` = slot in the invoked table.
Clears it; what hung below it now hangs below the slot's parent,
or, when the slot is a root, each becomes a root, and the call may be made again, section 6.1.
Clearing an empty slot succeeds.
Deleting a frame does not uninstall it anywhere,
and deleting a `KernelPool` capability does not destroy the pool.

**`OP_CAP_REVOKE` (23).**
`a1` = slot in the invoked table, which must be filled (`KERR_INVALID_CAP`).
Clears everything below it, section 5.2, and leaves the slot.
Regions installed from capabilities below it are uninstalled,
and threads of those processes lose access at once;
threads made through a `Process` capability below it stop, section 5.6,
and `Irq`s bound through a `Notification` capability below it are disarmed, section 5.9.
A revoke that takes the caller's own process, its process's table,
or the capability the call is made through ends there,
returns `KERR_OK`, and leaves the rest below the slot for another call to revoke.
Below an Untyped, every pool made of it is destroyed, section 5.5;
`KERR_STATE` if the calling thread, its process, the process's table or the invoked table
lies in the Untyped's memory.
The call may be made again, section 6.1.

### 6.5 Operations on `Frame` and `Untyped`

**`OP_FRAME_INFO` (11).**
No right needed.
Returns `a1` = base, `a2` = size, `a3` = rights,
`a4` = the size of the smallest region in bytes, a power of two of at least 8.

**`OP_FRAME_CARVE` (5).**
No right needed.
`a1` = offset from the frame's base, `a2` = size, `a3` = destination slot in the caller's table.
Produces a frame with the same rights, hanging below the invoked one.
The size must be a power of two no smaller than the smallest region,
the offset a multiple of the size,
and the sub-range within the frame (`KERR_INVALID_ARG`),
so the new frame is a block.
The parent capability is unchanged.

**`OP_UNTYPED_INFO` (27).**
No right needed.
Returns `a1` = base, `a2` = size, `a3` = rights,
`a4` = 1 while something made of it is left, 0 while it is free, section 5.4.

**`OP_UNTYPED_RETYPE` (6).**
`a1` = the type, `CAP_FRAME` or `CAP_POOL` (`KERR_INVALID_ARG`),
`a2` = destination slot in the caller's table.
Makes the whole of the Untyped into it, and returns `a1` = its base.
A pool needs `RIGHT_R` and `RIGHT_W` on the Untyped (`KERR_NO_RIGHTS`)
and an Untyped no smaller than `POOL_MIN_SIZE` (`KERR_INVALID_ARG`);
a frame needs no right and carries the invoked one's.
`KERR_NO_MEMORY` while something made of the Untyped is left.
The new capability hangs below the invoked one, a pool's below the pool's own node.

**`OP_UNTYPED_SPLIT` (30).**
No right needed.
`a1` = destination slot for the lower half, `a2` = for the upper half, two slots in the caller's table.
Makes the two halves of the Untyped, each an Untyped with the invoked one's rights, hanging below it.
A half must be no smaller than the smallest region, and the two slots must differ (`KERR_INVALID_ARG`).
`KERR_NO_MEMORY` while something made of the Untyped is left.

### 6.6 Operations on `KernelPool`

A pool has one operation, which needs `RIGHT_W` on it;
it is destroyed by revoking below its Untyped, section 5.5.

**`OP_POOL_ALLOC` (7).**
`a1` = object type, `a2` = destination slot, `a3` = type-specific:

| `a1` | `a3` | Rule |
|---|---|---|
| `CAP_CAPTABLE` | number of slots, 1 to `CAPTABLE_MAX_SLOTS` (1024) | |
| `CAP_PROCESS` | slot of the `CapTable` capability the process will use, with `RIGHT_W` | the table may lie in any pool; the process's hold on it hangs below that capability |
| `CAP_THREAD` | slot of the `Process` capability the thread will run in, with `RIGHT_W` | the process may lie in any pool; the thread's hold on it hangs below that capability; the thread starts stopped |
| `CAP_NOTIFICATION` | unused | |

Any other type is `KERR_INVALID_ARG`; `Irq` objects come from `OP_IRQ_BIND`.
The new capability carries all rights and hangs below the invoked `KernelPool` capability.
`KERR_NO_MEMORY` when the pool is full,
`KERR_STATE` while the pool is being destroyed.

### 6.7 Operations on `Process`

Both need `RIGHT_W` on the process.

**`OP_PROCESS_INSTALL` (8).**
`a1` = region slot index, below `PROCESS_REGION_SLOTS`,
`a2` = slot of a frame in the caller's table,
`a3` = rights to install.
The rights must be a non-empty subset of the frame's (`KERR_NO_RIGHTS`);
write without read is `KERR_INVALID_ARG`, and so on ARM is execute without read;
an occupied region slot is `KERR_SLOT_IN_USE`;
overlap with another region in this process is `KERR_OVERLAP`,
and so, on the ESP32-C6, is a writable region ending where a readable but not writable one begins;
too few PMP entries is `KERR_LIMIT`.
The installed region hangs below the frame in the derivation tree,
so `OP_CAP_REVOKE` on that capability's slot, or on any slot above it, uninstalls it.

**`OP_PROCESS_UNINSTALL` (9).**
`a1` = region slot index.
Clears it and rebuilds the PMP image.
Threads of the process lose access at once.
Clearing an empty slot succeeds.

### 6.8 Operations on `Thread`

All need `RIGHT_W` on the thread,
and `OP_THREAD_CONFIGURE`, `OP_THREAD_RESUME`, `OP_THREAD_READ_REG` and `OP_THREAD_WRITE_REG` a stopped thread (`KERR_STATE`).

**`OP_THREAD_CONFIGURE` (12).**
`a1` = program counter, `a2` = stack pointer.
Neither is checked.
On ARM the program counter is taken as a branch takes it, with bit 0 set for Thumb as a function pointer has it;
one without bit 0 starts the thread in a fault.

**`OP_THREAD_RESUME` (13).**
Makes the thread ready.
It runs once it is bound to units, or to spare time.
A thread that faulted runs the faulting instruction again, unless `OP_THREAD_WRITE_REG` moved its program counter.
`KERR_STATE` too once the thread's process was taken, section 5.6.

**`OP_THREAD_WATCH` (31).**
`a1` = slot of a `Notification` capability, which needs `RIGHT_W`, and may lie in any pool;
`a2` = the bits the thread's faults signal there.
Replaces the watch the thread had.
`a2` = 0 clears the watch and ignores `a1`.
See section 5.6.

**`OP_THREAD_FAULT` (33).**
What stopped a thread that faulted:
`a1` = the cause, `a2` = the program counter, `a3` = the address, `a4` = the status.

| | RISC-V | ARM |
|---|---|---|
| `a1`, the cause | `mcause` | the exception number |
| `a2`, the program counter | `mepc` | the pc, with bit 0 set for Thumb |
| `a3`, the address | `mtval`: on an access fault the address reached for, on RP2350 always zero | `MMFAR` or `BFAR` while `CFSR` says it is valid, `SFAR` for a SecureFault, else zero |
| `a4`, the status | zero | `CFSR` |

The program counter is where a resume goes on, as `OP_THREAD_READ_REG` reads it.
On QEMU's Cortex-M3 a `bkpt` arrives as a HardFault, exception 3, with a status of zero,
and on RP2350's Cortex-M33 as a DebugMonitor exception, 12.
A status with `MSTKERR` or `STKERR` says the core could not stack the thread's registers,
so `r0` to `r3`, `r12`, `lr`, the pc and `xpsr` hold what its last trap left.
`KERR_STATE` unless the thread is stopped where it faulted:
a resume or a configure since, or no fault at all, leaves nothing to tell.

**`OP_THREAD_READ_REG` (34).**
`a1` = the register: `x0` to `x31` on RISC-V, where `x0` reads zero, `r0` to `r14` on ARM,
or `THREAD_REG_PC` (32) for the program counter, which on ARM carries bit 0 for Thumb, as `OP_THREAD_CONFIGURE` takes it.
Returns `a1` = its value.
`KERR_INVALID_ARG` for a number that names no register.

**`OP_THREAD_WRITE_REG` (35).**
`a1` = the register, as for `OP_THREAD_READ_REG`; a write to `x0` changes nothing.
`a2` = the value.
A pc written is taken as `OP_THREAD_CONFIGURE` takes it, and on ARM leaves an IT block, keeping the flags.
The thread stays stopped where it faulted, and `OP_THREAD_FAULT` tells of it until the resume.
While tracing, the thread can no longer run, as after `OP_THREAD_CONFIGURE`; section 9.
`KERR_INVALID_ARG` for a number that names no register.

### 6.9 Operations on `Notification`

**`OP_NOTIFY_SIGNAL` (14).**
Needs `RIGHT_W`.
`a1` = bits to set, non-zero (`KERR_INVALID_ARG`).
Never blocks.

**`OP_NOTIFY_WAIT` (15).**
Needs `RIGHT_R`.
Blocks until some bit is set, then returns `a1` = the bits and clears them.
Returns `KERR_INVALID_CAP` with no bits
if the notification's pool is destroyed while the thread waits.

### 6.10 Operations on `IrqLine`

**`OP_IRQ_CARVE` (19).**
No right needed.
`a1` = offset from the first line, `a2` = count, `a3` = destination slot.
A table operation like `OP_FRAME_CARVE`.

**`OP_IRQ_BIND` (20).**
Needs `RIGHT_W`, and the capability must name exactly one line (`KERR_INVALID_ARG`).
`a1` = slot of the `KernelPool` capability to allocate the `Irq` from, with `RIGHT_W`,
`a2` = slot of the `Notification` capability the `Irq` signals, with `RIGHT_W`,
which may lie in any pool, and the `Irq`'s hold on it hangs below that capability, section 5.9,
`a3` = destination slot for the `Irq` capability, which may be the invoked slot.
The invoked slot is cleared with everything below it,
and the `Irq` capability hangs below the `KernelPool` capability.
`KERR_OVERLAP` if an `Irq` is already bound to the line,
`KERR_NO_MEMORY` if the pool has no room for it,
`KERR_STATE` while the pool is being destroyed,
both checked before anything is revoked,
and the revoke may make the call again, section 6.1.
The new `Irq` is masked until `OP_IRQ_SET` arms it.
A timer line binds the same way as a device's.

### 6.11 Operations on `Irq`

**`OP_IRQ_SET` (21).**
Needs `RIGHT_W`.
`a1` = bits the next interrupt signals; unmasks the line.
`a1` = 0 masks the line; it takes back no signal that already happened.
On the log's line, arming while bytes are untaken signals at once.
On a timer line, `a2` = the delay in microseconds, and the line fires
at the first tick that surely lies past it, section 5.8;
elsewhere `a2` is unused.
A delay longer than 32 bits of microseconds, about 71 minutes, is several calls.
On a timer line, `a3` = 0 or `IRQ_SET_PERIOD`, and unused elsewhere.
With `IRQ_SET_PERIOD`, `a2` is a period counted from the line's last deadline, section 5.8,
and the call returns `a1` = the periods skipped.
`KERR_INVALID_ARG` for any other bit of `a3`, or a period of zero.
With `a1` = 0, `a2` and `a3` are ignored.
`KERR_STATE` once the `Irq`'s notification was taken, section 5.9: it has nothing to signal.

### 6.12 Operations on `Clock`

The `Clock` capability needs no particular right.

**`OP_CLOCK_INFO` (25).**
Returns `a1` = the counter's rate in hertz, `a2` = the address of its low word;
the high word is at `a2 + 4`.
The rate does not change while the machine runs.

**`OP_CLOCK_FRAME` (26).**
`a1` = destination slot in the caller's table.
Produces a frame, read only, the smallest block that holds the counter,
hanging below the invoked capability.
On a PMP grain coarser than eight bytes the frame holds the registers beside the counter too.

### 6.13 Operations on `Time`

**`OP_TIME_CARVE` (28).**
No right needed.
`a1` = offset from the first unit, `a2` = count, `a3` = destination slot.
A table operation like `OP_IRQ_CARVE`.

**`OP_TIME_BIND` (29).**
Needs `RIGHT_W`.
`a1` = slot of the `Thread` capability, with `RIGHT_W`,
`a2` = the first unit, as an offset from the capability's first
(`KERR_INVALID_ARG` past the last, even for none),
`a3` = how many units (`KERR_INVALID_ARG` past the last), zero for none.
Fails with `KERR_OVERLAP` if another thread earns one of the units.
The thread leaves the units it earned, whoever bound it to them,
and earns these; with `RIGHT_X` on the invoked capability it runs on spare time too.
Its account keeps what it held, up to what the new units hold at most,
and a ready thread joins the back of the queue its account puts it on, section 5.11.
Its hold on the units hangs below the invoked capability,
so revoking below that capability unbinds it:
it keeps its state and does not run until it is bound again, and its account is emptied.
A thread unbound or moved while it runs, the caller itself for one, finishes the turn it had:
only a wait, the tick, a fault, section 5.6, or a revoke that takes its own process, section 6.4, takes the processor from it.

### 6.14 Operation codes in numeric order

| Code | Operation | Type |
|---|---|---|
| 1 | `OP_DEBUG_PUTC` | `Debug` |
| 2 | `OP_DEBUG_HALT` | `Debug` |
| 3 | `OP_CAP_COPY` | `CapTable` |
| 4 | `OP_CAP_DELETE` | `CapTable` |
| 5 | `OP_FRAME_CARVE` | `Frame` |
| 6 | `OP_UNTYPED_RETYPE` | `Untyped` |
| 7 | `OP_POOL_ALLOC` | `KernelPool` |
| 8 | `OP_PROCESS_INSTALL` | `Process` |
| 9 | `OP_PROCESS_UNINSTALL` | `Process` |
| 10 | `OP_DEBUG_TRACE` | `Debug` |
| 11 | `OP_FRAME_INFO` | `Frame` |
| 12 | `OP_THREAD_CONFIGURE` | `Thread` |
| 13 | `OP_THREAD_RESUME` | `Thread` |
| 14 | `OP_NOTIFY_SIGNAL` | `Notification` |
| 15 | `OP_NOTIFY_WAIT` | `Notification` |
| 17 | `OP_DEBUG_TICK` | `Debug` |
| 18 | `OP_CAP_MOVE` | `CapTable` |
| 19 | `OP_IRQ_CARVE` | `IrqLine` |
| 20 | `OP_IRQ_BIND` | `IrqLine` |
| 21 | `OP_IRQ_SET` | `Irq` |
| 22 | `OP_DEBUG_IRQ` | `Debug` |
| 23 | `OP_CAP_REVOKE` | `CapTable` |
| 24 | `OP_CAP_DERIVE` | `CapTable` |
| 25 | `OP_CLOCK_INFO` | `Clock` |
| 26 | `OP_CLOCK_FRAME` | `Clock` |
| 27 | `OP_UNTYPED_INFO` | `Untyped` |
| 28 | `OP_TIME_CARVE` | `Time` |
| 29 | `OP_TIME_BIND` | `Time` |
| 30 | `OP_UNTYPED_SPLIT` | `Untyped` |
| 31 | `OP_THREAD_WATCH` | `Thread` |
| 32 | `OP_DEBUG_PREEMPT` | `Debug` |
| 33 | `OP_THREAD_FAULT` | `Thread` |
| 34 | `OP_THREAD_READ_REG` | `Thread` |
| 35 | `OP_THREAD_WRITE_REG` | `Thread` |

`OP_COUNT` is 36, one above the highest code; 16 is unused.

## 7. What the root task starts with

The image in RAM contains the kernel followed by the root task.
The kernel builds the root process by hand in the **boot pool**,
4 KiB of the root task's own memory,
and drops into user mode with:

- the program counter at the start of the code region, `0x80100000` on QEMU,
- the stack pointer at the top of the data region, `0x80120000` on QEMU,
- region slot 0: the code region, read and execute,
- region slot 1: the data region, read and write,
- a capability table of `ROOT_TABLE_SLOTS`, 64, filled as below.

| Slot | Constant | Capability | Rights |
|---|---|---|---|
| 0 | `BOOT_CAP_NULL` | empty on purpose | |
| 1 | `BOOT_CAP_CAPTABLE` | the root task's own table | all |
| 2 | `BOOT_CAP_PROCESS` | the root task's own process | all |
| 3 | `BOOT_CAP_THREAD` | the root task's first thread | all |
| 4 | `BOOT_CAP_POOL` | the boot pool | all |
| 5 | `BOOT_CAP_DEBUG` | `Debug` | all |
| 6 | `BOOT_CAP_CODE` | `Frame`: the root task's code | read, execute |
| 7 | `BOOT_CAP_DATA` | `Frame`: the root task's data and stack | read, write |
| 8 | `BOOT_CAP_FREE_RAM` | `Untyped`: the block of RAM the board sets aside for the root task | all |
| 9 | `BOOT_CAP_INPUT` | `Frame`: test input the loader placed in RAM | read |
| 10 | `BOOT_CAP_IRQ_LINES` | `IrqLine`: line 0 (the log) and every controller line | write |
| 11 | `BOOT_CAP_UART` | `Frame`: the board's console registers, a 16550 on QEMU, the USB Serial/JTAG controller on the ESP32-C6, a block of RAM on RP2350 | read, write |
| 12 | `BOOT_CAP_LOG` | `Frame`: the kernel log's header and ring | read, write |
| 13 | `BOOT_CAP_TIMER_LINES` | `IrqLine`: every timer line, `TIMER_LINES` of them | write |
| 14 | `BOOT_CAP_CLOCK` | `Clock`: the machine's counter | all |
| 15 | `BOOT_CAP_TIME` | `Time`: every unit of time, `TIME_UNITS` of them, all earned by the root task's thread | write, execute |
| 16 | `BOOT_CAP_ROOT_RAM` | `Untyped`: the root task's own memory, its code, data and input frames and the boot pool's block, all made already | all |
| 17 | `BOOT_CAP_POOL_RAM` | `Untyped`: the boot pool's block, below `BOOT_CAP_ROOT_RAM`, made into the boot pool | all |

`BOOT_CAP_COUNT` is 18; a root task puts its own slots from there upwards.

The root task's own table, process and thread take about 2.1 KiB of the boot pool,
so roughly 1.9 KiB remain for objects the root task allocates from `BOOT_CAP_POOL`.
Anything larger goes into a pool the root task makes out of free RAM.
The capabilities to the boot pool, its table, process and thread
hang below the boot pool's own node, not below `BOOT_CAP_POOL`,
so revoking below `BOOT_CAP_POOL` takes only what was allocated through it.

`BOOT_CAP_ROOT_RAM` is an Untyped over the root task's own memory,
with its code, data and input frames and the boot pool's block, `BOOT_CAP_POOL_RAM`, right below it,
so it makes nothing until those are gone, and the rest of it, past the boot pool, waits for them.
The boot pool's own node hangs below `BOOT_CAP_POOL_RAM`, as a retype would have put it,
so revoking below that block destroys the boot pool and leaves the root task's code where it is.
The root task cannot revoke below either, since it lives there, and nothing else sets it apart:
a process holding its capabilities can do all it could.
So the root task can hand its place over:
it moves every capability into a successor's table with `OP_CAP_MOVE`, each to the same slot, and waits,
and the successor stops the root task's thread and revokes below `BOOT_CAP_POOL_RAM`,
which destroys the boot pool, and the root task with it.
`user/init.c` ends its demo this way; `DESIGN.md`, "The root task is its capabilities", gives the steps.

Device ranges are frames, granted read and write, never execute,
the counter behind `BOOT_CAP_CLOCK` read only,
and no Untyped covers them, so they can never become a pool.
Neither can the log, which is a frame too.

Which capability sits in which slot is a convention between loader and program,
not something the kernel enforces.
The `BOOT_CAP_*` slots are the kernel's own instance of that convention
for the one program it starts itself.

## 8. Writing a program

### 8.1 Toolchain and layout

A user program is a flat binary linked against `user/user.ld.S`,
with the board's addresses, see "Boards",
and embedded into the kernel image by the Makefile.
Constraints of the current linker script:

- code and read-only data live in the read-execute region, at `0x80100000` on QEMU,
- zero-initialised data and the stack live in the read-write region, at `0x80110000` on QEMU,
- **initialised data is forbidden**: nobody would copy it into the data region,
  and the link fails if `.data` is not empty,
- `user/arch/<arch>/start.S` zeroes `.bss`, calls `main`,
  and halts with code 1 through `BOOT_CAP_DEBUG` if `main` returns.

Compile flags are `-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib`,
or `--target=thumbv7m-none-eabi -mcpu=cortex-m3 -mfloat-abi=soft -ffreestanding -nostdlib` on ARMv7-M,
with `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33+nodsp+nofp` in place of the first two on ARMv8-M.
There is no libc; `user/rvuos.h` provides the system call wrappers:

| Wrapper | Operation |
|---|---|
| `rv_invoke(op, cap, a1, a2, a3)` | any |
| `rv_frame_info(cap, &base, &size)` | `OP_FRAME_INFO` |
| `rv_frame_min_size(cap, &min)` | `OP_FRAME_INFO`, reading `a4` |
| `rv_untyped_info(cap, &base, &size, &made)` | `OP_UNTYPED_INFO` |
| `rv_retype(cap, type, dst, &base)` | `OP_UNTYPED_RETYPE` |
| `rv_split(cap, lower, upper)` | `OP_UNTYPED_SPLIT` |
| `rv_signal(cap, bits)` | `OP_NOTIFY_SIGNAL` |
| `rv_wait(cap, &bits)` | `OP_NOTIFY_WAIT` |
| `rv_timer_set(cap, bits, us)` | `OP_IRQ_SET` on a timer line, with the delay |
| `rv_timer_period(cap, bits, us, &skipped)` | `OP_IRQ_SET` on a timer line, with `IRQ_SET_PERIOD` |
| `rv_irq_set(cap, bits)` | `OP_IRQ_SET` |
| `rv_putc(cap, c)`, `rv_puts(cap, s)`, `rv_put_hex(cap, v)` | `OP_DEBUG_PUTC` |
| `rv_halt(cap, code)` | `OP_DEBUG_HALT` |

To replace the demo, edit `user/init.c` or add a program to `USER_PROGRAMS` in the Makefile;
each program becomes its own kernel image.

### 8.2 Building a second process

Today one program is embedded, and a second process runs code
from the same code region with its own data and stack.
Loading a separate binary is a userspace job the root task does not do yet;
`DESIGN.md`, open decision 3.
The steps below are what `user/init.c` does.

1. **Take memory** out of `BOOT_CAP_FREE_RAM`:
   a frame to share, a frame for the child's data and stack,
   and a pool for its kernel objects.
   The demo keeps no allocator: `take_untyped` splits what is left of the free RAM,
   keeps the lower half as the block and the upper as what is left,
   so its blocks halve one after another, section 5.4.
   It deletes the Untypeds in between, so every block hangs right below the free RAM
   and three working slots do for all of them.
   `take_frame` makes the block a frame and deletes its Untyped, since revoking below a frame takes it back;
   the pool's Untyped stays, since revoking below it is what destroys the pool.

   ```c
   take_frame(SLOT_SHARED, &shared_base, &shared_size);
   take_frame(SLOT_CHILD_DATA, &child_data_base, &child_data_size);
   take_untyped(SLOT_POOL_MEMORY, &pool_base, &pool_size);
   rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base);
   ```

2. **Allocate** the child's objects from the pool,
   table first, then the process that uses it, then a thread in it.

   ```c
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_PROCESS, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0);
   ```

3. **Install regions** into the child: code, data, and the shared chunk.

   ```c
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 1, SLOT_CHILD_DATA, RIGHT_R | RIGHT_W);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 2, SLOT_SHARED, RIGHT_R | RIGHT_W);
   ```

4. **Fill the child's table** with exactly the authority it should have,
   narrowing rights as you go.

   ```c
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG,  BOOT_CAP_DEBUG, RIGHT_ALL);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED,    RIGHT_R | RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP,     SLOT_UP,        RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN,   SLOT_DOWN,      RIGHT_R);
   ```

5. **Configure, bind and start** the thread.
   The stack grows down from the top of the child's data frame.
   The thread earns half the processor, units carved out of the boot grant.
   The root task's thread earns every unit at boot, so it keeps the first half and leaves the rest;
   binding the child through `BOOT_CAP_TIME` with no units would give it spare time alone,
   which it gets only while the root task does not want the processor.

   ```c
   rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
             child_data_base + child_data_size, 0);
   rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS / 2, TIME_UNITS / 2, SLOT_CHILD_TIME);
   rv_invoke(OP_TIME_BIND, SLOT_CHILD_TIME, SLOT_CHILD_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0);
   ```

   Revoking below `SLOT_CHILD_TIME` stops the child's thread; binding it again starts it.
   The demo gives the child no watch; section 5.6 says how a parent hears its child fault.

6. **Talk** through the shared region and the two notifications.
   The child writes a word, signals `SLOT_UP`; the parent waits, answers, signals `SLOT_DOWN`.

7. **Tear down** by revoking below the pool's Untyped, which destroys the pool.
   The child's thread, process, table and notifications go with it,
   every capability to them is cleared in every table,
   and the Untyped is free again, to be retyped into something else.

   ```c
   rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_POOL_MEMORY, 0, 0);
   ```

The child must touch no global variable, since it shares no data region with the parent:
everything it needs is on its stack or behind a capability in its table.

### 8.3 Writing a driver

A driver needs a frame for the device's registers and one interrupt line.

```c
rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W);
rv_invoke(OP_POOL_ALLOC, POOL, CAP_NOTIFICATION, NTFN, 0);
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, UART_IRQ, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, IRQ);

for (;;) {
    rv_irq_set(IRQ, BIT_UART);      /* arm: unmask the line */
    rv_wait(NTFN, &bits);           /* the interrupt masked it and signalled */
    /* service the device */
}
```

Arming again after servicing is the acknowledgement.
Give a timer line a second bit on the same notification for a timeout.

## 9. Debugging and testing interfaces

**Tracing.**
`OP_DEBUG_TRACE` makes the kernel print one line per system call,

```
trace: op=0x00000007 slot=0x00000004 a1=0x00000007 a2=0x0000000d a3=0x00000000 -> 0x00000000
```

with `blocked` in place of a status when the call blocked,
and `trace: wake <bits>` after a call that woke a waiter,
or after the `user fault` of a thread whose watch woke one.
A call stopped where `OP_DEBUG_PREEMPT` armed prints `trace: preempt` instead of its line,
followed by what the tick woke,
and its line comes once the call is made again and finishes.
After every traced call the kernel runs the self-check,
the executable form of the properties in `DESIGN.md`,
and halts with code 3 on the first violation.
While tracing is on the tick preempts nobody and moves no time;
only `OP_DEBUG_TICK` does,
so that the host build can reproduce the transcript.
The clock that charges turns is then the tick count alone:
a turn with time pays a whole tick at each `OP_DEBUG_TICK` and nothing at a wait.
A thread whose program counter was set while tracing was on,
or any of whose registers `OP_THREAD_WRITE_REG` wrote then,
cannot be run under tracing; the kernel halts with code 6 instead.

**The self-check** (`kernel/selfcheck.c`) verifies, among other things:
no installed region or frame overlaps a pool,
and what one Untyped made lies within it and overlaps nothing else it made,
every process's PMP image matches its region slots,
every capability names a live object of its own type or an in-bounds frame, Untyped or line range,
and every capability to a pool or an object lies below the pool's own node,
pools tile their memory exactly,
every waiting thread names a live notification,
every ready thread but the running one that may run is on the queue its account says,
and each unit of time is earned by the thread bound to it and by no other,
every `Irq` names a live notification, or none and is disarmed,
every thread's watch names a live notification, through a capability that could signal it,
no `Irq` armed on a timer line is past its deadline,
and the controller forwards exactly the lines an `Irq` is armed on.

**Replay input.**
`BOOT_CAP_INPUT` maps 64 KiB where QEMU's loader places a file:

```c
struct replay_header { uint32_t magic; uint32_t count; };  /* magic "RVFZ", 0x5a465652 */
struct replay_record { uint8_t op; uint8_t actor; uint16_t slot; uint32_t a1, a2, a3; };
```

Each record is one system call and the thread that makes it:
`actor` 0 is whichever thread runs, 1 the driver's root thread, 2 its second thread,
3 a third that starts stopped until a record resumes it,
and lies in the root thread's pool while it runs in the second's process.
The replay driver in `user/fuzzdrv.c` performs them in order,
the host build in `host/` performs the same records natively under ASan and UBSan,
and `tests/differential.py` requires the two transcripts to match line for line.
A driver thread the records unmapped faults, and the next goes on with the records;
every driver thread ends in a breakpoint, a fault too, once the records are done,
so a transcript ends in `no runnable thread` unless something stopped it first.
Hand-written scenarios live in `tests/seeds`,
what the fuzzer found in `tests/corpus`,
and `tests/mutants/` holds one planted bug per invariant.

## 10. Limits

| Limit | Value | Where |
|---|---|---|
| region slots per process | 8 | `PROCESS_REGION_SLOTS` |
| PMP entries the kernel uses | 16, or `PMP_MAX_ENTRIES` | Makefile |
| minimum PMP entries to boot | 4 | `kernel/main.c` |
| slots per `CapTable` | 1 to 1024 | `CAPTABLE_MAX_SLOTS` |
| root task's table | 64 slots | `ROOT_TABLE_SLOTS` |
| boot pool | 4 KiB | `BOOT_POOL_SIZE` in `kernel/layout.h` |
| smallest pool | 64 bytes | `POOL_MIN_SIZE` |
| object alignment | 8 bytes | `OBJ_ALIGN` |
| notification bits | 32 | the word size |
| timer lines | 16, on the whole machine | `TIMER_LINES` |
| units of the processor's time | 64, on the whole machine | `TIME_UNITS` |
| most of its units an account holds | 100 ticks' worth, 100 ms (`ACCOUNT_TICKS`) | `kernel/timer.h` |
| timer delay or period per call | 2^32 - 1 µs | `OP_IRQ_SET` |
| tick | 1 ms (`TIMER_HZ` 1000) | `kernel/timer.h` |
| interrupt lines | 96 on QEMU, 77 on the ESP32-C6, 52 on RP2350, 32 on mps2-an385, line 0 the log's | `IRQ_LINES` |
| smallest region | 8 bytes, or the PMP grain if coarser, 32 bytes on ARM | `region_min_size` in `kernel/pmp.h` |
| kernel log ring | 4 KiB less a 32-byte header | `KLOG_SIZE` |
| replay records per input | 256 | `REPLAY_MAX_RECORDS` |

## 11. What is not there yet

These are documented gaps, not surprises;
`TODO.md` carries the items and `DESIGN.md` the open decisions.

- **A watcher on ARM cannot set the flags**, nor step a thread past an instruction inside an IT block;
  `DESIGN.md`, open decision 23.
- **No priorities and no yield.**
  Round-robin on a tick, threads with time before threads on spare time,
  is the whole policy, and units promise their part of the processor and a turn within 64 ticks, no more;
  `DESIGN.md`, open decisions 9 and 10.
- **No badged notifications.**
  A client's identity to a server is a convention, not kernel-enforced;
  open decision 6.
- **No synchronous endpoints.**
  Shared memory and notifications carry everything; open decision 5.
- **One `Irq` per line**; open decision 11.
- **ARM with no escape suite and no replay yet**, under QEMU and on RP2350's Cortex-M33; `TODO.md`.
- **No boot from flash.**
  The ESP32-C6 and RP2350 run from RAM, loaded by their ROMs over USB.
- **No loader.**
  The image embeds one program, linked at fixed addresses;
  a second process runs code from the same region.
- **No initialised data** in user programs, until something copies it.
- **The log across a reset** is not yet trustworthy; open decision 12.
