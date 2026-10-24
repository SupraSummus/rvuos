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
- a counter of the board's the kernel reads as memory, for `BOOT_CAP_CLOCK`.

On ARMv8-M the kernel and every program run in the Secure state,
and no program reaches a coprocessor or the floating-point unit, whose instructions fault.
The build is `thumbv7m-none-eabi` for the Cortex-M3
and `thumbv8m.main-none-eabi` for the Cortex-M33, with no floating point and no DSP extension.

### Boards

Seven boards are supported, chosen with `make BOARD=<board>`:
`qemu`, QEMU `virt` for RV32, the default,
`esp32c6`, an Espressif ESP32-C6, as on Seeed Studio's XIAO ESP32-C6,
`esp32c5`, an Espressif ESP32-C5, as on Waveshare's ESP32-C5-LCD-1.47,
`rp2350`, a Raspberry Pi RP2350 on its RISC-V cores, or with `ARCH=arm` on its Cortex-M33 ones, as on a Pico 2,
`nrf52840`, a Nordic nRF52840, a Cortex-M4, as on Fanstel's BT840X,
`mps2-an385`, QEMU's model of ARM's MPS2 board with a Cortex-M3,
and `mps2-an521`, QEMU's model of the same board with the AN521 image, two Cortex-M33s.
A board picks its architecture, whose files lie in `kernel/arch/<arch>/` and `user/arch/<arch>/`;
RP2350 has both, and keeps the files that differ between them in `kernel/board/rp2350/<arch>/`.
Everything board-specific lives in `kernel/board/<board>/`,
`board.h`, `board.c`, `irq.c`, `timer.c` and `halt.c`,
with no `irq.c` on ARM, whose controller is the architecture's,
and in `user/board/<board>/console.h`,
which drives the device behind `BOOT_CAP_UART` for the demo and the replay driver,
and `devices.h`, which says how many devices `board.h` lists for the root task and names their slots;
boards of one family share what they have alike in a directory beside theirs, as the MPS2 boards share `mps2/`.
The linker scripts take their addresses from `board.h` through `kernel/layout.h`.
Porting to a board means providing those.
A program learns every address it needs from its frames and its Untyped;
only the line numbers of its devices and the slots of the devices it starts with are the board's to know.

#### QEMU `virt`, RV32

Memory map:

| Range | Size | What |
|---|---|---|
| `0x0200BFE0` | 32 B | the CLINT's words that end with `mtime`, 10 MHz, the clock's counter, granted to the root task read only, `BOOT_CAP_COUNTER` in slot 22 |
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

`BOOT_CAP_COUNT` is 23 here and the root task's table has 69 slots.
The kernel has no tick: the CLINT's compare is set for the first count that could change what runs.
QEMU's PMP has sixteen entries and a four-byte grain.
`BOOT_CAP_ROUTES` names four channels of a router that wires nothing, as on the MPS2 boards and the host,
so that a program's routes are bound, connected, armed and revoked here as on a board with a router;
any word of a frame is an end, an event or a task alike.
`BOOT_CAP_DMA` names four channels of a DMA controller that moves nothing, as on the MPS2 boards and the host,
so that a program's `Dma`s are bound, connected, started and revoked here as on a board with one:
channels 0 and 2 read memory, their source, and 1 and 3 write it, their sink,
any frame is memory and any word of one a device, a transfer is whole words and at most 4096 bytes,
and it runs from its start until a stop.

#### ESP32-C6

