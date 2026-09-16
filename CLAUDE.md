# rvuos: repository conventions

rvuos is a capability-based microkernel for RISC-V microcontrollers
without an MMU.
Read `DESIGN.md` before changing anything in the kernel.

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
When the kernel's object model changes,
update `kernel/selfcheck.c` with it;
it is the executable form of the properties in `DESIGN.md`.

## Documentation of decisions

Architectural decisions live in `DESIGN.md`.
When a decision changes, edit the design document in the same commit
as the code that implements the change.
Open questions are listed explicitly in the design document
rather than left implicit in the code.
Work items go to `TODO.md`.
