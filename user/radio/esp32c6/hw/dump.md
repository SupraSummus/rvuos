# The dump engine

The modem holds a block at `0x600A9000`, inside the modem's frame, that no header of Espressif's names:
an engine that copies what the receiver hears into a bank of SRAM, one 32-bit word a sample,
for the factory's RF tests, which Espressif's RF test library, `librftest.a`'s `adctrig`, drives.
ESPARGOS's ESP-SDR found it and drives it on ten of Espressif's chips;
its firmware is GPL-3.0, so this driver takes facts from its description, never its code,
and reads `adctrig` in the ESP32-C6's `librftest.a` of `esp-phy-lib` itself, as it read `libphy.a`.
The engine is the radio's, `user/radio/esp32c6/dump.h` and `dump.c`, and the sampler a program of the radio's,
`prog/sampler.c`, which the root task gives the bank and HP_SYSTEM as its header asks;
it was the root task's own at first, behind a build of its own, until the radio's programs were apart from the station.

To run it, a configuration file in `local/`, which git does not track, holds `channel=`, a channel of Wi-Fi's,
with `run=` and the keys `dump.h` lists:

    printf 'channel=1\nrun=30\ndump=4096\ndumpsource=15\ndumprate=3\ndumphex=4096\n' > local/sampler.conf
    make BOARD=esp32c6 radio PROG=sampler RADIO_CONFIG=local/sampler.conf PORT=/dev/serial/by-id/usb-Espressif_...
    tools/dump-spectrum.py build/esp32c6/radio-run.log 16e6

A signal of known frequency comes from the nRF52840, once the C6's log says its driver listens:
`make BOARD=nrf52840 thread THREAD_CONFIG=` a file of `channel=12`, `stream=100` and `run=10`
sends 802.15.4 frames on 2410 MHz for ten seconds, which fails at its end, as the C6 acknowledges none.
The captures of those seconds, and only those, hold them; see the control below.

The words, as `adctrig` writes them and the board answered:

| Word | Bits | What | Source |
|---|---|---|---|
| `0x600A9004` | 0 to 14 | the samples a capture takes | `adctrig` writes its first argument plus one |
| | 15, 16 | the 80 MHz clock divided by 1, 2, 3 or 5 | ESP-SDR; 3 gave 16 MS/s on the board, measured below |
| | 17 | cleared before a capture | `adctrig` |
| | 18 | set once the samples are written | `adctrig` waits on it; ESP-SDR writes it before a capture, to clear it |
| | 19 | written 1 and then 0, the capture starts | `adctrig`, for its first mode alone |
| | 31 | the engine on | `adctrig` |
| `0x600A9008` | 15 to 18 | the source, see below | `adctrig`'s modes write 15, 1, 0, 1, 2, 3, 4, 5, and 0 for BLE |
| `0x600A9014` | 0 to 23 | four lanes of six bits, which signal each part of the word is | `adctrig`: 0 to 3 for Wi-Fi's receiver, 14 to 17 for BLE's; bit 24 cleared |
| `0x600A20B4` | 0 | set, the BLE baseband's side; cleared, Wi-Fi's | `adctrig`'s eighth mode sets it, the others clear it |
| `0x600A210C` | 7, 8 to 11, 14, 15, 28 to 31 | the BLE side's set-up, and in bit 7 its start; see below | `adctrig`'s eighth mode |
| `0x600A4E30` | 8, 0 to 7 | set, and a byte, for source 5 | `adctrig`'s seventh mode; a word of the Wi-Fi MAC's |
| MODEM_SYSCON `CLK_CONF` `0x600A9804` | 31 | `CLK_DATA_DUMP_EN`, the engine's clock | ESP-IDF's `modem_syscon_reg.h`; `libphy.a`'s `phy_get_adc_rand` sets it too |
| HP_SYSTEM `SRAM_USAGE` `0x60095004` | 8 to 11 | a bit a 128 KiB bank of SRAM, set the engine's | ESP-IDF's `hp_system_reg.h`; `adctrig` sets bit 10, the bank at `0x40840000` |
| | 16 | `MAC_DUMP_ALLOC`, the bank's upper 64 KiB | ESP-IDF's `hp_system_reg.h` |

