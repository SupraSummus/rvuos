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
| `0x600A4E30` | 8, 0 to 7 | set, and a byte, for source 5: a reception's end in that state, see "What stops the engine" | `adctrig`'s seventh mode; a word of the Wi-Fi MAC's |
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
the bank's first 16384 words, 64 KiB, as ESP-SDR found, until the word is cleared or the source's event comes;
`dump_ring_start` and `dump_ring_stop` in `dump.c` drive it,
and `prog/ring.c` keeps a ring that its source stopped on a frame the Wi-Fi MAC took.
Measured with `prog/ring.c` on channel 1:

- From source 15 the engine wrote all 16384 words and none past them, the rest of the bank marked; it does not stop by itself.
- `0x600A9008` read while it runs holds the word written next in bits 0 to 13: at 16 MS/s it moved 1603 in 100 us
  and read 1789 at a stop with 1790 written. Its bit 14 is set once the ring has come round, at every stop after that,
  and stays set. It reads 0 once the engine stops, so the ring's newest word is the one before the index read last;
  its bits 15 and 16 are the source's.
- From source 2 the engine stops by itself as the AGC lets a packet go, see "What stops the engine" below,
  and the Wi-Fi MAC's `RX_LAST` moves with it: in 24 rings that stopped on a frame the MAC took,
  the program, reading both at every poll, saw `RX_LAST` move at the very poll it saw the stop.
  In 23 of them the power fell between the seventh and the sixth microsecond before the ring's newest word,
  by 20 dB and more, or by 6 to 9 dB in three that lay in a stronger signal,
  and the gain went from about 40 to 60 half a microsecond before it;
  in the other, a frame of 33 us, the power fell 4 us before.
  So the ring holds the frame up to its end, whatever the program's thread did meanwhile.
  Six microseconds are 802.11's signal extension after an OFDM frame on 2.4 GHz; whether that is why is not known.
- Between the frames, source 2 stops the ring on packets the MAC does not take, which the program lets go,
  starting the ring over, `dump_ring_restart`; with no frame within 100 ms it gives up, as in 5 of 29 seconds on channel 1.

## What stops the engine

A source a capture waits on to start stops the ring instead, on the same event.
`prog/etm.c`, `survey=3` and `rounds=4`, on channel 1: the ring from sources 0, 1, 2 and 10 stopped by itself within 100 ms
in each of four tries, from 5 in three in one run and in four in another, and from 3, 4, 6, 7, 9 and 11 to 15 in none.
`prog/ring.c` with `any=1`, which keeps every stop, `dumphex=8192` and each of them as `dumpsource=`, 17 to 19 rings each,
told what each stops on, by the AGC's bits 28 to 31 at the ring's end:

- 0: the bits turned 8, in three rings 7, a tenth of a microsecond before the newest word, in all 17:
  the AGC starting on a packet, so the ring holds what came before it.
- 1 and 10: they turned b 1.8 or 1.9 us before it, in 16 of 19 and in 14 of 19; whether the two ever differ is not known.
- 2: the gain going to 60, 0.4 us before it, at a packet's end; see "The ring" above.
- 5, `0x600A4E30` clear: they turned 5 and then 2, 0.4 to 0.7 us before it, in 18 of 19, at a packet's end as 2 does,
  but not at a frame the MAC took: with `altsource=2`, all 19 rings from 5 kept had `RX_LAST` move 1070 to 50472 polls
  before the stop, a frame going by without stopping it, where 15 from 2 stopped at their frames.

Source 5 takes the Wi-Fi MAC's word `0x600A4E30`, `dumpmac=` of `dump.h`, which `adctrig`'s seventh mode writes:
bit 8 set, the byte below it picks a reception's end by its state, the MAC's descriptor's `RX_CTRL_STATE` of `esp.h`.
`prog/etm.c`, `survey=4` and `rounds=3`: the ring from 5 stopped within 100 ms only with bytes 0x00 in 3 of 3 tries,
0x41 in 3, 0x51 in 2, 0x52 in 2, 0x53 in 3 and 0x54 in 1, and with the word clear in 43 of 48.
`prog/ring.c`, `dumpsource=5`, `dumpmac=321` and `altmac=256`, `broken=1` having the MAC list the frames it did not receive whole:
with 0x100, 26 of 29 stops came at the poll `RX_LAST` named a frame in state 0, the others at a packet the MAC listed nothing of,
perhaps an acknowledgement, which the sniffer does not pass; with 0x141, 9 of the 21 stops that named a frame named one in state 0x41,
the state `mac.c` names broken frames by, and the other twelve's two bytes no state of the MAC's,
read, though, as the MAC's thread may be handing the descriptor back.
What states 0x51 to 0x54 are is not known; the MAC listed no frame in them.

Source 8 waits for the modem's ETM, at `0x600A8800`, which ESP-IDF's `ieee802154_reg.h` names
for the 802.15.4 MAC's timed sending and receiving.
A channel takes an event's id in its first word, from `0x600A8818`, and a task's in its second, eight bytes a channel;
`0x600A8804`'s bit n enables channel n and `0x600A8808`'s disables it, and an id keeps 7 bits of what is written.
ESP-IDF names events 58 and 59, the 802.15.4 MAC's timer1 and timer0 reaching their thresholds,
and tasks 64, 65 and 68, its sending on energy detection, its receiving and its sending.
Measured with `prog/etm.c`, the 802.15.4 MAC's clocks on beside Wi-Fi's receiver on channel 1:

- `survey=1`, `rounds=4`, timer0's event routed to each task in turn: task 21 started a capture armed from source 8 in each round,
  and no other task of 1 to 127 started any source that never finished by itself, 64 and 68 left out;
  source 15, triggered by bit 19, the control, wrote in every probe.
- `survey=2`: task 21 stops the ring from source 8 as its event comes, and of the events 0 to 127 only timer0's did,
  50 ms in, as timer0 fired, with 427 frames going by in the 128 events' windows.
- `i154=1`, `first=0` and `first=64`, the 802.15.4 listener on channel 12 with the nRF52840 sending there through half the run:
  only event 63 stopped the ring, in both seconds it was routed, with the nRF52840's frames and without,
  of which the listener heard about 2700 a run; what event 63 is is not known.

So no frame of either MAC reaches the ETM as an event:
the ETM can start or stop the engine at a microsecond the 802.15.4 MAC's timer0 sets, and at a packet only the AGC's own sources can.

Open questions of the sampler, beside `TODO.md`'s:

- What the AGC's bits 28 to 31 are, on whose steps sources 0, 1, 2 and 10 stop; whether 1 and 10 differ;
  the reception's states 0x51 to 0x54; what the ETM's event 63 is.
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
