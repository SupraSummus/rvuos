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
A change to `user/wifi/esp32c6/mac.c` runs it again with `listen=` and `rx=own` in `WIFI_CONFIG`,
which fails if the driver's own receiving hears nothing, again with `rx=own` alone, the driver's own scan,
and again with `listen=`, `probe=` and `tx=own`, without `rx=own`, the driver's own sending,
which fails if the access point answers none; and with `listen=`, `probe=`, `rx=own` and `tx=own` together,
which fails if a probe is not sent or not answered, the driver's own completion in place of the libraries'.
A change to the station's own code in `user/wifi/esp32c6/main.c`, `sta.c` or `mgmt.c` runs it with a network named and `sta=own`,
the driver's own scan and authentication, which fails if the access point answers none.
That run first runs `make sae-test`, WPA3's SAE on the host over the driver's `ec.c` and Mbed TLS,
and `make mgmt-test`, the station's management frames of `mgmt.c` over hostap's parser of elements,
which `make check` leaves out, as it fetches nothing;
a change to `ec.c`, `mgmt.c` or `user/wifi/esp32c6/mbedtls/` runs them even with no ESP32-C6 connected.
The log comes as the run goes, each line timed, and stays in `build/esp32c6/wifi-run.log`;
the configuration's `debug=` and `lib=`, see `user/wifi/esp32c6/root.c`, have the driver tell more,
and a fault of one of its threads comes with its stack and the names of its functions.
`make check` builds no PHY harness either, so a change to `user/phyblob/`, `user/phytrace.h` or the ESP32-C6's device ranges
runs `make BOARD=esp32c6 phymap`, which builds the harness and lists what it reaches that no frame covers;
`make BOARD=esp32c6 wifi-esp32c6-map` lists the same for the Wi-Fi driver,
which a change to its device ranges or `phy.c` runs too.
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
