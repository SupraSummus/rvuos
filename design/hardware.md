# rvuos design: the hardware model

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Hardware model

This section is RISC-V's, where rvuos began;
"Architectures" says what ARM has in each place instead.

### Privilege modes

The kernel runs in machine mode.
Processes run in user mode.
Supervisor mode is not used even when the core has it,
so the same kernel runs on cores that lack it.

All traps, including interrupts, land in machine mode.
The kernel is the only code that runs in machine mode.
The trap vector is in vectored mode on every board,
a table whose every entry jumps to the one entry point,
because the ESP32-C6 has no direct mode.
User mode runs with `mstatus.MIE` set, from the `MPIE` the kernel sets before its first `mret`:
the specification takes machine interrupts in user mode whatever `MIE` holds,
but RP2350's Hazard3 takes none there while it is clear, erratum RP2350-E7.

### Physical Memory Protection

Isolation rests entirely on PMP.
PMP is a small set of CSR pairs, `pmpaddrN` and `pmpcfgN`,
each describing a physical address range
and the read, write, and execute permissions that apply to it
while the core is in user mode.
Machine mode is not subject to PMP
unless an entry is locked,
and rvuos does not lock entries.

Consequences that shape the design:

- **The entry count is a hard, small budget.**
  The privileged specification allows up to 16 entries in version 1.11
  and up to 64 in version 1.12.
  Real microcontrollers ship with 4 to 16.
  A process therefore has a small fixed number of *region slots*,
  not a page table.
- **Every region is a NAPOT block.**
  A region's size is a power of two and its base a multiple of its size,
  which is what one NAPOT entry describes.
  A region costs one entry wherever it lies,
  so an install fits while the process has fewer regions than the core has entries,
  and two regions are either disjoint or one contains the other.
  The price is rounding: a range that is not a power of two
  is rounded up or made of several blocks, one slot each.
  Open decision 8 records why this replaced TOR.
  The ESP32-C6 faults user fetches under a NAPOT entry for the whole address space,
  which rvuos writes only as RP2350's fence, below, since no region is larger than RAM.
- **The smallest region is eight bytes, or the grain.**
  A platform rounds every PMP boundary to a grain of `2^(G+2)` bytes,
  one value per hart, and ignores the address bits below it.
  QEMU and the ESP32-C6 have four bytes, RP2350 has 32.
  The smallest NAPOT block is eight bytes; four would need NA4, which rvuos does not use.
  The kernel probes the grain at boot, taking none finer than the board's `PMP_GRAIN_MIN`,
  since RP2350's address registers do not show theirs and the probe finds four bytes there,
  and a `Frame` or `Untyped` that is not a block of at least that size never exists:
  the boot layout is checked once and carving and retyping require it,
  so every frame is exactly one PMP entry.
  Whether it may be installed depends on nothing but the process it goes into,
  since no frame overlaps a pool; see "Kernel pools and revocation".
  `OP_FRAME_INFO` returns the smallest region, since user mode cannot read the PMP CSRs.
- **An access must lie within one entry.**
  The lowest-numbered entry that matches any byte of an access decides,
  and it must match every byte or the access fails,
  whatever either entry grants.
  Two regions that touch share a boundary
  that no single load, store or instruction fetch may cross;
  the creator lays out a process so that nothing straddles one.
  A core may split a misaligned access and check each part on its own, though, and both boards do,
  so a misaligned load across two readable regions may go through,
  and a store that faults may leave its first part written.
  The ESP32-C6 checks the second part for the wrong kind of access; see "Boards".
- **The CSRs are WARL and may be hardwired.**
  RP2350 fixes entries 8 to 10 to its ROM, peripheral and SIO ranges, with every right for user mode,
  in the silicon, not by its bootrom:
  their configuration is read only, though their address seems to take a write.
  The probe at boot writes each entry's mode, with no rights, and then its address, and reads them back,
  and the budget is the leading run of entries that take both, eight on RP2350, less the fence where there is one.
  RP2350 also transposes R and X in every configuration field, erratum RP2350-E6,
  which `pmp.c` swaps on the way to the CSRs and back.
- **What the core hardwires past the run is fenced off.**
  The lowest-numbered entry that matches decides,
  so where an entry past the run is still on after the probe and grants a right, as RP2350's three do,
  the last entry of the run becomes the fence: NAPOT over the whole address space, with no right.
  An access no region holds faults there before any hardwired entry sees it,
  so a device is a process's only through a frame, and the budget is seven on RP2350.
  `process_fence` writes it once at boot, and loading an image stops below it, so a switch costs nothing more.
  The datasheet suggests the same; ACCESSCTRL, the other way it names, shuts a peripheral to every process at once.
- **Context switch cost is the PMP reload.**
  Switching processes rewrites every `pmpaddr` and `pmpcfg` in use.
  Threads within one process share regions
  and switching between them touches no PMP state.
- **Each hart has its own.**
  The CSRs are a hart's, so only that hart can change what its user mode reaches,
  and a region taken from a process another hart runs stays there until that hart traps; see "Cores".
  The kernel takes the harts as alike: the first probes the entries and the grain, and each fences what the first did.
- **Sharing is free.**
  Granting a region to another process installs the same physical range
  into one of that process's slots.
  Bulk data never passes through a kernel copy.
