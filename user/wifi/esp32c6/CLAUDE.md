# The ESP32-C6's Wi-Fi driver: conventions

These rules add to the repository's `CLAUDE.md`, which names the changes they apply to.

## Before committing

A change to `user/wifi/esp32c6/` or `kernel/board/esp32c6/` with an ESP32-C6 connected runs `make BOARD=esp32c6 wifi-esp32c6`,
which writes the driver into the flash at `0x210000` if it changed and scans,
or, with `WIFI_CONFIG` naming a network as the repository's `CLAUDE.md` says for the Pico 2 W, joins it,
and `tools/wifi-run.py` checks it from the host as it checks the Pico 2 W's, but for the logger, before it ends the run.
The log comes as the run goes, each line timed, and stays in `build/esp32c6/wifi-run.log`;
the configuration's `debug=`, see `user/wifi/esp32c6/root.c`, has the driver tell more,
and a fault of one of its threads comes with its stack and the names of its functions.

A change to `user/wifi/esp32c6/mac.c` or `macstart.c` runs it again with `listen=` in `WIFI_CONFIG`,
which fails if the driver hears nothing, again with `listen=` and `probe=`,
which fails if a probe is not sent or not answered, and again with neither, the driver's scan.

A change to how the C6 sends, `mac.c`'s sending or `txctl.c`, with a Pico 2 W connected too,
runs `make BOARD=esp32c6 wifi-esp32c6-share` with `SHARE_BEFORE` naming the driver image from before it:
the Pico must keep as much beside the new C6 as beside the old, within its rounds' spread,
or the new takes more of the medium.

### The trace

A change to `user/tracer/` or to the trace in `user/wifi/esp32c6/` runs it with `trace=1` in `WIFI_CONFIG`,
which has the root task carry out and log every device access of the driver's bring-up,
and again with `trace=2`, the same window with the driver mapping its frames and nothing faulting;
the two runs must agree on the driver's lines, its events and its code, and the stream comes as `build/esp32c6/wifi-run.log`.

The driver's lines beginning `trace:` are the tracer's own instrumentation,
present only with `trace=1` and left out of that agreement:
the driver's own phase markers,
and, from `osi.c`'s `osi_trace`, the calls the driver's own start makes into the adapter (clocks, phy, interrupts, events),
each `trace: req <n> t<thread> <name> <a0> <a1>`,
so the window's device accesses and adapter calls read as one sequence.

The final state is held to the dry run's too: `tools/mac-trace.py snapshot` makes the window's `snap=` lines from a traced log,
and `compare`, with dry logs and the traced one, fails on any divergence outside the words the dry runs differ in,
the bits whose value is the moment's -- a count over the window, a measurement, a status the hardware keeps -- read apart by `compare`, `mac-trace.py`'s `MOMENT` naming each with its mask and reason.

`tests/mac-trace-test.py`, a part of `make check`, holds what the tool lets pass and what it stops;
a change to what it compares changes that test with it.

`make mac-replay-test`, a part of `make check` too, runs the own start's register sequences of `macstart.c` on the host,
each read answered and each write held by the libraries' accesses `user/wifi/esp32c6/test/replay/` holds,
taken once from a run of the libraries' start; net80211 and pp are linked no more, so the files are frozen, and the make
target that once took them, `wifi-esp32c6-replay`, is in git history.
It runs `btbb.c`'s baseband bring-up there too, against `bb.txt` and `bb-other.txt`,
the whole window of `libbtbb.a`'s `bt_bb_v2_init_cmplx` from the two states the modem was left in,
since its registers outlive a reset, taken once from `trace=1 i154=` logs of an image whose baseband was still the
library's; the library is linked no more, so the two files are frozen.

`make BOARD=esp32c6 wifi-esp32c6-attrib` attributes every access of a traced log to its library and function,
and `WIFI_TRACE_SEGMENTS=1` splits that log into the segments between the `trace: ` lines' requests.

### The own start

A change to the own start, `macstart.c` or `mac.c`'s bring-up, runs the gate, `make BOARD=esp32c6 wifi-esp32c6-gate WIFI_GATE_JOINS="..."`.
It makes a `trace=1` run and four `trace=2` dry runs of the start, compares the traced run's final state against them -- four, not two, since a word that varies run to run can agree between two by chance and show as a difference that is not one -- and runs the ordinary runs and `make check`, each verdict told.
A change meant to leave the start's device work as the gate held it -- a refactor, a write that does nothing dropped -- needs one `trace=1` run and `make BOARD=esp32c6 wifi-esp32c6-seq`, which holds its accesses to the MAC to the gated run's, `build/esp32c6/gate/own.log`, value by value;
a change that leaves the image's sha256 as it was needs neither.
A step whose `mac_config` leaves one of the libraries' accesses out adds it to `replay-expected.txt`, which `mac-replay-test` steps over.
A gate that fails is read from its logs, its counts held to a baseline's, before it runs again.

### The station

A change to the station in `user/wifi/esp32c6/main.c`, `sta.c` or `mgmt.c` runs it with a network named,
the station's scan, SAE, association and 4-way handshake at an access point that offers WPA3, which fails if one is not answered,
and then serves the link the network's frames cross,
and again with `sae=0`, its open-system authentication and WPA2's handshake;
the Pico 2 W's access point takes the second only when its own configuration says `sae=0`, see `TODO.md`.

That run first runs `make sae-test`, WPA3's SAE on the host over the driver's `ec.c` and Mbed TLS,
`make mgmt-test`, the station's management frames of `mgmt.c` over hostap's parser of elements,
and `make ccmp-test`, its CCMP of `ccmp.c` over hostap's CCM,
which `make check` leaves out, as it fetches nothing;
a change to `ec.c`, `mgmt.c`, `ccmp.c` or `user/wifi/esp32c6/mbedtls/` runs them even with no ESP32-C6 connected.
`make keys-test`, the pairwise key's transitions of `keys.c`, fetches nothing and is a part of `make check`.

### Calls into Espressif's libraries

`make BOARD=esp32c6 wifi-esp32c6-refs` counts what the driver's own code calls of Espressif's `libphy.a`, the closed
library it still links; `net80211` and `pp` are gone, and a change that drops such a call gives the count before and
after in its commit.