The chip boots from flash: its ROM loads the kernel into SRAM and enters it,
and the root task's code runs from flash, through the cache.
`tools/esp32c6-run.py` writes the image into flash over the chip's USB port, and the same port carries the console.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x42000000` | 1 MiB | the window onto flash, from `0x210000` in it, through the cache; see below |
| `0x42840000` | 256 KiB | root task code, read and execute, from `0x40000` in flash |
| `0x42880000` | 4 KiB | input region, read only, from `0x80000` in flash, where `tools/esp32c6-run.py --text` writes a configuration |
| `0x40800000` to `0x4080F000` | 60 KiB | kernel code, data and stack |
| `0x4080F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x40810000` | 64 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the two ranges below and makes nothing more |
| `0x40810000` | 32 KiB | root task data and stack, read and write |
| `0x40818000` | 4 KiB | the boot pool |
| `0x40820000` | 128 KiB | more free RAM, granted to the root task as an Untyped with all rights, `BOOT_CAP_MORE_RAM` |
| `0x40840000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |

The code and input regions lie in flash, outside the root task's memory,
so their frames hang below nothing, as a device's do.
The root task's code runs from flash through the chip's cache of 32 KiB, which the window onto flash shares;
a program that would rather run from RAM copies itself there.

The flash, 4 MiB on the XIAO ESP32-C6, holds the image the build makes, `build/esp32c6/kernel-<program>.bin`, from 0:

| Offset | What |
|---|---|
| `0x0` | the kernel, behind Espressif's header, which the ROM loads into SRAM |
| `0x40000` | the root task's code |
| `0x80000` | the root task's input |
| `0x210000` | the window onto flash, 1 MiB |

No partition table or bootloader of Espressif's is left: the ROM boots the kernel itself.
The header tells the ROM to read the flash as dual I/O, and the ROM leaves the cache reading it so.

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log |
| 1 to 77 | the interrupt matrix's sources, numbered as in Espressif's `soc/interrupts.h`, plus one: the Wi-Fi MAC's source 0 is line 1, and the USB Serial/JTAG controller's is line 49 |

The root task is granted a frame over each of these devices, from `BOOT_CAP_DEVICES` up,
and after them, in slot 33, `BOOT_CAP_MORE_RAM`, an Untyped over the further block of RAM, 128 KiB from `0x40820000`,
so `BOOT_CAP_COUNT` is 34 here and the root task's table has 80 slots; it has no router, so `BOOT_CAP_ROUTES` is empty.
They are the SAR ADC and the whole modem,
where `make BOARD=esp32c6 phymap` and `make BOARD=esp32c6 wifi-esp32c6-map` find what ESP-IDF's PHY and Wi-Fi libraries reach,
the eFuse's registers, the window onto flash, the ROM, the random number generator's data register,
the IO MUX and the GPIO matrix, HP_SYSTEM, the clock's counter, and SPI2:

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_SARADC` | `0x6000E000` | 4 KiB | APB_SARADC, the SAR ADC and the temperature sensor | read, write |
| 23 | `BOOT_CAP_MODEM` | `0x600A0000` | 64 KiB | the modem: its RF front end, basebands and MACs, MODEM_SYSCON and MODEM_LPCON, its clocks and resets, and the analog I2C master from `0x600AF800` | read, write |
| 24 | `BOOT_CAP_EFUSE` | `0x600B0800` | 1 KiB | the eFuse's registers, the factory MAC address among them | read |
| 25 | `BOOT_CAP_FLASH` | `0x42000000` | 1 MiB | the window onto flash, for a program larger than the SRAM | read, execute |
| 26 | `BOOT_CAP_ROM` | `0x40000000` | 512 KiB | the ROM, whose functions ESP-IDF's libraries call | read, execute |
| 27 | `BOOT_CAP_RNG` | `0x600B2808` | 8 bytes | LPPERI's RNG_DATA, the random number generator, whose entropy comes from the radio while it runs | read |
| 28 | `BOOT_CAP_IO_MUX` | `0x60090000` | 4 KiB | the IO MUX, the pins' functions and pads | read, write |
| 29 | `BOOT_CAP_GPIO` | `0x60091000` | 4 KiB | the GPIO matrix, the pins' routes, levels and interrupts | read, write |
| 30 | `BOOT_CAP_HP_SYSTEM` | `0x60095000` | 4 KiB | HP_SYSTEM, whose `SRAM_USAGE` gives a 128 KiB bank of SRAM to the modem's dump engine | read, write |
| 31 | `BOOT_CAP_COUNTER` | `0x20001C08` | 8 bytes | the CLINT's `UTIME`, a copy of `mtime` for user mode at the CPU clock, 160 MHz, the clock's counter | read |
| 32 | `BOOT_CAP_SPI2` | `0x60081000` | 4 KiB | SPI2, the general-purpose SPI controller | read, write |

