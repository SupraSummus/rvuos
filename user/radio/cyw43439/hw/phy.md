# The 802.11 PHY and its radio

The PHY and the radio sit behind D11's address and data words, `phyregaddr` and `radioregaddr`, see `d11.md`,
and are read with the WLAN's processor held in reset, so that no access of the firmware's comes between an address and its data.
The WLAN firmware's ROM names them an LCN20 PHY and a 20692 radio; what its code does to them is the last section's.

## The PHY's registers

`phyversion`, D11's halfword at `0x3e0`, reads `0xdc00`, **read**:
analog version 13, PHY type 12, revision 0, as brcmsmac's `PV_*` masks split it;
brcmsmac names the types 4, 6, 8 and 9, N, SSN, LCN and LCNXN, and not 12;
the WLAN's ROM names its PHY code `wlc_lcn20phy_*` and its radio 20692, `wlc_lcn20phy_radio20692_lpf_tx_set`, **known**: an LCN20 PHY.

`user/radio/cyw43439/tools/cyw43-regmap.py phy` reads the PHY's registers through `phyregaddr` and `phyregdata`, each a halfword,
with the bridge's `h`, once the WLAN's processor is held in reset as the driver disables a core,
`0x18103000`'s IOCTRL cleared and RESETCTRL set, so that no access of the firmware's comes between an address and its data:
the PHY then stays as the firmware left it, up and idle, and answers, **read**.
Of the addresses 0 to `0xfff`, 630 read nonzero, by blocks of `0x100`:
`0x000` 46, `0x300` 5, `0x400` 128, `0x500` 75, `0x600` 82, `0x700` 5, `0x800` 37, `0x900` 91, `0xa00` 133, `0xb00` 27;
none from `0xc00` up, and no block repeats another, so the space is not a smaller one seen again.
Nine moved between two reads in a row, the first value and the second:
`0x305` (0x2c7b, 0x2cfc), `0x306` (0x2de3, 0x0011), `0x45c` (0x2af8, 0x4462), `0x944` (0x009c, 0x009b), `0xb1b` (0x0000, 0x0001), `0xb23` (0x0052, 0x004e), `0xb25` (0x1790, 0x2240), `0xb28` (0x001c, 0x001d), `0xb2a` (0x0184, 0x0186).

### From the WLAN's processor

Read by our code on the WLAN's processor, an access point up, the PHY answers at 1044 of the addresses 1 to `0xbff`, **measured**,
and at the other 2025 the read ends in a precise bus fault, where the bridge's read through the backplane gives a value:

    001 003-005 007 009-00a 00d-00e 010-013 018-019 027 030-031 033-035 038-039 03f-04d 04f 052-053 05d-05f 068-06d 070-072
    304-308 310-31a 321-327 401 404-405 407-40e 410-411 41a-41b 41e-427 429 42c 42e-430 432 436-43e 440-445 447-44f 451-453
    458-467 469-46a 46c-48a 490-496 498-4ac 4ae 4b0-4b2 4b5-4bc 4d0-4d3 4d7-4e9 4eb-4ed 4ef-4f0 4f2 4f9-4fe 501-50a 50c-50e
    512-513 524-527 531 534-53d 542-543 547 54b 54d-554 564-565 570-575 578-579 594-59e 5b0-5b9 5bb-5c9 5ce 5d1-5d7 5e0-5ed
    5f0-60c 62a-62f 631 634-638 63a-681 687-689 68b-68c 695-69a 69d 6a1-6ad 6b8 6c2-6c5 6c8-6ce 6d0-6de 6e0 775 785 787-788
    7c3 7d0-7de 7e0-7ee 7f6 7f9-7fa 800-801 803-809 810-817 821-829 830-831 833-839 840-849 850-859 860-86d 880-899 900-912
    91e-929 930-932 938-94b 950-959 960-961 965-97a 97f-989 990-999 9a0-9aa 9ac-9b4 9cd-9e0 9f4 9f7-9fa a00 a04-a10 a13-a26
    a28-a2a a31-a34 a37 a39-a47 a49-a70 a7f-a81 a85-a9a aa2 aaf-ab0 ab7 aba-ae3 aed-af5 b00-b2d

