# The Bluetooth core's registers

The core is a Cortex-M3; its ROM lies below `0xd0000`, the vendor patch's code from `0xd0000`,
its RAM from `0x200000` to `0x220000`, and its registers from `0x300000` up, see `user/wifi/BT.md`.
The host reaches all of it through the backplane's window, the core's address plus `0x19000000`;
the Cortex-M's own registers at `0xe000e000` it does not.

## The pages

Every 4 KiB page the vendor firmware touched, from the core's reset through the LE transmitter test,
with what the firmware did there, **traced**, and what the pages read, **read**.
The trace is one run of `user/radio/cyw43439/tools/cyw43-rehost.py --board` with the departures user/wifi/BT.md names
and the commands `HCI_Reset`, `Read Local Version`, `LE Read Local Supported Features`, the transmitter test and its end;
`user/radio/cyw43439/tools/cyw43-regmap.py trace` counts it.
The reads are `user/radio/cyw43439/tools/cyw43-regmap.py read` and `vendor`, every word of the page twice in a row:
parked, as the park leaves the core; set, once `btcore/bt-tx.rec` is in; sending, while the executor sends a packet a turn;
and vendor, once the record of the trace has been played back up to the transmitter test's answer.
Parked, set and sending are one power-up of the chip, vendor another.

| Page | Registers | Reads | Writes | Places | Parked | Set | Sending | Vendor | Moving | What it is |
|---|---|---|---|---|---|---|---|---|---|---|
| `0x300000` | 13 | 54 | 27 | 16 | 7 | 7 | 7 | 7 | 0 | the timers, line 16, **measured** |
| `0x310000` | 24 | 19 | 48 | 5 | 143 | 143 | 143 | 144 | 0 | the patch's remapper, **known** from the patch's use |
| `0x314000` | 4 | 195 | 89 | 31 | 2 | 2 | 3 | 3 | 1 | the link controller's status, `0x314004` |
| `0x318000` | 112 | 496 | 427 | 248 | 354 | 362 | 370 | 396 | 29 | the modem |
| `0x320000` | 52 | 1557 | 198 | 138 | 66 | 66 | 66 | 68 | 0 | the power manager |
| `0x321000` | 8 | 14 | 28 | 23 | 45 | 45 | 45 | 45 | 1 | holds `0x321520`, whose bit 17 cuts the host off |
| `0x326000` | 258 | 0 | 1295 | 5 | 0 | 0 | 0 | 0 | 0 | a table written and never read; reads zero |
| `0x327000` | 7 | 5 | 1630 | 9 | 0 | 0 | 0 | 0 | 0 | a table's index and data, see below; reads zero |
| `0x329000` | 2 | 0 | 5237 | 2 | 9 | 9 | 9 | 9 | 0 | the watchdog |
| `0x32a000` | 7 | 37782 | 14227 | 26 | 15 | 15 | 15 | 16 | 2 | the microsecond timer `0x32a004`, which counts down |
| `0x32c000` | 4 | 6 | 3 | 6 | 200 | 200 | 200 | 201 | 0 | unknown |
| `0x351000` | 2 | 0 | 6 | 2 | 3 | 3 | 3 | 198 | 0 | unknown; reads as anything only after the vendor's setup |
| `0x352000` | 4 | 97 | 8 | 7 | 23 | 23 | 23 | 23 | 0 | unknown |
| `0x360000` | 3 | 16 | 16 | 8 | 15 | 15 | 15 | 15 | 0 | unknown; the ROM waits on `0x360424` |
| `0x364000` | 6 | 4 | 16 | 10 | 3 | 3 | 3 | 3 | 0 | unknown |
| `0x370000` | 10 | 0 | 10 | 1 | 448 | 448 | 447 | 447 | 0 | memory: the payload the firmware writes at `0x370800` |
| `0x371000` | 256 | 768 | 768 | 4 | 464 | 464 | 464 | 464 | 0 | memory, each word read and written back |
| `0x410000` | 130 | 16999 | 12855 | 192 | 386 | 386 | 390 | 416 | 1 | the radio |
| `0x600000` | 129 | 928 | 2899 | 260 | 192 | 192 | 201 | 201 | 3 | unknown, the most places of any page |
| `0x640000` | 26 | 188 | 344 | 66 | 92 | 92 | 92 | 92 | 1 | holds `0x640894`, the core's power up, `BT_PWRUP` |
| `0x650000` | 25 | 179 | 188 | 55 | 124 | 124 | 124 | 161 | 4 | the GCI block: line 45, the host's doorbell, its status `0x6500ec` |
| `0x651000` | 34 | 78 | 0 | 4 | 78 | 78 | 78 | 78 | 0 | read only, unknown |
| `0x6e8000` | 11 | 38 | 0 | 10 | 117 | 117 | 117 | 117 | 0 | read only, unknown |
| `0x6e9000` | 19 | 0 | 34 | 7 | 0 | 0 | 0 | 0 | 0 | written only; reads zero |
| `0x6ea000` | 4 | 32 | 10 | 8 | 0 | 0 | 0 | 0 | 0 | reads zero, though the firmware reads it |
| `0x702000` | 1 | 5 | 5 | 2 | 22 | 22 | 22 | 22 | 0 | unknown |