- **The kernel must validate every user pointer.**
  Machine mode bypasses PMP,
  so a bad pointer from userspace would let the kernel
  read or write anything.
  rvuos avoids the problem rather than solving it:
  system call arguments travel in registers,
  no operation takes a pointer,
  and no operation moves data between processes,
  so the kernel never dereferences an address userspace chose.
  The one place it writes user memory at all
  is a range that becomes a pool, where it writes its objects,
  and that range belongs to the kernel from that moment on.

### Timer

The machine timer, `mtime` and `mtimecmp`, is the kernel's clock
and the kernel handles it directly.
Userspace sees it only through the timer lines,
whose deadlines are counted in ticks; see "Time".
Where the registers live, what starts the counter and how fast it counts are the board's business:
`board.h` names the two registers,
and `timer.c`, per board as `irq.c` is, starts the counter and says how many counts make a tick.
The tick is the same on every board and lives once in `kernel/timer.c`,
on a counter and a compare below it,
which `kernel/arch/riscv/clint.c` makes of `mtime` and `mtimecmp` on every RISC-V board.
The kernel counts ticks on the counter rather than on interrupts:
every trap counts the whole periods `mtime` has passed since the last tick it counted,
so the tick count keeps pace with the counter however late or seldom the interrupt is taken,
and a long system call costs one late tick and not a burst of them.
`mtimecmp` is set only for the first tick that could change what runs; see "Scheduling".
The counter also says how far into the tick a change of turn falls, which is what a turn is charged by.
QEMU `virt` counts at 10 MHz in a SiFive CLINT,
which has an `mtimecmp` for each hart, one after the other, and a word for each hart's software interrupt,
which one hart raises to make another trap or wake; see "Cores".
The ESP32-C6 has a CLINT of Espressif's,
whose counter and interrupt stay off until a control word starts them,
and which counts at the CPU clock, 160 MHz, which the kernel sets at boot, see "Boards",
then measures one tick of it against the 16 MHz system timer rather than trust the dividers.
RP2350's is the RISC-V platform timer in the Secure bank of SIO,
counting the microseconds a tick generator divides from the crystal.

A process reads the counter through a read-only region at `COUNTER_ADDR`, not with `rdtime`:
the ESP32-C6 has no `time` CSR, but its CLINT has `UTIME`, a read-only copy of `mtime` for user mode.
On QEMU the kernel clears `mcounteren` at boot, so `rdtime` traps on every board; see "Time".
User mode does not reach RP2350's SIO timer, so there the counter is TIMER0's, counting the same microseconds;
TIMER0 keeps its high half below its low one, so the word above the counter is not its high half; see `TODO.md`.

### Interrupt controller

Device interrupts reach the core as one signal, the machine external interrupt,
and an interrupt controller in front of it says which line it was.
The kernel owns the controller as it owns the timer:
it masks every line at boot,
and a line is unmasked only while an `Irq` object is armed on it; see "Interrupts".
The controller is the board's business, so `kernel/irq.c` is per board too.
QEMU `virt` has a PLIC: a line is claimed by reading a register,
which hands over the highest-priority pending line and holds it,
and completed by writing the line back.
The kernel masks the line between the two,
so a level that stays high, as a device's does until it is serviced,
does not come back before the driver asks for it.
The kernel gives every line the same priority,
so which of several pending lines is claimed first is not promised.
It enables lines in the first hart's machine-mode context alone, and only the first hart takes the external interrupt.
The PLIC has no source 0, and the kernel gives that number to its log,
whose line no controller raises; see "The kernel log".
QEMU's PLIC model does not recompute what it forwards on an enable write;
its `irq.c` says what it does about that.

The ESP32-C6 has no PLIC in that sense.
Each peripheral drives a source of the interrupt matrix,
the matrix routes a source to one of 31 CPU interrupts or to none,
and the core takes CPU interrupt `n` with mcause `n`.
rvuos makes a line a source, numbered as Espressif numbers them,
routes every unmasked line to one CPU interrupt, 2,
and masks a line by routing it nowhere.
A claim reads the matrix's status words, which show every source's level,
for the lowest line that is high and routed;
nothing is held between claim and completion,
since masking the line already lowers the CPU interrupt.
Source 0, the Wi-Fi MAC's, is shadowed by the log's line and cannot be bound.

RP2350's Hazard3 keeps its controller in CSRs of its own, the Xh3irq extension:
arrays of one bit per system IRQ that enable it, show it pending and force it,
each reached through a 16-bit window.
rvuos makes a line an IRQ, gives every IRQ the same priority,
and a claim takes the lowest line both pending and enabled;
as on the ESP32-C6, nothing is held between claim and completion.
IRQ 0, TIMER0's, is shadowed by the log's line.

### One address space

There is a single physical address space shared by everything.
Processes are linked at fixed addresses chosen by whoever builds the image,
or built as position-independent code.
RISC-V code is naturally PC-relative,
so position independence costs little,
and the root task can place processes wherever memory is free.

Code normally executes in place from flash.
Data, stacks, and shared regions live in SRAM.
A process's regions are typically:
a read-execute code region in flash,
a read-write data region in SRAM,
and zero or more shared or device regions.