A read by the scan's function, its address written and its data read, takes 40 to 55 of the processor's cycles, one that faults about 104, **measured**.
A write to some of them restarts the processor, at its reset vector, the PHY left as it was, **measured**:
to `0x404` and `0x405`, and to the registers the PHY itself keeps moving, of its gain and its counters,
`0x45a`, `0x45b`, `0x45d`, `0x47b` to `0x47d`, `0x48a`, `0x50d`, `0x553`, `0x5b0` to `0x5c2`, `0x665` to `0x66f`, `0x991` to `0x993` and `0xb1b` to `0xb2d`.

Every bit of the 636 registers from `0x100` up that no code names and that read nonzero or hold what is written,
flipped alone with the MAC suspended, while all 1044 were read twice, showed, **measured**:
`0x7e0` choosing what `0x7e7` shows, see "A converter's bits";
`0x321` bit 3 and `0x324` bit 2 setting `0x307` and `0x308` counting, as `0x305` and `0x306` count;
`0x896` bits 0 to 6 shown in `0x88f`;
bits of `0x459`, `0x4e3`, `0x4e6`, `0x62e`, `0x6a4`, `0x7e2` and `0x854` raising D11's busy medium, `ifsmedbusyctl`, and its receive status: thresholds of the PHY's detection, a hypothesis;
and a bit of `0x539` or `0x53a`, held about 40 ms, leaving the estimator without an end until the next power-up.

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

What the code does to each of them, block by block, is in `radio.md`.

`user/radio/cyw43439/tools/cyw43-regmap.py radio` reads the radio's registers through `radioregaddr` and `radioregdata`, each a halfword,
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

`user/radio/cyw43439/tools/cyw43-regmap.py phytbl` reads them so, the WLAN's processor held in reset, each entry at its own offset, **read**.
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
The PHY can therefore send any 256 samples in a loop; on the air, see below.

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
At that rate 256 samples last 6.4 µs, and the turn in the buffer is a tone of 2.5 MHz.

On the air, **measured**: our code on the WLAN's processor called `0x86ea6c` as the calibration does, `pi`, the tone in Hz, amplitude 64, 0, 1, 1,
and `0x86eb6c` three seconds later, without the calibration's loopback, on channel 6, the access point brought up first;
an ESP32-C6's scan held at 2437 MHz, its gain fixed at 24, 4096 samples at 16 MS/s about twice a second, took the air,
and in a first run of its own an nRF52840's carrier on 2441 MHz lay in the same captures.
With D11's microcode running, a tone lasted under half a second, and one asked for after it did not show:
the microcode undoes the forcing, as it takes the front end back for its frames or gives it to Bluetooth, a hypothesis.
With the MAC suspended and D11's processor halted first, as `cyw43psm.suspend` and `HALTED` leave them, each tone held its three seconds,
in seven captures each:

| Asked | The tone less the carrier's leak, MHz | The leak, below the tone | The mirror, below the tone |
|---|---|---|---|
| +2 MHz | +2.0000 in all seven | 29 to 30 dB | none within 40 dB; at three times the tone's frequency, mirrored, -6 MHz, 39 dB |
| -3 MHz | -3.0000 in all seven | 28 dB | 43 dB, at +3 MHz |

So the play is 40 MS/s of the radio's own clock, to within 5 parts in 10^5, and the tone lies where the samples put it.
Against the nRF's carrier the leak lay 4.0002 MHz below it and the tone 2.0001 below:
the two crystals within 0.1 ppm of each other in that run,
where in the runs of `--tune` they lay 20 to 40 ppm apart, the nRF warming, see "A channel";
so the nRF fixes no frequency here to better than tens of ppm.
The leak moved by about 1 kHz a second against the C6 through each tone, the radio warming or the C6, which one is not known.
Our own samples play as well, **measured** the same way, D11 halted:
written into a buffer of RAM and through `0x874960` into table `0x15`, its descriptor the buffer, the count, `0x15`, 0 and 32,
after `0x49c` bit 0 set, `0x86e95c` with 1, `0x6d6` bits 0 and 1 cleared and `0x6da` bit 3 set, as `0x86ea6c` does,
and played by `0x872334` with the count, `0xffff`, 0, 0 and 1, then stopped by `0x86eb6c`, three seconds each:

| Samples | Seen, MHz from the C6's tuning |
|---|---|
| 16, a turn of amplitude 64, I `64 cos`, Q `64 sin` | +2.469 to +2.484 in seven captures |
| the same, I and Q swapped | -2.531 in seven |
| 32, two turns of amplitude 32 summed, one turn up and three down | +1.219 to +1.234 and -3.766 to -3.781 in seven |

So I is bits 10 to 19 and Q bits 0 to 9 as the table reads, and a sample turning as `exp(+j ω t)` sends above the carrier,
the C6's own offset, about -0.02 MHz, aside.
Each line had lines 62.5 kHz either side of it, 40 dB below for one tone; with two, those were 10 to 20 dB below,
and the two tones' peaks over the transform's median bin moved from capture to capture by up to 20 dB, a measure the next run showed unsound.
The level follows the amplitude, **measured** as the captures' RMS, the C6's gain fixed: a turn of 16 samples at amplitudes 16, 32, 64 and 128,
three seconds each, read 52, 108, 200 and 385, against 2 to 3 with nothing sent, 6.4, 5.3 and 5.7 dB for each doubling:
within 0.7 dB of a doubling's 6 dB to 128, the C6's samples reaching 511; and each fell by 0.6 to 0.7 dB over its three seconds.
The amplitude, 64 of the samples' 511, is below the calibration's own `0xb5`, which the firmware plays with the same forcing;
against the firmware's own frames, **measured** in one run, the C6's gain fixed at 14 and 1024 samples a capture:
the access point's frames, caught wholly inside four captures in its first 20 s, read an RMS over 2 µs of 250 to 277,
and then the tone, D11 halted, 50 to 53 at amplitude 32 and 92 to 97 at 64:
the tone at 64 about 9 dB below the frames, at 32 about 14, the frames' band wider than the C6's 16 MHz making them, if anything, stronger still.

## The receiver's samples

D11's sample collection takes nothing from this PHY, see `d11.md`;
the PHY's I/Q estimator gives its samples one at a time, eight bits each,
and its register `0x7e7` the bits of a converter, three times as fast, but of one the calibration's loopback reaches and the air does not,
see "A converter's bits".

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

`user/radio/cyw43439/tools/cyw43-iq.py` has the vendor's firmware bring a mode up, an access point for one,
then holds the WLAN's processor in reset, stops D11's receive DMA, so that no frame lands in the RAM,
and starts the processor again in `user/radio/cyw43439/wlancore/iq-cap.c`, at vectors written over the firmware's;
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
and every 260, 313.8 kS/s, all of 1000 alike, once the code grew and its loop fell elsewhere;
and with the tool's check of each estimate's end 260 to 264 cycles apart, 20000 samples at 312.2 kS/s.
The rest of a sample's time is the code's accesses, each of the PHY's about 16 cycles,
two for a read, the address and then the data, and one for a write, address and data in one word.
The estimate ends before the first read after its start comes back, **measured**:
with `--mode fast,check`, `0x481` read once between the start and the results, none of 3000 samples found it set.
So the code need not poll, and of an 8-bit sample the low halfwords hold all, the squares to 16384 and the product signed:

| `--mode` | What a sample reads | Cycles | Rate |
|---|---|---|---|
| none | polls `0x481`, then six halfwords | 260 to 262 | 311.4 to 313.8 kS/s |
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
That is the route's floor, a hypothesis from its parts: three accesses of about 14 cycles each, the start and two reads, one after another,
as the external IHR takes one at a time, and the estimate's own time between the start and the first read,
the least wait `0x482` takes, two samples at 20 MHz, and the sample, about 12 cycles: 54 of the 56.
A read of the last sample's results while the next estimate runs would come back after that estimate ends, torn, as the delays above show;
so a faster receiver needs samples that come otherwise than an estimate each; `0x7e7`'s do, but not of the air, see "A converter's bits".

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

### A converter's bits