A capture writes from the bank's first word, as many words as it takes,
each two 10-bit two's complement samples, bits 10 to 19 the in-phase and 0 to 9 the quadrature,
then the receiver's gain in bits 20 to 27, on Wi-Fi's side its index in 20 to 26, see `phy.md`,
and in 28 to 31 what moves with the AGC's steps.
Which of the two is I was told by a signal of known frequency, below;
ESP-SDR's description names them both ways.

What the board answered, the driver listening on Wi-Fi's channel 1:

- A capture of 4096 samples from source 15 at the full clock is done in about 51 us, and writes every word
  and nothing past them; the AGC idle, the gain index stays 78, the samples sit well off zero,
  and their spread is a noise of about 20 counts.
- While the engine holds the bank, the CPU loses it: a read of the bank's last word, which no capture reaches,
  gave another word of the bank, and a write there was gone once the bank was the CPU's again.
  So a bank given to the engine must hold nothing a trap or another process may touch, see `dump.h`.
- Source 15 starts at once. Sources 0, 1, 2, 5 and 10 finish only after milliseconds,
  each capture with the gain index moving and the AGC's bits stepping, so they wait for something the receiver
  hears; 3, 4, 6 to 9 and 11 to 14 did not finish in 150 ms of a channel with only beacons and traffic on it.
  With `dumpsweep=1`, 4096 samples at 80 MS/s, five captures of each in 70 s, as a hypothesis from them:
  source 0 starts as the AGC takes its first step on a packet, the gain already down from 78 to about 56 at the first sample
  and its next step, to about 42, 336 to 340 samples on, in four of five, 262 at times in another run,
  where it started on the nRF52840's frames too, the gain at 72, in six of the ten captures of their seconds;
  nothing from before the first step is in it.
  Sources 1 and 10 start once the AGC has settled, the gain 35 to 43 throughout, in three of four each;
  2 starts a microsecond before the gain goes back to 78, in two of five;
  5's show nothing of the gain's, `adctrig` setting a byte of the Wi-Fi MAC's for it.
- The control: the nRF52840 sending IEEE 802.15.4 frames on channel 12, 2410 MHz, for 10 s of the run,
  and the C6 capturing source 15 at the clock divided by 5. The frames stood 30 dB over the floor
  2 MHz below the middle, in the captures of those 10 s and in none before or after,
  so the divider gives 16 MS/s, and with bits 0 to 9 taken as I they would have stood 2 MHz above.
  A line 4 MHz above the middle stands in most captures, 10 to 15 dB over the floor:
  2400 MHz, folded in, as the next shows.
- The divided clock keeps samples with no filter of its own: what lies past half the rate folds into the band,
  with only the analog filter before it, `phy.md`, "The receiver's gain and filter".
  Measured with `prog/scan.c` from 2390 to 2430 MHz in steps of 4 MHz, the nRF52840's frames on 2410 MHz,
  once at 80 MS/s and once at 16: at 16, the frames 12 MHz from the middle stood 4 MHz from it on the other side,
  7 dB below those within the band, where at 80 the analog filter took 10 dB off them,
  and in a run of steps of 10 MHz those 10 MHz off stood 6 MHz off on the other side, at the level of those in the middle.
  So below the full clock a capture holds what is a rate away from the middle as if it were there,
  and the scan takes its captures at 80 MS/s.

The BLE side, the driver listening for IEEE 802.15.4 on channel 12, as the root task's sampler of then had it:

- With `0x600A20B4`'s bit 0 set, lanes 14 to 17 and source 0, the engine waits and never finishes,
  though the 802.15.4 MAC heard hundreds of the nRF52840's frames meanwhile.
  With `adctrig`'s eighth mode's start written too, `0x600A210C`'s bits 28 to 31 set to 4,
  its bits 8 to 11 and 14 and 15 cleared, and then its bit 7 set, each capture finishes in the same 51 us
  as Wi-Fi's at the full clock, and the 802.15.4 MAC goes on hearing frames beside it.
  The BLE receiver's own start, `ble_rx_start`, is left out, since the 802.15.4 MAC's receiving is on.
- Its words hold the same two 10-bit samples, but a gain code of the BLE side's own, 151 idle and 130 under a frame,
  and 4 in bits 28 to 31 while idle.
