# rvuos design: bounded work and bounded stack

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Bounded work

**The rule.**
The work of a system call, a tick or an interrupt
is bounded by constants of the machine and the kernel:
PMP entries, interrupt lines, timer lines, units of time, region slots, the size of an object.
It does not grow with how many objects, pools or capabilities exist.
Otherwise a process with one small pool allocates thousands of minimal objects,
and every tick and every interrupt, other processes' included,
gets that much slower.

**The exception is destruction.**
A revoke, a pool destroy, and a delete below a root
take as long as what they touch.
That is kept to the subtree below the caller's own capability,
never the whole machine.
Such a loop is paid for:
each step takes away something an earlier call made,
so over a run its steps are bounded by the calls before it.
What it costs is latency, and preemption is the answer to that.

**Preemption.**
A revoke, a delete below a root, a pool destroy, and the revoke that begins a bind
take one step per capability, object or waiter in constant time
and ask `intr_pending` between two steps.
If the tick or a device interrupt is pending, or another core waits for the kernel's lock, the walk stops,
`syscall_dispatch` puts the thread back on its `ecall` with its registers as they were,
the `mret` takes the interrupt through `mtvec` as in any user code, as the exception return does on ARM,
and the thread makes the same call again when it next runs, as in seL4.
The kernel only notices the interrupt; the processor takes it.
The progress stays in the derivation tree and in the pool:
the next step is always at the first child of the slot the call names,
or at the newest object of the pool being destroyed and the slot its descriptor says,
or, for a notification of it, at the first thread still waiting on it,
so the kernel needs no record of where it stopped, no second stack and no worker,
and the time lands in the slice of the thread that made the call.
A pool's destroy goes on from where it stopped whichever revoke takes the next step,
below the Untyped that began it or below one above that.
A walk asks only after a step, so every attempt takes something away and the restarts end.
Whether `intr_pending` is right changes when a walk stops, never what it does.
A restart is a new call:
it checks everything again, and revokes what was derived in between too.
A revoke that takes its caller's table, the caller's process,
or the capability the call was made through ends at that step:
nothing can make the call again,
so where an interrupt landed would otherwise decide how far it got.
A pool the revoke destroys takes none of them,
since a revoke below an Untyped refuses when the caller's process or table,
or the table the call names, lies in the Untyped's memory;
see "Kernel pools and revocation".
So a bind revokes only once every check that can fail has passed,
its room in the pool among them, and builds after.

**The check.**
Every loop a trap can run says what bounds it with an annotation from `kernel/work.h`:
a constant, what it pays with, a row of the table below, an argument, a wait on the hardware,
or a wait on another core, for the kernel's lock or for a trap to begin there; see "Cores".
An annotation is a `_Static_assert`, so it changes no code the compiler makes.
The kernel's link runs `tools/loop-bounds.py`,
which finds the loops in the machine code, inlined and compiler-made ones too,
matches each to its source loop through the debug information and clang's AST,
and fails on one that says nothing.
The table is checked both ways, so it stays the kernel's own list of walks.
Code that leads only to a halt is left out,
and so is the self-check, which may be called only under `if (debug_trace)`.

The claims are checked too.
A bound the loop's own shape limits, a counter below a constant, is compared with that limit at the link;
this is the only check of board code, which the host does not run.
The shape is read from the source, so a loop the compiler unrolled is held to its bound too.
A wait must wait on every way round, which the link checks too:
a `wfi`, a load from a fixed address outside RAM, or a call to a function holding a `wfi`,
and on ARM a `wfe` counts as a `wfi` does.
A wait on another core must read with an acquire on every way round,
a `fence` after a load or an `lr` or `amo`, or a `dmb` on ARM,
which is how it reads what the other core writes;
what bounds it is the other core: a lock held a step of a walk at most, a trap that user mode takes at once.
Other bounds rest on an invariant, as `i < img->count` does,
and a paid loop names its unit: a node, a link, an object or a waiter.
The host harness `fuzz-work` counts both after every call:
a bound per entry of its loop,
and paid steps at most twice what the call took away of that unit, plus one;
twice since a delete below a root makes a root of a node, a link each,
and a pool destroy may then clear that node as one its tables hold.
It also fails a paid loop that takes two steps without asking `intr_pending` between them,
so a walk that is paid for is a walk that is preempted,
and a `memset` or `memcpy` longer than the `CALL_BOUND` before it.
It finds a false claim only where the corpus reaches.
The host answers every `intr_pending` with yes, the worst case,
so each preemptible call stops after its first step, is made again,
and the self-check runs between any two steps;
every host harness requires a stopped call to have taken a node, a link, an object or a waiter away,
and to leave its caller running, holding its table and the capability it made the call through,
so that it can make the call again.

**Where the kernel falls short.**
Nowhere, since `Untyped` and `Frame` replaced the overlap checks and the sweep;
see "Kernel pools and revocation" and open decision 14.
A walk the kernel takes on again is listed here with its fix,
and the check requires the table to name every one and nothing else.

| Walk | When | Fix |
|---|---|---|

**Zeroing goes with the object.**
`pool_alloc` zeroes each object, at most `OBJ_MAX_SIZE`, a table of `CAPTABLE_MAX_SLOTS`,
and a destroy zeroes each object as it forgets it, paid for by the call that made it.

## Bounded stack

The kernel has one stack for each core, `KERNEL_STACK_SIZE` in `kernel/layout.h`, each below the one of the core before,
and every entry starts its core's from the top:
the reset, a core that starts, a trap from user mode, the idle a trap ends in, and a trap in the kernel, which halts.
Interrupts stay off in the kernel, so no entry nests in another,
and a thread's state lives in its trap frame, not on the stack.
The deepest the stack goes is therefore the heaviest call chain from an entry,
a constant of the image.

**The rule.**
The kernel does not recurse and has no frame of run-time size:
no variable-length array, no `alloca`.
A walk that would recurse walks with no stack, as `cap_revoke_below` does.
A call through a pointer counts as a call to every function whose address is taken.

**The check.**
The kernel's link runs `tools/stack-depth.py`,
which reads the frames from `-fstack-usage` and the calls from the disassembly,
fails on a broken rule or a bound above `KERNEL_STACK_SIZE`,
and prints the bound and its chain.
Both tools read the kernel linked alone, once for all its images.
An image adds its root task in `.user_code`, which neither tool reads,
and its link fails unless the rest of it is the checked kernel byte for byte.
The kernel is compiled with `-ffunction-sections`,
so the linker drops dead functions,
and a C function the tool sees no call to is a call it could not read,
which fails the link too.
The deepest chain runs through the self-check that `OP_DEBUG_TRACE` turns on,
and the stack is sized just above it;
the check, not headroom, is what keeps it from overflowing.
Nothing guards the stack at run time but on ARMv8-M, whose `MSPLIM` faults a push below it, a kernel fault that halts;
elsewhere an overflow would silently overwrite `.bss` below it, since PMP does not bind machine mode.
