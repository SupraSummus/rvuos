# The ESP32-C6's radio: conventions

These rules add to the repository's `CLAUDE.md` and to the Wi-Fi station's, `user/wifi/esp32c6/CLAUDE.md`,
whose checks a change to the radio's shared files runs, as the station links them all.

## Before committing

A change to the radio's shared files with an ESP32-C6 connected runs, besides the station's checks,
each program of `prog/` that calls what changed, with `make BOARD=esp32c6 radio PROG=name RADIO_CONFIG=file`;
a change to the root task, `root.c`, or to the page and the header of `drv.h`, runs every program.

A new program is a file of `prog/` that names itself and what it needs with `DRV_PROGRAM`,
and defines `drv_run` and `drv_radio_off`, see `osi.h`; its configuration's keys are its own, read from the page,
and its run ends when the root task asks it to leave.

### The sampler

A change to `dump.c`, `dump.h` or `prog/sampler.c` runs the sampler with `channel=1`,
with the nRF52840's frames as the signal of known frequency, as `hw/dump.md` says,
and `user/radio/esp32c6/tools/dump-spectrum.py` must find them in the captures of their seconds, at the offset of their channel,
and in none before them.

A change to the ring's functions in `dump.c` or to `prog/ring.c` runs the ring with `channel=1`, `dumprate=3` and `dumphex=8192`:
most rings must stop on a frame, `RX_LAST` moving at the poll the stop was seen,
and `user/radio/esp32c6/tools/dump-spectrum.py LOG 16e6 --tail 10` must show the frame's end in its words,
the power falling six microseconds before the ring's newest word and the gain going to 60 half a microsecond before it,
as `hw/dump.md`, "The ring", says.

A change to `prog/etm.c`, or to `dump_arm` and `dump_disarm`, runs the survey with `channel=1` and `rounds=4`,
which must find what `hw/dump.md`, "What stops the engine", says: task 21 starting source 8 alone, timer0's event alone
stopping the ring, and sources 0, 1, 2 and 10 stopping it by themselves.

### The 802.15.4 programs

A change to `i154.c` runs `tools/thread-pair.sh`, as the repository's `CLAUDE.md` says,
and `prog/i154listen.c` on channel 12 with the nRF52840 sending there, as for the sampler,
which must hear its frames whole, and with `dump=`, `dumpbt=1`, `dumplane=14` and `dumpsource=0`
take a capture every second of the run and still answer the root task.

### The scan

A change to `drv_phy_tune` in `phy.c`, to `prog/scan.c` or to `user/radio/esp32c6/tools/dump-spectrum.py`'s `--scan` runs the scan
from 2405 to 2415 MHz with `divided=2` for 50 s and the nRF52840 sending on channel 12 for 20 s in the middle of the run,
as `hw/phy.md`, "Off the channels' grid", says,
and `user/radio/esp32c6/tools/dump-spectrum.py LOG 80e6 256 --scan 2410` must find the frames, each way, in several captures of its seconds
and in at most one before them, as 2410 MHz is BLE's channel 3 and within Wi-Fi's channel 1, whose senders the scan hears too.
A change to `drv_phy_rx_gain` or `drv_phy_rx_filter` runs it with `gain=` and `gainstep=`, and with `filter=` and `filterstep=`,
as `hw/phy.md`, "The receiver's gain and filter", says: every word's gain must be the one held,
and the noise's band must narrow as the filter's code rises.

### The loopback

A change to `drv_phy_loopback` or `drv_phy_tone` in `phy.c`, or to `prog/loopback.c`, runs the loopback with `channel=6`,
`dump=2048` and `dumphex=2048`, and `user/radio/esp32c6/tools/dump-spectrum.py LOG 80e6 2048` must find,
as `hw/phy.md`, "The loopback", says, every quiet loopback's words within a few counts,
and every tone's line at +5.00 MHz, 70 dB or more over the median.
A change to `rhythm=` or `half=`, or to the nRF52840's `watch=` in `user/thread/root.c` or `user/thread/tools/watch-spectrum.py`,
runs the loopback with `channel=13`, `step=77`, `rhythm=35` and `half=50` while the nRF52840 watches channel 25 with `watch=10`,
and `prog/i154.c` on channel 25 with `stream=7` under the same watch, its positive control:
`user/thread/tools/watch-spectrum.py` must find the tone's line at 10 Hz and the frames' at 7 Hz, each far over the rates beside it,
and neither at the other's rate.

### The player

A change to `prog/player.c` runs it with `est=1`, `count=4096`, `at=4096`, `w10s=0,10000000` and `raws=0,40,10000,200`,
and its log must show what `hw/dump.md`, "A player", says: with the block's 0x10 at 0 no word moves the receiver's mean,
the words it plays all 0, and with 0x10000000 the word 0 none, 40 and 10000 a quarter turn apart,
and 200 some eight times 40's the other way.
A change to the nRF52840's `sweep=` in `user/thread/root.c` runs the player with `rhythm=40`, `half=0`, `loop=1`, `period=4` and `amp=200`,
and again with `swap=1`, while the nRF52840 sweeps from 2417 to 2457 MHz with `sweep=10`, and once with `amp=0`, its control:
the means at 2427 and 2428 MHz must stand a few dB over the control's in the first, and at 2447 and 2448 in the second.

A change to `prog/synth154.c`, or to the nRF52840's `listen=` in `user/thread/root.c`, runs synth154 with `amps=0,511` and `send=40`
while the nRF52840 listens on channel 24 with `listen=15`: setting 1 must come whole most of the times it was played meanwhile,
setting 0, nothing played, never, as `hw/dump.md`, "A frame made up", says.
A change to `kind=1`, `gaps=`, `hear=` or `user/radio/esp32c6/tools/demod154.py` runs synth154 with `kind=1`, `amps=511`,
`dsts=21124,21124,4660` and `hear=4` while the nRF52840 exchanges on channel 24 with `run=20` and `survey=0`,
and `demod154.py LOG 16e6 -2e6` must read acknowledgements with sequence numbers 0 and 1 after the first two settings' pings,
their FCS right, and none after the third's, as `hw/dump.md`, "Frames longer than a play, and the answer heard", says.

### The register map

A change to `prog/regmap.c` or `user/radio/esp32c6/tools/regmap.py` runs the map with `channel=1`, and `user/radio/esp32c6/tools/regmap.py` must put its four passes together.

## The hardware's files

A fact of `hw/` says where it comes from and how sure it is, as `hw/README.md` says;
one a run measured names the program and its keys, and its numbers go in the commit, as the repository's `CLAUDE.md` says,
the file keeping what they showed.