The modem's two MACs, Wi-Fi's from `0x600A4000` and 802.15.4's at `0x600A3000`, are bus masters, and nothing confines them:
a program that holds the modem's frame reaches all RAM, the kernel's too,
so the root task gives it only to a driver it trusts as it trusts itself; see `DESIGN.md`, open decision 13.
The modem's dump engine, from `0x600A9000`, writes what the receiver hears into whichever bank `HP_SYSTEM` gives it,
the kernel's too, so whoever holds both reaches all RAM as well.
PCR, PMU and the LP domain are not listed, nor the GDMA, which is the kernel's.
`BOOT_CAP_DMA` names the GDMA's three transmit channels, 0 to 2, section 5.14,
each reading memory, its source, named 0, which may be any of the SRAM a frame holds,
into a device, its sink, named by the request the GDMA takes for it, through a frame over the device's registers whole:
SPI2 is request 0, `DMA_REQUEST_SPI2` in `devices.h`, the one device listed that the GDMA feeds.
A transfer is whole words, from a word of the frame, and at most 32736 bytes, eight of the GDMA's descriptors.
The kernel takes the SAR ADC out of the reset the ROM leaves it in and turns its clocks on.
SPI2's clocks lie in PCR too, so the kernel sets them at boot: its master clock runs at 80 MHz, PLL_F80M,
which a program divides further in SPI2's `SPI_CLOCK`, and turns on in SPI2's own `SPI_CLK_GATE`,
and the controller comes out of a reset of its own, whatever the last program left in it.
It sends what the CPU writes into its 64-byte buffer, or, by a channel of `BOOT_CAP_DMA`, what the GDMA reads of memory.
It also powers the radio's analog I2C buses and sets the power detector's capacitance, in the PMU and LP_AON,
as ESP-IDF's PHY library would;
a PHY driver leaves out its own writes there, as `user/radio/esp32c6/phy.c` does.
The pins are as the ROM leaves them, for a program to set as its board wires them.
On the XIAO ESP32-C6, GPIO3 low powers the RF switch between the chip and its two antennas,
and GPIO14 picks the antenna on the board, low, or the U.FL connector, high, as the Wi-Fi system's root task sets them.
The flash's pins, GPIO24 to GPIO30, are in the IO MUX too, so a program that holds it can cut the flash off,
the root task's code and the window onto flash with it;
and so are the USB pins, GPIO12 and GPIO13, which carry the console and the reset into the ROM's download mode,
which the chip's BOOT button gives too.
The modem's clocks are off, in MODEM_SYSCON and MODEM_LPCON, for a driver to turn on.
The window onto flash shows 1 MiB of the flash from `0x210000`, past the image;
`tools/esp32c6-run.py --flash 0x210000:<image>` puts a program there, and nothing but that tool writes the flash.

A device's interrupt reaches its `Irq` only while the device itself has it enabled,
in the USB Serial/JTAG controller's case in its `INT_ENA` register.
The CPU runs at 160 MHz, which the kernel sets at boot, and the clock counts its cycles;
`OP_CLOCK_READ` gives the rate the kernel measured against the chip's 16 MHz system timer,
and the boot's log gives it too, with the rate the kernel found: `rvuos: cpu clock <hz> hz, found at <hz> hz`.
The compare is the CLINT's here too, set as on QEMU.
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
Whenever another process has run, a program finds the counter as it left it, having stopped meanwhile,
so the ROM's `ets_delay_us`, which counts on it, waits at least as long as asked,
once told the clock's rate, `ets_update_cpu_frequency(hz / 1000000)`, since it counts by the clock the ROM found;
and the rest set back: `utvec` at 1, the others at zero.
A new process finds the counter stopped at zero, counting nothing.
No interrupt is delegated to user mode.
The dedicated GPIO reaches a pad only through the GPIO matrix, `BOOT_CAP_GPIO`,
and a pad routed to it is every process's, each driving it in its turns;
the Wi-Fi system's root task routes none.

#### ESP32-C5

