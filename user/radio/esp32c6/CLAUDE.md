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
and `tools/dump-spectrum.py` must find them in the captures of their seconds, at the offset of their channel,
and in none before them.

### The 802.15.4 programs

A change to `i154.c` runs `tools/thread-pair.sh`, as the repository's `CLAUDE.md` says,
and `prog/i154listen.c` on channel 12 with the nRF52840 sending there, as for the sampler,
which must hear its frames whole, and with `dump=`, `dumpbt=1`, `dumplane=14` and `dumpsource=0`
take a capture every second of the run and still answer the root task.

### The scan

A change to `drv_phy_tune` in `phy.c`, to `prog/scan.c` or to `tools/dump-spectrum.py`'s `--scan` runs the scan
from 2395 to 2425 MHz with `divided=2` and the nRF52840 sending on channel 12 in the middle of the run, as `hw/phy.md`, "Off the channels' grid", says,
and `tools/dump-spectrum.py LOG 80e6 256 --scan 2410` must find the frames, each way, in several captures of its seconds
and in at most one before them, as 2410 MHz is BLE's channel 3 and within Wi-Fi's channel 1, whose senders the scan hears too.
A change to `drv_phy_rx_gain` or `drv_phy_rx_filter` runs it with `gain=` and `gainstep=`, and with `filter=` and `filterstep=`,
as `hw/phy.md`, "The receiver's gain and filter", says: every word's gain must be the one held,
and the noise's band must narrow as the filter's code rises.

### The register map

A change to `prog/regmap.c` or `tools/regmap.py` runs the map with `channel=1`, and `tools/regmap.py` must put its four passes together.

## The hardware's files

A fact of `hw/` says where it comes from and how sure it is, as `hw/README.md` says;
one a run measured names the program and its keys, and its numbers go in the commit, as the repository's `CLAUDE.md` says,
the file keeping what they showed.
