# rvuos: repository conventions

rvuos is a capability-based microkernel for RISC-V microcontrollers
without an MMU.
Read `DESIGN.md` before changing anything in the kernel.
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
or anything the demo exercises, and an ESP32-C6 is connected,
run `make BOARD=esp32c6 test` as well;
it loads the demo into the chip's RAM and checks the same transcript.
A new kernel global needs a reset in `host_boot` too,
since the host boots once per input in one process.
A new `BOOT_CAP_*` slot moves the replay's slots in `include/rvuos/replay.h`,
so the seeds and the corpus have to be rewritten with it; see `TODO.md`.
When the kernel's object model changes,
update `kernel/selfcheck.c` with it;
it is the executable form of the properties in `DESIGN.md`.
An object bound to another at creation needs a line in `host/history.c` too.
When a change adds or moves an invariant,
run `make mutants` as well
and plant a mutant for the new one as a patch under `tests/mutants/`,
with the header lines the run prints for it.
When a change edits code a mutant touches or stands next to,
run `make mutants-refresh`, then `make mutants` if it moved any,
and fix by hand only the patches it calls stale.

## Environment

In a cloud session, `.claude/hooks/session-start.sh` installs what `make check` needs.
If a tool is still missing, run the hook, or install the tool and add it to the hook;
never skip the check.
A check that could not run is reported as not run, never as passed.

## Documentation of decisions

Architectural decisions live in `DESIGN.md`.
When a decision changes, edit the design document in the same commit
as the code that implements the change.
Open questions are listed explicitly in the design document
rather than left implicit in the code.
Work items go to `TODO.md`.

`MANUAL.md` is the user-facing description of the kernel:
what it offers a program, the system call reference,
and what the root task starts with.
When `include/rvuos/abi.h` or a limit a program can see changes,
update the manual in the same commit.
