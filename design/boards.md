# rvuos design: boot, boards and architectures

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Boot

The image contains the kernel followed by the root task.
How it reaches RAM is the board's; see "Boards".
The kernel:

1. makes the board ready, which on the ESP32-C6 means watchdogs off
   and the radio's analog power on,
   and on RP2350 its clocks set and a watchdog armed,
   and sets up its own stack and trap vector,
2. discovers the PMP entry count and grain by writing the CSRs and reading them back,
   and fences off the entries the core hardwires open to user mode, if it has any,
3. carves its own static state out of a small fixed SRAM range,
4. constructs the root process by hand, including a `KernelPool`
   in a block of the root task's own memory,
5. hands the root task an `Untyped` for that memory, `BOOT_CAP_ROOT_RAM`,
   with its code, data and input frames and the boot pool below it,
   an `Untyped` for the block of RAM the board sets aside for it,
   frames for the UART's registers,
   to the kernel's log
   and to each device the board lists, on the ESP32-C6 a window onto flash among them,
   the machine's counter as a `Clock`,
   every line of the interrupt controller with the log's line before them,
   and every unit of the processor's time, with its own thread earning them all,
6. programs the tick, masks every interrupt line,
   starts the other cores, which idle until a thread is bound to their units, see "Cores",
   and drops to user mode into the root task.

A device range is granted read and write, or read alone where a write could do harm, never execute,
but for the ESP32-C6's window onto flash, read and execute;
the counter read only,
and can never become a pool:
it is a frame, and no Untyped covers it,
because the kernel writes objects into a pool,
which on a device would drive its registers from machine mode.
The two Untypeds the root task starts with cover RAM and nothing else.

Every range the root task is granted is a block,
so a board lays RAM out in blocks and what lies in none of them stays unused.

The slots the root task finds filled are listed in `include/rvuos/abi.h`
as the `BOOT_CAP_*` constants.
Slot zero is left empty so that an uninitialised index fails.
The devices' frames come last, from `BOOT_CAP_DEVICES` up, as many as the board lists, `BOOT_DEVICES`,
so `BOOT_CAP_COUNT`, where the root task's own slots start, is the board's,
and its table, `ROOT_TABLE_SLOTS`, has 46 slots past them on every board.

Everything after that is policy set by the root task.
The kernel does not know what a driver, a file system,
or a shell is.

### The root task is its capabilities

After boot the kernel names the root task nowhere.
The root task lives in memory it holds: `BOOT_CAP_ROOT_RAM` is an Untyped
with its code, data and input and the boot pool's block, `BOOT_CAP_POOL_RAM`, right below it,
what a loader leaves that halved an Untyped down to them, let the halves between go
and kept the block it made the pool of, so that the pool can go alone.
So it makes nothing while they are there,
and the rest of its memory, past the boot pool, is its holder's again once they are gone.
It cannot revoke below that memory or below the boot pool's, since it lives there,
as no process can what it runs on.
Before, the boot pool lay in a range the linker reserved, below no Untyped,
and its destroy was refused by name.
Everything else the root task does it does through its table,
so a process holding the same capabilities in the same places of the derivation tree can do all it could.

That lets the root task hand its place over and end, as a bootloader chains to the next stage.
`user/init.c` does it at the end of the demo:

1. The root task builds a successor out of free RAM, as it builds any process.
2. It moves every capability it holds into the successor's table with `OP_CAP_MOVE`, each to the same slot,
   the capability it moves them through last, and waits on a notification it kept.
   A copy would not do: what the root task lent hangs below its own capabilities,
   and an Untyped is never copied.
3. The successor stops the root task's thread for good,
   binding it to no units through a capability without `RIGHT_X`,
   and revokes below `BOOT_CAP_POOL_RAM`, which destroys the boot pool,
   the root task's thread, process and table and everything else allocated there.
4. It copies its own table, process and thread into the slots the destroy emptied,
   so a program written as a root task runs on as one.

The successor in the demo runs the root task's code, so it keeps those frames;
one whose code lies elsewhere revokes below `BOOT_CAP_ROOT_RAM` instead,
which unmaps them wherever they are and gives it the whole block back.
Only a holder of `BOOT_CAP_POOL_RAM` or `BOOT_CAP_ROOT_RAM` can destroy the root task:
a capability to the boot pool allocates from it and nothing more; see open decision 19.

## Boards

