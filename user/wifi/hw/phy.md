# The 802.11 PHY and its radio

The PHY and the radio sit behind D11's address and data words, `phyregaddr` and `radioregaddr`, see `d11.md`,
and are read with the WLAN's processor held in reset, so that no access of the firmware's comes between an address and its data.
The WLAN firmware's ROM names them an LCN20 PHY and a 20692 radio; what its code does to them is the last section's.

## The PHY's registers

`phyversion`, D11's halfword at `0x3e0`, reads `0xdc00`, **read**:
analog version 13, PHY type 12, revision 0, as brcmsmac's `PV_*` masks split it;
brcmsmac names the types 4, 6, 8 and 9, N, SSN, LCN and LCNXN, and not 12;
the WLAN's ROM names its PHY code `wlc_lcn20phy_*` and its radio 20692, `wlc_lcn20phy_radio20692_lpf_tx_set`, **known**: an LCN20 PHY.

`user/wifi/tools/cyw43-regmap.py phy` reads the PHY's registers through `phyregaddr` and `phyregdata`, each a halfword,
with the bridge's `h`, once the WLAN's processor is held in reset as the driver disables a core,
`0x18103000`'s IOCTRL cleared and RESETCTRL set, so that no access of the firmware's comes between an address and its data:
the PHY then stays as the firmware left it, up and idle, and answers, **read**.
Of the addresses 0 to `0xfff`, 630 read nonzero, by blocks of `0x100`:
`0x000` 46, `0x300` 5, `0x400` 128, `0x500` 75, `0x600` 82, `0x700` 5, `0x800` 37, `0x900` 91, `0xa00` 133, `0xb00` 27;
none from `0xc00` up, and no block repeats another, so the space is not a smaller one seen again.
Nine moved between two reads in a row, the first value and the second:
`0x305` (0x2c7b, 0x2cfc), `0x306` (0x2de3, 0x0011), `0x45c` (0x2af8, 0x4462), `0x944` (0x009c, 0x009b), `0xb1b` (0x0000, 0x0001), `0xb23` (0x0052, 0x004e), `0xb25` (0x1790, 0x2240), `0xb28` (0x001c, 0x001d), `0xb2a` (0x0184, 0x0186).

### As the WLAN firmware leaves them

The same reading, the bridge opened only once the configuration's mode was up, `--conf`, each a power-up of its own, **read**:
idle, the firmware up and nothing asked of it; once a scan was done; once joined to a network with WPA2;
and with an access point up on channel 6.
65 registers that did not move read differently between the four, among them:

| Register | Idle | Scan | Joined | Access point |
|---|---|---|---|---|
| `0x880` | `0000` | `802b` | `8040` | `8001` |
| `0x9f9` | `0000` | `803d` | `8052` | `8067` |
| `0x479` | `8040` | `8041` | `8000` | `8000` |
| `0x47a` | `81ab` | `81a2` | `8000` | `8000` |
| `0x485` | `0000` | `0000` | `ffff` | `ffff` |
| `0x553` | `0030` | `0c78` | `26d0` | `2688` |

With the access point asked for channels 1, 6, 11 and 13 in turn, 64 registers that did not move read differently;
28 of them read on channel 6 as they did on it in the run before, another power-up, so they follow the channel:

| Register | Channel 1 | 6 | 11 | 13 |
|---|---|---|---|---|
| `0x05f` | `3140` | `2140` | `3140` | `3140` |
| `0x404` | `0000` | `0001` | `0001` | `0000` |
| `0x405` | `0000` | `00a4` | `00a1` | `0000` |
| `0x47b` | `009e` | `009d` | `009d` | `009d` |
| `0x47c` | `00aa` | `00aa` | `00aa` | `00a9` |
| `0x485` | `0000` | `ffff` | `0000` | `ffff` |
| `0x4a6` | `809e` | `809d` | `809d` | `809d` |
| `0x4fb` | `ff22` | `ff14` | `ff14` | `ff14` |
| `0x552` | `0000` | `0000` | `0000` | `6021` |
| `0x554` | `0000` | `0000` | `0000` | `0060` |
| `0x5bf` | `007f` | `014f` | `007f` | `0083` |
| `0x657` | `03c5` | `03cf` | `03d9` | `03c5` |
| `0x658` | `043f` | `0434` | `0429` | `043f` |
| `0x666` | `1802` | `0001` | `0100` | `0103` |
| `0x980` | `0065` | `0064` | `0063` | `0065` |
| `0x981` | `e3fb` | `d866` | `d240` | `e3fb` |
| `0x984` | `0065` | `0064` | `0063` | `0065` |
| `0x985` | `e3fb` | `d866` | `d240` | `e3fb` |
| `0x986` | `00c9` | `0985` | `04cf` | `00c9` |
| `0x991` | `03c3` | `0043` | `0043` | `0043` |
| `0x992` | `0004` | `0008` | `0008` | `000a` |
| `0x9ae` | `19e2` | `19e7` | `19ec` | `19e2` |
| `0x9b1` | `0140` | `0500` | `0280` | `0140` |
| `0xa9a` | `016c` | `00b0` | `00cf` | `015e` |
| `0xadc` | `0000` | `0000` | `0002` | `0000` |
| `0xade` | `0000` | `0000` | `0013` | `0000` |
| `0xae0` | `0000` | `0000` | `0002` | `0000` |
| `0xb1f` | `0000` | `0002` | `0001` | `0000` |