The chip boots from flash as the ESP32-C6 does, through the same `tools/esp32c6-run.py`, given `--chip esp32c5`,
and the root task's code runs from flash through the cache;
its ROM looks for the kernel at `0x2000` in flash rather than at its start.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x6000F000` | 256 B | USB Serial/JTAG controller registers, granted to the root task |
| `0x42840000` | 256 KiB | root task code, read and execute, from `0x40000` in flash |
| `0x42880000` | 4 KiB | input region, read only, from `0x80000` in flash, where `tools/esp32c6-run.py --text` writes a configuration |
| `0x40800000` to `0x4080F000` | 60 KiB | kernel code, data and stack |
| `0x4080F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x40810000` | 64 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the two ranges below and makes nothing more |
| `0x40810000` | 32 KiB | root task data and stack, read and write |
| `0x40818000` | 4 KiB | the boot pool |
| `0x40820000` | 128 KiB | more free RAM, granted to the root task as an Untyped with all rights, `BOOT_CAP_MORE_RAM` |
| `0x40840000` | 128 KiB | free RAM, granted to the root task as an Untyped with all rights |

The flash, 4 MiB in the ESP32-C5HF4's package, holds the image the build makes, `build/esp32c5/kernel-<program>.bin`, from 0:

| Offset | What |
|---|---|
| `0x2000` | the kernel, behind Espressif's header, which the ROM loads into SRAM |
| `0x40000` | the root task's code |
| `0x80000` | the root task's input |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log |
| 1 to 85 | the interrupt matrix's sources, numbered as in Espressif's `soc/interrupts.h`, plus one: the Wi-Fi MAC's source 0 is line 1, and the USB Serial/JTAG controller's is line 55 |

The root task is granted a frame over each of these devices, from `BOOT_CAP_DEVICES` up,
and after them, in slot 28, `BOOT_CAP_MORE_RAM`, an Untyped over the further block of RAM, 128 KiB from `0x40820000`,
so `BOOT_CAP_COUNT` is 29 here and the root task's table has 75 slots:

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_EFUSE` | `0x600B4800` | 1 KiB | the eFuse's registers, the factory MAC address among them, short of the OTP debug registers above them | read |
| 23 | `BOOT_CAP_ROM` | `0x40000000` | 512 KiB | the ROM | read, execute |
| 24 | `BOOT_CAP_IO_MUX` | `0x60090000` | 4 KiB | the IO MUX, the pins' functions and pads | read, write |
| 25 | `BOOT_CAP_GPIO` | `0x60091000` | 4 KiB | the GPIO matrix, the pins' routes, levels and interrupts | read, write |
| 26 | `BOOT_CAP_SPI2` | `0x60081000` | 4 KiB | SPI2, the general-purpose SPI controller, which drives the board's LCD | read, write |
| 27 | `BOOT_CAP_RNG` | `0x600B2800` | 128 B | LPPERI's first words, the PMP's grain, which hold the random number generator's: `RNG_DATA_SYNC` at `0x28`, a new word at every read, and `RNG_CFG` at `0x24` | read |

The modem is not listed yet, nor PCR, PMU and the LP domain beyond the random number generator's words, nor the GDMA, which is the kernel's.
`BOOT_CAP_DMA` names the GDMA's three transmit channels, 0 to 2, section 5.14,
each reading memory, its source, named 0, which may be any of the SRAM a frame holds,
into a device, its sink, named by the request the GDMA takes for it, through a frame over the device's registers whole:
SPI2 is request 1, `DMA_REQUEST_SPI2` in `devices.h`, the one device listed that the GDMA feeds.
A transfer is whole words, from a word of the frame, and at most 32736 bytes, eight of the GDMA's descriptors;
the GDMA's own window keeps every channel within the SRAM besides.
The kernel has the generator sample its ring oscillator, as ESP-IDF does at its start, besides the two timers it stirs in as the ROM leaves it;
its noise from the SAR ADC and the radio comes only while they run, which nothing yet makes them do on this chip,
and its words look random whether it samples or not, so how much entropy they carry is not known.
SPI2's clocks lie in PCR, so the kernel sets them at boot: its master clock runs at 80 MHz, PLL_F160M halved,
which a program divides further in SPI2's `SPI_CLOCK`, and the controller comes out of a reset of its own, whatever the last program left in it.
It sends what the CPU writes into its 64-byte buffer, a transfer at a time,
or, by a channel of `BOOT_CAP_DMA`, a transfer from memory of up to the GDMA's longest,
with its `SPI_DMA_CONF` set to take its data from the GDMA.
`user/lcd/` drives the LCD of Waveshare's board with it, an ST7789 of 172 by 320 pixels,
and renders on the host, against a model of the LCD, what it shows: `make lcd-sim`.
`make BOARD=esp32c5 lcd LCD_CONFIG="scene=stress mhz=80"` runs it on the board, its scene and its bus's clock as named;
`scene=noise` draws snow from the random number generator, 600 pictures, or `pictures=` as many, `pictures=0` without end.
`scene=veins` grows a network of glowing threads from it, a slime mould's, as many pictures,
in 256 KiB of the free RAM and the further block below it, which take the region slots of the input and the IO MUX once the pins are set up,
its pictures going out by DMA from the further block, or through SPI2's buffer for `dma=0`,
and as fast as it can draw them for `sync=0`, torn, rather than every other refresh.
GPIO_EXT, with its sigma-delta modulator, lies in the GPIO's own page, on the IO MUX's clock, which the kernel leaves as the chip starts,
so a program that holds the GPIO can drive a pin at a density of its own; `user/lcd/` dims the backlight so.
The flash's pins are in the IO MUX, so a program that holds it can cut the flash off, the root task's code with it,
and so are the USB pins, which carry the console and the reset into the ROM's download mode.