A board is the files of `kernel/board/<board>/` and `user/board/<board>/`,
and of a directory beside them that boards of one family share, as the MPS2 boards share `mps2/`,
chosen with `make BOARD=<board>`:
`board.h`, where RAM, the root task, the console and the timer's registers lie,
how many interrupt lines there are and which mcause the controller raises,
whether the core checks a misaligned store's second word for reading, `PMP_SPLIT_STORE_AS_READ`,
which `kernel/layout.h` and both linker scripts read,
whether it transposes R and X in `pmpcfg`, `PMP_CFG_RX_TRANSPOSED`,
and the finest grain it may have, `PMP_GRAIN_MIN`;
`board.c`, what the board needs before anything else and the CSRs it sets back for each process;
`timer.c`, which starts the timer, and on ARM has the counter the tick compares with;
`irq.c`, but on ARM, where the NVIC is the architecture's, and `halt.c`;
`console.h`, the device behind `BOOT_CAP_UART` as the root task drives it;
`devices.h`, the slots of the devices the boot grants, as many as `board.h` lists;
and `csrs.h`, the user-mode CSRs the demo checks the kernel sets back.
Nothing else in the kernel names an address.

**Devices.**
A board lists, in `DEVICE_RANGE_LIST`, the devices a driver in user mode may have beyond the console,
each a block with its rights, and the boot grants each as a frame, from `BOOT_CAP_DEVICES` up in the list's order,
which the root task carves and lends as it does RAM; `DESIGN.md`, open decision 25.
The kernel keeps the ranges after the boot's other grants, so what is made of them is checked as the rest is.
A program's `devices.h` says how many there are and names their slots, in the order of the kernel's `board.h`,
and the two must agree.
RP2350 lists the pins' functions and pads and the three PIO blocks, which the Wi-Fi system of `user/wifi/` drives;
the ESP32-C6 lists the SAR ADC and the whole of its modem, in one frame,
since a process's eight regions cannot hold the modem's blocks one by one,
where `tools/phymap.py` finds what ESP-IDF's PHY and Wi-Fi libraries reach, through the ROM's functions too,
the eFuse's registers, read only, so that no program burns a fuse,
a window onto flash, read and execute, for a program larger than its SRAM,
the ROM, read and execute, whose functions those libraries call,
the random number generator's data register, read only, the eight bytes of LPPERI that hold it,
since the rest of LPPERI reaches the LP domain's clocks and resets,
and the IO MUX and the GPIO matrix, the pins, which a program sets as its board wires them,
as the Wi-Fi system's root task sets the XIAO's RF switch;
QEMU lists none, since its devices are the kernel's or the console, and the host build has no hardware;
nor do the MPS2 boards yet.
So on QEMU `BOOT_CAP_COUNT` is 18, the number the seeds and the corpus are written for.
A device that is a bus master reaches whatever its driver points it at, so its frame is the machine's:
the ESP32-C6's modem is the one a board lists, for a driver the root task trusts with all RAM; open decision 13.
Nor does it list the devices the kernel drives itself, the timer under the tick and the watchdog:
a program reaches those through the clock's operations, which every board can answer; see "The watchdog".

**QEMU `virt`** is the development target and the one `make check` runs.
QEMU loads the image and enters it in machine mode.
`make CORES=2` builds the kernel for two harts, which QEMU starts with `-smp 2`;
`make smp-test` runs the demo on them, which goes on to the second core.
Under `-icount` QEMU runs the harts in turns, one until the next deadline of any timer or until it waits,
and another hart's software interrupt does not cut that turn short,
so the demo sets a timer before it spins for what another core does.
A hart that spins for the kernel's lock spins out its turn too, with no timer near up to 50 ms, half of QEMU's own 100 ms kick,
so a trap on one hart may wait that long for the other,
and the demo checks only that the second core answers before a timer, not how soon.