Asked for channel 13, the radio stayed on 2412 MHz, channel 1's, as its frequency register says, see below:
the country the driver sets, `XX`, lets no access point use channel 13, a hypothesis for why.
On channels 1, 6 and 11, five words are the carrier's frequency, f in MHz, to the last bit, **read**:

| Register | Is |
|---|---|
| `0x657` | 2f/5, rounded |
| `0x658` | 10·2^18/f, rounded |
| `0x980` and `0x981`, the high and the low half | 15·2^30/f, rounded |
| `0x984` and `0x985` | the same again |
| `0x9ae` | `0x19e2` on channel 1, one more each channel up |

Three of the five go as one over the frequency:
since the carrier and the sampling clock come from one crystal, an offset measured of one is an offset of the other in that ratio,
which a receiver that corrects both needs, a hypothesis.

## The radio's registers

`user/wifi/tools/cyw43-regmap.py radio` reads the radio's registers through `radioregaddr` and `radioregdata`, each a halfword,
the WLAN's processor held in reset as for the PHY's, **read**.
brcmsmac or's a flag into the address for a read, which depends on the radio, 2055, 2057 or 2064, and this one is none of them;
read with no flag, `0x100`, `0x200` and `0x4000`, the radio needs none:
the flags act as the address's bits, `0x100` the upper half of the space, `0x200` a half that reads all zeros,
and `0x400` and `0x4000` none, the same words again.
The radio's space is 512 registers, `0x000` to `0x1ff`, of which 202 read nonzero, 176 below `0x100`;
none moved between two reads in a row.

brcmsmac makes a radio's id of its registers 0, 1 and 2 on a core of revision 24 or more;
they read `0x0003`, `0x03da` and `0x0004`, which it would make `0x307da000`, a radio it does not know;
the ROM's names make it the 20692.

With the same states and channels as the PHY's, 25 registers read differently, **read**.
Of them, three are the carrier's frequency, to the last bit, on channels 1, 6 and 11, and read 2412 MHz's in every run not on 6 or 11:

| Register | Is |
|---|---|
| `0x055` | f in MHz: `0x096c` is 2412 |
| `0x04e` and `0x04f`, the high and the low half | f·3·2^18/37.4, rounded down: the synthesizer's word from a reference of 37.4 MHz, as the arithmetic fits |
| `0x066` | 22360 - 8f |

The carrier's word is therefore the radio's, and the PHY's five follow it.
Of the rest, `0x032`, `0x034`, `0x072` to `0x076` and `0x092` to `0x094` differ from run to run without the channel's order:
results of calibrations, a hypothesis.
`0x075` reads `0x66` idle and once a scan is done, and `0x08` once joined or with the access point up.

## The PHY's tables

The PHY keeps tables behind an id and an offset, **known** from the WLAN's ROM, whose LCN20 code reaches them at `0x874960`, to write, and `0x8721e0`, to read:
`0x9a5`'s bit 1 set for the access and put back after, the id written to `0x0d`, the offset to `0x0e`,
and the data through `0x456`, the low half, and `0x457`, the high;
a write gives the high half first, a read takes the low half first.
A PHY register is written as one word at `phyregaddr`, the data in its upper half, the address in its lower.
brcmsmac's LCN PHY puts its address at `0x455` instead, which this PHY does not take.