`0x7e0`'s bits 9 to 11 choose what `0x7e7` shows, **measured**, with the MAC suspended:
1 a word that differs at nearly every read; 2 `0x0ffe`, 3 `0x000a` and 4 `0xf531`, steady; 0, 5, 6 and 7 zero.
With 1, bit 8 chooses between two such streams, `0x0200` and `0x0300`, alike in all measured here,
and read in turns, one of each 48 cycles apart, a tone in each as strong and as far in phase as a read of `0x0300` 48 cycles after another;
bits 12 to 15 change nothing, and none of 16 values of the low byte changed where a tone falls nor how strongly.
No code of the firmware's names either register.

Each bit of the word is one about half the time, in runs of two more often than chance would make them and of four and more less often,
0.39 and 0.02 of runs within a word, where random words give 0.25 and 0.05, **measured**;
two words read 207 ns apart share no bits at any shift of up to 7, so the bits run faster than one in 26 ns:
the output of a converter, a sigma-delta modulator whose bits the word samples, a hypothesis.
Within a byte two bits are as alike as they are near, whichever two they are, **measured**, 4800 words without a tone:
-0.06 apart by one, -0.37 by two, -0.28 by three, +0.1 to +0.17 by four and five;
the bytes of I and of Q alike, and nothing shared between them.
So a byte is eight bits one after another, a hypothesis that the tones bear out:
a bit's phase in a tone grows from bit 0 to bit 7 by 0.65 degrees a bit for each MHz, ±0.09, over 22 projections of ±2, ±4 and ±6 MHz,
bit 0 the first, as a phase that grows with time has it, and one at 430 to 760 MHz, at twice the error either way, eight in 10.5 to 18.6 ns.
The bits' likeness also weighs them, as the noise's least for one sum:
0.64, 0.84, 1.15, 1.37, 1.37, 1.15, 0.84 and 0.64 from bit 0, which raised the tone's share from 18 to 29% at ±2 MHz
and from 6 to 10% at ±6, **measured**, 2400 words a run; the tool counts the ones alone.

Bits 0 to 7 belong to one channel and bits 8 to 15 to the other, **measured**, with the calibration's tones, 500 words a run:
each bit follows the tone, bits 0 to 7 in one phase and 8 to 15 about 100 degrees from it, before it for +2 MHz and after it for -2;
and the sample I + jQ, I the ones of bits 0 to 7 and Q those of bits 8 to 15, each less its mean,
puts +2, -2 and +1 MHz where the play puts them, sign and all, -862, +862 and +999 kHz at 2860 kS/s, against -860, +860 and +1000,
and with the tool's `--mode bits` +2, -2 and +6 MHz at +1999.4, -1999.4 and +1529.0 kHz at 4470.3 kS/s, against +2000.0, -2000.0 and +1529.7.
So one read gives a whole sample, I and Q, with its sign, where the estimator gives its square;
but of nine levels each, and the tone holds 15 to 23% of the power, where the estimator's samples hold 60 to 75%.

They are not the receiver's samples of the air, **measured**:
an nRF52840 sending IEEE 802.15.4 frames on 2435 MHz at 8 dBm, the access point on channel 6,
the WLAN's processor took 30 blocks of 4096 reads of `0x7e7`, 0.9 ms each, between 30 blocks of 512 estimates, 0.5 ms each,
the gain held at 10, 9 and 0, and again at 10, 9 and 13.
The estimates held frames, `|I|` up to 34 over a noise of 1 or 2, and up to 90 over a noise of 12, the third stage's 17 dB;
the bits' power, their weighted ones less their mean, was 0.44 to 0.47 in every block,
and no 256 reads of any block rose past 0.58, at either gain.
Without a held gain, 42 ms of bits held five bursts of two to four times the power across the whole fold, centred near 0 Hz,
as the AGC's steps would make them and no frame on 2435 MHz would, a hypothesis.
So `0x7e7` shows a converter the calibration's loopback reaches and the antenna does not,
the calibration's own receiver, a hypothesis; the receiver's decimated samples, which the estimator takes, show in no selection.
Once in some 20 runs its stream read one value throughout, and the run after did not.

Read so, a sample takes, **measured**:

| By | Cycles | Rate |
|---|---|---|
| the WLAN's processor, the address written once and the data read again and again, nothing kept | 18.0 | 4526 kS/s |
| the same, two reads to a word stored between them and the next, `--mode bits` | 18.25 | 4470 kS/s |
| the same, each read with its cycle count | 28.5 | 2860 kS/s |
| the PSM, through the external IHR, its busy bit awaited | 16.9 | 4822 kS/s |

With `--mode bits` the reads are as evenly apart as the store between each two lets them be,
four pairs to the loop's test, and a run fills the RAM up to `0x7e000`: 190464 samples, 42 ms, **measured**,
which held one burst of about 114 µs over twice the median's power, with the access point up on channel 6.
Eight reads before their four stores took 18.5 cycles each, and each read stored alone 24.8.

The PSM's rate is D11's TSF over two runs of 1075200 and 4300800 reads into its shared memory, the second less the first,
which leaves out the start and the host's poll; the tones put it at 4800 kS/s, their frequencies scaled by 0.854 against the tool's 4100,
whose timing through `psm_base_0` holds for the estimator's loop and not for this one, a hypothesis.

The two processors reach the PHY by paths of their own, **measured**:
with the WLAN's processor reading `0x7e0` while the PSM read `0x7e7`, all 16000 of its reads gave `0x0200`,
and none of the PSM's 3584 did, which held the tone;
reading for seconds at once, the WLAN's processor kept its 18.0 cycles and the PSM went from 4778 to 4461 kS/s, by D11's TSF.
So the two together read about 9 MS/s, two streams of samples at instants of their own,
which make one only once each sample's instant is known on one clock; that is open.

The registers beside them hold what is written and choose nothing that shows, **measured**:
`0x7d0` to `0x7de`, `0x7e1` to `0x7ee` but `0x7e7`, `0x7c3`, `0x7f6`, `0x7f9` and `0x7fa`, 32 registers and 396 bits that hold,
read 0 as the firmware leaves them, and no code names any; `0x7df` faults.
Each bit flipped alone, and 64 settings of all of them at once, at random but for all 0 and all that hold set,
left `0x7e7` as it was under each of the eight selections, 0 to 7:
the steady ones at their values, and the stream as it was, by the likeness of a byte's bits two apart, the high bits' agreement and the distinct reads;
so did each bit of `0x6da`, the clocks.
Selections 2, 3 and 4 held one value over three million reads each, and their values differ from run to run, `0x0fff` or `0x0ffd`, `0x000a` to `0x000c`:
values latched, not samples.

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

Held at 10, 9 and 0, one of I and Q read 0 throughout in 15 of 35 runs, I in 11 and Q in 4, **measured**;
the run's other channel and its frames were as in the rest.
It is the power-up's, and the held gain's, **measured**, estimates over the bridge:
with the AGC free both channels read, in each of 18 power-ups; held, the same channel read 0 at every one of seven holds, each after a release,
in 5 of 6 power-ups, and in none of them free between the holds.
Over 36 held gains, 4, 7 and 10 of the first stage by 5, 7, 9 and 11 of the second and 0, 4 and 7 of the third, in four power-ups,
a channel read 0 only near the ends, at 4 and 5 with the third at 0 or 4, and at 10 with 9 or 11, or 7 with 11, whatever the third;
7 with 7 or 9, and 10 with 5 or 7, kept both in all four.
The correction of each channel's offset following the AGC's own gain and not the held one, so that at an end
a channel's offset, which differs from power-up to power-up, holds it against a rail that the PHY's removal of the offset then makes 0,
is a hypothesis; at 4 and 5 the noise may lie under a step.
`--gain` takes 60 estimates once the gain is held and stops if a channel reads 0 in all.

### The receiver's strength

`0x944`'s low byte is the strength the receiver hears, signed, about a dB a step, **measured**:
an nRF52840's carrier on 2440 MHz, the radio tuned over the bridge to 2437 MHz and the AGC free,
the carrier's power stepped 12 s a step while the bridge read the registers 27 times a second;
the steps' most common readings, with the median of I^2 + Q^2 over 30 estimates:

| Carrier, dBm | none | 8 | 4 | 0 | -4 | -8 | -12 | -16 | -20 | -40 |
|---|---|---|---|---|---|---|---|---|---|---|
| `0x944` | `0x9d` | `0xbe` | `0xbc` | `0xb7` | `0xb4` | `0xb2` | `0xae` | `0xaa` | `0xa6` | `0x9d` |
| As signed | -99 | -66 | -68 | -73 | -76 | -78 | -82 | -86 | -90 | -99 |
| I^2 + Q^2 | 4 | 404 | 444 | 489 | 472 | 491 | 212 | 89 | 36 | 4 |

The reading less the carrier's power is 70 to 74 throughout, the path's loss and the reading's offset together;
the nRF's steps are nominal, so the slope is a dB a step to within their accuracy.
With nothing in the channel it reads -99, about the noise of 20 MHz: a strength in dBm, a hypothesis whose offset no measurement here fixes.
It follows the channel without a frame, as the steps show, and is read live.

The estimator's power rises 3.6 to 3.9 dB for each 4 dB up to -8 dBm and stays near 480 above it:
the AGC keeps the gain at its most until the signal reaches about -78 by `0x944`, and then holds the signal at its level.
As it takes the gain down, **measured** in the same run:
`0x5b0` goes from `0x3110` to `0x2510`, `0x5b2` from `0x0110` through `0x0510` to `0x1510`,
and the radio's `0x02b` from `0x740` to `0x742`, then `0x746` at 4 and 8 dBm, its bits 1 and 2 an attenuation of its front, a hypothesis.
`0xb1f` rises with a weak carrier as the estimator's power does, 1, 11, 27 and 65 from -40 to -12, and past the gain's change reads values that do not follow it.
`0x5b5` reads `0x80` or `0x60` at every level.

### The transmitter's gain

The gain's stages, **read** in the ROM:
`0x873514` sets gm and pga into `0x4b5` (gm bits 0 to 4, pga 8 to 15), pad into `0x4fb` bits 0 to 7 beside the PA's byte in bits 8 to 15, `0x8733c4`'s,
and the DAC's gain into `0x439` bits 7 to 10, then the override on;
`0x87309c` writes the baseband multiplier, bbmult, into table `0x00` at 99 and 115, the IQ and LO corrections beside it at 96 to 98 and 112 to 114;
`0x8705e4` reads gm, pga, pad and the DAC's gain back and `0x870594` bbmult.
The override, `0x873564`, is `0x4b0` bit 7 with `0x43a` bit 6; with it clear the power control drives the stages.
The power control's mode is `0x4a4` bits 13 to 15, `0xe000` the hardware's loop, which `0x87358c` sets, its start index in `0x4a4` bits 0 to 8 as twice the index;
the loop's index reads in `0x473` as twice it, `0x86e970`.
Table `0x07`, 32 bits wide, holds for each index 0 to 127 the gains at `0xc0` and up, gm, pga, pad and the PA in bytes from bit 0, 5, 13 and 21,
and at `0x240` and up `0x807`'s bits, bbmult in bits 9 to 16 and the DAC's gain in 17 to 20,
copied from the firmware's RAM, one of two tables of 8 bytes an index, `0x2cb85` on this board;
`0x86ea54` holds the gains at an index, the loop off and the override on, out of that table.
A higher index sends less: the loop raises it when its estimate passes its target.
On this board gm is 6, pga 15, the PA's byte 255 and the DAC's gain 0 at every index; pad and bbmult fall with it, 63 and 98 at 0, 20 and 25 at 66, 8 and 15 at 100, 1 and 16 at 127.
The firmware starts the loop at 66, `0x4a4` `0xe484`; after 30 s of the access point's beacons it stood at 58.

What they do on the air, **measured** with the turn of 16 samples at amplitude 64 three seconds each, D11 halted,
as the tone's three strongest bins about +2.47 MHz of 1024-point transforms of the ESP32-C6's captures at 2437 MHz, gain 14, all in one run:

| Gains | pad, bbmult | The tone |
|---|---|---|
| the loop, from index 66 just after the access point came up | 20, 25 | 0 dB |
| held at index 66 | 20, 25 | -5.7 dB |
| held at 100 | 8, 15 | -14.6 dB |
| held at 127 | 1, 16 | -19.2 dB |
| held at 127, bbmult then 8 | 1, 8 | -25.5 dB |