- Taken at 80 MS/s, nearly all of a capture's power lies within 2.5 MHz of the middle and the rest is flat:
  the side gives what its receiver's filters have passed, not the ADC's own.
  The nRF52840's frames, on the very channel the C6 listens on, stand 2 MHz below the middle,
  so the receiver, it seems, hears on an intermediate frequency of 2 MHz, which the capture is taken before;
  a line about 1 MHz below the middle stands 45 dB over the floor while the channel is quiet, whence not known.

The diagnostic bus: `IO_MUX_MODEM_DIAG_EN`, `0x600900BC`, lets the modem's 32 `MODEM_DIAG` bits into the GPIO matrix,
and pins nothing on the XIAO drives, each an output with its input on, read them back in `GPIO_IN` with no wire;
commit f63a3ab7 had the sampler do so, and the next took it out again, for what it found:
listening on channel 1 the bus reads `0x01221111`, its bits 16 to 22 moving now and then as the receiver hears something,
the receiver's slow state, not its samples; the dump engine running changes nothing there,
and no value of `WIFI_BB_CFG`'s four-bit fields from bit 12, nor of the dump block's word `0x600A9020`,
moved it more than its default does. What chooses the bus's signals is not found.
`0x600A9020` was a poor guess: the register map, see `README.md`, reads it as the block's date word, `0x02202221`,
the last of its sixteen words, which repeat every 64 bytes from `0x600A9000` to `0x600A9400`.

## The ring

`0x600A9004` with bit 17 set as well, and 16384 in the samples' field, has the engine write round and round
the bank's first 16384 words, 64 KiB, until the word is cleared, as ESP-SDR found;
`dump_ring_start` and `dump_ring_stop` in `dump.c` drive it, and `prog/ring.c` stops it as the Wi-Fi MAC ends a frame, its `RX_LAST` moving.
Measured with `prog/ring.c` on channel 1:

- The engine wrote all 16384 words and none past them, the rest of the bank marked; it does not stop by itself.
- `0x600A9008` read while it runs holds the word written next in bits 0 to 13: at 16 MS/s it moved 1603 in 100 us
  and read 1789 at a stop with 1790 written. Its bit 14 is set once the ring has come round, at every stop after that,
  and stays set. It reads 0 once the engine stops, so the ring's newest word is the one before the index read last;
  its bits 15 and 16 are the source's.
- At 16 MS/s, a ring of 1 ms, stopped as the MAC ends a frame, the program polling every 0.4 us:
  the AGC let the gain go, to 60 and then to 78, 36.1 to 36.5 us before the stop in 41 of 48 rings, and at most 42.4,
  so the MAC names a frame about 36 us after the AGC lets go.
- A thread that polls may lose the core. In two runs it ran out of its time about once every 0.8 ms of polling,
  `OP_THREAD_OVERRUNS` read around it, and was away for up to 0.7 ms, as the ring's index read at every poll shows,
  and stops came up to 213 us after the frame's end; in two runs an hour later, the same build in one of them,
  it ran out once a ring and was away at most 29 us. What wanted the core in the first is not known.

Open questions of the sampler, beside `TODO.md`'s:

- What source 5 waits for, and whether 1, 2 and 10 start on the AGC's steps as their few captures suggest.
- The BLE side's rate: its captures take as long as Wi-Fi's at the full clock, yet hold a band of 5 MHz;
  whether its words are the BLE receiver's own samples held or made again at 80 MHz is not known,
  nor whether `0x600A210C`'s fields other than bit 7 matter.
- The line 1 MHz from the middle on the BLE side.

## What ESP-SDR found that is not tried here

ESP-SDR's description and firmware, GPL-3.0, name these for the ESP32-C6; they are facts to try, not yet the board's.
Its tuning, its gain and its filter are `phy.md`'s, "Off the channels' grid" and "The receiver's gain and filter".

- The bank, in `SRAM_USAGE`'s bits 9 to 11 for banks 1 to 3;
  moving the engine from one bank to another while it runs splits the stream into pieces with nothing lost between them,
  so that the CPU reads one bank while the engine fills the other: a stream that does not stop wants two banks,
  where the radio's programs have one.
- The clocks it forces on: `0x600A9804` written all ones, `0x600A9814` 0x7FFFF and `0x600A980C` all ones.