The CPU runs at 160 MHz, which the kernel sets at boot, and the clock counts its cycles,
measured against the 16 MHz system timer as on the ESP32-C6.
The compare is the CLINT's here too.
The PMP has sixteen entries and a 128-byte grain, so the smallest frame a program maps is 128 bytes.
Misaligned loads and stores go through in hardware.
The core refuses user mode the CLINT, and holds no copy of `mtime` user mode may load,
so no counter is listed among the devices, and a program reads the time through `OP_CLOCK_READ` alone;
the demo skips what it times with loads of the counter.

User mode can write some of the core's CSRs:
`ustatus`, `utvec`, `utvt`, `uscratch`, `uepc`, `ucause`, `uintthresh`, one at `0x017` Espressif names nowhere,
and the dedicated GPIO at `0x803` and `0x805`.
Whenever another process has run, a program finds them set back: `utvec` at 3, `uintthresh` at 15, the others at zero.
`ucause` holds `ustatus`'s UPIE in its bit 27, so a write of either is a write of both.
No interrupt is delegated to user mode.

#### RP2350

The bootrom's BOOTSEL mode loads the image into SRAM over USB and reboots into it,
on the RISC-V cores or on the Arm ones, whichever the image is for;
nothing is written to flash.
The two have the same layout, lines and console; what differs is said below.
The chip has no serial port of its own on USB,
so the console is the USB controller, of which the root task makes a USB serial port,
`user/board/rp2350/console.h`, and the halt makes another of it to send the host what no logger took.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20000000` to `0x2001F000` | 124 KiB | kernel code, data and stack |
| `0x2001F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x20020000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x20020000` | 64 KiB | root task code, read and execute |
| `0x20030000` | 32 KiB | root task data and stack, read and write |
| `0x20038000` | 4 KiB | input region, read only; nothing fills it yet |
| `0x20039000` | 4 KiB | the boot pool |
| `0x20040000` | 256 KiB | free RAM, granted to the root task as an Untyped with all rights |
| `0x50100000` | 128 KiB | the console: the USB controller's DPRAM, and its registers 64 KiB on, granted to the root task |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; TIMER0's IRQ 0 cannot be bound |
| 1 to 51 | the system IRQs, numbered as in the datasheet; the console's is 14, `USBCTRL_IRQ` |
| 26 | SIO's doorbell; on the Cortex-M33 with `CORES=2` the kernel's, for its cores, and an `Irq` cannot be bound to it |

The root task is granted a frame over each of these devices, from `BOOT_CAP_DEVICES` up,
so `BOOT_CAP_COUNT` is 28 here and the root task's table has 74 slots, and `BOOT_CAP_ROUTES` is empty, since it has no router;
the kernel takes each device out of reset at boot and opens it to user mode in ACCESSCTRL,
and on the RISC-V cores opens every pin to Non-secure access, which is how user mode reaches the bus there:

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_IO_BANK0` | `0x40028000` | 16 KiB | IO_BANK0, the pins' functions, overrides and interrupts | read, write |
| 23 | `BOOT_CAP_PADS_BANK0` | `0x40038000` | 16 KiB | PADS_BANK0, the pins' pads | read, write |
| 24 to 26 | `BOOT_CAP_PIO0` to `BOOT_CAP_PIO2` | `0x50200000`, `0x50300000`, `0x50400000` | 16 KiB each | PIO0, PIO1 and PIO2 | read, write |
| 27 | `BOOT_CAP_COUNTER` | `0x400B0020` | 32 bytes | TIMER0's, `TIMERAWL` at 8 into them, 1 MHz, the clock's counter on either kind of core; the word below it is the high half | read |

