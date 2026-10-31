---
name: run-promises
description: How to find what the kernel fails to promise where no check of a state or of a call can look - that a walk ends, that what was lent comes back, that one process cannot see or slow another. Use when hunting for bugs or for properties worth proving, when the design says something ends or holds whatever other threads do, and when every check passes but a doubt remains.
---

# Promises over a run

The checks here hold what they see to a statement about one state or one call;
see "What no check reaches" in `design/verification.md`.
A defect that lives only in an unbounded run, or only between two runs, passes all of them,
and such defects come in families, since one assumption in the design carries several mechanisms.
The first family found this way is open decision 43.

**Start from a need, not from the code.**
Name someone who builds on the kernel and what they must rely on:
a parent that lends memory to a driver it does not trust, a control loop beside a radio stack, a supervisor that restarts a component.
A model written from the code agrees with the code; only a statement from outside it can disagree.

**Give the need an adversary.**
Say what a hostile program would hold in that setting, and let it act between every two steps of the party it attacks.
On the host build `OP_TEST_PREEMPT` stops a walk after any step and lets another thread run,
and on two cores a thread waiting for the lock does the same.

**Say which kind of promise it is.**
Of a state, or of a call: the self-check, `host/history.c` and the model hold these.
Of a run, that something ends or happens in time: nothing holds these, as each step of an endless run is good.
Of two runs, that one party sees the same whatever another does: nothing holds these either.
Look where nothing has looked.

**Find the design's argument, and what it quantifies over.**
Search the design for "ends", "comes back", "whatever the other threads do", "within".
A reason of the form "every step takes something away" proves an end only if nothing else adds;
list every operation, by any thread, that can add, and try each.
A reason that counts only what the caller does says nothing of a thread that is not the caller.

**Look for the siblings.**
Find every other place that rests on the same assumption, and try the same adversary there:
the revoke led to the destroy in both its phases, the delete below a root and the bind's revoke.

**Make it a seed before it is a sentence.**
Write the adversary for `tools/seed.py`, replay it with `build/host/fuzz -runs=0 --verbose`,
and compare with and without the adversary's records.
The seed goes in `tests/seeds/`, the promise in "Properties" saying it does not hold,
and the remedies in `design/decisions.md`, since a remedy changes what the kernel refuses and is the maintainer's to choose.
A promise over a run has a safe half one call can break, which `host/history.c` can hold once it holds;
a promise across two runs is a test that runs one input twice.
