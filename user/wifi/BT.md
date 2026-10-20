# The CYW43439's Bluetooth core

An experiment behind `WIFI_BT=1` in `user/wifi/`:
the chip's Bluetooth core, brought up beside the running WLAN firmware
and taken over with a little code of our own in the core's RAM.
Without `WIFI_BT` the Wi-Fi system is as it would be without this file, and nothing of it is built.

## Building and running

    make BOARD=rp2350 wifi WIFI_BT=1

`WIFI_BT=1` builds in a directory of its own and defines `WIFI_BT` for the sources,
and its blob, `build/cyw43/blob-bt.bin`, carries the core's patch after the NVRAM:
`tools/cyw43-blob.py --bt` fetches it beside the firmware, from the same commit of embassy, and the header says where it lies.
`bt.c` brings the core up with our code on it, checks that the code answers and what the core's timer and a write into the modem cost,
and runs the LE transmitter test, 37 bytes of PRBS9, on one channel for about a second.

The configuration, the file `WIFI_CONFIG` names, may carry:

- `bt_channel=`, the test's channel, `0..39`, at `2402 + 2 * channel` MHz; 19 if absent.
- `bt_mem=0x…` and `bt_mem_at=0x…`: a word the host writes into the core's RAM before the test.
- `bt_poke=0x…` and `bt_poke_at=0x…`: a word our code on the core writes into a register on every call of its point, the persistent poke below.

`bt.c` arms a poke only at a word it can vouch for keeping the carrier in the band:
the channel index `0x318200` (`0..39`),
the carrier `0x3186a4` (`0x80 | (f - 2402 MHz)`, its low seven bits `0..78` with bit 7 set and nothing above),
and the access address `0x318b70` (any word).
A poke anywhere else, or with a value outside those, is refused and none is armed,
so a typo cannot carry the carrier out of the band.
Each run must show the transmission where it belongs:
the observer hearing it on the channel it listens on, and nothing outside it.

## Seeing the bits be ours

The observer is a BLE adapter on the host in the LE receiver test on the test's channel, counting the packets it hears.
With

    bt_mem=0x12345678
    bt_mem_at=0x20315c

it hears nothing, where the same run with the test's own access address, `0x71764129`, in that RAM word hears the test as usual, in the thousands.
One word differs, the access address the firmware itself programs into the modem, so the bits on the air are ours.

## What holds

- The core rises beside the WLAN firmware,
  and it answers HCI over the ring the WLAN firmware keeps in its RAM.
- Its memory is reachable over the backplane at `0x19000000` and up.
  The first read in that window lags, so a word is taken from the second read.
- Its ROM is read only; the vendor patch's code lies in writable RAM above it, from `0xd0000` up.
- The ROM branches into that RAM at eighteen points, and taking one over runs our own code on the core.
  Our code is `btcore/bt-stub.S`, built for the core, a Cortex-M3, and linked where it sees it by `btcore/bt-stub.ld`:
  `.takeover` over the point at `0xd0200`, `.text`, the path every call runs, at `0xd0d00`,
  the mailbox at `0xd0e00`, and `.measure`, the timed run, at `0xd0e24`; `btcore/bt-map.h` holds the addresses alone.
  `bt.c` writes each piece as one block, the way the patch itself is written; word by word, a longer stub did not take.
  The call target in it, `0x1b1e6`, is a label in a section pinned at that address,
  so the assembler encodes the call as an address, where a bare number would become a branch by that offset.
- The point is reached as the firmware programs the modem for a transmission:
  our code runs hundreds of times in a burst as the transmitter test starts,
  and not while the controller idles, nor while the test sends.
- Our code leaves the core's registers as it found them, since the code it returns into is the firmware's, mid-flow.
- Some of what the firmware programs the modem with it reads out of RAM as the test starts, the access address at `0x20315c` among them,
  so a word the host writes there before the test the firmware itself writes into the modem, last;
  a write to the register itself it overwrites.
- With all of it in place, the controller came up in 30 starts of 30,
  10 each of the Wi-Fi system without `WIFI_BT`, with the patch and no takeover, and with ours.

## The mailbox

One word each, from `BT_MBOX`: `magic`, `cmd`, `arg0`, `arg1`, `result`, `done`, `runs`, and the poke's register and value.
The host fills it and our code on the core serves it.

- The host puts the address in `arg0`, a value in `arg1`, then `cmd`, and `magic`: `MB_ARMED`.
  It waits for `done` to move and reads `result`.
  Command 1 reads the word at `arg0`; 2 writes `arg1` there;
  3 counts into the word at `arg0` `arg1` times and 4 writes its own value back as often, each returning the ticks of the core's timer the run took.
- Our code serves a command only when `magic` is `MB_ARMED`, and clears it and `cmd` when done,
  so the rubbish a boot leaves in RAM is never acted on.
  `runs` counts every call of the point, whether or not a command waits.
- Since the point runs only as the modem is set up, the host arms its calls right after the test starts.
- The poke is the one thing our code does on every call:
  it writes the value into the register the mailbox names, right after the point's own write, so ours is the last one in.
  The firmware writes the modem's words itself on each call, so a one-time write of them is overwritten and says nothing.
  The register is the switch, zero, as the mailbox comes up, meaning no poke.
- The timer is the firmware's own: its microsecond delay, `fn 0x7130`, spins on a free-running counter at `0x0032a004`,
  which counts down, and reading it is passive.
  Writing SysTick's reload or control instead disturbs the transmission.
  Runs of 64 and 128 writes into a word of our RAM take 16 and 32 ticks, the word moving by the run's length,
  and 50 ms of the host's time is about 52,500 ticks, so it runs at about 1.05 MHz.