**ESP32-C6** runs the demo root task and its transcript check, `make test BOARD=esp32c6`.
The chip's ROM loads the image's segments into SRAM over USB and enters it,
so nothing is written to flash; `tools/esp32c6-run.py` drives that.
Everything the image carries must lie below `0x4086ad08`,
where the ROM keeps its buffers while it loads;
the kernel takes all 512 KiB once it runs, laid out as `manual/targets.md` shows.
The console is the chip's USB Serial/JTAG controller,
a CDC-ACM port on its own USB connector.
The ROM leaves the SAR ADC's registers in reset, where every write is dropped, and its clocks off;
the kernel takes it out and starts them, since `board.h` lists it.
The kernel maps 1 MiB of the flash through the cache's MMU at `0x42000000`, a window `board.h` places,
which `tools/esp32c6-run.py` attaches before the jump, as a bootloader does;
the Wi-Fi system's driver, too large for the SRAM, is to run from it.
Before anything else the kernel turns off the four watchdogs the ROM leaves running,
which stay off however the kernel's own watchdog is fed,
and the access permission management units.
Those silently refuse the CPU in user mode every peripheral,
reads returning zero and writes dropped;
ESP-IDF turns them off at startup too.
PMP is what confines a process, so rvuos loses nothing;
they could confine the modem's DMA too, which open decision 13 leaves to trust.
The core has user-mode traps, the N extension,
and the ROM leaves `mideleg` at `0x111`,
which delegates the user software, timer and external interrupts to a handler in user mode;
the kernel clears it, so every trap is its own.
User mode can also write CSRs, and nothing shuts it out of them:
the N extension's `ustatus`, `uie`, `utvec`, `uepc` and `ucause`,
the performance counter through `0x800` to `0x802`, user mode's names for `mpcer`, `mpcmr` and `mpccr`,
which the TRM leaves out,
and the dedicated GPIO at `0x803` and `0x805`.
The ROM leaves the counter counting cycles.
The kernel sets them all back at boot.
Whenever another process's thread runs, it keeps the counter with the process that leaves and gives the one that comes its own,
stopped at zero for a process that never started it, since the ROM's delays count on it;
the rest it sets back, so nothing passes through any of them from one process to the next; see decision 22.
`uscratch` faults in user mode, whatever the TRM says, and `uip` takes no write with nothing delegated.
Measured on the chip: sixteen PMP entries, all unlocked after the ROM,
a four-byte grain, TOR, NAPOT with an entry of RAM's size, `mtval` on access faults,
PMA entries all zero, which leave every range its default attributes,
and machine interrupts taken in user mode whatever `mstatus.MIE` says.

A misaligned access that crosses a word is split in two,
and the second word is checked for writing if a store comes next and for reading otherwise.
So a load followed at once by a store faults wherever the two regions differ in rights,
and faults again when resumed: Espressif's erratum DIG-694, which lists nothing more.
And a store followed by anything else writes up to three bytes of a region above it
that the process may read but not write, and raises nothing.
Fetches, AMOs, and parts that land where the process may not read fault as they should;
a fault on the second word reports the access's address plus four in `mtval`.
So the install keeps such regions apart, see "Region slots",
and the layout leaves a block between the log and the code and one between the data and the input.
A program compiled for strict alignment meets neither half.

**mps2-an385** is QEMU's model of ARM's MPS2 board with the AN385 image, a Cortex-M3, and ARM's development target:
`make BOARD=mps2-an385 test` runs the demo root task of `user/init.c` there, the same program as on RISC-V,
`make BOARD=mps2-an385 escape` the escape suite in ARM's instructions,
and `make check` runs both.
QEMU loads the image into SSRAM1, where the core's reset finds the vector table,
and the halt leaves QEMU through semihosting, which only privileged code reaches.
The layout is QEMU virt's, moved to SSRAM1 at 0;
the console is UART0 of the CMSDK, whose transmitter latches its interrupt as each byte leaves.
The clock's counter is the FPGA's `COUNTER`, 32 bits at 25 MHz with the prescaler above it,
so it wraps every 171 seconds;
the kernel counts the wraps for its tick and for `OP_CLOCK_READ`, see `kernel/board/mps2/timer.c`.
Measured under QEMU 8.2: eight MPU regions and a bkpt taken as a HardFault, DebugMonitor or not.
QEMU 8.2's `wfe` only yields to another core; from QEMU 11.0 it sleeps until an event,
and only an external line becoming pending is one there, not SysTick, as it is on RP2350's Cortex-M33.
So the MPS2 boards set `SYSTICK_WAKES_WFE` to 0, and the kernel's idle wait polls on `yield` there,
as it did on QEMU 8.2; see `intr_wait` in `kernel/arch/arm/trap.c`.

