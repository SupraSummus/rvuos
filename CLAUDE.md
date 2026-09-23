# rvuos: repository conventions

rvuos is a capability-based microkernel for RISC-V microcontrollers
without an MMU.
Read `DESIGN.md` before changing anything in the kernel.
No new walk over every object, pool or table
goes on the path of a system call, a tick or an interrupt;
see goal 4 in `DESIGN.md`.

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
When the kernel's object model changes,
update `kernel/selfcheck.c` with it;
it is the executable form of the properties in `DESIGN.md`.
When a change adds or moves an invariant,
run `make mutants` as well
and plant a mutant for the new one as a patch under `tests/mutants/`.

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
