# rvuos user manual: targets, building and running

This file holds chapters 3 and 4 of the user manual.
[`MANUAL.md`](../MANUAL.md) lists all chapters,
and explains the words this chapter uses.

## 3. Hardware requirements and targets

The kernel needs a RISC-V core with:

- machine mode and user mode (supervisor mode is not used even when present),
- PMP with NAPOT addressing mode and at least four entries;
  this is the kernel's own minimum, and it refuses to boot with fewer,
- the `A` extension in user mode, if programs want atomics in shared memory,
- a machine timer (`mtime` and `mtimecmp`),
- an interrupt controller in front of the machine external interrupt.

Cores without PMP cannot run rvuos.
TOR is not needed.
`DESIGN.md`, open decision 8, records why rvuos uses NAPOT alone.

The build is `rv32imac`, `ilp32`, compiled with clang and linked with lld.

On ARM, the kernel needs an ARMv7-M or ARMv8-M Mainline core with:

- an MPU with at least four regions, PMSAv7 on ARMv7-M or PMSAv8 on ARMv8-M,
- SysTick and the NVIC, which every such core has,
- a counter on the board that the kernel reads as memory, for `BOOT_CAP_CLOCK`.

On ARMv8-M, the kernel and every program run in the Secure state.
No program can reach a coprocessor or the floating-point unit:
their instructions fault.
The build is `thumbv7m-none-eabi` for the Cortex-M3,
and `thumbv8m.main-none-eabi` for the Cortex-M33.
Both are built with no floating point and no DSP extension.

### Boards

Seven boards are supported.
`make BOARD=<board>` chooses one:

- `qemu`: QEMU `virt` for RV32.
  This is the default.
- `esp32c6`: an Espressif ESP32-C6, for example on Seeed Studio's XIAO ESP32-C6.
- `esp32c5`: an Espressif ESP32-C5, for example on Waveshare's ESP32-C5-LCD-1.47.
- `rp2350`: a Raspberry Pi RP2350, for example on a Pico 2.
  It runs on the RP2350's RISC-V cores, or with `ARCH=arm` on its Cortex-M33 cores.
- `nrf52840`: a Nordic nRF52840, which is a Cortex-M4, for example on Fanstel's BT840X.
- `mps2-an385`: QEMU's model of ARM's MPS2 board with a Cortex-M3.
- `mps2-an521`: QEMU's model of the same board with the AN521 image, which has two Cortex-M33 cores.

A board picks its architecture.
The files of an architecture are in `kernel/arch/<arch>/` and `user/arch/<arch>/`.
RP2350 has both architectures.
It keeps the files that differ between them in `kernel/board/rp2350/<arch>/`.

Everything board-specific is in two directories:

- `kernel/board/<board>/` holds `board.h`, `board.c`, `irq.c`, `timer.c` and `halt.c`.
  On ARM there is no `irq.c`, because the interrupt controller is the architecture's.
- `user/board/<board>/` holds `console.h` and `devices.h`.
  `console.h` drives the device behind `BOOT_CAP_UART` for the demo and the replay driver.
  `devices.h` says how many devices `board.h` lists for the root task, and names their slots.

Boards of one family keep what they have in common in a directory next to their own.
For example, the MPS2 boards share `mps2/`.
The linker scripts take their addresses from `board.h` through `kernel/layout.h`.
To port rvuos to a new board, provide these files.

A program learns every address it needs from its frames and its Untyped.
Only two things are specific to the board:
the line numbers of its devices,
and the slots of the devices that the program starts with.

#### QEMU `virt`, RV32

Memory map:

| Range | Size | What |
|---|---|---|
| `0x0200BFE0` | 32 B | the CLINT's words that end with `mtime`, the clock's counter at 10 MHz; granted to the root task read only, as `BOOT_CAP_COUNTER` in slot 22 |
| `0x10000000` | 256 B | 16550 UART registers, granted to the root task |
| `0x80000000` to `0x800FF000` | just under 1 MiB | kernel code, data and stack |
| `0x800FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x80100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights; the four ranges below were made from it, and it makes nothing more |
| `0x80100000` | 64 KiB | root task code, read and execute |
| `0x80110000` | 64 KiB | root task data and stack, read and write |
| `0x80120000` | 64 KiB | replay input, placed there by QEMU's loader; read only |
| `0x80130000` | 4 KiB | the boot pool |
| `0x80400000` | 4 MiB | free RAM, granted to the root task as an Untyped with all rights |

Every range granted to the root task is a block (section 5.4).
So the memory between the blocks is not used by anything.

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log (no controller behind it) |
| 1 to 95 | the PLIC's sources; the UART is line 10 |

`BOOT_CAP_COUNT` is 23 here, and the root task's table has 69 slots.
The kernel has no tick.
It sets the CLINT's compare for the first count that could change what runs.
QEMU's PMP has sixteen entries and a four-byte grain.

`BOOT_CAP_ROUTES` names four channels of a router that connects nothing.
The MPS2 boards and the host have the same kind of router.
With it, a program's routes are bound, connected, armed and revoked here
in the same way as on a board with a real router.
Any word of a frame can be an end, either an event or a task.

`BOOT_CAP_DMA` names four channels of a DMA controller that moves nothing.
The MPS2 boards and the host have the same kind of controller.
With it, a program's `Dma`s are bound, connected, started and revoked here
in the same way as on a board with a real controller.
The controller behaves like this:

- Channels 0 and 2 read memory: memory is their source.
- Channels 1 and 3 write memory: memory is their sink.
- Any frame is memory, and any word of a frame is a device.
- A transfer is whole words, and at most 4096 bytes.
- A transfer runs from its start until it is stopped.

#### ESP32-C6

The chip boots from flash.
Its ROM loads the kernel into SRAM and enters it.
The root task's code runs from flash, through the cache.
`tools/esp32c6-run.py` writes the image into flash over the chip's USB port.
The same port carries the console.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x42000000` | 1 MiB | the window onto flash, from `0x210000` in the flash, through the cache; see below |
| `0x42840000` | 256 KiB | root task code, read and execute, from `0x40000` in flash |
| `0x42880000` | 4 KiB | input region, read only, from `0x80000` in flash; `tools/esp32c6-run.py --text` writes a configuration there |
| `0x40800000` to `0x4080F000` | 60 KiB | kernel code, data and stack |
| `0x4080F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x40810000` | 64 KiB | the root task's memory, granted to it as an Untyped with all rights; the two ranges below were made from it, and it makes nothing more |
| `0x40810000` | 32 KiB | root task data and stack, read and write |
| `0x40818000` | 4 KiB | the boot pool |
| `0x40820000` | 128 KiB | more free RAM, granted to the root task as an Untyped with all rights, `BOOT_CAP_MORE_RAM` |
| `0x40840000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |

The code region and the input region lie in flash, outside the root task's memory.
So their frames are roots, like the frames over devices.
The root task's code runs from flash through the chip's cache of 32 KiB.
The window onto flash shares this cache.
A program that prefers to run from RAM copies itself there.

The flash holds the image that the build makes, `build/esp32c6/kernel-<program>.bin`, from offset 0.
The flash is 4 MiB on the XIAO ESP32-C6.

| Offset | What |
|---|---|
| `0x0` | the kernel, behind Espressif's header; the ROM loads it into SRAM |
| `0x40000` | the root task's code |
| `0x80000` | the root task's input |
| `0x210000` | the window onto flash, 1 MiB |

No partition table or bootloader of Espressif's remains:
the ROM boots the kernel itself.
The header tells the ROM to read the flash in dual I/O mode,
and the ROM leaves the cache reading the flash in that mode.

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log |
| 1 to 77 | the interrupt matrix's sources; a line's number is the source's number in Espressif's `soc/interrupts.h` plus one; the Wi-Fi MAC's source 0 is line 1, and the USB Serial/JTAG controller's source is line 49 |

The root task is granted a frame over each of the devices in the next table, from `BOOT_CAP_DEVICES` up.
After them, in slot 33, it is granted `BOOT_CAP_MORE_RAM`:
an Untyped over the further block of RAM, 128 KiB from `0x40820000`.
So `BOOT_CAP_COUNT` is 34 here, and the root task's table has 80 slots.
The board has no router, so `BOOT_CAP_ROUTES` is empty.

The devices are:
the SAR ADC, the whole modem, the eFuse's registers, the window onto flash, the ROM,
the random number generator's data register, the IO MUX and the GPIO matrix, HP_SYSTEM,
the clock's counter, and SPI2.
The SAR ADC and the modem are where `make BOARD=esp32c6 phymap` and `make BOARD=esp32c6 wifi-esp32c6-map`
find what ESP-IDF's PHY and Wi-Fi libraries reach.

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_SARADC` | `0x6000E000` | 4 KiB | APB_SARADC, the SAR ADC and the temperature sensor | read, write |
| 23 | `BOOT_CAP_MODEM` | `0x600A0000` | 64 KiB | the modem: its RF front end, basebands and MACs, MODEM_SYSCON and MODEM_LPCON, its clocks and resets, and the analog I2C master from `0x600AF800` | read, write |
| 24 | `BOOT_CAP_EFUSE` | `0x600B0800` | 1 KiB | the eFuse's registers, which include the factory MAC address | read |
| 25 | `BOOT_CAP_FLASH` | `0x42000000` | 1 MiB | the window onto flash, for a program larger than the SRAM | read, execute |
| 26 | `BOOT_CAP_ROM` | `0x40000000` | 512 KiB | the ROM; ESP-IDF's libraries call its functions | read, execute |
| 27 | `BOOT_CAP_RNG` | `0x600B2808` | 8 bytes | LPPERI's RNG_DATA, the random number generator; its entropy comes from the radio while the radio runs | read |
| 28 | `BOOT_CAP_IO_MUX` | `0x60090000` | 4 KiB | the IO MUX: the pins' functions and pads | read, write |
| 29 | `BOOT_CAP_GPIO` | `0x60091000` | 4 KiB | the GPIO matrix: the pins' routes, levels and interrupts | read, write |
| 30 | `BOOT_CAP_HP_SYSTEM` | `0x60095000` | 4 KiB | HP_SYSTEM; its `SRAM_USAGE` gives a 128 KiB bank of SRAM to the modem's dump engine | read, write |
| 31 | `BOOT_CAP_COUNTER` | `0x20001C08` | 8 bytes | the CLINT's `UTIME`, a copy of `mtime` for user mode; the clock's counter, at the CPU clock of 160 MHz | read |
| 32 | `BOOT_CAP_SPI2` | `0x60081000` | 4 KiB | SPI2, the general-purpose SPI controller | read, write |

**The modem reaches all RAM.**
The modem has two MACs:
Wi-Fi's, from `0x600A4000`, and 802.15.4's, at `0x600A3000`.
Both are bus masters, and nothing confines them.
So a program that holds the modem's frame reaches all RAM, the kernel's RAM too.
The root task therefore gives the modem's frame only to a driver that it trusts as much as itself;
see `DESIGN.md`, open decision 13.
The modem's dump engine, from `0x600A9000`, writes what the receiver hears
into whichever bank `HP_SYSTEM` gives it, the kernel's too.
So a program that holds both the modem and HP_SYSTEM reaches all RAM as well.

**What is not listed.**
PCR, PMU and the LP domain are not listed.
The GDMA is not listed either, because it belongs to the kernel.

**DMA.**
`BOOT_CAP_DMA` names the GDMA's three transmit channels, 0 to 2 (section 5.14).
Each channel reads memory into a device:

- The source is memory, named 0.
  It may be any of the SRAM that a frame holds.
- The sink is a device.
  It is named by the DMA request that the GDMA takes for it,
  through a frame over all of the device's registers.

SPI2 is request 0, `DMA_REQUEST_SPI2` in `devices.h`.
It is the only listed device that the GDMA feeds.
A transfer is whole words, and starts at a word of the frame.
It is at most 32736 bytes, which is eight of the GDMA's descriptors.

**What the kernel sets up.**
The ROM leaves the SAR ADC in reset.
The kernel takes it out of reset and turns its clocks on.
SPI2's clocks are in PCR too, so the kernel sets them at boot:

- SPI2's master clock runs at 80 MHz, from PLL_F80M.
  A program divides it further in SPI2's `SPI_CLOCK`,
  and turns it on in SPI2's own `SPI_CLK_GATE`.
- The controller comes out of a reset of its own,
  whatever the last program left in it.

SPI2 sends the bytes that the CPU writes into its 64-byte buffer.
Or it sends the bytes that the GDMA reads from memory, through a channel of `BOOT_CAP_DMA`.