- A write into the modem costs about 0.45 µs:
  64 and 128 writes of `0x318054`'s own value back take 30 and 59 ticks.
  That is against the 0.5 µs a loop setting the frequency for each chip of O-QPSK at two megachips a second would have to keep to.

## What the core's memory holds

Dug out of the ROM and a dump of the core's memory off the board, with a disassembler, an emulator and Ghidra:

- Two RAM regions, from the firmware's own accessor `fn 0x9210`:
  `[0x200000, 0x220000)`, 128 KiB, the main RAM,
  and `[0xd0000, 0xd8000)`, 32 KiB, where the vendor patch's code is loaded.
- The reset, `0x1dc`, sets `SP` to `0x00200400`, so the stack is `0x200000..0x200400`, growing down,
  and the firmware's globals run up from about `0x200400`.
  The mailbox sat at `0x2003f8` once, the top of the stack, where its run counter landed on a word the firmware uses.
- The vendor patch writes code at `0xd0200..0xd0cd4`
  and data at `0x2038d4`, `0x204164`, `0x21e000..0x21e042` and `0x21f000..0x21fc22`;
  the eighteen ROM branches into it come from a table at `0x209d28`.
- The rest of `[0xd0000, 0xd8000)`, from `0xd0cd4` up, the firmware uses at run time, through a computed base,
  since no word of the image points into it.
  The windows that measured clean are where our code lies: `0xd0d00` to `0xd0d99`, and `0xd0e00` to about `0xd0e77`.
  Code reaching about `0xd0d9a` breaks the core even with no command armed, since the firmware reads there on a transmission,
  and a block reaching about `0xd0e78` or `0xd0f1c` breaks the bring-up.
  Where exactly the edges lie depends on the bytes, so the code is kept inside the measured windows.

## Setting the carrier

- The word that sets the carrier is `0x3186a4`, a byte: `0x80 | (f - 2402 MHz)`.
  The firmware's own hopping gives it:
  the handler at `0x714ce` picks a data channel with BLE's `(x + 5) mod 37`,
  maps it to a frequency as `2 * channel + (2 if channel <= 10 else 4)`, which is `f - 2402` in MHz,
  and writes that, with bit 7, there.
  The transmitter test's setup writes the same word, out of the structure it is handed,
  and the hopping function `0x6b4a8` writes it beside `0x318200`, the channel index, `0x318220` and `0x318038[31:24]`.
- Our poke sets it.
  With the test on channel 19, 2440 MHz, where the word reads `0xa6`, and the poke at `0xa4`, channel 18's word,
  the observer on channel 18 hears the test (2045, 2049 and 2051 packets in three runs) and on channel 19 nothing (0, 0, 0),
  where with no poke channel 19 hears it (2080).
  With the test on channel 10 and the poke at `0x92`, channel 9's, the observer on channel 9 hears it (1995, 2022).
- The channel index `0x318200` does not set the carrier:
  with the test on channel 10 and the poke writing `0x318200 = 9`, channel 9 hears nothing and channel 10 the test (0 and about 2060, three runs each),
  though poking the access address the same way does land, the observer on the test's channel then hearing nothing.
  It is written and never read, 19 writers and no reader, as is its neighbour `0x318204`,
  and its common writer, `fn 0xa1ec`, has 31 callers: a general parameter word of the modem, the channel one of its uses.
- The carrier word is live, not latched at a tuning:
  the host writing `0xa4` into the modem well into a transmission on channel 19
  leaves channel 19 the part before the write (784 of the test's 3189 packets) and gives channel 18 the rest (1909),
  where without the write channel 18 hears nothing.
  Writing `0xa6` back returns the carrier to channel 19.
  Our own code cannot make such a write while the test sends, since its point is reached only as the modem is set up.
- The word is whole megahertz, and every writer of it writes whole megahertz;
  no finer word for the carrier was found in the image.
- The test's setup also writes `0x318b6c`, `0x01002340`, a configuration of the modem's path and mode
  that other writers set to `0x01002140`, `0x0d020100` and `0x0d000140`, and whose single bits several functions set and clear.
  The hardware reads it live: clearing it while the test sends stops the transmission rather than moving it
  (the test's channel hears 783 of 3190 packets against 2692 with no write, and the neighbour nothing).
  It is the same on every channel, so it is not the carrier; until its fields are worked out, only `0` or the firmware's own value is written there.

## What does not

- The controller exposes no LE 2M and no extended advertising.
  The LE feature mask is a RAM field, and writing it changes what the controller reports, but it adds no PHY commands.
- Writing the modem's clock and mode fields from the host does not retune the radio:
  two receivers saw it stay where it was.
- The clock fields the firmware reads out of RAM it fills and reads as the core starts, before the host can write,
  so the RAM way does not reach them.

## Not known

- Whether the silicon has a 2 Msym/s mode at all.
  The vendor patch carries one modulator configuration,
  and the newest such patch found, `CYW4343A2_001.003.016.0065`, also a Raspberry Pi build, against `001.003.016.0031` here,
  loads and reports the same LE feature mask, its 2M bit clear.
  That is a hint, not a proof about the silicon: a 2 Msym/s mode would be a clock, not a table, and our own code runs on the core.
- Whether the carrier can be set finer than a megahertz, and how fast a write of it settles;
  the observer counts whole packets, so "at once" above means within a packet's time, not within a chip's.
- What the firmware keeps in the RAM beside the patch, and so where more of our code could go;
  asking the firmware's own allocator for a block would avoid the question.