Registers, reads, writes and places, the instructions that made them, are the trace's;
parked to vendor, the words of the page that read nonzero; moving, those that read differently twice in a row in any state.
No word failed to answer: a register that gives the read before was read again, as the bridge's `R` does.

What the reads leave open:
the pages from `0x370000` read alike in parked, set and sending, and differ in vendor, another power-up,
in about three words of four: memory whose contents a power-up leaves as they come.
`0x326000`, `0x327000`, `0x6e9000` and `0x6ea000` read zero throughout:
the firmware never reads `0x326000` or `0x6e9000`, and of `0x327000` only `0x327040`, so written only,
or not readable through the window.

## The modem, `0x318000`

| Word | What it is | How sure |
|---|---|---|
| `0x318014` | the trigger: `0x80000005` sends one packet, at most one each 1.25 ms; with `0x318204`'s bit 7 clear, `0x80000005` and then `0x80000085` arm the receiver once: bit 7 receives, bit 31 LE, bits 3:0 the event's kind, the last two read in the code | measured, read |
| `0x318028` | one of the transmitter's eight words, `0x202`: without bit 1 nothing is sent, bit 9 changes nothing seen | measured |
| `0x318038` | bits 31:24 written beside the carrier by the hopping function `0x6b4a8`; writing them changes nothing measured | traced, measured |
| `0x318088` | the Bluetooth clock, stepping every 312.1 µs; `0x318084`, read after it, counts up | measured |
| `0x3180a0` | written in the test's setup | traced |
| `0x318200` | written by 19 places, read by none, and does not set the carrier; `0xa204` writes it before it reads `0x31822c`, a clock, and `0x318228`, the microseconds in its slot, so an index of those, a hypothesis; the value that matched the channel a coincidence | traced, measured, read |
| `0x318204` | `0x84` for the transmitter's test, `0x04` for the receiver's: bit 7 sends | traced, measured |
| `0x318220` | written beside the carrier by the hopping function; changes nothing measured | traced, measured |
| `0x318394` | 0 parked, `0x9a0100ff` the vendor's: its low byte from 4 or its bit 27 keeps the first packet after a change of the carrier; the low byte adds microseconds between `0x314004`'s bits 2 and 0 | measured |
| `0x3186a4` | the carrier, `0x80 \| (f - 2402 MHz)`, read live | measured |
| `0x318b68` | the packet's header, bits 7:0 its first byte, 15:8 its length, 0 at reset; the payload from `0x370800`, as many bytes as the length's low 7 bits | measured |
| `0x318b10` | the receiver's window, in half microseconds and about 100 µs more, a short one ending at the next half slot; `0xffffffff` listens for good | measured |
| `0x318b6c` | the path and mode, `0x1006340` in either test; cleared while sending, the transmission stops; bit 14 the LE mode, set by `0x5b988` with `0x410430`'s bits 14:8, and not the sending; bit 21 the receiver's whitening, bit 20 nothing seen | measured, read |
| `0x318b70` | the access address | measured |
| `0x318b90` | the test service's other write a packet; written alone it sends nothing | measured |
| `0x318b98` | the received header: bits 7:0 its first byte, 15:8 its length, the same again in the high half | measured |
| `0x318ba4` | the CRC's start, `0x555555` | measured |
| `0x318bac` | the receiver's status: bit 1 the access address found, 2 the CRC good, 6 the CRC bad; `0x130036` for a good packet, `0x100072` for a bad one, `0x100041` for a window that ended empty, `0x104000` while it listens | measured |
| `0x318bc0` | one of the eight words, `0x2b08289`: bit 25 needed; bits 15:0 a time in 1/256 µs, bit 2 of `0x314004` that much nearer bit 0, `0x1` stopping the transmitter for the rest of the run; bits 16 to 23 change nothing seen; the ROM's LE receiver test sets bits 15:8 to `0xa1` | measured, read |
| `0x318bf4` | the whitening's seed, the channel's index: 37 dewhitens channel 37's packets, 0 and 38 do not | measured |

