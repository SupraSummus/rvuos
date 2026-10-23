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

## The receiver's samples

D11's sample collection takes nothing from this PHY, see `d11.md`; the PHY's I/Q estimator does, one sample at a time.

### The I/Q estimator

The ROM's `wlc_lcn20phy_rx_iq_est`, at `0x872e5c`, **known**, runs it as brcmsmac's LCN PHY runs its own, one register higher:

| Register | What |
|---|---|
| `0x481` | bit 0 starts an estimate and reads set until it ends; bit 1 a mode the caller passes |
| `0x482` | bits 0 to 7, the samples it waits before it takes its own |
| `0x483` | the samples it takes |
| `0x484`, `0x485` | the sum of I times Q, the low halfword first, two's complement |
| `0x486`, `0x487` | the sum of I squared, I's as brcmsmac's order has it, a hypothesis |
| `0x488`, `0xa9a` | the sum of Q squared |

It ends only with `0x6da`'s bits 5 and 8 set, **measured**: without them bit 0 of `0x481` stayed set, and with them it ended within the bridge's next read;
the firmware sets them around its estimates, in `0x871060`, and brcmsmac's LCN PHY sets bit 5 of the same register for its own.
`0x6da` reads 0 as the firmware leaves it, and its bits are the clocks of the PHY's parts, a hypothesis:
the play sets bits 7 and 15, `0x8080`, the estimate bits 5 and 8, and brcmsmac's sample collection bits 3 and 13.

An estimate of one sample, `0x483` at 1, gives that sample, **measured**:
in more than 400 estimates over the bridge and more than 30000 by our code on the WLAN's processor, with an access point up on channel 6,
the product squared equalled the two squares' product every time, as for one sample alone, and with either mode.
So it gives I and Q but for one sign common to both, I's taken as positive,
and the sample's square, (I + jQ)^2, I^2 - Q^2 and 2IQ, whole: the sample at twice its phase.
No register beside it gives the sign: of `0x470` to `0x49f` and `0xa90` to `0xaa7`, read after each of 40 estimates,
none held I or Q, **measured**.
Nor does any register show the sample as it comes:
53 moved over three readings of all 4096, a third of a second apart, with the access point up, and of 23 of them, read 400 times in a row each,
`0x305`, `0x306`, `0x45c` and `0xb29` count up and the rest keep a value over many reads, **measured**.

### Our code on the WLAN's processor

`user/wifi/tools/cyw43-iq.py` has the vendor's firmware bring a mode up, an access point for one,
then holds the WLAN's processor in reset, stops D11's receive DMA, so that no frame lands in the RAM,
and starts the processor again in `user/wifi/wlancore/iq-cap.c`, at vectors written over the firmware's;
the PHY and the radio stay as the firmware left them, receiving on the mode's channel.
The code takes one estimate of one sample after another, polls `0x481`, and keeps each with its cycle count.
The processor runs at 81.600 MHz, its cycle counter against D11's TSF over 100 ms, **measured**.

The code lies in RAM the firmware had cleared, `0x1e000` to `0x2d000`, **read**:
with an access point up, the RAM reads 0 from `0x180b0` to `0x2d11c` and from `0x2d124` to `0x38568`,
inside what the image loaded, the microcode's copy among it, and nowhere else for more than 13 KiB;
the PHY's state, `pi`, lies at `0x63788`, its type `0xc` first and D11's address at `0x44`,
and the LCN20's own state, its word at `0x40`, at `0x62a3c`.

How long a sample takes depends on the wait, **measured**, in the processor's cycles, the code then at `0x70000`:

| Wait | 2 | 4 | 8 | `0x10` | `0x20` | `0xff`, 0 or 1 |
|---|---|---|---|---|---|---|
| Cycles | 258 | 264 | 295 | 325 | 394 | 1310 |

A unit of the wait is about four cycles, a sample at 20 MHz, and 0 and 1 wait as long as 256 would.
At a wait of 2 a sample came every 258 cycles there, 316 kS/s;
at `0x20000`, where the code now lies, every 262, 311.4 kS/s, all of 430 steps alike,
and with the tool's check of each estimate's end 260 to 264 cycles apart, 20000 samples at 312.2 kS/s.
The rest of a sample's time is the code's accesses, each of the PHY's about 16 cycles,
two for a read, the address and then the data, and one for a write, address and data in one word.
The estimate ends before the first read after its start comes back, **measured**:
with `--mode fast,check`, `0x481` read once between the start and the results, none of 3000 samples found it set.
So the code need not poll, and of an 8-bit sample the low halfwords hold all, the squares to 16384 and the product signed:

| `--mode` | What a sample reads | Cycles | Rate |
|---|---|---|---|
| none | polls `0x481`, then six halfwords | 262 | 311.4 kS/s |
| `fast,check` | `0x481` once, then three low halfwords | 154 to 158 | 526.5 kS/s |
| `fast` | three low halfwords | 118 to 122 | 682.9 kS/s |
| `fast,no-q` | the product and I squared, Q taken as the product over I, and as 0 where I is | 90 to 92 | 901.7 kS/s |

Each with the calibration's tones, **measured**, 600 samples a run:
the square's strongest within the transform's step of where the 40 Msps play puts it,
-211.8 kHz for +2 MHz at 526.5 kS/s, -97.4 at 682.9, +393.6 at 901.7, against -211.73, -97.10 and +393.30,
and so for -2, +1 and -0.625 MHz; a stale estimate read for a new one would have spread them.
`fast,no-q` loses Q wherever I is 0, a few in a hundred at a noise of a few steps.
On this processor five accesses are the least a sample can take, a start and two reads, about 80 cycles and 1 MS/s;
faster needs the PHY reached otherwise, by D11's own processor through its own registers, see "On D11's processor".

What the samples show, **measured**, the access point up on channel 6:
I^2 + Q^2 has a median of 2 to 5, a few of the converter's steps;
one run of 2000 held a burst of 77 samples, 1.2 ms, up to 19865, a magnitude of about 140, the length of a frame, a hypothesis.
At 311 kS/s the channel's 20 MHz folds onto itself, so only a signal narrower than the rate, tuned near the carrier, is seen whole.

### On D11's processor

With `--mode psm` D11's processor, the PSM, takes the samples, by microcode the tool writes, `psm_loop`, see `d11.md`,
once the vendor's microcode has suspended the MAC and the front end has been taken back from Bluetooth, see "Its receive engine" there;
the code on the WLAN's processor starts it for each run, times it, and copies its samples out of D11's shared memory,
which holds 1280 of three halfwords, 1920 of two.
Each sample starts an estimate, waits 14 instructions, and reads the results through the external IHR, `0x18` and `0x19`,
each access waiting on its busy bit; a read of the PHY takes about 14 cycles of the processor's, a hypothesis from the difference below.

The estimate has ended by a read 10 instructions after the start's access, **measured**, 896 samples each,
by `0x481` read once at once after a delay of instructions, with the WLAN's processor reading D11 at the same time:

| Delay | 1 | 2 | 3 | 4 | 6 | 8 | 10 | 12 | 14 | 16 |
|---|---|---|---|---|---|---|---|---|---|---|
| Found set | 614 | 416 | 225 | 53 | 37 | 21 | 0 | 0 | 0 | 0 |

Without that read, the first result read where it would have been,
an estimate read before it ends shows as a product that does not fit the squares, **measured**, 2000 samples of two tones each:
111 at a delay of 4, 34 at 8, none at 11 and none at 14.
None of the runs of 14 below held a sample that does not fit.

| `--mode` | What a sample reads | Cycles | Rate |
|---|---|---|---|
| `psm` | three low halfwords | 76.0 | 1073.6 kS/s |
| `psm,no-q` | the product and I squared | 62.0 | 1316.4 kS/s |

Each with the calibration's tones, **measured**, 1000 samples a run:
the square's strongest within the transform's step of where the play puts it,
-294.6 kHz for +2 MHz and +221.2 for -1.5 MHz at 1073.6 kS/s, against -294.25 and +220.69,
+51.4 and -367.7 at 1316.4 kS/s, against +50.88 and -367.26;
and so for -2, +1 and -0.625 MHz in earlier runs.
A sample's cycle count is a line through two moments the WLAN's processor sees `psm_base_0` pass, an eighth of the way and the end,
since its reads of D11 slow the PSM, see `d11.md`; the rate so measured varies by up to 1% from run to run, with how the processor reads.
So D11's processor takes a sample in about two thirds of the time the WLAN's does, and is held by the PHY's accesses and the estimate's own time;
no register gives a sample in one read, see "The I/Q estimator", so two reads and a start are the least a sample takes:
62 cycles with the loop's own instructions, 1.3 MS/s, and 56 without them, 1.46 MS/s, see below.

