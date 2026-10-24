> There are two ways of constructing a software design:
> One way is to make it so simple that there are obviously no deficiencies
> and the other way is to make it so complicated that there are no obvious deficiencies.
>
> — C. A. R. Hoare, *The Emperor's Old Clothes*, the 1980 ACM Turing Award Lecture

# rvuos: repository conventions

rvuos is a capability-based microkernel for RISC-V microcontrollers
without an MMU, and for ARMv7-M and ARMv8-M ones with an MPU; see "Architectures" in `DESIGN.md`.
Read `DESIGN.md` before changing anything in the kernel,
and the chapters in `design/` the change touches.
No new walk over every object, pool or table
goes on the path of a system call, the timer or an interrupt;
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
A change meant to make a trap cheaper compares `make trap-profile` before and after, with `TRAP_SAVE` and `TRAP_AGAINST`,
and its commit says which classes moved.

It runs the laboratory, `make lab`, to its end, and holds its numbers to nothing;
a change to how threads take turns, wait or wake compares them before and after, and its commit says what moved.

### On a board

`make check` runs on QEMU and the host alone.
With a board connected, a change to what its row names runs the row's command too,
and a change to anything the demo or the escape suite exercises runs the board's `test escape`.

| Board | A change to | Runs | The only check of |
|---|---|---|---|
| ESP32-C6 | `kernel/board/esp32c6/`, `user/board/esp32c6/`, `kernel/board/esp32/`, `user/board/esp32/` | `make BOARD=esp32c6 test escape` | the board's install rule |
| ESP32-C5 | `kernel/board/esp32c5/`, `user/board/esp32c5/`, the same `esp32/`, `kernel/arch/riscv/clic.h` | `make BOARD=esp32c5 escape`, the demo too once open decision 41 is decided | the CLIC and the TEE controllers |
| RP2350 in BOOTSEL mode | `kernel/board/rp2350/`, `user/board/rp2350/` | `make BOARD=rp2350 test escape` | its PMP quirks |
| RP2350 in BOOTSEL mode | the same, `kernel/arch/arm/` | `make BOARD=rp2350 ARCH=arm test escape` | PMSAv8 and the Cortex-M33 on silicon |
| RP2350 in BOOTSEL mode | the same, how the cores lock, start or interrupt each other | both again with `CORES=2` | two cores on silicon |
| nRF52840 on an ST-Link | `kernel/board/nrf52840/`, `user/board/nrf52840/`, `kernel/arch/arm/` | `make BOARD=nrf52840 test escape` | ARMv7-M on silicon |

A change to `kernel/route.c` or a board's router runs `make BOARD=nrf52840 thread` as well, whose start checks a route.

A change to `user/thread/` or to the ESP32-C6's `user/radio/esp32c6/i154.c`, with both boards connected,
runs `tools/thread-pair.sh`, IEEE 802.15.4's frames between them, which fails if either hears nothing of the other
or nothing of either is acknowledged, and a change to how either sends runs it again with `STREAM=`,
as only a stream has the two contend for the channel;
a traced Wi-Fi run of the C6 after it waits for a plain Wi-Fi run first, see the driver's `NOTES.md`.

`make check` runs the test of `user/lib/` under QEMU alone,
so a change to the library with a board connected runs `make BOARD=<board> lib-test` there too, on each kind of core,
and on the RP2350 with `CORES=2` as well, where a snapshot of `user/lib/seqlock.h` is written on one core while read on the other.

Of the Wi-Fi system `make check` runs only what makes no system call, the IP stack and the clients' sockets, on the host;
a change to `user/wifi/` with a Pico 2 W connected runs `make BOARD=rp2350 wifi` too,
and `WIFI_CONFIG` names a network to join from a file git does not track,
`local/wifi.conf` on the maintainer's machine, never from one it does.
A change to `user/wifi/esp32c6/`, `user/radio/esp32c6/`, `kernel/board/esp32c6/`, `user/tracer/` or `tools/mac-trace.py`
follows the ESP32-C6's Wi-Fi driver's own rules as well, in `user/wifi/esp32c6/CLAUDE.md`,
and a change to `user/radio/esp32c6/` the radio's programs' too, in `user/radio/esp32c6/CLAUDE.md`.

`make check` builds no PHY harness, so a change to `user/phyblob/`, `user/phytrace.h` or the ESP32-C6's device ranges
runs `make BOARD=esp32c6 phymap`, which builds the harness and lists what it reaches that no frame covers;
`make BOARD=esp32c6 wifi-esp32c6-map` lists the same for the Wi-Fi driver,
which a change to its device ranges or `phy.c` runs too.

### Updated together

A new kernel global needs a reset in `host_boot` too,
since the host boots once per input in one process.

A new `BOOT_CAP_*` or `REPLAY_CAP_*` slot, a device QEMU's board lists, or an operation whose arguments change,
changes what the corpus means, see `TODO.md`.
The seeds name their slots and operations, so a renumbering only rebuilds them,
but an operation whose arguments change has the seeds that call it rewritten with it.

When the kernel's object model changes,
update `kernel/selfcheck.c` with it;
it is the executable form of the properties in `DESIGN.md`.

A change to an operation the model in `model/` models, or to what `host/model.c` writes of the state,
changes the model in the same commit, and `make model-test` holds the two to each other;
see "The model" in `DESIGN.md`.
A change to a check of the self-check that `invariants` in `model/Main.lean` states changes its statement with it.

An object bound to another, at creation or by a call, needs a line in `host/history.c` too.

### Mutants and the fuzzer

When a change adds or moves an invariant,
run `make mutants` as well
and plant a mutant for the new one as a patch under `tests/mutants/`,
with the header lines the run prints for it.

When a change edits code a mutant touches or stands next to,
run `make mutants-refresh`, then `make mutants` if it moved any,
and fix by hand only the patches it calls stale.
The full run takes minutes, so while working run only the mutants a change touches,
`tests/mutants.sh -j 4 name...`,
or every mutant's witness alone, `tests/mutants.sh -q`,
which says whether each is still caught but not by what;
a change to the seeds, the corpus or the checks moves every mutant's reports,
so it needs one full run before it is committed.

A change to `host/mutator.c` or to the flags `make fuzz` runs with
is measured with `make mutants-fuzz`, from the corpus and from nothing, before and after, over a few seeds,
and the commit says what it found.

`make fuzz` follows the default machine's coverage alone,
so a change to what a second core does runs `make fuzz FUZZ_HARNESS=fuzz-smp2` for a few minutes too.

## Environment

In a cloud session, `.claude/hooks/session-start.sh` installs what `make check` needs;
its clang 23 comes from apt.llvm.org, and Lean from GitHub's releases, which the environment's network has to let in.
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