Each but the counter's is a device's registers and their atomic aliases, and none is a bus master.
The ADC is not listed: its clock is not running, and it never leaves reset without one.
Nor is the DMA, which is the kernel's: `BOOT_CAP_DMA` names its sixteen channels, 0 to 15, section 5.14,
each reading memory, its source, named 0, which may be any of the SRAM a frame holds,
into a transmit FIFO of a PIO block's state machine, its sink, named by the FIFO's DREQ, through a frame over the PIO block whole:
0 to 3 PIO0's, 8 to 11 PIO1's, 16 to 19 PIO2's, as the datasheet numbers them.
A transfer is whole words, from a word of the frame, and as long as the frame;
the FIFO's DREQ paces it, so it reads no further ahead than the state machine takes.

The kernel takes the USB controller out of reset at boot and opens it to user mode in ACCESSCTRL,
and starts its 48 MHz clock.
`user/board/rp2350/console.h` makes a CDC-ACM port of it, "rvuos console", which `tools/rp2350-run.py` reads as the machine runs.
It waits for the host to open the port before it sends,
and the halt resets the controller to make its own port, "rvuos",
so a program flushes its console before it halts, or what it had not sent by then is lost.
A watchdog reboots the chip into BOOTSEL about seventeen seconds after boot,
unless the kernel's watchdog is fed, section 5.8: each feed sets the chip's to a second past the kernel's deadline.

On the RISC-V cores the timer is the RISC-V platform timer's compare, on its microseconds.
The PMP holds seven regions, none smaller than 32 bytes:
the core has eight entries, and the kernel spends one shutting the three it hardwires open to user mode.
A fault reports `mtval` as zero, and a misaligned access raises a misaligned exception.
A program reaches a peripheral only through a frame, as it reaches RAM,
and only where the chip's ACCESSCTRL lets user mode in too: TIMER0, for the clock, is the one opened.

On the Cortex-M33 the timer is SysTick against TIMER0's counter, the one `BOOT_CAP_CLOCK` names.
The MPU has eight regions, none smaller than 32 bytes, and a fault is reported as on `mps2-an385`.
A program reaches a peripheral only through a frame, as it reaches RAM.

With `CORES=2` the kernel starts the second core at the end of the boot, on either kind of core,
and a process has the same regions on both, where an atomic operation on RAM reaches the other core;
every device line reaches both cores' controllers, of which only the first core's enables any.

#### nRF52840

