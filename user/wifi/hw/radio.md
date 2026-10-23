# The WLAN's radio, the 20692

The radio's registers, `0x000` to `0x1ff`, reached through D11's `radioregaddr` and `radioregdata`, see `phy.md`,
by what the WLAN ROM's code does to them, **known**, from every caller of the radio's access functions,
`0x86c064`, `0x86e2f8`, `0x86bedc`, `0x86bec0` and `0x86bf00`, read in `user/wifi/tools/cyw43-code.py decompile`:
27 functions; no list `0x86bfa8` plays reaches the radio, and only the radio's id, `0x86c578`, sets `radioregaddr` otherwise.
What a block is, where the code does not name it, is a hypothesis, and said as one.

## Overrides

Registers `0x096` to `0x0a7` are enables, **known**: the code sets one beside each value it forces,
and a bit not overridden is the PHY's sequencer's to drive.

| Enable | Bit: the field it lets the code set |
|---|---|
| `0x096` | 0: `0x003` bits 0 to 9; 1: `0x002` bit 0; 2: `0x002` bits 1 to 3; 3: `0x002` bits 8 to 10 |
| `0x099` | 0: `0x009` bit 0; 1: `0x009` bits 4 to 6 |
| `0x09a` | 0: `0x01c` bit 0, `0x017` bit 14 |
| `0x09b` | 1 to 6: `0x019` bits 0, 1, 2, 3, 4 and 7 |
| `0x09c` | 1: `0x027` bits 8 to 11; 2: `0x026` bit 12; 3, 4, 5: `0x01f` bits 0, 2 and 3 |
| `0x09d` | 6, 7, 8: `0x02d` bits 9, 10, 11; 12: `0x02b` bit 0 |
| `0x09e` | 0: `0x035` bit 0 |
| `0x09f` | 1, 2: `0x03e` bits 4, 5; 4, 5: `0x03d` bits 13, 14; 6: `0x03a` bit 5; 7, 8, 9: `0x038` bits 0, 1, 2 |
| `0x0a0` | 4 to 7: `0x042` bits 0 to 3; 8, 9: `0x042` bits 4, 5 |
| `0x0a1` | 0: `0x067` bit 0 |
| `0x0a2` | 0, 1: `0x068` bits 0, 1 |
| `0x0a4` | 0, 1: `0x07e` bits 8, 14 |
| `0x0a5` | 5, 6: `0x081` bits 3, 4; 9, 10: `0x080` bits 5, 6 |
| `0x0a6` | 0: `0x088` bit 0 |

As an access point leaves them, `0x09b` reads `0x7f`, `0x09c` 4, `0x09f` `0x26`, `0x0a0` `0x3f0`, `0x09a` and `0x0a7` 1, the rest 0.

## The functions

