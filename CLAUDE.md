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
so a change to the library with a board connected runs `make BOARD=<board> lib-test` there too, on each kind of core.
Of the Wi-Fi system it runs only what makes no system call, the IP stack and the clients' sockets, on the host;
a change to `user/wifi/` with a Pico 2 W connected runs `make BOARD=rp2350 wifi` too,
and `WIFI_CONFIG` names a network to join from a file git does not track,
`local/wifi.conf` on the maintainer's machine, never from one it does.
`make check` builds no PHY harness either, so a change to `user/phyblob/`, `user/phytrace.h` or the ESP32-C6's device ranges
runs `make BOARD=esp32c6 phymap`, which builds the harness and lists what it reaches that no frame covers.
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

`MANUAL.md` and its chapters in `manual/` are the user-facing description of the kernel:
what it offers a program, the system call reference,
and what the root task starts with.
When `include/rvuos/abi.h` or a limit a program can see changes,
update the manual in the same commit.