`user/wifi/tools/cyw43-regmap.py phytbl` reads them so, the WLAN's processor held in reset, each entry at its own offset, **read**.
Ids `0x1f`, `0x20` and `0x21` hang the PHY's access, each over a power-up of its own:
every read of D11 gives the halfword read last from then on, until the chip's next power-up.
Of the others, 18 read anything but zeros; a table's size is where its offset wraps, where it does within 2048 entries:

| Id | Entries | Bits | What it holds | brcmsmac's LCN PHY's table of that id |
|---|---|---|---|---|
| `0x00` | 256 | 16 | a ramp in steps of `0x100` and 1 | IQ and LO calibration |
| `0x02` | 8 | 9 | `0x1b7` throughout | |
| `0x07` | 1024 | 29 | a ramp falling from `0xd0` | transmit power control |
| `0x08` | | 13 | | RF sequence |
| `0x0d` | | 22 | `0x308000`, then rising by `0x80` | gain index |
| `0x11` | | 7 | | |
| `0x12` | 62 | 22 | falling words | gain |
| `0x14` | 1024 | 2 | ones | spur |
| `0x15` | 256 | 20 | a tone's turn, see below | sample play |
| `0x17` | | 12 | multiples of 26 | |
| `0x1a` | 64 | 9 | | |
| `0x1b` | 256 | 20 | what the memory powered up with, a hypothesis | |
| `0x1c` | | 28 | two falling and rising halves | |
| `0x1d` | | 26 | | |
| `0x22` | 16 | 17 | | |
| `0x23` | 8 | 18 | | |
| `0x24` | 8 | 10 | | |
| `0x25` | 36 | 32 | what the memory powered up with, a hypothesis | |

`0x1b` and `0x25` read alike twice in one power-up and differ in a few bits from one power-up to the next, as memory no one has written does.

### The sample play buffer, `0x15`

The ROM's tone generator, `wlc_lcn20phy_start_tx_tone` at `0x86ea6c`, **known**, makes the samples of a tone,
refuses more than 256, "Too many samples to fit in SPB", writes them to table `0x15` 32 bits wide, and plays them in a loop through `0x872334`.
Table `0x15` has 256 entries, **read**, each a sample of 10-bit I and Q, Q in bits 0 to 9 and I in bits 10 to 19, two's complement:
as the firmware leaves it idle, its first 16 a turn of a tone of amplitude 100, a sixteenth of a turn each sample,
`0x19000`, I 100 and Q 0, then I 92 and Q 38, from a calibration of the firmware's, a hypothesis,
and the other 240 no tone, of amplitudes from 36 to 693.
The PHY can therefore send any 256 samples in a loop; nothing here has played them.

How the ROM plays and stops them, **known**, the registers brcmsmac's LCN PHY uses for the same where it has them:

| Step | Where | What |
|---|---|---|
| set up | `0x86ea6c` | `0x49c` bit 0 set, `0x6d6` and `0x6da` changed, the samples into table `0x15` |
| play | `0x872334` | `0x6da` or'ed with `0x8080`; `0x642` bits 0 to 7 the samples less one; `0x640` the loops less one, `0xffff` for ever; `0x641` the wait between loops |
| start | `0x872334` | `0x63f` bit 0, or, for an IQ calibration, `0x453` bit 15; then up to 10 µs for `0x644` bit 0 or bit 1, playing |
| transmit | `0x874478` | the radio forced to send: bits 13 and 14 of `0x44c` and `0x44d`, bit 11 of `0x4b0`, bit 14 of `0x4b1` cleared, bits of `0x938`, and the call `0x873424` |
| stop | `0x86eb6c` | while `0x644` bit 0 is set, the transmitter's forcing undone through `0x874478`, then `0x63f` bit 1; or `0x453` bit 15 cleared; then 11 registers put back and `0x49c` bit 0 cleared |

Its rate is 40 million samples a second as the ROM reckons it, **known**:
`0x87121c` counts a tone's samples as 40·10^6·k over the tone's frequency in Hz, k the least that makes it whole,
as brcmsmac's LCN PHY does with its `phy_bw` of 40.
At that rate 256 samples last 6.4 µs, and the turn in the buffer is a tone of 2.5 MHz; measured on the air, neither is yet.

## What the firmware's code does to the PHY and the radio