The kernel also powers the radio's analog I2C buses,
and sets the power detector's capacitance, in the PMU and LP_AON.
ESP-IDF's PHY library would do the same.
A PHY driver leaves out its own writes to these registers,
as `user/radio/esp32c6/phy.c` does.

**Pins.**
The pins are as the ROM leaves them.
A program sets them up to match how its board is wired.
On the XIAO ESP32-C6:

- GPIO3 low powers the RF switch between the chip and its two antennas.
- GPIO14 picks the antenna:
  low for the antenna on the board, high for the U.FL connector.

The Wi-Fi system's root task sets these two pins.

The flash's pins, GPIO24 to GPIO30, are in the IO MUX too.
So a program that holds the IO MUX can cut the flash off,
and with it the root task's code and the window onto flash.
The USB pins, GPIO12 and GPIO13, are in the IO MUX as well.
They carry the console, and the reset into the ROM's download mode.
The chip's BOOT button also gives that download mode.

The modem's clocks are off, in MODEM_SYSCON and MODEM_LPCON.
A driver turns them on.

The window onto flash shows 1 MiB of the flash from `0x210000`, past the image.
`tools/esp32c6-run.py --flash 0x210000:<image>` puts a program there.
Nothing but that tool writes the flash.

**Interrupts, clock and PMP.**
A device's interrupt reaches its `Irq` only while the device itself has that interrupt enabled.
For the USB Serial/JTAG controller, this is in its `INT_ENA` register.
The CPU runs at 160 MHz, which the kernel sets at boot.
The clock counts the CPU's cycles.
`OP_CLOCK_READ` gives the rate that the kernel measured against the chip's 16 MHz system timer.
The boot's log gives this rate too, together with the rate that the kernel found:
`rvuos: cpu clock <hz> hz, found at <hz> hz`.
The compare is the CLINT's here too, and it is set in the same way as on QEMU.
The PMP has sixteen entries and a four-byte grain.

**Alignment.**
Keep memory accesses aligned.
Clang does this unless it is told otherwise.
Misaligned accesses cause two problems:

- A misaligned load, followed at once by a store into a region with other rights, faults.
  It faults again when the thread is resumed (Espressif's erratum DIG-694).
  A `nop` between the load and the store avoids it.
- A misaligned store would write past a writable region into a read-only region right above it.
  This is why `OP_PROCESS_INSTALL` does not let two such regions touch.

**Console.**
The console is the controller's CDC-ACM port.
Bytes written to its FIFO leave as one USB packet when `WR_DONE` is written.
They leave only while a host has the port open.

**CSRs that user mode can write.**
User mode can write some of the core's CSRs:

- `ustatus`, `uie`, `utvec`, `uepc` and `ucause`,
- the performance counter at `0x800` to `0x802`,
- the dedicated GPIO at `0x803` and `0x805`.

When another process has run in between, a program finds these CSRs as follows:

- The performance counter is as the program left it.
  It stopped while the other process ran.
- The others are set back: `utvec` to 1, and the rest to zero.

Because the counter stops while other processes run,
the ROM's `ets_delay_us`, which counts with it, waits at least as long as asked.
This holds once `ets_delay_us` is told the clock's rate, with `ets_update_cpu_frequency(hz / 1000000)`,
because it counts by the clock that the ROM found.
A new process finds the counter stopped at zero, counting nothing.
No interrupt is delegated to user mode.

The dedicated GPIO reaches a pad only through the GPIO matrix, `BOOT_CAP_GPIO`.
A pad routed to the dedicated GPIO belongs to every process:
each process drives it during its own turns.
The Wi-Fi system's root task routes no pad to it.

#### ESP32-C5

The chip boots from flash, in the same way as the ESP32-C6.
The same tool writes it: `tools/esp32c6-run.py`, given `--chip esp32c5`.
The root task's code runs from flash through the cache.
One thing differs:
the ROM looks for the kernel at `0x2000` in flash, not at the start of flash.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x42840000` | 256 KiB | root task code, read and execute, from `0x40000` in flash |
| `0x42880000` | 4 KiB | input region, read only, from `0x80000` in flash; `tools/esp32c6-run.py --text` writes a configuration there |
| `0x42000000` | 1 MiB | the window onto flash, read and execute, from `0x210000` in flash, `BOOT_CAP_FLASH` |
| `0x40800000` to `0x4080F000` | 60 KiB | kernel code, data and stack |
| `0x4080F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x40810000` | 64 KiB | the root task's memory, granted to it as an Untyped with all rights; the two ranges below were made from it, and it makes nothing more |
| `0x40810000` | 32 KiB | root task data and stack, read and write |
| `0x40818000` | 4 KiB | the boot pool |
| `0x40820000` | 128 KiB | more free RAM, granted to the root task as an Untyped with all rights, `BOOT_CAP_MORE_RAM` |
| `0x40840000` | 128 KiB | free RAM, granted to the root task as an Untyped with all rights |

The flash holds the image that the build makes, `build/esp32c5/kernel-<program>.bin`, from offset 0.
The flash is 4 MiB, in the ESP32-C5HF4's package.

| Offset | What |
|---|---|
| `0x2000` | the kernel, behind Espressif's header; the ROM loads it into SRAM |
| `0x40000` | the root task's code |
| `0x80000` | the root task's input |
| `0x210000` | a radio program, which runs from the window onto flash; `tools/esp32c6-run.py --flash` writes it there |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log |
| 1 to 85 | the interrupt matrix's sources; a line's number is the source's number in Espressif's `soc/interrupts.h` plus one; the Wi-Fi MAC's source 0 is line 1, and the USB Serial/JTAG controller's source is line 55 |

The root task is granted a frame over each of the devices in the next table, from `BOOT_CAP_DEVICES` up.
After them, in slot 31, it is granted `BOOT_CAP_MORE_RAM`:
an Untyped over the further block of RAM, 128 KiB from `0x40820000`.
So `BOOT_CAP_COUNT` is 32 here, and the root task's table has 78 slots.
A kernel built with `PSRAM=1` adds one more block, see "PSRAM" below.

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_EFUSE` | `0x600B4800` | 1 KiB | the eFuse's registers, which include the factory MAC address; the frame stops short of the OTP debug registers above them | read |
| 23 | `BOOT_CAP_ROM` | `0x40000000` | 512 KiB | the ROM | read, execute |
| 24 | `BOOT_CAP_IO_MUX` | `0x60090000` | 4 KiB | the IO MUX: the pins' functions and pads | read, write |
| 25 | `BOOT_CAP_GPIO` | `0x60091000` | 4 KiB | the GPIO matrix: the pins' routes, levels and interrupts | read, write |
| 26 | `BOOT_CAP_SPI2` | `0x60081000` | 4 KiB | SPI2, the general-purpose SPI controller; it drives the board's LCD | read, write |
| 27 | `BOOT_CAP_RNG` | `0x600B2800` | 128 B | LPPERI's first words, as many as the PMP's grain; they hold the random number generator's registers: `RNG_DATA_SYNC` at `0x28`, which gives a new word at every read, and `RNG_CFG` at `0x24` | read |
| 28 | `BOOT_CAP_SARADC` | `0x6000E000` | 4 KiB | the SAR ADC and the temperature sensor | read, write |
| 29 | `BOOT_CAP_MODEM` | `0x600A0000` | 64 KiB | the whole modem: the Wi-Fi and 802.15.4 MACs, the basebands, the front end, and the analog I2C's master. The MACs are bus masters, so whoever holds this frame reaches all RAM | read, write |
| 30 | `BOOT_CAP_FLASH` | `0x42000000` | 1 MiB | the window onto flash, from `0x210000` in flash | read, execute |

**What is not listed.**
PCR, PMU and the LP domain are not listed,
apart from the random number generator's words.
The radio needs a few of their bits, so the kernel sets them at boot:

- the SAR ADC's clocks, and the temperature sensor's;
- the analog I2C buses that the PHY's calibration uses, the 5 GHz ones among them;
- the power detector's capacitance in LP_AON.
The GDMA is not listed, because it belongs to the kernel.

**PSRAM.**
Some ESP32-C5 chips have PSRAM in their package.
The ESP32-C5HR8 on Seeed's XIAO ESP32-C5 has 8 MiB.
A kernel built with `PSRAM=1` grants it to the root task:

- The build goes to `build/esp32c5-psram/`.
- The kernel starts the PSRAM at boot, at 40 MHz.
  If no PSRAM answers, the kernel stops with a panic.
- The PSRAM is read and written through the cache, from `0x43000000`.
- In slot 32, after `BOOT_CAP_MORE_RAM`, the root task is granted `BOOT_CAP_PSRAM`:
  an Untyped with all rights over the 8 MiB.
  So `BOOT_CAP_COUNT` is 33 with `PSRAM=1`.

A program uses the PSRAM as any other RAM.
It can make frames of it, for data and for code, and pools.
Atomic instructions work on it.
But it is slower than SRAM:

- A read that misses the cache costs about 360 cycles at the 160 MHz clock.
  A read of SRAM costs about 3.
- A store that misses the cache first reads the line, and costs about 680 cycles.
- A pool there makes the kernel slower too, because the kernel's objects then lie in the PSRAM.
- The GDMA cannot read it, so `OP_DMA_END` refuses a frame there as a source.

The cache holds 32 KiB, and the flash shares it.

**DMA.**
`BOOT_CAP_DMA` names the GDMA's three transmit channels, 0 to 2 (section 5.14).
Each channel reads memory into a device:

- The source is memory, named 0.
  It may be any of the SRAM that a frame holds.
- The sink is a device.
  It is named by the DMA request that the GDMA takes for it,
  through a frame over all of the device's registers.

SPI2 is request 1, `DMA_REQUEST_SPI2` in `devices.h`.
It is the only listed device that the GDMA feeds.
A transfer is whole words, and starts at a word of the frame.
It is at most 32736 bytes, which is eight of the GDMA's descriptors.
In addition, the GDMA's own window keeps every channel within the SRAM.

**The random number generator.**
The ROM leaves the generator stirring in two timers.
In addition, the kernel has the generator sample its ring oscillator,
as ESP-IDF does when it starts.
The generator also takes noise from the SAR ADC and the radio, but only while they run.
Nothing makes them run on this chip yet.
The generator's words look random whether it samples or not.
So it is not known how much entropy they carry.

**SPI2.**
SPI2's clocks are in PCR, so the kernel sets them at boot:

- SPI2's master clock runs at 80 MHz: PLL_F160M halved.
  A program divides it further in SPI2's `SPI_CLOCK`.
- The controller comes out of a reset of its own,
  whatever the last program left in it.

SPI2 sends data in one of two ways:

- the bytes that the CPU writes into its 64-byte buffer, one transfer at a time,
- or, through a channel of `BOOT_CAP_DMA`, a transfer from memory of up to the GDMA's longest.
  For this, SPI2's `SPI_DMA_CONF` is set to take its data from the GDMA.

**The LCD.**
`user/lcd/` uses SPI2 to drive the LCD of Waveshare's board, an ST7789 of 172 by 320 pixels.
`make lcd-sim` renders on the host what it shows, against a model of the LCD.
`make BOARD=esp32c5 lcd LCD_CONFIG="scene=stress mhz=80"` runs it on the board,
with the scene and the bus clock that the configuration names.
Two scenes use the random number generator:

- `scene=noise` draws snow from the random number generator:
  600 pictures, or as many as `pictures=` says.
  `pictures=0` draws without end.
- `scene=veins` grows a network of glowing threads from it, like a slime mould's.
  It draws the same number of pictures.

The veins scene works in 256 KiB of memory: the free RAM and the further block below it.
Once the pins are set up, these take the region slots of the input and the IO MUX.
Its pictures go out by DMA from the further block, or through SPI2's buffer with `dma=0`.
With `sync=0`, it draws them as fast as it can, torn, and not in step with the refresh.

GPIO_EXT, with its sigma-delta modulator, lies in the GPIO's own page.
It runs on the IO MUX's clock, which the kernel leaves as it is when the chip starts.
So a program that holds the GPIO can drive a pin at a density of its own choice.
`user/lcd/` dims the backlight in this way.

**Pins.**
The flash's pins are in the IO MUX.
So a program that holds the IO MUX can cut the flash off, and with it the root task's code.
The USB pins are in the IO MUX too.
They carry the console, and the reset into the ROM's download mode.

**Clock, PMP and alignment.**
The CPU runs at 160 MHz, which the kernel sets at boot.
The clock counts the CPU's cycles.
The kernel measures their rate against the 16 MHz system timer, as on the ESP32-C6.
The compare is the CLINT's here too.
The PMP has sixteen entries and a 128-byte grain,
so the smallest frame that a program can map is 128 bytes.
The hardware carries out misaligned loads and stores.

The core refuses user mode access to the CLINT.
It also holds no copy of `mtime` that user mode may load.
So no counter is listed among the devices,
and a program reads the time only through `OP_CLOCK_READ`.
The demo skips what it times with loads of the counter.

**CSRs that user mode can write.**
User mode can write some of the core's CSRs:

- `ustatus`, `utvec`, `utvt`, `uscratch`, `uepc`, `ucause` and `uintthresh`,
- one CSR at `0x017`, which Espressif names nowhere,
- the dedicated GPIO at `0x803` and `0x805`.

When another process has run in between, a program finds them set back:
`utvec` to 3, `uintthresh` to 15, and the rest to zero.
`ucause` holds `ustatus`'s UPIE in its bit 27.
So a write of either CSR is a write of both.
No interrupt is delegated to user mode.

#### RP2350

The bootrom's BOOTSEL mode loads the image into SRAM over USB, and reboots into it.
The image runs on the RISC-V cores or on the Arm cores, whichever it was built for.
Nothing is written to flash.
Both kinds of core have the same layout, interrupt lines and console.
The differences are described below.

The chip has no serial port of its own on USB.
So the console is the USB controller:
the root task makes a USB serial port of it, in `user/board/rp2350/console.h`.
The halt makes another USB serial port of it,
to send the host what no logger took.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20000000` to `0x2001F000` | 124 KiB | kernel code, data and stack |
| `0x2001F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x20020000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights; the four ranges below were made from it, and it makes nothing more |
| `0x20020000` | 64 KiB | root task code, read and execute |
| `0x20030000` | 32 KiB | root task data and stack, read and write |
| `0x20038000` | 4 KiB | input region, read only; `tools/rp2350-run.py --text` writes a configuration there, as `make BOARD=rp2350 wifi` does |
| `0x20039000` | 4 KiB | the boot pool |
| `0x20040000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |
| `0x50100000` | 128 KiB | the console: the USB controller's DPRAM, and its registers 64 KiB further on; granted to the root task |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; TIMER0's IRQ 0 cannot be bound |
| 1 to 51 | the system IRQs, numbered as in the datasheet; the console's is 14, `USBCTRL_IRQ` |
| 26 | SIO's doorbell; on the Cortex-M33 with `CORES=2` it is the kernel's, for its cores, and an `Irq` cannot be bound to it |

The root task is granted a frame over each of the devices in the next table, from `BOOT_CAP_DEVICES` up.
So `BOOT_CAP_COUNT` is 28 here, and the root task's table has 74 slots.
The board has no router, so `BOOT_CAP_ROUTES` is empty.
At boot, the kernel takes each device out of reset and opens it to user mode in ACCESSCTRL.
On the RISC-V cores, user mode reaches the bus as Non-secure,
and IO_BANK0 and PADS_BANK0 show it only the pins open to Non-secure access.
So there the kernel also opens every pin to Non-secure access.

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_IO_BANK0` | `0x40028000` | 16 KiB | IO_BANK0: the pins' functions, overrides and interrupts | read, write |
| 23 | `BOOT_CAP_PADS_BANK0` | `0x40038000` | 16 KiB | PADS_BANK0: the pins' pads | read, write |
| 24 to 26 | `BOOT_CAP_PIO0` to `BOOT_CAP_PIO2` | `0x50200000`, `0x50300000`, `0x50400000` | 16 KiB each | PIO0, PIO1 and PIO2 | read, write |
| 27 | `BOOT_CAP_COUNTER` | `0x400B0020` | 32 bytes | TIMER0's registers, with `TIMERAWL` at 8 into them: the clock's counter at 1 MHz, on either kind of core; the word below it is the high half | read |

Each frame except the counter's covers a device's registers and their atomic aliases.
None of these devices is a bus master.
The ADC is not listed:
its clock is not running, and without a clock it never leaves reset.

**DMA.**
The DMA controller is not listed either, because it belongs to the kernel.
`BOOT_CAP_DMA` names its sixteen channels, 0 to 15 (section 5.14).
Each channel reads memory into the transmit FIFO of a PIO block's state machine:

- The source is memory, named 0.
  It may be any of the SRAM that a frame holds.
- The sink is the FIFO.
  It is named by the FIFO's DREQ, through a frame over the whole PIO block.
  The DREQs are numbered as in the datasheet:
  0 to 3 for PIO0, 8 to 11 for PIO1, and 16 to 19 for PIO2.

A transfer is whole words, and starts at a word of the frame.
It can be as long as the frame.
The FIFO's DREQ paces the transfer,
so the transfer reads no further ahead than the state machine takes.

**Console and watchdog.**
At boot, the kernel takes the USB controller out of reset, opens it to user mode in ACCESSCTRL,
and starts its 48 MHz clock.
`user/board/rp2350/console.h` makes a CDC-ACM port of the controller, called "rvuos console".
`tools/rp2350-run.py` reads this port while the machine runs.
The console waits for the host to open the port before it sends.
The halt resets the controller to make its own port, called "rvuos".
So a program must flush its console before it halts.
What it has not sent by then is lost.

The chip's watchdog reboots the chip into BOOTSEL about seventeen seconds after boot,
unless the kernel's watchdog is fed (section 5.8).
Each feed sets the chip's watchdog to one second past the kernel's deadline.

**On the RISC-V cores.**
The timer is the compare of the RISC-V platform timer, which counts microseconds.
The PMP holds seven regions, none smaller than 32 bytes.
The reason is that the core has eight entries,
and it also hardwires three entries open to user mode.
The kernel spends one of the eight entries to shut those three.
A fault reports `mtval` as zero, and a misaligned access raises a misaligned exception.
A program reaches a peripheral only through a frame, in the same way as it reaches RAM.
The chip's ACCESSCTRL must let user mode into the peripheral too.
On either kind of core, the kernel opens ACCESSCTRL for the devices listed above and for the USB controller.
The DMA controller stays shut to user mode.

**On the Cortex-M33.**
The timer is SysTick, against TIMER0's counter, which `BOOT_CAP_CLOCK` names.
The MPU has eight regions, none smaller than 32 bytes.
A fault is reported as on `mps2-an385`.
A program reaches a peripheral only through a frame, in the same way as it reaches RAM.

**Two cores.**
With `CORES=2`, the kernel starts the second core at the end of the boot, on either kind of core.
A process has the same regions on both cores,
and an atomic operation on RAM reaches the other core.
Every device line reaches the interrupt controllers of both cores,
but only the first core's controller enables any line.

#### nRF52840

`tools/nrf52840-run.py` loads the image into RAM over SWD, through an ST-Link and OpenOCD,
and starts the Cortex-M4.
Nothing is written to flash.
A program that never halts keeps running after the runner stops, until the chip is reset.
The console is a block of RAM, and no pin is wired for it.
The runner reads the console while the core runs.
`--text address:file` leaves a text in RAM before the core starts,
as the runners of the other boards do.
Two make targets use it to fill the input region:

- `make BOARD=nrf52840 thread` leaves the file that `THREAD_CONFIG` names, the configuration of `user/thread/`.
- `make BOARD=nrf52840 observer` leaves the file that `OBSERVER_CONFIG` names, the configuration of `user/observer/`.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20000000` | 8 KiB | the console, granted to the root task |
| `0x20002000` to `0x2000F000` | 52 KiB | kernel code, data and stack, the vector table first |
| `0x2000F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x20010000` | 64 KiB | free RAM, granted to the root task as an Untyped with all rights |
| `0x20020000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights; the four ranges below were made from it, and it makes nothing more |
| `0x20020000` | 64 KiB | root task code, read and execute |
| `0x20030000` | 32 KiB | root task data and stack, read and write |
| `0x20038000` | 4 KiB | input region, read only; `tools/nrf52840-run.py --text` writes a configuration there, as `make BOARD=nrf52840 thread` and `make BOARD=nrf52840 observer` do |
| `0x20039000` | 4 KiB | the boot pool |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; POWER_CLOCK cannot be bound |
| 1 to 47 | the peripherals' lines, numbered by their IDs; nothing raises line 20, SWI0, the console's line |

The root task is granted these frames, from `BOOT_CAP_DEVICES` up:
one over GPIO, one over the radio, one over each of TIMER2, TIMER3 and TIMER4,
and one over the clock's counter.
So `BOOT_CAP_COUNT` is 28 here, and the root task's table has 74 slots:

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_GPIO` | `0x50000000` | 4 KiB | GPIO, both ports: P0's registers, and P1's at `0x50000300` | read, write |
| 23 | `BOOT_CAP_RADIO` | `0x40001000` | 4 KiB | RADIO, the 2.4 GHz radio of Bluetooth LE and IEEE 802.15.4; its line is 1 | read, write |
| 24 | `BOOT_CAP_TIMER2` | `0x4000A000` | 4 KiB | TIMER2, which times what is shorter than a kernel's step, such as 802.15.4's backoffs; its line is 10 | read, write |
| 25 | `BOOT_CAP_TIMER3` | `0x4001A000` | 4 KiB | TIMER3, for routes, such as an acknowledgement's turnaround; its line is 26 | read, write |
| 26 | `BOOT_CAP_TIMER4` | `0x4001B000` | 4 KiB | TIMER4, for routes, such as a count of the frames received; its line is 27 | read, write |
| 27 | `BOOT_CAP_COUNTER` | `0x40008540` | 32 bytes | TIMER0's `CC` registers, with `CC[1]` at 4 into them: the clock's counter at 1 MHz; no word holds the high half | read |

**Routes.**
The router is the PPI.
`BOOT_CAP_ROUTES` names its channels 1 to 19.
Channel 0 belongs to the kernel's counter (see below).
An end is the offset of a register in its device's 4 KiB block,
as the product specification gives it:

- a task, `ROUTE_TASK`, at 0x000 to 0x07C,
- an event, `ROUTE_EVENT`, at 0x100 to 0x17C.

The device must lie between `0x40000000` and `0x4003FFFF`.
Any other end fails.
For example, `0x10C` of the radio's frame is the radio's `EVENTS_END`,
and `0x008` of a TIMER's frame is its `TASKS_COUNT` (section 5.13).

**The radio reaches all RAM.**
The radio is a bus master.
Its EasyDMA reads and writes a packet wherever its `PACKETPTR` points, the kernel's RAM too.
So a program that holds the radio's frame reaches all RAM.
The root task therefore gives the radio's frame only to a driver that it trusts as much as itself,
as with the ESP32-C6's modem;
see `DESIGN.md`, open decision 13.

**Console, clock and faults.**
The console's first word counts every byte written.
Byte n lies at offset 16 + (n modulo 8176).
So the console is a ring.
When the runner falls a whole ring behind, it says how much it lost.

The timer is the counter, with SysTick for the compare.
The counter is TIMER0's.
TIMER1 and a PPI channel capture it into `CC[1]` every microsecond.
It is 32 bits wide and wraps every 71 minutes.
`OP_CLOCK_READ` counts the wraps.

The MPU has eight regions, none smaller than 32 bytes.
A fault is reported as on `mps2-an385`, with one exception.
When a thread makes a system call and the core cannot save the thread's registers on its stack,
the fault is reported as the call itself, exception 11, and not as a MemManage fault.
The floating-point unit stays shut, so a floating-point instruction faults its thread.

#### mps2-an385

QEMU loads the image into SSRAM1 and resets the Cortex-M3.
The Cortex-M3 finds the kernel's vector table at 0.
The halt leaves QEMU through semihosting.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x40028000` | 32 B | the FPGA's first registers, with `COUNTER` at `0x18` into them: the clock's counter at 25 MHz; granted to the root task read only, as `BOOT_CAP_COUNTER` in slot 22 |
| `0x40004000` | 4 KiB | UART0 of the CMSDK, granted to the root task |
| `0x00000000` to `0x000FF000` | just under 1 MiB | kernel code, data and stack, the vector table first |
| `0x000FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x00100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights; the four ranges below were made from it, and it makes nothing more |
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

The timer is the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide and wraps every 171 seconds.
`OP_CLOCK_READ` counts the wraps.
UART0's transmitter holds one byte.
It raises its line when that byte leaves,
and the line stays latched until the driver clears it.
A fault is reported as `exception`, `cfsr`, `pc` and `addr` (section 4).

#### mps2-an521

QEMU loads the image into SSRAM1 and resets the first Cortex-M33.
That core finds the kernel's vector table at `0x10000000`, SSRAM1's Secure alias.
The second core waits until a kernel built for both cores starts it.
The halt leaves QEMU through semihosting.
The kernel and every program run in the Secure state,
so every address is a Secure alias, with bit 28 set.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x50302000` | 32 B | the FPGA's first registers, with `COUNTER` at `0x18` into them: the clock's counter at 20 MHz; granted to the root task read only, as `BOOT_CAP_COUNTER` in slot 22 |
| `0x50200000` | 4 KiB | UART0 of the CMSDK, granted to the root task |
| `0x10000000` to `0x100FF000` | just under 1 MiB | kernel code, data and stack, the vector table first |
| `0x100FF000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x10100000` | 256 KiB | the root task's memory, granted to it as an Untyped with all rights; the four ranges below were made from it, and it makes nothing more |
| `0x10100000` | 64 KiB | root task code, read and execute |
| `0x10110000` | 64 KiB | root task data and stack, read and write |
| `0x10120000` | 64 KiB | input region, read only; nothing fills it yet |
| `0x10130000` | 4 KiB | the boot pool |
| `0x10200000` | 2 MiB | free RAM, granted to the root task as an Untyped with all rights |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; the Non-secure watchdog's reset cannot be bound |
| 1 to 47 | the NVIC's first lines: the SSE-200's 31 and the board's first 16; UART0's combined line, which includes its transmitter's, is 42 |
| 6 | MHU0's; with `CORES=2` it is the kernel's, for its cores, and an `Irq` cannot be bound to it |

The timer is the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide and wraps every 214 seconds.
`OP_CLOCK_READ` counts the wraps.

The SSE-200 refuses user mode every device,
unless its Secure Privilege Control block lets user mode through.
The kernel lets it through for UART0 and the counter only.
A program reaches either of them only through a frame, in the same way as it reaches RAM.
UART0 is the same as on mps2-an385, and a fault is reported in the same way.

With `CORES=2`, the kernel starts the second core at the end of the boot.
Every device line reaches both NVICs,
but only the first core's NVIC enables any line.

## 4. Building and running

Requirements:

- clang and lld with RISC-V and ARM support, and llvm-objcopy,
- GNU make,
- `qemu-system-riscv32` and `qemu-system-arm`.
  `qemu-system-arm` must be no newer than 10.2,
  because QEMU 11 never wakes the ARM demo from its first sleep; see `TODO.md`.

No separate cross toolchain is needed.
The host build needs clang's sanitizer and libFuzzer runtimes.
Some boards need more:

- The ESP32-C6 and the ESP32-C5 need Espressif's `esptool`, version 5, both as a command and as a Python module.
- RP2350 needs Python's `pyusb`, and write access to the chip's USB devices.
  A udev rule such as `SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", TAG+="uaccess"` gives this access.
- The nRF52840 needs OpenOCD, version 0.12, and an ST-Link on its SWD pins.
  OpenOCD's udev rules open the ST-Link to the user.

```
make             # build/qemu/kernel-init.elf and build/qemu/kernel-fuzzdrv.elf
make run         # boot the demo root task under QEMU
make test        # boot under QEMU and check the transcript
make lib-test    # boot the library's test of user/libtest/ under QEMU and check what it says of itself
make bench       # what a system call and a switch cost, in instructions, held to tests/bench/qemu.txt
make lab         # the laboratory of user/lab/: servers and clients, scenario by scenario, in microseconds
make host-test   # replay the fuzz corpus on the host build with invariants on
make fuzz        # fuzz the system call surface for FUZZ_TIME seconds in FUZZ_JOBS processes
make qemu-replay # replay the corpus on QEMU and compare with the host
make smp-test    # boot the demo on two harts of QEMU and check its transcript, the second core's tests among it
make mutants     # plant each bug under tests/mutants/ and require the checks to catch it
make arm-test    # boot the demo on mps2-an385 and mps2-an521, there on one core and on two, and check its transcript,
                 # with the escape suite, the library's test and the benchmark on one
make check       # test, escape, lib-test, bench, lab, host-test, wifi-test, qemu-replay, arm-test and smp-test; run before committing
```

`PMP_MAX_ENTRIES=8 make check` runs everything with a smaller PMP budget.
Images go under `build/<board>/`, and the host build under `build/host/`.
A smaller budget adds `-pmp<n>` to both directory names.

`SELFCHECK=1` builds a board's kernel with the self-check and the tracing (section 9).
Otherwise, only QEMU `virt`'s kernel has them.
`SELFCHECK=0` builds QEMU `virt`'s kernel without them.
These two builds add `-selfcheck` and `-noselfcheck` to the directory name.

`make CORES=2` builds the kernel for two harts of QEMU `virt`, under `build/qemu-smp2/`.
`make CORES=2 run` boots it with `-smp 2`.
`make BOARD=mps2-an521 CORES=2` builds the kernel for that board's two cores, under `build/mps2-an521-smp2/`.
`make BOARD=rp2350 CORES=2` builds it for RP2350's two cores, under `build/rp2350-smp2/`,
or under `build/rp2350-arm-smp2/` with `ARCH=arm`.
No other board takes `CORES`.

`DMA=0` builds a kernel that lends no channel of the board's DMA controller.
`ROUTES=0` builds a kernel that lends no channel of the board's router.
These builds add `-nodma` and `-noroutes` to the directory name.
`BOOT_CAP_DMA` or `BOOT_CAP_ROUTES` is then empty, as on a board without a controller or a router.
The kernel then leaves their code out:
about 2 KiB for DMA on a board with a controller, and 1.4 KiB for routes on the nRF52840.
A kernel leaves this code out by itself where the board has no controller or no router.

`make BOARD=esp32c5 PSRAM=1` builds a kernel that grants the PSRAM in the chip's package, as section 3 says.
The build adds `-psram` to the directory name.
Only an ESP32-C5 with PSRAM boots it, such as the ESP32-C5HR8 on Seeed's XIAO ESP32-C5.
No other board takes `PSRAM`.

On the ESP32-C6, connected over USB:

```
make BOARD=esp32c6                    # build/esp32c6/kernel-init.bin
make BOARD=esp32c6 run                # write it into flash, boot it and print the console until the halt
make BOARD=esp32c6 test               # the same, and check the transcript
make BOARD=esp32c6 escape lib-test    # the escape suite, and the library's test
make BOARD=esp32c6 bench              # what a system call and a switch cost, in the core's cycles
make BOARD=esp32c6 PORT=/dev/ttyACM1 run
```

On the ESP32-C5, `make BOARD=esp32c5 PORT=<port> test escape` does the same.

Opening the port resets the chip into the ROM's download mode.
There, `tools/esp32c6-run.py` writes into flash whatever the flash does not hold already.
Then it resets the chip to boot from flash,
and reads the console on the same connection.
For now, the replay driver runs only on QEMU.

On RP2350, in BOOTSEL mode.
The chip is in this mode when it is plugged in with BOOTSEL held, and again after each run:

```
make BOARD=rp2350                              # build/rp2350/kernel-init.elf
make BOARD=rp2350 run                          # load it into RAM and print its console, then what the halt writes
make BOARD=rp2350 test escape                  # check the transcripts
make BOARD=rp2350 ARCH=arm test escape         # the same on the Cortex-M33, from build/rp2350-arm/
make BOARD=rp2350 CORES=2 test escape          # the same on both Hazard3 cores, which goes on to the second
make BOARD=rp2350 ARCH=arm CORES=2 test escape # and on both Cortex-M33s
make BOARD=rp2350 lib-test                     # the library's test, and with ARCH=arm on the Cortex-M33
make BOARD=rp2350 bench                        # what a system call and a switch cost, in cycles, on either kind of core
make BOARD=rp2350 wifi                         # the Wi-Fi system of user/wifi/, on a Pico 2 W
```

`make BOARD=rp2350 wifi` also builds `build/cyw43/blob.bin`:

- `user/wifi/tools/cyw43-blob.py` fetches the CYW43439's firmware, CLM and NVRAM from one commit of embassy,
  under Infineon's Permissive Binary License.
  It checks each against its SHA-256, and packs them.
- `tools/rp2350-run.py --ram` puts the blob at the start of free RAM, next to the image.
  Nothing is written to flash.

`tools/wifi-run.py` runs the loader,
and prints the system's log as it comes over the network.
Once the clients answer, it checks them from the host:

- the status port,
- the echo,
- the clock,
- the echo built again after a fault and after a hang,
- ping.

The run lasts half a minute, or the number of seconds that the configuration's `run=` gives.
With `run=0` it runs without end.

`tools/rp2350-run.py` loads the image through the bootrom's PICOBOOT interface.
Then it waits for the halt's serial port and reads it.
Closing the port reboots the chip into BOOTSEL.

On the nRF52840, through an ST-Link:

```
make BOARD=nrf52840 test escape                        # boot the demo and the escape suite, and check the transcripts
make BOARD=nrf52840 build/nrf52840/kernel-blink.elf    # user/blink.c, which blinks the LED of a Fanstel BT840X
tools/nrf52840-run.py build/nrf52840/kernel-blink.elf  # load it, and print its console until Ctrl-C
tools/thread-pair.sh                                   # user/thread/: 802.15.4's frames between it and an ESP32-C6
user/observer/le-observer.py --channel 19 --seconds 10         # user/observer/: BLE's test packets counted, and how strong
```

On ARM, under QEMU:

```
make BOARD=mps2-an385                # build/mps2-an385/kernel-init.elf
make BOARD=mps2-an385 run            # boot the demo root task
make BOARD=mps2-an385 test escape    # the same, and check the transcripts
make BOARD=mps2-an385 lib-test       # the library's test
make BOARD=mps2-an385 bench          # the benchmark, held to tests/bench/mps2-an385.txt
make BOARD=mps2-an385 lab            # the laboratory
make BOARD=mps2-an521 test escape    # the same on QEMU's Cortex-M33, from build/mps2-an521/
make BOARD=mps2-an521 CORES=2 test   # the same on its two Cortex-M33s, which goes on to the second core
```

The replay driver is not built here, because its layout is QEMU `virt`'s.

The kernel image embeds one user program, the root task.
The images differ only in that program:

- `kernel-init.elf` carries the demo of `user/init/`,
- `kernel-fuzzdrv.elf` the replay driver of `user/fuzzdrv.c`,
- `kernel-escape-*.elf` one scenario of the escape suite each,
- `kernel-libtest.elf` the library's test of `user/libtest/`,
- `kernel-bench.elf` the benchmark of `user/bench/`,
- `kernel-lab.elf` the laboratory of `user/lab/`,
- `kernel-wifi.elf` the Wi-Fi system of `user/wifi/`.

### Reading the transcript

The kernel has no console.
Everything the kernel prints, and everything a program writes with `OP_PRINT`,
goes into the kernel log.
Two things carry that log to the board's UART:

- the root task's logger thread, while the machine runs,
- the halt, which writes out every byte that no reader has taken,
  under the line `rvuos: halting, the log follows`.

So a line that appears before that marker was carried out from user mode.
A line after the marker was salvaged by the halt.
On RP2350, the halt's lines reach the host on a USB serial port of their own,
after the console's port has gone.

The kernel prints three lines at boot:

```
rvuos: machine mode up
rvuos: pmp entries 0x00000010 grain 0x00000004
rvuos: entering user mode
```

On QEMU, a halt exits the emulator with a status code.
The ESP32-C6 parks its core instead, after the line `rvuos: halted with code <n>`.
RP2350 reboots into BOOTSEL once the host has read that line.
On QEMU, the code is the emulator's exit status.
On a board, the board's runner exits with the code.
The codes are:

| Code | Meaning |
|---|---|
| 0 | `OP_HALT` with code 0 |
| 1 | kernel panic |
| 3 | the self-check found an invariant violated |
| 5 | no runnable thread, and nothing armed that could make one runnable |
| 6 | tracing is on, and a thread that the host build cannot follow was about to run |
| 7 | the watchdog was not fed in time; this comes after the line `watchdog: not fed in time` (section 5.8) |
| other | what the root task passed to `OP_HALT` |

A user fault is an access fault, an illegal instruction, a misaligned access or a breakpoint.
It prints `user fault`, followed by:

- on RISC-V, `mcause`, `mepc` and `mtval`,
- on ARM, the exception number, the configurable fault status `cfsr`, the `pc`,
  and the address that `MMFAR` or `BFAR` names, or zero where neither is valid.

A user fault stops the thread that made it, and nothing else (section 5.6).
So a program whose only thread faults ends in `no runnable thread`, code 5.