| Function | Called by | What it does |
|---|---|---|
| `0x871668` | the LCN20's initialisation, before the first channel | powers the radio up, **known**: `0x08c` bit 0 set for a 37.4 MHz crystal; `0x019`, the main power, bits 7 and 3, then 4, 2, 1, 0, with waits of 100 and 200 µs; `0x0a7`, `0x089`, `0x097`, `0x004`, `0x043` bit 0; the RC calibration, the receiver's filter codes, the tables of `phy.md`; and fixed values into `0x008`, `0x06f`, `0x06c`, `0x089`, `0x018`, `0x017`, `0x08d`, `0x079` to `0x07d`, `0x083`, `0x087` |
| `0x871b28` | `0x871668` | the RC calibration, **known**: two runs from a table at `0x892ca4`, each a pulse of `0x01e` bit 0, a start at `0x08f` bit 0, `0x091` bit 0 awaited, and a count, `0x093` less `0x092`, 13 bits; the first gives the LCN20 state's `0x5ae`, which scales the transmitter's low-pass filter, the second its `0x5b4`, which sets the receiver's; `0x08a` bit 10 gates its clock |
| `0x8714c4`, `0x86f05c`, `0x86eed4` | `0x871668` | the receiver's baseband filter, see below |
| `0x8714e4`, `0x8720a4` | the channel's setter `0x874a14` | the channel and the synthesizer's calibration, see `phy.md`; `0x08a` bit 8 gates the calibration's clock |
| `0x8725a8` | `0x874a14` | the channel's receiver, **known**: `0x019` bit 0, `0x01c` bit 0, `0x017` bit 14, `0x03d` bit 14, `0x03e` bits 4 and 5, `0x026` bit 12, each with its enable; `0x030` bits 0 to 5 at 8, `0x031` bits 1 to 6 at 8, `0x039` bits 0 to 3 at 0, and through `0x86fe7c` `0x032` bits 1 to 6 at 2, beside the PHY's `0x965` to `0x976` |
| `0x86f2e8` | `0x874a14` | the channel's end: fields of `0x035`, `0x036` and `0x03a`, biases of the receiver's front, a hypothesis |
| `0x8719a8`, patched at `0x10480` | `0x874a14` | the transmitter's low-pass filter, `0x071` to `0x077` and `0x0a3`, see `phy.md` |
| `0x873e18`, `0x875b48`, `0x875a90` | the calibrations' `0x86fa64` | the transmitter's IQ and LO calibration, as brcmsmac's LCN PHY's, **known**: its envelope detector, `0x068` and `0x081` bits 3 to 8, with `0x002` and `0x003` at the NVRAM's `AvVmidIQcal`, the receiver off |
| `0x8738ac`, `0x87577c`, `0x87567c` | `0x86ec6c` | temperature, mode 1, and the battery's voltage, mode 2, as brcmsmac's sense, read in the PHY's `0x475` to `0x47a`, **known**: `0x009` bits 4 to 6 at 1 or 3, `0x067` or `0x088` bit 0, `0x002` and `0x003` at 3 and `0xa4` |
| `wlc_lcn20phy_tssi_setup`, `0x872028` | the idle TSSI's estimate | the transmitter's power detector, **known**: `0x00a` to `0x00c`, `0x01d` bit 5, `0x068`, `0x079` bit 5, `0x06f` bits 12 and 13 |
| `0x874ef0`, `0x874d34`, `0x874d90` | `0x874bb8`, from `0x86fa64` | the transmitter's predistortion's calibration, a hypothesis from its table of 64 words: the loopback, `0x07e`, `0x080`, `0x081`, `0x03e`, `0x03d` bit 12, `0x038`, `0x02d` and `0x027`, 24 registers saved and restored |
| `0x871d5c`, `0x871ce0` | the receiver's IQ calibration, `0x872770` | the same loopback, with `0x081` bits 1 and 2 and `0x03e` bits 0 and 1 set |
| `0x873a88` | `0x871d5c`, `0x874ef0` | gives the front's gain to the PHY: `0x09d` bits 0 to 4, 7, 9 and 11 to 14 cleared, the PHY's `0x53d` bit 3 set and `0x53c` bits 9 to 12 at 3 |
| `wlc_lcn20phy_rx_iq_cal_txrxgain_control` | the receiver's IQ calibration | reads `0x037` bits 8 and 11 eight times each and counts them as clips, **known** |
| `0x870b40`, `0x8700bc` | `0x86fa64` | a spur's calibration, a notch at the PHY's `0x9d4` to `0x9d7`, a hypothesis; it plays a tone |
| RAM `0x1147e` | by a pointer | powers the radio down, a hypothesis: `0x019` at 0 |

The sensing works from our code on the WLAN's processor, the firmware stopped, **measured**, a call a time, an access point up:
`0x86ec6c(pi, 1)` returned the die's temperature, 28 °C rising to 30 over a few minutes, in 9893 cycles,
`(0xa1 raw + 0x660f + 128) >> 8` with the LCN20 state's words at `0x5c0`, `0x5c4` and `0x5c8`, 8 the shift,
of `0x8738ac(pi, 1)`'s raw reading, -113 to -115; with 2 it returned the last temperature in 264 cycles, measuring nothing.
`0x8738ac(pi, 2)`, the battery's voltage by `0x475`, read 255 every time, the most of its 9 bits signed: out of its range.

`0x86fa64` runs the calibrations its mask chooses, **known**:
the receiver's offsets, `0x86fdd4`, in the PHY alone; the transmitter's IQ and LO; the receiver's IQ; the spur; the idle TSSI; the predistortion.
The channel's setter calls it, and so does a periodic `0x875f88`.
Those of the transmitter, and the spur's, make the transmitter play; the receiver's IQ calibration drives it too, into the loopback.

## The registers