### Streamed through D11's receive DMA

With `--mode stream` the microcode, `stream_loop`, pushes its samples into D11's receive FIFO, whose DMA lays them in the WLAN's RAM,
a frame of 62 halfwords to a descriptor, see `d11.md`, so that a run is held by the ring, 510 frames, not by the shared memory.
The loop is unrolled over two chunks of the shared memory, a frame each:
a chunk's first sample, in its wait for the estimate, points `psm_base_0` at its chunk, counts it and pushes the other,
and the loop's jump and its end lie in waits too, so that every sample takes the same instructions, and none is spent on the loop.
A frame holds its count in halfword 0, nothing in halfword 1, whose bits 8 to 11 the receive engine clears, and the samples after.
The code on the WLAN's processor sets each run's ring up and times the frames as they land, by the DMA's status, read sparsely.

| `--mode` | Samples a frame | Cycles | Rate | Most a run |
|---|---|---|---|---|
| `stream` | 20 | 70 | 1164.0 kS/s | 10180 |
| `stream,no-q` | 30 | 56 | 1455.4 kS/s | 15270 |

**Measured**: runs of 10180 and 15270 samples without tones, every frame there by its count and no sample that does not fit;
and with the calibration's tones, 1860 and 2790 samples a run, the square's strongest within the transform's step of the play,
+507.8 kHz for +2 MHz and +492.2 for -1.5 at 1164.0 kS/s, against +508.03 and +491.97,
-366.2 and -89.2 at 1455.4 kS/s, against -366.15 and -89.23, with 60 to 75% of its power;
and so for +1 MHz in earlier runs.
Before halfword 1 was left empty, the first sample of each frame had its I squared there, and about 70 a run did not fit.
The waits are counted in the tool's model, an instruction a cycle and a taken jump two; by it every sample takes the same,
and a slot the loop's end jumps over, once counted as a cycle, left the first sample of every other frame one short:
the runs then took 1164.5 and 1455.8 kS/s, and with it counted as none 1164.0 and 1455.4, as the model has it, **measured**.
The tool's transform takes twice as many frequencies as samples, as a tone's peak is a rate over the samples wide;
at 1024 for 2070 samples it fell between two, and the +2 MHz tone seemed gone.

### The receiver's gain

The AGC sets the receiver's gain unless the overrides hold it, as the firmware's calibration holds it, **known**:
`0x872534` sets their enables, bits 10 and 12 of `0x4b0`, 7 of `0x4f9`, 0 and 3 of `0x53d`, 0 of `0x9ac` and of `0x979`,
and `0x872484` their values, bit 15 of `0x4b1`, bits 8 to 11 of `0x4fa`, 11 to 13 of `0x4b1`, 0 to 3 and 9 to 12 of `0x53c`,
6 to 9 of `0x9ac` and 1 to 8 of `0x979`; the calibration's gains are rows of tables at `0x88fdce` and `0x88fe0e`.
The firmware's measurement of noise, `0x871060`, holds only `0x979` at 0 and `0x9ac`'s four bits at an index p,
and reckons the gain 3 dB a step of p.
With an access point up, none is enabled, **read**.

What each does, **measured**, as the mean of I^2 + Q^2 over estimates of 1024 samples, the median of five, all enabled:
all at 0 leaves no noise at all; with `0x9ac`'s bits at 11, each field alone from 0:

| Field | Codes | What the noise does |
|---|---|---|
| `0x53c` bits 9 to 12 | 0 to 10 | from 2.5 to 26, the noise above the floor doubling a step, 3 dB, over the whole range: the first stage, a hypothesis |
| | 11 to 15 | 400, or with `0x4b1` bit 15 set from 19 to 175: no gain to use |
| `0x9ac` bits 6 to 9 | 0 to 12 | about 3 dB a step, as the firmware reckons; 13 to 15 leave no noise |
| `0x979` bits 1 to 8 | 0 to 15 | 2.5, then doubling at 4, 7, 10 and 13: 3 dB steps of a gain counted in dB; it doubles the floor too, so it lies late |
| `0x4fa` bits 8 to 11, `0x4b1` bits 11 to 13 and 15, `0x53c` bits 0 to 3 | all | nothing seen: stages this board does not use, such as an external LNA, a hypothesis |