## The synthesizer, `0x600000`

`0x600104` holds the frequency as `0x80 | (f - 2400 MHz)`, **measured**: with the receiver armed at 2440, 2441 and 2450 MHz
through the carrier word it read `0xa8`, `0xa9` and `0xb2`; the channels' assessment writes it itself, **read**.
Of the page's 512 words from `0x600000` to `0x6007fc`, read in each of those states, 197 nonzero,
`0x600300`, `0x600310`, `0x60067c` and `0x60076c` follow the frequency, a code or so a MHz, `0x8d`, `0x8e` and `0x95` for the first,
the oscillator's band as its calibration finds it, a hypothesis,
and `0x60024c`, `0x60025c` and `0x60026c` differ from run to run at one frequency; no other word moved,
and none as a fraction of the reference would between two whole MHz:
the synthesizer works its fraction out of the whole MHz itself, and no word of the page sets the carrier finer.
A read from `0x600800` up stalls the core: the executor's list never ends.

## The link controller's status, `0x314004`

Cleared by writing ones.
After a trigger its bits 3 to 6 latch 290 to 590 µs on, bit 2 about 408 µs later, and bit 0 127 to 129 µs after that,
**measured** with the eight words of `user/wifi/BT.md`'s table alone; what each stands for is unknown.
The span from bit 2 to bit 0 is about 259.5 µs, plus `0x318394`'s low byte in µs, less `0x318bc0`'s bits 15:0 in 1/256 µs,
**measured** for each word with the other at the vendor's value, `0xff` and `0x8289`, and a hypothesis for the two together.

## The radio, `0x410000`

`0x410494` is one of the transmitter's eight words, `0x26c`, **measured**: without bit 9 nothing is sent, without bit 5 the packets are 8 dB weaker, and bits 2, 3 and 6 change nothing seen; see `user/wifi/BT.md`.
`0x410434`'s low byte is a received packet's strength, signed, a dB a step, **measured**, see "The receiver from the host";
it reads 0 when no packet came, and its bits 8 to 12 `0xc` with packets and `0x14` without.
The firmware adds a byte of its RAM, `0x202666`, to it for every report, `0xe7f4`, **read**.
Its bits 12:8 are the AGC's gain, live, **measured**, the receiver armed at 2440 MHz with its window open and an nRF52840's carrier there:
`0x14` with nothing sent, and with the carrier from -40 to 8 dBm in 4 dB steps `0x14`, `0x10`, `0x10`, `0xe`, `0xc`, `0xc`, `0xb`, `0x8`, `0x9`;
the firmware reckons it 3 dB a step, `(11 - gain) * 3` in `0x2fc4`, **read**.
`0x4106e8`, written `0x8000`, read and written 0 as `0x1fd92` does, reads in bits 13:0 a level after that gain, **measured**:
6 with nothing sent and 33 with the carrier at 8 dBm, the median of 100 reads 23.7 µs apart, any two within 4 of each other.
Over the steps 20 log10 of it less 3 dB a step of the gain went 36.4, 33.1, 27.6, 24.6, 22.1, 18.2, 15.4 and 11 from 8 dBm to -20,
0.9 dB for each, as the strength byte goes, where 10 log10 of it went by 0.2 to 7 dB a step:
a magnitude, and the gain's step 3 dB, a hypothesis.
Neither the level nor the gain is the channel's, **measured**, five tunings in each run:
with the carrier on 2440 MHz the receiver read 32 to 35 and a gain of 9 tuned to 2402, 2410, 2440 or 2480, armed again after the carrier word or not,
and with the carrier off 5 to 7 and 20 at the same tunings, where a packet is heard only on its own channel, see "The receiver from the host":
both see the band ahead of the channel's filter.
A sweep of the 79 channels with the carrier off read the same at every one, within the reads' spread.
`0x60029c` to `0x6002ac`, five words, are one each for the transmitter's sub-bands, **measured**:
with `bt-tx.rec`'s packets on 2440 MHz only `0x6002a4` set to 0 changes what the nRF observer hears, 1600 of 1600 at -69.4 dBm becoming 410 at -92.4,
and on 2410 MHz only `0x60029c`, 1577 at -67.5 becoming 115 at -93.1;
the code places the five at 2410, 2426, 2442, 2458 and 2474 MHz, **read**.
The parked core reads them `0x5c` or `0x5e`, then `0x52` or `0x54` for the other four, a little apart from one power-up to the next,
and the vendor's firmware searches `0x6002a0` through 14, `ff`, 89, `4e`, 31, `3f` and 46 as it starts, `0x5bc50`, and leaves them `4d`, 46, `3e`, `3b`, 39:
a calibration's results, of what is not known.
`0x6002a4` stepped down from `0x54` read -69.2, -70.5, -71.1, -72.3, -69.9, -65.5, -70.9, -76.6 and -92.5 at the observer
for `0x54`, `0x48`, `0x3c`, `0x30`, `0x24`, `0x18`, `0xc`, 6 and 0, the packets heard falling to about 950 of 1600 from `0x18` down:
not a power, which would fall with it.
At `0x24` against `0x54`, with the observer and the ESP32-C6's sampler at 2438 MHz on the same packets, two runs each alternately,
the observer read 3.5 dB less and lost 5 in 100, and the sampler saw the frequency's excursions unchanged, ±170 to 220 kHz either way.
`0x4106e8` is the busiest register of the trace, 16262 reads and 10843 writes from 7 places,
the strobe of a measurement the calibration takes thousands of times, whose results the firmware writes elsewhere, **traced**.

