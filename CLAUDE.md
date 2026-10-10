# rvuos: repository conventions

rvuos is a capability-based microkernel for RISC-V microcontrollers
without an MMU, and for ARMv7-M and ARMv8-M ones with an MPU; see "Architectures" in `DESIGN.md`.
Read `DESIGN.md` before changing anything in the kernel,
and the chapters in `design/` the change touches.
No new walk over every object, pool or table
goes on the path of a system call, a tick or an interrupt;
see goal 4 in `DESIGN.md`.
Every loop a trap can run says what bounds it with an annotation from `kernel/work.h`,
and every link checks it, see "Bounded work" in `DESIGN.md`.
The kernel does not recurse and has no frame whose size is known only at run time;
every link checks its stack, see "Bounded stack" in `DESIGN.md`.

## Language

Everything in this repository is written in English:
code, comments, docstrings, documentation, commit messages.
Conversation with the maintainer may happen in Polish,
but nothing Polish lands in the repo.

## Prose style

All prose uses semantic line breaks.
Each sentence starts on a new line.
Long sentences break at clause boundaries,
so that a diff touches only the clause that changed.
This applies to Markdown files, docstrings, and long comments.
Do not reflow paragraphs to a fixed column width.

## Before committing

Run `make check`.
It boots QEMU, replays the fuzz corpus on the host build,
and compares host and QEMU transcripts.
It runs its parts side by side, a processor each, and `make CHECK_JOBS=1 check` one after another;
QEMU's `-icount` keeps what they print the same however many run at once.
It also holds what a system call and a switch cost, `make bench`, to the records in `tests/bench/` within a tenth;
a change that moves a cost further, and means to, runs `make bench-refresh` with `BOARD=qemu`, `mps2-an385` and `mps2-an521`,
and its commit says what it cost.
It runs the laboratory, `make lab`, to its end, and holds its numbers to nothing;
a change to how threads take turns, wait or wake compares them before and after, and its commit says what moved.
When a change touches `kernel/board/esp32c6/`, `user/board/esp32c6/`,
or anything the demo or the escape suite exercises, and an ESP32-C6 is connected,
run `make BOARD=esp32c6 test escape` as well;
it loads the demo into the chip's RAM and checks the same transcript,
then boots each escape scenario, the only check of the board's install rule.
The same goes for `kernel/board/rp2350/` and `user/board/rp2350/`
with an RP2350 connected in BOOTSEL mode, `make BOARD=rp2350 test escape`,
the only check of its PMP quirks,
and `make BOARD=rp2350 ARCH=arm test escape`, the only run of PMSAv8 and of the Cortex-M33 on silicon,
which a change to `kernel/arch/arm/` needs too.
Both again with `CORES=2` are the only runs of two cores on silicon,
which a change to how the cores lock, start or interrupt each other needs too.
The same goes for `kernel/board/nrf52840/` and `user/board/nrf52840/`
with an nRF52840 on an ST-Link connected, `make BOARD=nrf52840 test escape`,
the only run of ARMv7-M on silicon, which a change to `kernel/arch/arm/` needs too.
`make check` runs the test of `user/lib/` under QEMU alone,
so a change to the library with a board connected runs `make BOARD=<board> lib-test` there too, on each kind of core,
and on the RP2350 with `CORES=2` as well, where a snapshot of `lib/seqlock.h` is written on one core while read on the other.
Of the Wi-Fi system it runs only what makes no system call, the IP stack and the clients' sockets, on the host;
a change to `user/wifi/` with a Pico 2 W connected runs `make BOARD=rp2350 wifi` too,
and `WIFI_CONFIG` names a network to join from a file git does not track,
`local/wifi.conf` on the maintainer's machine, never from one it does.
A change to `user/wifi/esp32c6/` or `kernel/board/esp32c6/` with an ESP32-C6 connected runs `make BOARD=esp32c6 wifi-esp32c6`,
which writes the driver into the flash at `0x210000` if it changed and scans,
or, with `WIFI_CONFIG` naming a network as for the Pico 2 W, joins it,
and `tools/wifi-run.py` checks it from the host as it checks the Pico 2 W's, but for the logger, before it ends the run.
A change to `user/wifi/esp32c6/mac.c` or `macstart.c` runs it again with `listen=` in `WIFI_CONFIG`,
which fails if the driver hears nothing, again with `listen=` and `probe=`,
which fails if a probe is not sent or not answered, and again with neither, the driver's scan.
A change to how the C6 sends, `mac.c`'s sending or `txctl.c`, with a Pico 2 W connected too,
runs `make BOARD=esp32c6 wifi-esp32c6-share` with `SHARE_BEFORE` naming the driver image from before it:
the Pico must keep as much beside the new C6 as beside the old, within its rounds' spread,
or the new takes more of the medium.
A change to `user/tracer/` or to the trace in `user/wifi/esp32c6/` runs it with `trace=1` in `WIFI_CONFIG`,
which has the root task carry out and log every device access of the libraries' bring-up,
and again with `trace=2`, the same window with the driver mapping its frames and nothing faulting;
the two runs must agree on the driver's lines, its events and its code, and the stream comes as `build/esp32c6/wifi-run.log`.
The driver's lines beginning `trace:` are the tracer's own instrumentation,
present only with `trace=1` and left out of that agreement:
the driver's own phase markers,
and, from `osi.c`'s `osi_trace`, the calls the libraries make into the adapter (clocks, phy, interrupts, events),
each `trace: req <n> t<thread> <name> <a0> <a1>`,
so the window's device accesses and adapter calls read as one sequence.
The final state is held to the dry run's too: `tools/mac-trace.py snapshot` makes the window's `snap=` lines from a traced log,
and `compare`, with dry logs and the traced one, fails on any divergence outside the words the dry runs differ in,
the bits whose value is the moment's -- a count over the window, a measurement, a status the hardware keeps -- read apart in `compare` and `diff` alike, `mac-trace.py`'s `MOMENT` naming each with its mask and reason.
`tests/mac-trace-test.py`, a part of `make check`, holds what the tool lets pass and what it stops;
a change to what it compares changes that test with it.
`make mac-replay-test`, a part of `make check` too, runs the own start's register sequences of `macstart.c` on the host,
each read answered and each write held by the libraries' accesses `test/replay/` holds;
a step that takes a group over adds it there, and `make BOARD=esp32c6 wifi-esp32c6-replay` takes the file again
from a `trace=1 libstart=1` log of the build at hand, so that a wrong mask shows before the board does.
`make BOARD=esp32c6 wifi-esp32c6-attrib` attributes every access of a traced log to its library and function,
and `WIFI_TRACE_SEGMENTS=1` splits that log into the segments between the `trace: ` lines' requests.
A step of the own start goes through four checks:
`compare` above, against the step's own dry runs;
`make BOARD=esp32c6 wifi-esp32c6-diff WIFI_TRACE_BASE=... WIFI_TRACE_OWN=...`, the own start's access stream against the libraries' start's, thread by thread, WIFI_TRACE_BASE naming four or more of the libraries' start's logs, each one the build at hand flashed,
and the places the own start differs in by design passing alone and exactly, each with its reason in `user/wifi/esp32c6/test/diff-expected.txt`;
`tools/mac-trace.py compare LIB... OWN`, the final state across images, the dry logs four or more trace=2 `libstart=1` runs made interleaved with the driver's, since what the air carries changes over time, and the traced log the driver's own start's trace=2;
and the ordinary runs below.
`make BOARD=esp32c6 wifi-esp32c6-gate WIFI_GATE_JOINS="..."` runs the four in that order, with `make check`, and tells each verdict.
A step that makes a new difference by design adds its place to `diff-expected.txt` with the reason, in the same commit,
and a word whose final state it changes to `compare-expected.txt` beside it, which the compare across images reads,
and the libraries' accesses it leaves out of `mac_config` to `replay-expected.txt`, which `mac-replay-test` steps over.
The diff's bases and each `compare`'s dry logs are four, not two: a word that varies run to run can agree between two by chance and show as a difference that is not one.
The gate is a step's, at its end, not each change's:
a change after it that is to leave the start's device work as the gate held it -- a refactor, a call that writes the libraries' memory alone dropped --
needs one `trace=1` run and `make BOARD=esp32c6 wifi-esp32c6-seq`, which holds its accesses to the MAC to the gated run's, `build/esp32c6/gate/own.log`, value by value,
and the ordinary runs the change touches; a change that leaves the image's sha256 as it was needs none.
A gate that fails is read from its logs, its counts held to a baseline's, before it runs again.
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
The log comes as the run goes, each line timed, and stays in `build/esp32c6/wifi-run.log`;
the configuration's `debug=` and `lib=`, see `user/wifi/esp32c6/root.c`, have the driver tell more,
and a fault of one of its threads comes with its stack and the names of its functions.
`make check` builds no PHY harness either, so a change to `user/phyblob/`, `user/phytrace.h` or the ESP32-C6's device ranges
runs `make BOARD=esp32c6 phymap`, which builds the harness and lists what it reaches that no frame covers;
`make BOARD=esp32c6 wifi-esp32c6-map` lists the same for the Wi-Fi driver,
which a change to its device ranges or `phy.c` runs too.
`make BOARD=esp32c6 wifi-esp32c6-refs` counts what the driver's own code calls of Espressif's `net80211` and `pp`,
and a change that drops such a call gives the counts before and after in its commit.
A new kernel global needs a reset in `host_boot` too,
since the host boots once per input in one process.
A new `BOOT_CAP_*` or `REPLAY_CAP_*` slot, a device QEMU's board lists, or an operation whose arguments change,
changes what the seeds and the corpus mean,
so they have to be rewritten with it; see `TODO.md`.
When the kernel's object model changes,
update `kernel/selfcheck.c` with it;
it is the executable form of the properties in `DESIGN.md`.
An object bound to another, at creation or by a call, needs a line in `host/history.c` too.
When a change adds or moves an invariant,
run `make mutants` as well
and plant a mutant for the new one as a patch under `tests/mutants/`,
with the header lines the run prints for it.
When a change edits code a mutant touches or stands next to,
run `make mutants-refresh`, then `make mutants` if it moved any,
and fix by hand only the patches it calls stale.
The full run takes minutes, so while working run only the mutants a change touches,
`tests/mutants.sh -j 4 name...`;
a change to the seeds, the corpus or the checks moves every mutant's reports,
so it needs one full run before it is committed.
A change to `host/mutator.c` or to the flags `make fuzz` runs with
is measured with `make mutants-fuzz`, from the corpus and from nothing, before and after,
and the commit says what it found.
`make fuzz` follows the default machine's coverage alone,
so a change to what a second core does runs `make fuzz FUZZ_HARNESS=fuzz-smp2` for a few minutes too.

