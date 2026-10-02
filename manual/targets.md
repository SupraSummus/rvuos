# rvuos user manual: targets, building and running

Chapters 3 and 4 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

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

Five boards are supported, chosen with `make BOARD=<board>`:
`qemu`, QEMU `virt` for RV32, the default,
`esp32c6`, an Espressif ESP32-C6,
`rp2350`, a Raspberry Pi RP2350 on its RISC-V cores, or with `ARCH=arm` on its Cortex-M33 ones, as on a Pico 2,
`mps2-an385`, QEMU's model of ARM's MPS2 board with a Cortex-M3,
and `mps2-an521`, QEMU's model of the same board with the AN521 image, two Cortex-M33s.
A board picks its architecture, whose files lie in `kernel/arch/<arch>/` and `user/arch/<arch>/`;
RP2350 has both, and keeps the files that differ between them in `kernel/board/rp2350/<arch>/`.
Everything board-specific lives in `kernel/board/<board>/`,
`board.h`, `board.c`, `irq.c`, `timer.c` and `halt.c`,
with no `irq.c` on ARM, whose controller is the architecture's,
and in `user/board/<board>/console.h`,
which drives the device behind `BOOT_CAP_UART` for the demo and the replay driver;
boards of one family share what they have alike in a directory beside theirs, as the MPS2 boards share `mps2/`.
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
| 26 | SIO's doorbell; on the Cortex-M33 with `CORES=2` the kernel's, for its cores, and an `Irq` cannot be bound to it |

`OP_DEBUG_FRAME` makes frames within these ranges, which no boot capability grants;
the kernel takes each device out of reset at boot and opens it to user mode in ACCESSCTRL,
and on the RISC-V cores opens every pin to Non-secure access, which is how user mode reaches the bus there:

| Range | Size | What | Rights |
|---|---|---|---|
| `0x40028000` | 16 KiB | IO_BANK0, the pins' functions, overrides and interrupts | read, write |
| `0x40038000` | 16 KiB | PADS_BANK0, the pins' pads | read, write |
| `0x50200000`, `0x50300000`, `0x50400000` | 16 KiB each | PIO0, PIO1 and PIO2 | read, write |

Each is a device's registers and their atomic aliases, and none is a bus master.
The ADC is not listed: its clock is not running, and it never leaves reset without one.

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

With `CORES=2` the kernel starts the second core at the end of the boot, on either kind of core,
and a process has the same regions on both, where an atomic operation on RAM reaches the other core;
every device line reaches both cores' controllers, of which only the first core's enables any.

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

#### mps2-an521

QEMU loads the image into SSRAM1 and resets the first Cortex-M33,
which finds the kernel's vector table at `0x10000000`, SSRAM1's Secure alias;
the second waits until a kernel built for both starts it, and the halt leaves QEMU through semihosting.
The kernel and every program run Secure, so every address is a Secure alias, with bit 28 set.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x50302018` | 8 B | the FPGA's `COUNTER`, 20 MHz, read only through `BOOT_CAP_CLOCK`; the word above it is the prescaler, zero |
| `0x50200000` | 4 KiB | UART0 of the CMSDK, granted to the root task |
| `0x10000000` to `0x100FF000` | just under 1 MiB | kernel code, data and stack, the vector table first |
| `0x100FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x10100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x10100000` | 64 KiB | root task code, read and execute |
| `0x10110000` | 64 KiB | root task data and stack, read and write |
| `0x10120000` | 64 KiB | input region, read only; nothing fills it yet |
| `0x10130000` | 4 KiB | the boot pool |
| `0x10200000` | 2 MiB | free RAM, granted to the root task as an Untyped with all rights |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; the Non-secure watchdog's reset cannot be bound |
| 1 to 47 | the NVIC's first lines, the SSE-200's 31 and the board's first 16; UART0's combined line, its transmitter's among it, is 42 |
| 6 | MHU0's; with `CORES=2` the kernel's, for its cores, and an `Irq` cannot be bound to it |

The tick is 1 kHz, on the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide, so `rv_counter_read` gets a zero high word and the counter wraps every 214 seconds.
The SSE-200 refuses user mode every device unless its Secure Privilege Control block lets it through,
which the kernel does for UART0 and the counter alone;
a program reaches either only through a frame, as it reaches RAM.
UART0 is mps2-an385's, and a fault is reported as there.
With `CORES=2` the kernel starts the second core at the end of the boot,
and every device line reaches both NVICs, of which only the first core's enables any.

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
make smp-test    # boot the demo on two harts of QEMU and check its transcript, the second core's tests among it
make mutants     # plant each bug under tests/mutants/ and require the checks to catch it
make arm-test    # boot the demo on mps2-an385 and mps2-an521, there on one core and on two, and check its transcript
make check       # test, escape, host-test, qemu-replay, arm-test and smp-test; run before committing
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.
Images go under `build/<board>/`, the host build under `build/host/`;
a smaller budget adds `-pmp<n>` to both.
`make CORES=2` builds the kernel for two harts of QEMU `virt`, under `build/qemu-smp2/`,
and `make CORES=2 run` boots it with `-smp 2`;
`make BOARD=mps2-an521 CORES=2` builds it for that board's two cores, under `build/mps2-an521-smp2/`,
and `make BOARD=rp2350 CORES=2` for RP2350's, under `build/rp2350-smp2/`, or `build/rp2350-arm-smp2/` with `ARCH=arm`.
No other board takes `CORES`.

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
make BOARD=rp2350                              # build/rp2350/kernel-init.elf
make BOARD=rp2350 run                          # load it into RAM and print the transcript the halt writes
make BOARD=rp2350 test escape                  # check the transcripts
make BOARD=rp2350 ARCH=arm test escape         # the same on the Cortex-M33, from build/rp2350-arm/
make BOARD=rp2350 CORES=2 test escape          # the same on both Hazard3 cores, which goes on to the second
make BOARD=rp2350 ARCH=arm CORES=2 test escape # and on both Cortex-M33s
```

`tools/rp2350-run.py` loads the image through the bootrom's PICOBOOT interface,
waits for the halt's serial port and reads it; closing it reboots the chip into BOOTSEL.

On ARM, under QEMU:

```
make BOARD=mps2-an385                # build/mps2-an385/kernel-init.elf
make BOARD=mps2-an385 run            # boot the demo root task
make BOARD=mps2-an385 test escape    # the same, and check the transcripts
make BOARD=mps2-an521 test escape    # the same on QEMU's Cortex-M33, from build/mps2-an521/
make BOARD=mps2-an521 CORES=2 test   # the same on its two Cortex-M33s, which goes on to the second core
```

Only the demo and the escape suite are built; the replay driver's layout is QEMU virt's.

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