`tools/nrf52840-run.py` loads the image into RAM over SWD, through an ST-Link and OpenOCD, and starts the Cortex-M4;
nothing is written to flash, and a program that never halts runs on once the runner stops, until the chip is reset.
The console is a block of RAM, which the runner follows as the core runs, with no pin wired.
`--text address:file` leaves a text in RAM before the core starts, as the other boards' runners do;
`make BOARD=nrf52840 thread` leaves `THREAD_CONFIG`'s file in the input region, `user/thread/`'s configuration,
and `make BOARD=nrf52840 observer` `OBSERVER_CONFIG`'s, `user/observer/`'s.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x20000000` | 8 KiB | the console, granted to the root task |
| `0x20002000` to `0x2000F000` | 52 KiB | kernel code, data and stack, the vector table first |
| `0x2000F000` | 4 KiB | the kernel log: a 32-byte header and the ring |
| `0x20010000` | 64 KiB | free RAM, granted to the root task as an Untyped with all rights |
| `0x20020000` | 128 KiB | the root task's memory, granted to it as an Untyped with all rights, which has made the four ranges below and makes nothing more |
| `0x20020000` | 64 KiB | root task code, read and execute |
| `0x20030000` | 32 KiB | root task data and stack, read and write |
| `0x20038000` | 4 KiB | input region, read only; nothing fills it yet |
| `0x20039000` | 4 KiB | the boot pool |

Interrupt lines:

| Line | What |
|---|---|
| 0 | the kernel log; POWER_CLOCK cannot be bound |
| 1 to 47 | the peripherals' lines, numbered by their IDs; nothing raises the console's line, 20, SWI0 |

The root task is granted a frame over GPIO, one over the radio, one over each of TIMER2, TIMER3 and TIMER4,
and one over the clock's counter, from `BOOT_CAP_DEVICES` up, so `BOOT_CAP_COUNT` is 28 here and the root task's table has 74 slots:

| Slot | Constant | Range | Size | What | Rights |
|---|---|---|---|---|---|
| 22 | `BOOT_CAP_GPIO` | `0x50000000` | 4 KiB | GPIO, both ports, P0's registers and P1's at `0x50000300` | read, write |
| 23 | `BOOT_CAP_RADIO` | `0x40001000` | 4 KiB | RADIO, the 2.4 GHz radio of Bluetooth LE and IEEE 802.15.4; its line is 1 | read, write |
| 24 | `BOOT_CAP_TIMER2` | `0x4000A000` | 4 KiB | TIMER2, which times what is shorter than a kernel's step, such as 802.15.4's backoffs; its line is 10 | read, write |
| 25 | `BOOT_CAP_TIMER3` | `0x4001A000` | 4 KiB | TIMER3, for routes, such as an acknowledgement's turnaround; its line is 26 | read, write |
| 26 | `BOOT_CAP_TIMER4` | `0x4001B000` | 4 KiB | TIMER4, for routes, such as a count of the frames received; its line is 27 | read, write |
| 27 | `BOOT_CAP_COUNTER` | `0x40008540` | 32 bytes | TIMER0's `CC` registers, `CC[1]` at 4 into them, 1 MHz, the clock's counter; no word holds the high half | read |

The router is the PPI, and `BOOT_CAP_ROUTES` names its channels 1 to 19; channel 0 is the kernel's counter's, below.
An end is the offset of a register in its device's 4 KiB block, as the product specification gives it:
a task, `ROUTE_TASK`, at 0x000 to 0x07C, an event, `ROUTE_EVENT`, at 0x100 to 0x17C,
of a device between `0x40000000` and `0x4003FFFF`, and any other end fails;
so `0x10C`, the radio's `EVENTS_END`, and `0x008`, a TIMER's `TASKS_COUNT`, of their frames, section 5.13.

The radio is a bus master: its EasyDMA reads and writes a packet wherever its `PACKETPTR` points, the kernel's RAM too,
so a program that holds its frame reaches all RAM,
and the root task gives it only to a driver it trusts as it trusts itself, as the ESP32-C6's modem; see `DESIGN.md`, open decision 13.

The console's first word counts every byte written, and byte n lies at offset 16 plus n modulo 8176,
so it is a ring, and the runner says how much it lost when it fell a whole ring behind.
The timer is the counter, with SysTick for the compare.
The counter is TIMER0's, which TIMER1 and a PPI channel capture into `CC[1]` every microsecond;
it is 32 bits wide and wraps every 71 minutes, and `OP_CLOCK_READ` counts the wraps.
The MPU has eight regions, none smaller than 32 bytes, and a fault is reported as on `mps2-an385`,
but for a call whose frame the core cannot stack, which comes as the call, exception 11, not as a MemManage.
The floating-point unit stays shut, so a floating-point instruction faults its thread.

#### mps2-an385

QEMU loads the image into SSRAM1 and resets the Cortex-M3, which finds the kernel's vector table at 0;
the halt leaves QEMU through semihosting.

Memory map:

| Range | Size | What |
|---|---|---|
| `0x40028000` | 32 B | the FPGA's first registers, `COUNTER` at `0x18` into them, 25 MHz, the clock's counter, granted to the root task read only, `BOOT_CAP_COUNTER` in slot 22 |
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

The timer is the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide and wraps every 171 seconds; `OP_CLOCK_READ` counts the wraps.
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
| `0x50302000` | 32 B | the FPGA's first registers, `COUNTER` at `0x18` into them, 20 MHz, the clock's counter, granted to the root task read only, `BOOT_CAP_COUNTER` in slot 22 |
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

The timer is the FPGA's counter, with SysTick for the compare.
The MPU has eight regions, none smaller than 32 bytes.
The counter is 32 bits wide and wraps every 214 seconds; `OP_CLOCK_READ` counts the wraps.
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
The ESP32-C6 and ESP32-C5 need Espressif's `esptool`, version 5, as a command and as a Python module.
RP2350 needs Python's `pyusb` and write access to the chip's USB devices,
which a udev rule such as `SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", TAG+="uaccess"` gives.
The nRF52840 needs OpenOCD, version 0.12, and an ST-Link on its SWD pins, which OpenOCD's udev rules open to the user.

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
Images go under `build/<board>/`, the host build under `build/host/`;
a smaller budget adds `-pmp<n>` to both.
`SELFCHECK=1` builds a board's kernel with the self-check and the tracing, section 9, which only QEMU `virt`'s has otherwise,
and `SELFCHECK=0` QEMU `virt`'s without them, under `-selfcheck` and `-noselfcheck`.
`make CORES=2` builds the kernel for two harts of QEMU `virt`, under `build/qemu-smp2/`,
and `make CORES=2 run` boots it with `-smp 2`;
`make BOARD=mps2-an521 CORES=2` builds it for that board's two cores, under `build/mps2-an521-smp2/`,
and `make BOARD=rp2350 CORES=2` for RP2350's, under `build/rp2350-smp2/`, or `build/rp2350-arm-smp2/` with `ARCH=arm`.
No other board takes `CORES`.

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

