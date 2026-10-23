# The CYW43439's Bluetooth core

An experiment behind `WIFI_BT=1` in `user/wifi/`:
the chip's Bluetooth core, brought up beside the running WLAN firmware
and taken over with a little code of our own in the core's RAM.
Without `WIFI_BT` the Wi-Fi system is as it would be without this file, and nothing of it is built.
The registers' facts gather in the chip's map, `user/wifi/hw/`; this file tells how they were found.

## Building and running

    make BOARD=rp2350 wifi WIFI_BT=1

`WIFI_BT=1` builds in a directory of its own and defines `WIFI_BT` for the sources,
and its blob, `build/cyw43/blob-bt.bin`, carries the core's patch after the NVRAM:
`user/wifi/tools/cyw43-blob.py --bt` fetches it beside the firmware, from the same commit of embassy, and the header says where it lies.
`bt.c` brings the core up with our code on it, checks that the code answers and what the core's timer and a write into the modem cost,
and runs the LE transmitter test, 37 bytes of PRBS9, on one channel, twice:
a fifth of a second for the self-check, which needs our code's point to come round,
and, two seconds after it, `BT_SETTLE`, about a second for the observer.
For its first second or so after the core comes up, the firmware passes the point for every packet it sends,
and our code there makes each packet late, so that the observer hears none of them, or only some;
after that it passes the point once, as it sets the modem up, and the observer hears all but a few in a thousand.
Why it settles so is not known.
While a millisecond's sleep took nearer two, the test began after that second;
once the scheduler without a tick gave the host its millisecond, it began inside it, and the observer heard nothing.
A poke made once, at the settled test's one pass, moves the carrier as one made on every pass did.

The tools that take the core off the board, below, speak to the chip through the Wi-Fi system's bridge,
a build of its own on the Hazard3, which `user/wifi/tools/cyw43bridge.py` makes and boots:

    make BOARD=rp2350 WIFI_BRIDGE=1

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
- Its memory is reachable over the backplane at `0x19000000` and up, its registers among it.
  A read of its ROM or of the patch's RAM there gave the word of the read before
  while the core ran a tight loop out of the patch's RAM, and none did with the core asleep,
  so the core's own fetches likely hold that memory's port; `bt.c` takes a word from a second read.
  A slow register, `0x320104` among them, gives the word of the read before whatever the core does,
  and a second read of it its own;
  the bridge's `R` reads a known word of the core's RAM first,
  so that an answer repeating that word is known to be stale, and the read is made again.
- Once the vendor's patch has set the core up, the host reaches its memory and not its registers:
  the patch's table of settings writes `0x321520` with bit 17 set,
  and with that bit set every read of a register through the window gives the read before, while the RAM still answers.
  Written alone into the value the ROM leaves there, `0x403c0`, the bit silences the registers,
  and the other bits the patch changes do not.
- The core runs its ROM from the chip's power-up, before the host writes anything for it:
  with no patch, its threads start and idle, its timer counting.
  The patch's last two records are what the ROM waits for,
  `0x2038d4`, the address of the patch's configuration `0x21e000`, and `0x204164`, a function's;
  written before the rest, the core never gets ready.
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
  and not while the controller idles;
  once the firmware has settled, see above, not while the test sends either.
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
- Since the settled point runs only as the modem is set up, the host arms its calls right after the test starts.
- The poke is the one thing our code does on every call:
  it writes the value into the register the mailbox names, right after the point's own write, so ours is the last one in.
  The firmware writes the modem's words itself as it reaches the point,
  so a write of them the host made before is overwritten and says nothing.
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

## The core off the board

`user/wifi/tools/cyw43-rehost.py` boots the core from its reset in an emulator,
the ROM out of the dump and the vendor patch written over it as the host writes it,
and lists every register access the firmware makes, with the instruction that made it.
It takes the Cortex-M's exceptions itself, which the emulator leaves to its user,
so the firmware's threads start and switch as on the chip.
With nothing behind the registers, a loop that waits on hardware waits for ever,
and `--auto` answers it with guesses, which say how far the code goes, not what the chip would have said.
So answered, the firmware comes from its reset to its threads' idle loop
in about twenty register windows, `0x300000` to `0x650000`, and the Cortex-M's own at `0xe000e000`.