## Environment

In a cloud session, `.claude/hooks/session-start.sh` installs what `make check` needs.
If a tool is still missing, run the hook, or install the tool and add it to the hook;
never skip the check.
A check that could not run is reported as not run, never as passed.
A board's run gives its loader seconds, as `tests/escape.sh` gives each boot 10,
so a host busy with `make check` fails it with "kernel did not boot": run a board's checks with nothing else building.
To see what the harts did under QEMU, `-d int -D file` logs every trap and leaves icount's timing as it was;
lldb, unlike the session's gdb, debugs the kernel through QEMU's `-gdb`, though a stop may change the harts' turns.

## Documentation of decisions

Architectural decisions live in `DESIGN.md` and its chapters in `design/`.
When a decision changes, edit the design document in the same commit
as the code that implements the change.
Open questions are listed explicitly in the design document
rather than left implicit in the code.
Work items go to `TODO.md`,
and leave it in the commit that finishes them;
it holds no record of what was done.
Numbers a run measured, the lab's, the bench's or a board's, go in the commit message,
which keeps them with the change they belong to;
a file says what they showed, which stays true longer.
The records of `tests/bench/` are the exception, since `make bench` holds costs to them.

`MANUAL.md` and its chapters in `manual/` are the user-facing description of the kernel:
what it offers a program, the system call reference,
and what the root task starts with.
When `include/rvuos/abi.h` or a limit a program can see changes,
update the manual in the same commit.