Together not every pair adds: with `0x979`'s at 0, `0x53c`'s at 10 and `0x9ac`'s from 5 to 12 give
0.55, 0.99, 1.8, 3.63, 6.76, 6.46, 12.77 and 0, and at 9, 0.27 to 7.64, then 7.08 and 14.9.
The AGC idles at a noise of 6.8 to 7.4, about where 10, 9 and 0 hold it, 6.76: below its most, so that it can raise the gain.

It does raise it for a weak frame, **measured**, 20000 samples each, one after another in one power-up:
with the AGC free, 1830 samples of frames, louder than 20 times the median, had a power of 101 to 1469 between their quartiles,
and 56 clipped; held at 10, 8 and 0, 3 dB below the idle gain, 345 such samples had 41 to 85, and one clipped;
free again, 1338 had 101 to 541, and 4 clipped.
Held, a frame comes out as strong as it is; free, the AGC lifts a weak one toward its own level, past it at times.

The samples are 8 bits, **measured**: I and Q of a clipped sample read 128, the most and more often than 127,
and of all samples none is larger; at the idle gain the noise is about one step of them,
so that a signal has about 40 dB above it before it clips.
`cyw43-iq.py --gain P6,P7,P8` holds the three fields that act at the codes given, with every enable set.

### The control: the calibration's tones

The firmware's receive IQ calibration, `0x872770`, **known**,
sets up a loopback and its gains, `0x872848`, clears `0x9de` and `0x9df`, `0x872e00`,
and plays tones of 2, -2, 6 and -6 MHz at amplitude `0xb5`, `wlc_lcn20phy_start_tx_tone` with the transmitter forced, `0x874478`,
estimating each with `0x400` samples and stopping it, `0x86eb6c`, before it puts all back, `0x872818`;
the firmware runs it as part of its calibration of a channel, `0x111b4` and `0x86fa64`.
`cyw43-iq.py --tones` has our code call those same functions on the firmware's `pi`,
and take a run of samples before any tone and one during each tone instead of the estimate.
The ROM's buffer for a tone's samples is a fixed one, `0x86f1f4`, and its delay counts a timer through RAM code at `0x1420`,
so the functions need nothing of the firmware's that no longer runs, **known**.

With 430 samples a run, at 311.4 kS/s, **measured**, twice alike:
before any tone, I^2 + Q^2 is 0 throughout, the loopback's gains leaving no noise;
during a tone it is steady, 3530 and 3370 for +2 and -2 MHz, 3088 and 2960 for ±1 MHz, 2157 for -0.625 MHz, 1025 for 6 MHz.
The samples' square is strongest where a tone of the ROM's 40 Msps play, doubled and folded by the rate, falls:

| Tone | Where 40 Msps puts it | Strongest | Its share of the power |
|---|---|---|---|
| +2 MHz | -48.85 kHz | -49.0 kHz | 61 % |
| -2 MHz | +48.85 kHz | +49.0 kHz | 60 % |
| +1 MHz | +131.30 kHz | +131.4 kHz | 85 % |
| -1 MHz | -131.30 kHz | -131.4 kHz | 86 % |
| -0.625 MHz | -4.20 kHz | -4.3 kHz | 94 % |
| +6 MHz | -146.54 kHz | +97.6 kHz | 13 % |

So the samples are the receiver's at the times the cycle counter gives, with I and Q in the order and sign the PHY's own tones have,
and the play's rate is 40 Msps, **measured**: the five tones to 2 MHz fall within the transform's step, 0.3 kHz,
and +2 and -2 MHz, and +1 and -1 MHz, mirror each other.
The rest of the power lies 12 to 27 dB below, and grows with the tone's frequency;
that the estimator takes its sample on the PHY's own grid of 20 MHz, so that each sample's instant is late by up to 50 ns, is a hypothesis,
which a fit of the phases favours over 40 and 80 MHz and none, but by little.
The 6 MHz tone, the weakest in the loopback, is smeared over several peaks: doubled, 12 MHz turns 0.6 of a turn in 50 ns, where 4 MHz turns a fifth.

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