With `--board` the registers are the chip's own, through the Wi-Fi system's bridge, `WIFI_BRIDGE=1`, `user/wifi/bridge.h`:
each access the emulated firmware makes is the same access over the backplane, about half a millisecond,
so a loop that waits on hardware waits on the chip, and nothing is guessed.
The core's own processor is parked first, by the vendor's own way into the patch:
the patch goes in as the host writes it, but its code a slide of `cpsid i` into a loop of `wfi`,
so the ROM checks the signature, installs the patch and, at its first call into it, sleeps there, its interrupts masked.
Parked so, the core's memory and registers all read as they are, and its stack stays still.
The ROM then boots in the emulator a second time, over a chip it has set up once,
and comes to its threads' idle loop the same way, through 979 registers from 779 places in the code,
where the guesses of `--auto` had led it through 69 places the chip never sends it to,
the wait on `0x360424` among them.

Only the Cortex-M's own registers stay the emulator's.
The chip's interrupts reach the emulated core through the parked one:
the slide of `cpsid i` leads to a loop that, on its own SysTick, about a millisecond,
adds the lines pending in its NVIC to a word of its RAM and clears them, every line disabled, so none is taken there;
the emulator reads that word every few dozen accesses and pends the lines the firmware has enabled,
taking them by the NVIC's priorities and nesting them as the core does,
some hundreds of accesses apart, since the emulated core is far slower than the chip expects.
The parked processor sleeps in `wfi` only while the emulated one does, and otherwise spins,
since the chip's power manager watches the processor's sleep:
left asleep while the emulated firmware ran, the manager began a sleep cycle of its own,
and the firmware, finding one under way, cancelled it and waited for an interrupt its own mask kept from coming.

The patch goes in as the ROM reaches its idle loop, which calls `0x9834` on each turn,
and that calls the function at `0x204164` once the patch has set it, `0x5f7ec`:
the patch's configuration taken, at `0x200444`, and the core started again.
Started so, the firmware programs the patch's remapper,
a slot of it at `0x310000 + 4n` the address of a ROM word over four, and its enable bits from `0x310204`;
the emulator swaps the patched word in as a slot is enabled, out of the ROM read through the bridge before the patch, `--read-rom`,
which differs from the dump of a running core in exactly those words.
With bit 17 of `0x321520` kept clear, `--clear 0x321520=0x20000`, so that its registers stay within the host's reach,
the emulated firmware then brings the chip's core up:
the host reads the core ready, `BT_FW_RDY` and `BT_AWAKE` in `BT_CTRL`, as it does when the core runs the patch itself.

Past that, three more departures take the emulated firmware through HCI, `--hci`, which the run speaks as `bt.c` does,
through the ring in the WLAN core's RAM that the core reaches through its window at `0x680000`.
Each is where the emulation's slowness has the chip answer as it never would the core:

- The watchdog, `0x329000`, which the idle loop reloads with about a second's count:
  emulated, a second of the chip's passes long before the loop comes round, and the chip's core resets,
  which leaves the parked processor's ticks and its RAM as the ROM's start makes them.
  `--local 0x329008` keeps its control in the emulator, so the firmware reads it on and the chip never starts it.
- The link controller's status, `0x314004`, which the dispatcher of its lines, `0x16cce`, clears by writing ones and reads again
  until no event is left: its events latch again in less than the half millisecond a read takes,
  so the loop never ends, and with it the handler; `--w1c 0x314004` leaves the ones written out of its reads
  until the handler that wrote them returns, as on the chip, where a pass of the handler takes microseconds,
  and a newer event is the next interrupt's.
  Leaving them out of the next read alone was enough until the transmitter test,
  whose service reads the status at three places of its own between the dispatcher's two,
  and with the handler never returning, the core never took the doorbell's line again.
- The controller's sleep, which takes the registers' domain down as the emulated processor goes on:
  `--poke 0x201f60=1` sets a word that, when set, makes the idle loop's sleep check, `0x8a12`, return at once.