## The receiver from the host

The parked core receives with no firmware running, **measured**:
`btcore/bt-rx.rec` as `user/radio/cyw43439/tools/cyw43-exec.py`'s setup, the transmitter's nine words with `0x318204 = 0x04`,
and a list that writes `0x318b10 = 0xffffffff`, then `0x318014 = 0x80000005` and `0x80000085`,
waits for bit 0 of `0x314004` and reads what the packet left, clearing `0x314004` and arming again for the next.
The sender is an nRF52840 sending the LE test's packets on 2440 MHz, 37 bytes of PRBS9, one each 625 µs, at 0 dBm:

| What | Where | What it read |
|---|---|---|
| the status | `0x318bac` | `0x130036` |
| the header | `0x318b98` | `0x25002500`, type 0 and 37 bytes |
| the payload | `0x370a00` | the 37 bytes sent, and after them the packet's CRC, 3 bytes |
| the strength | `0x410434` | `0xcc6`, -58 |

In 39 packets of 39 the payload was the one sent; each is read 0.98 µs after the end's bit latches, the core's 48 MHz.

The vendor's own setup, every word of the modem, the power manager and the radio as its receiver test last wrote them before its first arming,
265 words, out of a record of `user/radio/cyw43439/tools/cyw43-rehost.py`'s run of the receiver test, `--hci 201d:13`,
reads the same packets 18 dB weaker, `0xcb4`, -76, and catches every packet,
where the nine catch one in four, 2500 µs apart, and at times one in three;
so the vendor's sets a gain or an offset of the strength that the nine leave, and a turnaround they leave long.
With the vendor's setup, **measured**:

- With the nRF silent no packet came in nine windows of a second, the status read `0x104000` and the strength 0.
- A packet sent with a CRC from another start lands all the same, its 37 bytes and its own CRC in memory, the status `0x100072`:
  the receiver hands over what it demodulates behind the access address, whatever the CRC.
- Of a packet of 255 bytes, `0x370a00` holds the 255 and the CRC's first byte, and reads 0 beyond: 256 bytes.
- The strength follows the sender's power, with the nRF's power changed packet by packet and written into the packet's first byte:

  | Sent, dBm | 8 | 4 | 0 | -4 | -8 | -12 | -16 | -20 | -40 |
  |---|---|---|---|---|---|---|---|---|---|
  | Strength, signed | -67 | -70 | -74 | -77 | -80 | -84 | -88 | -91 | not heard |

  24 over the nominal 28 dB, the slope the WLAN's receiver reads too,
  and within a dB of what the WLAN's reads on the same channel, `phy.md`'s "The receiver's strength", at each power:
  both read in dBm, a hypothesis the two receivers' agreement supports; the nRF's steps the likelier to be short.