| Registers | What they are |
|---|---|
| `0x000`, `0x001` | the radio's id, with `0x002`'s reading, see `phy.md` |
| `0x002`, `0x003` | a detector's buffer, **known**: `0x002` bits 8 to 10 its gain Av, `0x003` bits 0 to 9 its Vmid, the NVRAM's `AvVmidIQcal`, 7 and `0x92`; bit 0 its power, bits 1 to 3 its mode, 4 and 5 TSSI's choice |
| `0x004`, `0x008`, `0x017`, `0x018`, `0x019`, `0x01c`, `0x06c`, `0x089`, `0x08d`, `0x097`, `0x0a7` | power and biases set as the radio comes up; `0x019` the main power, reading `0x9f` |
| `0x009` | bit 0 the sensors' power, bits 4 to 6 their choice: 0 the detector, 1 temperature, 3 the battery, a hypothesis from `0x8738ac` |
| `0x00a` to `0x00c`, `0x01d` bit 5 | the transmitter's power detector |
| `0x01b`, `0x03c`, `0x042`, `0x043`, `0x045` to `0x066`, `0x086` | the synthesizer, see `phy.md`; `0x042` bits 0 to 5 its powers and its calibration's strobes |
| `0x01e`, `0x08f` to `0x094`, `0x08a` bit 10 | the RC calibration's engine |
| `0x01f` | bits 0 and 3 the receiver's baseband powers, on in every calibration that receives; bit 2 the filter codes' strobe |
| `0x020` to `0x02a` | the receiver's baseband filter, codes from the RC calibration and fixed biases, see below; `0x026` bit 12 a power |
| `0x027` bits 8 to 11 | the receiver's powers off, `0xf`, in the transmitter's calibrations, a hypothesis |
| `0x02b` bit 0, `0x02d` bits 9 to 11 | powers of the receiver's front, a hypothesis; `0x02b` bits 1 and 2 set as the AGC takes the gain down for a strong carrier, **measured**, see "The receiver's strength" in `phy.md` |
| `0x030`, `0x031`, `0x032` bits 1 to 6, `0x039` | set with the channel, beside the PHY's offset loop; `0x032` bits 8 to 11 and `0x034` differ from power-up to power-up and no code writes them: the offset's DACs, the PHY's to set, a hypothesis |
| `0x035`, `0x036`, `0x038`, `0x03a`, `0x03d` bits 13 and 14, `0x03e` bits 4 and 5 | biases and powers of the receiver's front, a hypothesis |
| `0x037` bits 8 and 11 | clips, read only, **known** |
| `0x03d` bit 12, `0x03e` bits 0 to 2, `0x081` bits 0 to 2, `0x07e` bits 8 and 14, `0x080` bits 5 and 6 | the loopback from the transmitter into the receiver, on only in the predistortion's and the receiver's IQ calibrations |
| `0x067` bit 0, `0x088` bit 0 | the temperature's and the battery's sensing |
| `0x068`, `0x06a`, `0x081` bits 3 to 8 | the transmitter's envelope detector: powers and gains |
| `0x071` to `0x077`, `0x0a3` | the transmitter's low-pass filter, see `phy.md` |
| `0x079` to `0x087` | the transmitter's mixer and amplifiers, set once as the radio comes up, a hypothesis; `0x079` bit 5 cleared for TSSI and the IQ and LO calibration |
| `0x0a9` to `0x114` | tables from the ROM, see `phy.md` |

## The receiver's baseband filter

`0x86f05c` makes eleven codes from the RC calibration's second count, the LCN20 state's `0x5b4`, **known**,
and `0x86eed4` writes them into fields of `0x020` to `0x029` and strobes `0x01f` bit 2 with its enable, `0x09c` bit 4, set:
`0x020` bits 0 to 6, `0x021` bits 0 to 6 and 8 to 14, `0x022` bits 0 to 6 and 8 to 14, `0x023` bits 0 to 6 and 8 to 14,
`0x028` bits 0 to 6 and 8 to 14, `0x029` bits 0 to 6 and 8 to 15.
Three are fixed, 16, 42 and 10; of the rest, with u = 2^24 over the count, one goes as u^2, five as u, and two the other way, as the code's divisions have it.
The function ported to Python, the count 3811 of one power-up gives the codes the access point left in it, 16, 36, 22, 21, 42, 82, 9, 10, 12, 10 and 24, to the last bit, **measured**.

The codes for other counts, written and strobed over the bridge, change the receiver's gain more than its width, **measured**,
an nRF52840's carrier on 2440 MHz at 8 dBm swept across the channel as in `phy.md`'s "The receiver's passband", the gain held at 10, 7 and 0,
the median of I^2 + Q^2 over 120 estimates, in and out of the channel:

| Count | In the channel | At 10 MHz | 12 and 14 MHz |
|---|---|---|---|
| 3822, the radio's own in that power-up | 4 to 8 | 2 | 1 |
| 1.5 times | 8 to 14 | 6 | 4 |
| twice | 51 to 86 | 50 | 34 and 40 |
| two thirds | 49 to 99, falling from 4 MHz | 36 | 39 and 48 |
| half | 300 to 650 | 338 | 350 and 576 |

Every count but the radio's raised the noise as well as the carrier, and the edge near 10 MHz stayed, but for a slower fall from 4 MHz at two thirds:
the 20 MHz edge is the PHY's, a hypothesis, and these codes set the baseband's gain and shape behind it.
At half the count two of the codes, 7 bits wide, reach their most, 127, so that the filter cannot be made much narrower by them:
no analog filter here narrows the channel to the width of a sampler of about 1 MS/s, as far as these codes go.
Every code scaled alike by 2 and 3 made the noise across the whole fold about 25 and 350 times its level; by a half the edge left the channel.