The doorbell is line 45, an event of the GCI block at `0x650000`, its status `0x6500ec` the host's control bits that changed;
line 16 is the timers' block at `0x300000`.
So run, the controller answers `HCI_Reset`, `Read Local Version` (HCI and LMP version 11, Cypress's 0x0131)
and `LE Read Local Supported Features`, the mask the native core reports, its 2M bit clear.

It takes the transmitter test as well, `--hci 201e:132500`, channel 19, 37 bytes of PRBS9,
and the chip sends it: the host's adapter in its receiver test hears it, packets with a good CRC and the test's access address,
none in the windows before the command or after the run, and the native test in the same run as a control.
The emulated core sends a hundred-odd a second, where the native one sends 1600,
and with the link's events held clear only for the next read, it never took the `LE Test End` after it;
held so until the handler returns, it answers the end too, with the count of packets it sent.
Its setup of the modem is the same from run to run, 39 writes from the command to the packet's header,
the link controller's events acknowledged, `0x318394`, `0x3180a0`, the path `0x318b6c`,
the access address `0x318b70`, the CRC's start `0x318ba4`, the carrier `0x3186a4`, the payload at `0x370800` and its header `0x318b68`,
apart from an interrupt's handler that may come between them.
`--record FILE` writes every access the emulated firmware makes of the chip, a line each, with the instruction that made it,
and each HCI command and event where it falls among them.

`user/wifi/tools/cyw43-replay.py` plays such a record back on the chip, the core parked as for the rehost and nothing emulated:
every access in its order, reads as well, their answers dropped, up to a line of HCI it is told,
and then writes words of its own again and again, `--again`.
Played up to the transmitter test's answer, the record leaves the chip silent,
every word of the test set up and nothing sent;
each write of `0x318014 = 0x80000005` from the host after it then sends one packet, at the bridge's pace,
5268 heard of 5695 written over ten seconds, where as many writes of `0x318b90`, the service's other write a packet, send none.
So the firmware starts each packet itself, by that word, and the hardware sends no more than one for it;
the whole of the radio's setup for the test, from the core's reset on, lies in the record,
and the host alone, writing it, makes the chip send the test.

## The transmitter from the host

Of the record's writes, eight set the transmitter up, written after the Wi-Fi system's start and the core's parking alone;
after them a write of `0x318014 = 0x80000005` sends one packet of the transmitter test,
at most one each 1.25 ms and with no payload, see below:

| Word | Value | What it is, as far as known |
|---|---|---|
| `0x410494` | `0x26c` | a word of the radio's block, `0x410000`; not known |
| `0x318028` | `0x202` | a word of the modem's; not known |
| `0x318bc0` | `0x2b08289` | a word of the modem's; not known |
| `0x318b70` | `0x71764129` | the access address |
| `0x318ba4` | `0x555555` | the CRC's start |
| `0x3186a4` | `0xa6` | the carrier, `0x80 \| (f - 2402 MHz)` |
| `0x318204` | `0x84` | the modem's parameter word beside the channel index |
| `0x318b6c` | `0x1006340` | the modem's path and mode, the test's `0x1002340` with bit 14 set as it starts sending |

They were found from the record of the emulated firmware's test, up to its answer, by leaving parts out and counting what the host's adapter heard.
Every read could go, if the writes kept the bridge's pace of one an ask;
then every window of registers but the modem's `0x318000`, the power manager's `0x320000` and the radio's `0x410000`,
each of which, left out alone, silenced the chip;
then the 10803 writes of `0x4106e8`, the strobe of a measurement the calibration takes 5400 times, whose results the record writes elsewhere;
then all but the last write of each register, which left 264 in the order of their last writes.
Of those, delta debugging kept eight, with each of the eight needed: any seven of them send nothing.
`user/wifi/tools/cyw43-replay.py` takes such a list as a record, its lines `w ADDR SIZE VALUE`.
The eight then send 1419, 1416 and 1422 packets heard in three runs of three seconds' writes,
about 470 a second at the bridge's pace,
none without `0x318b6c` and none with the trigger's writes made to a word of RAM instead;
with `0x3186a4 = 0xa4` the adapter hears 1383 on channel 18 and none on 19.
The power manager's words are not among the eight: left out with the rest of the record they silence the chip,
left out with what the eight leave out they do not.
The header `0x318b68` and the PRBS9 payload the firmware writes at `0x370800` are not among them either:
the packets carry no payload, and the adapter counts them all the same, checking the access address and the CRC.

## Our executor on the parked core

`user/wifi/tools/cyw43-exec.py` parks the core as the rehost does, but its slide leads to our executor, `btcore/bt-exec.c`,
which runs nothing of the vendor's: a list of operations the host leaves in the core's RAM,
writes, reads with the time, waits and loops, timed by the Cortex-M3's cycle counter at the parked core's 48 MHz.
An experiment is a list, and the code on the core stays the same.
The executor lies where `bt.c`'s stub does, beside the patch: the core fetches no code from its RAM at `0x200000` and up,
and a branch there locked the core up and reset it, its RAM cleared.
Its words are written once the core's power is up and before the patch, whose installing sends the core down the slide.
Its words lie from `0x21c000`, clear of the patch's data from `0x21e000` and of the rehost's mailbox at `0x21ff00`,
and its stack grows down below them.
Every write of the carrier in a list, or in the setup, is held to the band before the list goes over,
and no write goes to the patch's code, the executor's or its words.

With the eight words of the table set, the link controller's status `0x314004`, cleared before a trigger, shows a packet's time:
its bits 3 to 6 latch 290 to 590 µs after the trigger, bit 2 about 408 µs later and bit 0 127 to 129 µs after that,
while the Bluetooth clock `0x318088` steps every 312.1 µs, half a slot.
The packet itself is shorter: the ESP32-C6's sampler sees each on the air for about 95 µs,
a packet with no payload, 80 µs, and the ramps, as the eight words leave the header `0x318b68` at its reset value.
A trigger sends at most one packet each 1.25 ms, two slots:
written faster, the adapter hears the same 795 a second, and one written while a packet is under way goes for nothing.
Written once bit 2 has latched, a trigger sends every packet, 999 heard of 1000.

With the eight words alone, the first packet after the carrier changes is lost.
With the carrier written before each trigger, its packets in runs of k on channel 18 and k on channel 19, the adapter hears, of 600 on each,

| k | on 18 | on 19 |
|---|---|---|
| 1 | 50 | 0 |
| 2 | 328 | 300 |
| 3 | 415 | 400 |

and every packet when the carrier stays.
The lost packet is not sent on the carrier before the change either, for then alternate packets would all be heard, each on the other's channel.
How long before the trigger the carrier is written makes no difference, from 0 to about 840 µs,
nor does writing the hopping function's other words of the frequency, `0x318220` and `0x318038[31:24]`, beside it,
nor the parameter word `0x318204` again.
The ESP32-C6's sampler, its scan held at 2437 and 2438 MHz with its gain fixed, `gain=44`,
in 256 µs captures at 16 MHz, `dump=4096` and `dumprate=3`, sees what goes out instead.
A packet after a step down, 2440 to 2438 MHz, starts on its carrier and drifts up through the packet,
its last 16 µs 0.2 to 0.47 MHz above its first in all four seen, where the packets of a carrier that stays move by 0.01;
a packet after a step up, 2438 to 2440 MHz, is not seen at all, in 28 captures where about six would have held one,
the carrier's own packets being seen in 7 of 23.
So the synthesizer is set at the trigger and still pulls back toward where it was through the first packet,
or does not send it.
With its AGC left on, as the sampler's ring capture leaves it, the gain steps every 16 µs on so close a sender,
and no capture of it can be read.

One word more keeps every packet: `0x318394`, which reads 0 on the parked core
and which the vendor's test sets to `0x1a0100ff` and then `0x9a0100ff` as it starts.
With the record's last writes but the trigger's, 263, as the setup, alternate packets on 18 and 19 were heard 599 and 596 of 600,
and delta debugging over them, the eight kept, left that word alone: 598 of 600.
Either of two parts of it is enough on its own; with alternate packets on 18 and 19, the adapter hears, of 600 on each:

| `0x318394` | on 18 | on 19 |
|---|---|---|
| 0 | 54 | 0 |
| `0x1` | 1 | 93 |
| `0x2` | 220 | 600 |
| `0x4`, `0x8`, `0x10`, `0x80`, `0xff` | 599 to 600 | 599 to 600 |
| `0x02000000`, `0x10000000`, `0x14000000`, `0x00010000` | 31 to 56 | 0 |
| `0x08000000`, `0x18000000`, `0x1a000000` | 599 to 600 | 599 to 600 |

Its low byte is a time in microseconds: it lengthens the time from bit 2 of `0x314004` to bit 0 by one a unit,
129 µs at 0, 133 at 4, 194 at `0x40` and 384 at `0xff`, where bit 27 leaves it at 129;
a few microseconds more cannot settle a drift of 0.45 MHz, so what either part turns on is not known.
`btcore/bt-tx.rec` carries the vendor's value after the eight words, nine in all.

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
  Our own code cannot make such a write while the settled test sends, since its point is then reached only as the modem is set up.
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
- Whether the carrier can be set finer than a megahertz, and how fast a write of it settles
  beyond the first packet after a change, which drifts without `0x318394`.
- What `0x318394`'s low byte and its bit 27 turn on, either of which keeps the first packet after a change of the carrier,
  and what the rest of the word is.
- Why the firmware, for its first second or so, passes the point for every packet, and then settles.
- What `0x410494`, `0x318028` and `0x318bc0` of the transmitter's words are,
  and why the power manager's words silence the chip when left out with the rest of the record, and not with what the eight leave out.
- What the bits of `0x314004` that latch for a packet mean, beyond when each latches.
- What the firmware keeps in the RAM beside the patch, and so where more of our code could go;
  asking the firmware's own allocator for a block would avoid the question.