- The estimator's words `0x4103f0` to `0x4103fc` read 0 throughout, `0x4103e4` `0x7f60`: the estimator off.
- The strength latches as the access address is found and keeps its value through windows that end empty;
  a window's status keeps bits 16 and 17 of the last good packet too.
- Only the channel itself is heard: with the carrier word stepped a MHz at a time from 8 below the sender to 8 above,
  12 windows of 8 ms each, three times, the channel found 36 packets of 36, every one good, and the others none,
  but for 2436 MHz, 4 below, where 4 of 36 windows found an access address and no good CRC, at -82 to -102.
  Those came with the nRF silent too, while the channel then heard nothing, and with a window left open their times
  were about 158 ms apart for one, its payload the same each time, and 131 or twice that for a weaker one:
  senders of the room's, not the nRF's image; what they are is not known.

The strength the vendor's setup reads is not one word's, **measured** by delta debugging, 20 packets a test:
beside the nine, `0x3180a8 = 0x1337a17a` alone takes it from -58 to -74,
and `0x320000 = 0x862` with `0x318094 = 0xc2` catch every packet, either alone every second or third,
but with those two `0x3180a8` no longer moves it, and the words that bring it to -69 or below are ten,
`0x4106e4`, `0x318b94`, `0x3180a8`, `0x320108`, `0x318090`, `0x318340`, `0x318ae0`, `0x318b90`, `0x32005c` and `0x31801c`,
each worth a few dB, and the 21 together read -67 and catch every second packet.
Most are the modem's times, which `0x64fe4` works out of `0x318090`, `0x318094` and `0x318394`'s second byte, **read**:
when in the packet the strength is taken, and so what gain it is taken at, a hypothesis.

With the vendor's setup and the advertising channels' access address, `0x8e89bed6`, **measured**:

- The nRF's whitened packets on 2402 MHz, channel 37, came with their CRC failing and their bytes whitened,
  which dewhitened on the host with the channel's seed gave the header and the PRBS9 sent;
  with `0x318b6c`'s bit 21 set and `0x318bf4 = 37` they came dewhitened, every CRC good.
- So set, with the nRF silent, the receiver heard the room's advertisers on 2402, 2426 and 2480 MHz,
  39 packets in 1.2 s on each, every CRC good: three advertisers, the same on each channel, one non-connectable and two connectable.
- The demodulator's words `0x41053c` to `0x41054c` read 0 and `0x410550` `0x2000` at every packet,
  and the eleven metrics of `0x41018c` and `0x410190` `0xfff` but the last, 0, whatever the packet's strength:
  BR/EDR's, idle in LE.

## What the code says of the receiver

The ROM's code, **read**, and not tried:

- The LE test's receiver, `0x71252` and `0x71344`, sets `0x318394` bit 31, `0x3180a0` bit 1, `0x31801c` bit 25 and `0x318014` bit 7,
  `0x318bc0`'s bits 15:8 to `0xa1`, and `0x318b10` to `0xffffffff`;
  its packet's end, `0x7143e`, counts a good one on `0x318bac`'s bit 2, and arms again.
- Two modes, BR and LE, and no other: `0x5b952` and `0x5b988` take LE for 1 and BR for anything else,
  `0x5b952` writing `0x318ab8[17:0]` and `0x318b14`, `0x5b988` `0x318b6c`'s bit 14 and `0x410430[14:8]`,
  out of a table at `0x2035d0` whose words at `+0x14` and `+0x18` a patch may fill to take each over;
  `0x1d30` plays a list of a register and a value each, LE's `0x4105f4 = 0x80`, `0x410734 = 0x5f21`, `0x410784 = 0x85`, `0x318094 = 0xb0`.
  The parked core's table, **measured**: `0x410430[14:8]` 4 for BR and 7 for LE, `0x318ab8` `0x2551b` and `0x2151b`, bit 14 apart,
  `0x318b14` `0x42` for both, and both words a patch would fill 0;
  with the receiver set up, `0x318ab8` reads LE's and `0x410430` `0x76d`.
- `0x410000`'s `0x4106e8` is a measurement of power: written `0x8000`, read for bits 13:0, written 0.
  `0x1fd92` averages them and turns them into dB through a table at `0x8a2a8` and the gain's corrections;
  the calibration's loop of 5400 takes it with the transmitter looped back.