Opening the port resets the chip into the ROM's download mode,
where `tools/esp32c6-run.py` writes into flash what it does not hold already,
then resets the chip to boot from flash and reads the console on the same connection.
The replay driver is QEMU's alone for now.

On RP2350, in BOOTSEL mode, as it is when plugged in with BOOTSEL held and again after each run:

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
`user/wifi/tools/cyw43-blob.py` fetches the CYW43439's firmware, CLM and NVRAM from one commit of embassy,
under Infineon's Permissive Binary License, checks each against its SHA-256 and packs them,
and `tools/rp2350-run.py --ram` places the blob at the start of free RAM beside the image; nothing is written to flash.
`tools/wifi-run.py` runs the loader, prints the system's log as it comes over the network,
and once the clients answer, checks them from the host: the status port, the echo, the clock,
the echo built again after a fault and after a hang, and ping.
The run lasts half a minute, or the seconds of the configuration's `run=`, `run=0` for good.

`tools/rp2350-run.py` loads the image through the bootrom's PICOBOOT interface,
waits for the halt's serial port and reads it; closing it reboots the chip into BOOTSEL.

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

The replay driver is not built; its layout is QEMU virt's.

The kernel image embeds one user program, the root task.
The images differ only in that program:
`kernel-init.elf` carries the demo of `user/init/`,
`kernel-fuzzdrv.elf` the replay driver of `user/fuzzdrv.c`,
`kernel-escape-*.elf` a scenario of the escape suite each,
`kernel-libtest.elf` the library's test of `user/libtest/`,
`kernel-bench.elf` the benchmark of `user/bench/`,
`kernel-lab.elf` the laboratory of `user/lab/`,
and `kernel-wifi.elf` the Wi-Fi system of `user/wifi/`.

### Reading the transcript

The kernel has no console.
Everything the kernel prints, and everything a program writes with `OP_PRINT`,
goes into the kernel log.
Two things carry that log to the board's UART:

- the root task's logger thread, while the machine runs,
- the halt, which writes out every byte no reader has taken,
  under the line `rvuos: halting, the log follows`.

So a line that appears before that marker was carried out from user mode,
and a line after it was salvaged by the halt.
On RP2350 the halt's reach the host on a USB serial port of their own, after the console's has gone.

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
| 0 | `OP_HALT` with code 0 |
| 1 | kernel panic |
| 3 | the self-check found an invariant violated |
| 5 | no runnable thread and nothing armed that could make one |
| 6 | tracing is on and a thread the host build cannot follow was about to run |
| 7 | the watchdog was not fed in time, after the line `watchdog: not fed in time`, section 5.8 |
| other | what the root task passed to `OP_HALT` |

A user fault, an access fault, an illegal instruction, a misaligned access or a breakpoint,
prints `user fault` followed by `mcause`, `mepc` and `mtval`,
on ARM by the exception number, the configurable fault status `cfsr`, the `pc`,
and the address `MMFAR` or `BFAR` names, zero where neither is valid,
and stops the thread that made it and nothing else, section 5.6;
a program whose only thread faults therefore ends in `no runnable thread`, code 5.