bbmult halved took 6.3 dB, the amplitude's own factor; the loop at 58 sent 7.7 dB more than 66 held, in another run, by the captures' RMS.
The loop sends 5 to 6 dB more than its own index held, in three runs, 5.7, 5.1 and 5.9 dB, the last with the loop's tone again after the held ones, 0.4 dB from the first:
the loop adds a gain the held stages do not.
Holding the index changes `0x807` from 9 to `0xd`, and clearing its bit 2 again while held changed the tone by 0.1 dB; `0x4b5`, `0x4fb` and bbmult read the same either way.
Two things of the loop's own the hold undoes, **read** in `0x87358c`:
entering the loop it writes `0x46e` 1, and `0x87467c` puts the rates' offsets, the CLM's limits negated, into table `0x07` at `0x340`, 22 entries;
leaving it writes `0x46e` 0 and `0x86fc1c` zeroes the table at `0x340` and at `0x80`, 64 entries.
On this board the entries at `0x340` read `0xf2`, -14, in the loop and 0 held, and those at `0x80` 0 either way, **read**.
What each does, **measured** as above, in two runs, the loop's tone 0 dB in each:

| Gains | The tone |
|---|---|
| held at 66 | -5.2 dB |
| held at 52, 66 less 14 | -2.0 dB |
| held at 66, `0x46e` 1 | -3.0 dB |
| held at 66, the entries at `0x340` `0xf2` | -5.3 dB |
| held at 66, both | -3.0 dB |
| held at 52, `0x46e` 1 | -1.0 dB |
| the loop, the entries at `0x340` 0 | -0.2 dB |
| the loop, `0x46e` 0 | -1.4 dB |
| the loop, both | the noise's floor, -36.8 dB |

So an index step is about 0.23 dB, and `0x46e` adds about 2 dB to held gains, which the entries at `0x340` do not reach.
The loop is within 1 dB of the index less the offset with `0x46e` on, a hypothesis:
in the loop, either alone taken away costs less than its share, and both together stop the transmitter,
so the loop does not simply add the two.
`0x88e`, which differs between the loop and the hold and which no constant call of the ROM names, kept its `0x0002` when written through D11's window,
and a write of it from the WLAN's processor never returned, as one of `0x404` does, see "From the WLAN's processor".
The loop's estimate of the power sent, `0x4ab`, `0x804d` in one run and `0x804a` in another, and its target `0x4a7`, `0x34`, did not move while a tone played,
at amplitudes 32, 64 and 128, whose tones read 0, +4.7 and +10.5 dB: the estimate is the frames', taken as they are sent, a hypothesis.
Setting the index, `0x86ea54`, sent something for about 0.1 s itself, seen between two tones.
The level a run reads depends on the run: the beacons read an RMS of 250 to 277 in one and about 39 in another, the tone the same 16.5 dB apart,
so only levels of one run compare, as here.

### The receiver's passband

Across its channel, **measured**, an nRF52840's carrier on 2440 MHz at 8 dBm, the radio retuned over the bridge
by the synthesizer's word and its calibration's steps, `0x8720a4`'s, so that the carrier lay at each offset below it,
the gain held at 10, 7 and 0, and the median of I^2 + Q^2 over 120 estimates at each:

| Offset, MHz | 0.5 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Median | 9 | 13 | 20 | 25 | 26 | 25 | 20 | 18 | 20 | 18 | 5 | 1 |

Flat within 2 dB from 2 to 9 MHz, 7 dB down at 10 and gone at 12: the channel's 20 MHz.
Toward 0 Hz it falls, 4 dB at 0.5 MHz, as the removal of the offset reaches, see "The receiver's gain".
The transmitter's low-pass filter's registers, `0x072` to `0x077`, set to each of the modes 0, 1, 3 and 5 of `0x8719a8`'s tables,
for the firmware's mode 2, changed none of it by more than the readings' own spread, **measured**:
that filter is not the receiver's, or not the narrowest the receiver has.

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

`user/radio/cyw43439/tools/cyw43-code.py` puts the WLAN's ROM, at `0x800000`, and its RAM firmware, at 0, in Ghidra as one program, **known**:
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