**mps2-an521** is QEMU's model of the same board with the AN521 image,
an SSE-200 with two Cortex-M33s, and ARM's development target of ARMv8-M:
`make arm-test` runs the demo there after mps2-an385's, on one core and then on two, `CORES=2`,
and the escape suite on one,
the only check of PMSAv8 and of the Cortex-M33 short of RP2350.
The two boards share their UART, counter and halt, in `kernel/board/mps2/` and `user/board/mps2/`.
QEMU loads the image into SSRAM1 and resets the first core, which finds the vector table at INITSVTOR0's reset value,
SSRAM1's Secure alias at `0x10000000`.
The kernel and every thread run Secure, so the board names every address by its Secure alias.
The layout is mps2-an385's, moved there; UART0 raises its combined line, 42, and the counter counts at 20 MHz.
The SSE-200 puts every device behind a peripheral protection controller,
which refuses unprivileged accesses, reading zero and dropping writes,
unless the Secure Privilege Control block lets them through;
`board.c` lets user mode through to UART0 and the counter, the devices the root task is granted,
and the MPU confines a thread to its frames there as everywhere.
The NVIC has 124 lines, of which the kernel takes the first 48, every UART's among them,
so that its vector table stays 256 bytes.
The second core waits in CPUWAIT until `board_cores_start` writes the kernel's vector table to INITSVTOR1 and lets it go,
each core reads its number in CPU_IDENTITY,
and MHU0 interrupts one from the other on line 6 of its NVIC; see "Cores".
Measured under QEMU 8.2: eight Secure MPU regions on each core, a bkpt taken as a HardFault as on mps2-an385,
SysTick counting the system clock as the counter does,
and `wfe` handing the processor to the other core rather than waiting,
so a core that idles or waits on the other loops, and a demo of a few seconds takes a few more of the host's.

**RP2350** runs the demo root task and the escape suite on its Hazard3 cores, `make BOARD=rp2350 test escape`,
and on its Cortex-M33 cores, `make BOARD=rp2350 ARCH=arm test escape`, on one core, or on both with `CORES=2`.
It is one board with two architectures:
`ARCH` picks the cores, and the files that differ by them lie in `kernel/board/rp2350/<arch>/`.
The bootrom's BOOTSEL mode takes the image's segments into SRAM over USB and reboots into them,
on the cores the ELF is for, finding the image by the block `image.S` puts at `RAM_BASE`;
`tools/rp2350-run.py` drives that, and reads the transcript the halt writes.
Core 1 waits in the bootrom until a kernel built for both cores launches it through SIO's FIFOs, see "Cores".
Each core reaches SIO on a bus of its own, and sees its own copy of some of it at the same address:
on Hazard3 its `mtimecmp`, `CLINT_MTIMECMP_LOCAL` in `board.h`, which raises that core's timer interrupt,
and on the Cortex-M33 its number in `CPUID`.
Hazard3's cores raise each other's software interrupt through SIO's `RISCV_SOFTIRQ`, on no line,
and the Cortex-M33s ring each other's doorbell in SIO, on line 26, `SIO_IRQ_BELL`, which the kernel then keeps.
Each core has its own interrupt controller, Hazard3's CSRs or the NVIC.
A watchdog armed at boot reboots the chip into BOOTSEL after about seventeen seconds,
so a run that hangs comes back without a hand on the board;
each feed of the kernel's watchdog sets it to a second past that one's deadline, and nothing else feeds it,
so only a run that feeds the kernel's watchdog lasts longer.
The halt times its port on TIMER0, which counts on either kind of core.
Measured on an A2 chip: eight PMP entries before three hardwired ones,
a 32-byte grain the probe does not see, `mtval` always zero,
and misaligned accesses that raise misaligned exceptions rather than split.
The hardwired PMP entries leave every peripheral and the Non-secure bank of SIO to user mode,
so the kernel fences them off, see "Physical Memory Protection",
and a process reaches a peripheral only through a frame,
and only where ACCESSCTRL lets user mode in too, which at reset it does for few.
TIMER0 is opened, for the clock; `escape-store-clock` stores to it through no frame, and faults.
So are the devices of `DEVICE_RANGE_LIST`, which the boot also takes out of reset,
and on Hazard3, whose user mode is Non-secure on the bus, every pin is opened to Non-secure access,
without which IO_BANK0 and PADS_BANK0 show user mode none.
A device whose clock the kernel leaves off never leaves reset, and the boot waited for the ADC's for ever,
so the ADC is not listed.

