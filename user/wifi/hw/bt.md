# The Bluetooth core's registers

The core is a Cortex-M3; its ROM lies below `0xd0000`, the vendor patch's code from `0xd0000`,
its RAM from `0x200000` to `0x220000`, and its registers from `0x300000` up, see `user/wifi/BT.md`.
The host reaches all of it through the backplane's window, the core's address plus `0x19000000`;
the Cortex-M's own registers at `0xe000e000` it does not.

## The pages

Every 4 KiB page the vendor firmware touched, from the core's reset through the LE transmitter test,
with what the firmware did there, **traced**, and what the pages read, **read**.
The trace is one run of `user/wifi/tools/cyw43-rehost.py --board` with the departures BT.md names
and the commands `HCI_Reset`, `Read Local Version`, `LE Read Local Supported Features`, the transmitter test and its end;
`user/wifi/tools/cyw43-regmap.py trace` counts it.
The reads are `user/wifi/tools/cyw43-regmap.py read` and `vendor`, every word of the page twice in a row:
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
| `0x318014` | the trigger: `0x80000005` sends one packet, at most one each 1.25 ms | measured, BT.md |
| `0x318028` | one of the transmitter's eight words, `0x202`; its role is unknown | measured |
| `0x318038` | bits 31:24 written beside the carrier by the hopping function `0x6b4a8`; writing them changes nothing measured | traced, measured |
| `0x318088` | the Bluetooth clock, stepping every 312.1 µs; `0x318084`, read after it, counts up | measured |
| `0x3180a0` | written in the test's setup | traced |
| `0x318200` | the channel index, `0..39`: written by 19 places, read by none, and does not set the carrier | traced, measured |
| `0x318204` | the parameter word beside it, `0x84` for the test | traced, measured |
| `0x318220` | written beside the carrier by the hopping function; changes nothing measured | traced, measured |
| `0x318394` | 0 parked, `0x9a0100ff` the vendor's: its low byte from 4 or its bit 27 keeps the first packet after a change of the carrier; the low byte adds microseconds between `0x314004`'s bits 2 and 0 | measured |
| `0x3186a4` | the carrier, `0x80 \| (f - 2402 MHz)`, read live | measured |
| `0x318b68` | the packet's header, left at its reset value by the eight words, so the packets carry no payload | measured |
| `0x318b6c` | the path and mode, `0x1006340` sending; cleared while sending, the transmission stops | measured |
| `0x318b70` | the access address | measured |
| `0x318b90` | the test service's other write a packet; written alone it sends nothing | measured |
| `0x318ba4` | the CRC's start, `0x555555` | measured |
| `0x318bc0` | one of the eight words, `0x2b08289`; its role is unknown | measured |

## The link controller's status, `0x314004`

Cleared by writing ones.
After a trigger its bits 3 to 6 latch 290 to 590 µs on, bit 2 about 408 µs later, and bit 0 127 to 129 µs after that,
**measured**; what each stands for is unknown.

## The radio, `0x410000`

`0x410494` is one of the transmitter's eight words, `0x26c`, **measured**, its role unknown.
`0x4106e8` is the busiest register of the trace, 16262 reads and 10843 writes from 7 places,
the strobe of a measurement the calibration takes thousands of times, whose results the firmware writes elsewhere, **traced**.

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
the table shapes something else, the receiver's decimation from its converter's rate a hypothesis,
or the loading does not reach the hardware as the parked core stands.
Each load comes with a clearing of `0x326000`'s 258 words, which the firmware only ever writes with zeros,
four times as the core boots and once as it answers `HCI_Reset`: part of the controller's reset.