`cyw43-iq.py --tune MHZ` tunes the radio off the channels by the same function, **measured**:
its code on the WLAN's processor copies the nearest channel's entry, writes the frequency into its MHz and `0x055`'s,
and the word, f·3·2^18/37.4 rounded down, into `0x04e` and `0x04f`'s, points the LCN20's state's word at `0x620`, the table's, at that entry,
calls `0x8714e4` with channel 1, and points it back; the PHY's words of the frequency stay the channel's.
At 2415, 2435, 2437 and 2455 MHz the radio read the word back as written and `0x065` bit 0 set, the synthesizer locked.
A carrier where it is meant to be shows so, **measured**, an nRF52840's unmodulated carrier, its radio idle in transmission,
on 2440 MHz at 8 dBm, and runs of `--mode stream`, 10180 samples, the AGC free:
the sample's square, the carrier at twice its offset, was strongest at +199.97, -200.16 and -400.20 kHz
tuned to 2439.85, 2440.05 and 2440.15 MHz, 100.03, 100.16 and 100.20 kHz under twice the offset from 2440,
the same within 0.2 kHz, so that the carrier lay 50 kHz under 2440 MHz by this crystal, 20 ppm, the two crystals' difference;
on channel 6 tuned by the firmware it lay 95.4 kHz under, and tuned to 2439.9 by this 96.8, while the nRF52840 warmed.
So the tuning moves the receiver by what it is asked, to well within a kHz.
Tuned to 2439.95 MHz, the carrier at 0 Hz, the samples held a median power of 4 for the others' 2000 and more:
the PHY's removal of the offset takes a carrier at 0 Hz with it.
Held at 10, 7 and 0, the carrier 100 kHz off was gone too, a median of 1, while 3 MHz off, on channel 6, it held 25:
with the gain held, the removal of the offset reaches further from 0 Hz, a hypothesis.

The PHY's words of the frequency, which the readouts fitted, are the code's:
`0x657` is `((f << 2) / 5 + 1) >> 1`, `0x658` is `(0x500000 / f + 1) >> 1`,
`0x980` and `0x981`, and `0x984` and `0x985` again, come from `0x86ee3c` of the channel,
and `0x9ad` with `0x9ae`'s low 10 bits are f/5 with 16 bits of fraction, from a table of 14 words at `0x892cc4`, `0x1e26666` for channel 1;
`0x9b1` is `0x500` over the greatest common divisor of `0x500` and f, and `0x986` f over that of f and `0x780`,
as the readouts' `0x140`, `0x500` and `0x280`, and `0xc9`, `0x985` and `0x4cf`, on channels 1, 6 and 11 bear out.

### Tables of values

- Radio registers `0x0a9` to `0x0dc` take 52 bytes from the ROM at `0x89027c`, and `0x0dd` to `0x114` 56 halfwords from `0x89020c`, in `0x871fe0`.
- Radio registers `0x072` to `0x077` take values the RAM firmware computes, at `0x10480`, the ROM's `wlc_lcn20phy_radio20692_lpf_tx_set` patched:
  the readouts' registers that differ from run to run without the channel, **read**.
  `0x072`, `0x073`, `0x074` and `0x076` are rows of tables, six modes of 15 columns at `0x88ffac`, `0x89006c` and `0x890120`, and a word a mode at `0x890060`,
  each scaled by the LCN20 state's halfword at `0x5ae`, a calibration's result about 4350, over 2^15; `0x077` a mode's byte at `0x88fda6`;
  `0x0a3` bits 0 to 4 and 13 set.
  With an access point up they read mode 2, column 2, to the last bit.
- The transmitter's digital filters: `0x870a28` writes 17 registers from a row of a table chosen by a type,
  for CCK `0x910`, `0x91e` to `0x929`, `0x930` to `0x932` and `0x912`, 17 types at `0x88f68c`,
  and for OFDM `0x90f`, `0x900` to `0x90e` and `0x911`, 18 types at `0x88f8f0`:
  three sections of two feedback and three feedforward coefficients each, as the registers group, a hypothesis, and a word more,
  the registers brcmsmac's LCN PHY loads in `wlc_lcnphy_load_tx_iir_filter`, which has one fewer.
  The pulse the transmitter shapes is therefore a table's, and a type the firmware chooses for a channel.