`user/wifi/tools/cyw43-code.py` puts the WLAN's ROM, at `0x800000`, and its RAM firmware, at 0, in Ghidra as one program, **known**:
415 of their functions are named after the one identifier string only each refers to, its `__func__`,
such as `wlc_lcn20phy_start_tx_tone` at `0x86ea6c`,
and every call to the PHY's and the radio's access functions is listed with its arguments as the decompiler gives them.
`cyw43-code.py regs` writes them, and the register lists decoded, to a file, `build/cyw43/regmap/code-regs.json` here.

| Function | PHY | Radio |
|---|---|---|
| read | `0x86bf78` | `0x86c064` |
| write | `0x86bf9c`, one word at `phyregaddr` | `0x86e2f8` |
| modify, mask and value | `0x86bf38` | `0x86bedc` |
| and | `0x86bf20` | `0x86bec0` |
| or | `0x86bf5c` | `0x86bf00` |

`0x86bfa8` plays a list of halfwords, each a register in bits 0 to 12, the radio's if bit 15 is set, and an operation in bits 13 and 14:
0 modify, a mask and a value after it; 1 and, 2 write and 3 or, a value after it.
The code names 241 PHY registers and 124 radio registers by a constant, and writes 220 and 118 of them; 12 lists write more.

### A channel

The LCN20's setter of a channel, `0x874a14`, reached through a pointer, tunes the radio, `0x8714e4`, then sets the PHY's frequency words, `0x873d6c` and `0x8725a8`, and the transmitter's low-pass filter.
The radio's part copies an entry of a table in the RAM firmware, at `0x2bebe`, 14 entries of 66 bytes, a channel's number, its frequency in MHz,
and the values of 31 radio registers; then it runs the synthesizer's calibration, `0x8720a4`, which pulses `0x51` bit 0 and strobes `0xa0` and `0x42`,
and waits up to a millisecond for `0x65` bit 0.
Of the 31, six differ between channels:

| Radio register | Channel 1 | 14 | |
|---|---|---|---|
| `0x055` | `096c` | `09b4` | the frequency in MHz |
| `0x04e`, `0x04f` | `0305 e75b` | `031d 015e` | the synthesizer's word, f·3·2^18/37.4 |
| `0x045` | `5120` | `5121` | bit 0 from channel 5 |
| `0x046` | `6127` | `6128` | one more from channel 13 |
| `0x03c` | `6009` | `6007` | 9 to channel 6, 8 to 10, 7 from 11 |
| `0x086` | `703f` | `603f` | bit 12 cleared from channel 9 |

`0x066`, which reads 22360 - 8f, no code writes: the synthesizer's calibration leaves it, a hypothesis.

The PHY's words of the frequency, which the readouts fitted, are the code's:
`0x657` is `((f << 2) / 5 + 1) >> 1`, `0x658` is `(0x500000 / f + 1) >> 1`,
`0x980` and `0x981`, and `0x984` and `0x985` again, come from `0x86ee3c` of the channel,
and `0x9ad` with `0x9ae`'s low 10 bits are f/5 with 16 bits of fraction, from a table of 14 words at `0x892cc4`, `0x1e26666` for channel 1;
`0x9b1` is `0x500` over the greatest common divisor of `0x500` and f, and `0x986` f over that of f and `0x780`,
as the readouts' `0x140`, `0x500` and `0x280`, and `0xc9`, `0x985` and `0x4cf`, on channels 1, 6 and 11 bear out.

### Tables of values

- Radio registers `0x0a9` to `0x0dc` take 52 bytes from the ROM at `0x89027c`, and `0x0dd` to `0x114` 56 halfwords from `0x89020c`, in `0x871fe0`.
- Radio registers `0x072` to `0x077` take values the RAM firmware computes, at `0x10480`: the readouts' registers that differ from run to run without the channel.
- The transmitter's digital filters: `0x870a28` writes 17 registers from a row of a table chosen by a type,
  for CCK `0x910`, `0x91e` to `0x929`, `0x930` to `0x932` and `0x912`, 17 types at `0x88f68c`,
  and for OFDM `0x90f`, `0x900` to `0x90e` and `0x911`, 18 types at `0x88f8f0`:
  three sections of two feedback and three feedforward coefficients each, as the registers group, a hypothesis, and a word more,
  the registers brcmsmac's LCN PHY loads in `wlc_lcnphy_load_tx_iir_filter`, which has one fewer.
  The pulse the transmitter shapes is therefore a table's, and a type the firmware chooses for a channel.