On the Cortex-M33 the bootrom enters the image's vector table in the Secure state,
which the kernel and every thread keep, see "Architectures",
and ACCESSCTRL's reset value already lets Secure unprivileged code into TIMER0.
The MPU confines a thread's reach to peripherals as to RAM, so there a peripheral is a process's only through a frame.
The clock's counter is the kernel's counter too, and the compare is SysTick at 150 MHz, clk_sys.
Measured there: eight Secure MPU regions,
a SysTick becoming pending that wakes `wfe` with `SEVONPEND`,
and a MemManage's fault address register that reads its own address, `0xe000ed34`, once the status is cleared,
so the kernel reads the address first.
The escape suite measured more, as QEMU's model has it too:
a misaligned access across a region's end faults with the address of the first byte past it, not the access's own,
and a call whose frame the core cannot stack is taken as the MemManage, not as the call.
The bootrom leaves a stack limit set and its redundancy coprocessor on; the kernel resets both first, on either core.
The exclusive pair reaches the other core only on memory the MPU marks Shareable:
with the MPU off, two cores taking a million tickets each came to a million and some,
and the demo on both cores hung in two runs of ten, until the ticket borrowed a region, see "Cores";
the demo's own adds from both cores lost some too, until a process's RAM was marked Shareable.
Hazard3's `amoadd` came to two million.

On either kind of core the bootrom leaves the ROSC four times faster than at reset, at a random frequency,
behind a divider of four on `clk_ref`, and the kernel switches `clk_ref` to the crystal before dropping the divider;
dropping it first ran `clk_ref` and `clk_sys` that fast for a moment,
which hung Hazard3 on about one boot in five, and the Cortex-M33 in none of about ten,
so that no watchdog brought the chip back, the one that counts `clk_ref`'s ticks among them.
The crystal gets six milliseconds to start, as the pico-sdk gives it.

## Architectures

An architecture is the files of `kernel/arch/<arch>/` and `user/arch/<arch>/`,
chosen by the board: `riscv` for QEMU virt, the ESP32-C6 and RP2350's Hazard3,
`arm` for mps2-an385's Cortex-M3, ARMv7-M, and mps2-an521's and RP2350's Cortex-M33, ARMv8-M's Mainline.
`arm` is both, and `ARMV8M` in its `arch.h` says which the compiler builds for;
the MPU and the Security state are where they differ.
The kernel's objects, its capabilities and every operation are the same on both,
and so is `user/init.c`, which runs the same demo to the same transcript.
What differs is what a trap is, which registers carry a call, how a region is written to the hardware,
and which counter and compare the tick is made of;
`arch.h` says the first two to the rest of the kernel,
the architecture's `start.S` and `trap.c` take the trap,
its `frame.c` starts a thread, reports a fault and reads and writes a stopped thread's registers,
and `user/arch/<arch>/call.h` is the call as `rvuos.h` makes it.
A program written against `rvuos.h` is the same source on both, never the same binary.

**Why the MPU takes the model as it is.**
A PMSAv7 region is a power of two of at least 32 bytes, aligned to its size, which is a NAPOT block,
so every frame is one region as it is one PMP entry, and the image in `struct pmp_image` keeps PMP's encoding:
`kernel/arch/arm/mpu.c` implements `pmp.h`, entry i as region i, and reads a region back as the entry that made it,
so the self-check of the image and of what the hardware holds is the same code on both.
Had regions stayed TOR, open decision 8, ARMv7-M could not have held them.
PMSAv8, ARMv8-M's, takes a base and a limit on 32 bytes, so it holds every block as one region too,
and `mpu.c` reads a base and limit back as the block that made them.
The kernel runs with the MPU off, as machine mode runs outside PMP:
`mpu_kernel` turns it off as a trap begins and `mpu_thread` on as `frame_give` ends,
with the default memory map behind the regions, `PRIVDEFENA`, for the kernel's last instructions before the thread.
PMSAv8's regions bind privileged code too, and none lets the kernel write where the thread only reads, as into the log,
so on ARMv8-M the kernel could not run with them on.
PMSAv8 also faults an access two regions match, which no process has, since its regions may not overlap.
A region takes its memory type from where it lies, normal memory in RAM and a device elsewhere, so a frame carries none;
a device is nGnRnE on PMSAv8.
The MPU fetches only what it lets the thread read, so a region may not be execute only there,
`EXECUTE_NEEDS_READ` in `arch.h`, as no region anywhere may be write only.
The smallest region is 32 bytes, `PMP_GRAIN_MIN`, which `OP_FRAME_INFO` returns as on RP2350.