- The channels' assessment for adaptive hopping, `0x20370`, sweeps the 79 channels with it,
  setting the synthesizer itself through `0x600104 = 0x80 | (f - 2400)`, with a sequencer at `0x31e000` that bt.md's pages lack,
  and keeps a dB a channel in RAM at `0x204de0`.
- `0x4103dc` to `0x4103fc` are an estimator of strength: `0x5cccc` loads 103 coefficients in dB, Q8, through an address and a data word,
  and `0x4103f0` to `0x4103fc` are its outputs, used when RAM's `0x20251c` has bit 3.
- `0x41018c` and `0x410190` are an index and its 12 bits, eleven metrics read per voice packet.
- `0x41053c` to `0x410550` are the demodulator's statistics, against which `0x1dbe0` rejects BR/EDR packets.
- `0x3186a0` reads `f - 2400`, the synthesizer's frequency.
- `0x370000` holds BR/EDR's received packets, `0x318e78` the voice links'.
- No code points a DMA at the radio or keeps samples: the receiver hands over bits, the meters power.

## The table at `0x327000`

An index and a data word, **traced**:
the function at `0x53cb0` writes `0x3270c4` with an index, 0 to 159, and `0x3270c8` with a value,
the same 160 values each time, five times in the trace;
then `0x3270d8 = 1`, `0x3270dc = 8` and `0xff`, `0x3270cc = 0x2007`,
and, a few calls later, `0x3270f4 = 0x100` and back to 0 after a read of `0x327040`.
The loader, `0x53c8e`, has four callers and copies the values from a table in the ROM at `0x87bc4`.
The table is four FIR filters one after another, **inferred** from its numbers,
each symmetric about its centre, in units of 2^21:

| Words | Taps | Centre | Gain at 0 | at 1/8 | at 1/4 | at 3/8 | at 1/2 of the sample rate |
|---|---|---|---|---|---|---|---|
| 0 to 120 | 121 | 0.976 | 2.00 | 2.00 | 0.41 | 0.000 | 0.000 |
| 121 to 139 | 19 | 0.909 | 2.00 | 1.99 | 0.61 | 0.000 | 0.000 |
| 140 to 152 | 13 | 0.890 | 2.00 | 1.92 | 0.64 | 0.004 | 0.000 |
| 153 to 159 | 7 | 1.000 | 2.00 | 1.89 | 1.00 | 0.113 | 0.000 |

Each passes up to about a quarter of its sample rate and stops above, with a gain of two,
and the last is a half-band filter, its even offsets but the centre zero:
four stages that each double or halve the rate, sixteen in all, from the sharpest to the shortest.

Not the transmitter's, as `bt-tx.rec` sets it up, **measured**:
the table loaded through the index and data words as the vendor loads it, with the writes that follow,
and 2000 packets sent after each load, `user/observer/le-observer.py` on 2440 MHz heard

| Table | Whole | Bad | Strength |
|---|---|---|---|
| the vendor's | 1984 | 0 | -68.9 dBm |
| every word halved | 1987 | 1 | -68.7 dBm |
| the vendor's | 1982 | 0 | -68.7 dBm |
| every word quartered | 1986 | 0 | -69.2 dBm |
| the vendor's | 2000 | 0 | -66.7 dBm |
| all zeros | 2000 | 0 | -66.1 dBm |
| the vendor's | 2000 | 0 | -66.3 dBm |
| the sharpest filter's 121 words zero | 2000 | 0 | -66.3 dBm |

in two runs, the first and the second four rows, each beginning and ending with the vendor's.
A filter of the transmitter's set to zeros would leave no signal, or no deviation of its frequency, and the packets unheard:
the table shapes something else.
Each load comes with a clearing of `0x326000`'s 258 words, which the firmware only ever writes with zeros,
four times as the core boots and once as it answers `HCI_Reset`: part of the controller's reset.

The page is the audio's sample-rate converter, **inferred** from the code that writes it, **read**:
`0x55b34` stores 12288000, 256 times 48 kHz, in RAM at `0x20257c` as it sets the page up,
and `0x14ebe`, with eleven callers among the voice paths' code, writes a ratio of two rates, `(a << 4) / b`, into `0x3270d0`,
its remainder into `0x3270d4` and `0x3270cc = 0x2007`, the write that follows the table's load;
`0x326000`'s words are two buffers of 512 bytes, and `0x351000` holds a bit for each voice link, `1 << (n + 28)`.
So the filters are the converter's half-band stages, and not the radio's.