**Modes and traps.**
The kernel runs in handler mode and a thread in thread mode, unprivileged, on the process stack.
On ARMv8-M both stay in the Secure state the boot ROM entered in:
the kernel turns the attribution unit off with nothing Non-secure, shuts every coprocessor to both states,
the floating-point unit among them, so a thread keeps no state beyond its registers,
and takes a SecureFault as a thread's fault as it takes a MemManage.
`MSPLIM` bounds the kernel's stack there, and `PSPLIM` nothing.
Every exception and line has one priority, so none preempts another and the kernel runs as it does in machine mode,
with interrupts held off;
only a fault of the kernel's own nests, as a HardFault, and halts.
An interrupt taken while the kernel runs would have to preempt it, so `wfi` would not wake there:
`intr_wait` waits in `wfe`, with `SEVONPEND` making a line becoming pending an event, and polls otherwise.
The architecture makes any exception becoming pending an event, SysTick too, as the Cortex-M33 has it;
QEMU 11 wakes `wfe` for a line alone, so the ARM demo waits there for ever; see `TODO.md`.
Only becoming pending is an event: a SysTick left pending makes none when its next reload runs out.
A deadline further than one reload reaches, 112 ms at RP2350's 150 MHz,
woke the wait at the first reload and never again, and RP2350's Cortex-M33 slept through a driver's 250 ms;
so the wait sets SysTick for what is left whenever it finds it pending short of the deadline,
and the demo sleeps 130 ms once with nothing else to run, which hangs the wait without that on RP2350,
and on mps2-an521 when the sleep is longer than its 0.84 s.
The kernel leaves thread mode the first time through PendSV,
which kmain pends and lets in with interrupts, as another core does as it starts, and which nothing pends after.
A call is `svc`, two bytes, with the operation in `r12`, since Thumb code keeps `r7` as its frame pointer;
`trap.c` moves the pc back onto the `svc`, so the rest of the kernel sees a call as on RISC-V,
resumes past it and restarts at it.
A thread's pc is taken as a branch takes it, bit 0 for Thumb, so a function pointer is a valid entry,
and the root task starts at `ENTRY_PC(USER_CODE_BASE)`.

**The frame on the thread's stack.**
The core saves r0 to r3, r12, lr, pc and xpsr itself, on the thread's own stack, with the thread's rights,
and restores them from there on the way back; that half of the frame lies in user memory.
The rule that the kernel never dereferences an address userspace chose, "Physical Memory Protection",
holds as follows.
The kernel reads the hardware frame only right after the core wrote it,
so it reads memory the thread could write and nothing else;
after a stacking fault the core wrote nothing, and the thread loses those eight registers,
which keep what its last trap left, and a call whose stacking faulted is dropped, not taken for the next thread.
The kernel writes the frame back only where the thread's process holds a region it may read and write, in RAM,
a walk of the eight region slots;
a device is left out, since the kernel's store is privileged and a device may let it do what it keeps from the thread.
Where the frame cannot go, psp points at the kernel's own stack instead,
the core's unstacking faults there with the thread's rights,
and the thread stops with its frame as it was, as any fault stops it.
So a sp that points anywhere costs its thread a fault and nothing more,
and a thread's sp must always leave 32 bytes below it in memory it may write, as a thread's stack does.

**The tick and the controller.**
SysTick, which every Cortex-M has, is the compare: a 24-bit down-counter,
set for what is left before the compare and set again each time it fires until the counter reaches it,
`kernel/arch/arm/systick.c`;
the counter is the board's, since SysTick's own count lies in the System Control Space, which user mode never reaches,
and a process reads the clock through its call or the counter's frame.
The NVIC is the controller: the line a trap enters on is active and no longer pending, so `irq_claim` hands it out first,
and a line is unmasked with its stale pending state dropped, since the NVIC latches a level masked or not.

**What checks it.**
Both link checks read Thumb-2, `tools/kthumb.py`, and the ARM kernel links only as the RISC-V one does,
for ARMv7-M and for ARMv8-M alike.
A fault of the kernel's own nests, and the core pushes its 32-byte frame on the kernel's stack first,
which `tools/stack-depth.py` does not count; `kernel_trap` takes the stack back at once and halts.
The host build compiles the kernel with RISC-V's frame, as QEMU virt has it,
so the fuzzer and `make qemu-replay` see the portable kernel and not `kernel/arch/arm/`,
which only the link checks, the demo and the escape suite exercise, under QEMU and on RP2350's Cortex-M33,
PMSAv8 mps2-an521's under QEMU and RP2350's, and the demo the second core of each.
Beside RISC-V's scenarios the suite tries the frame on the thread's stack,
a call the core cannot stack and one it cannot unstack;
`TODO.md` says what that leaves unchecked.
