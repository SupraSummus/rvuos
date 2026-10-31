# rvuos design: properties and verification

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Properties

These invariants must hold in every state a process can reach,
or, where they say so, across every call or over a run.
`kernel/selfcheck.c` is the executable form of those about one state;
keep the two in step.

**Isolation.**
No region installed in any process overlaps a pool,
so user mode never has a mapping to memory that holds kernel objects.
No installed region lies outside the memory
the root task was granted at boot,
and the granted memory does not overlap the kernel's, which lies below its log,
so the kernel's own memory is unreachable,
the log excepted: it holds no object and can never become a pool.
Memory that may hold kernel objects, an Untyped or a pool,
overlaps another node standing for memory, an Untyped, a pool, a frame or an installed region,
only where one lies below the other,
or where one of the two is an Untyped whose delete as a root has begun,
which makes roots of its children one by one and nothing else until it goes.
So no frame overlaps a pool, and no Untyped makes a block over anything not below it.

**PMP fidelity.**
For every process, the PMP image the kernel built
and the region slots describe the same function
from address to access rights:
no byte is accessible with a right its slot does not grant,
and every byte in a slot is accessible with the slot's rights.
For the running process the same holds for the CSRs as the core holds them,
the fence and the entries the core hardwires included,
on the core the check runs on; another core's the next property holds.
A process has one region slot at least and no more than the core has entries for,
and its image no more entries than it has region slots, so an install never runs out of entries.
On the ESP32-C6 no process has a region it may write ending where one it may only read begins;
see "Region slots".

**Authority confinement.**
Every capability in every table names either
a frame or an Untyped within the granted memory,
a NAPOT block no smaller than the smallest region,
with rights no greater than the root task received for it
or a range of lines the interrupt controller has, the log's among them,
with no more than the right to bind,
or a range of the units of time there are, with no more than the rights to bind and to run on spare time,
or a range of the channels the kernel lends, with no more than the right to bind,
or `Print`, `Halt`, `Test` or the clock, which name no object,
or a live kernel object of the capability's own type,
which for a notification may signal some bit and for any other object carries no bits.
Every process's table slot is empty or names a live table, with the right to write it,
every thread's process slot is empty or names a live process,
every thread's watch is empty or names a live notification,
every `Irq`'s notification slot is empty or names a live notification,
every `Route`'s end is empty or names a frame its router finds the end in,
and every `Dma`'s end is empty or names a frame its controller finds the end in.

**Derivation.**
The slots of every live table,
the table slots and region slots of every live process,
the process, units and watch of every live thread,
the notification of every live `Irq`,
the ends of every live `Route` and `Dma`
and the own node of every pool
are the nodes of one forest.
Every link of a filled node lands on a live, filled node;
an empty node has no links.
The children of a node form one ring that closes through the node,
every node on it linked up to that node,
and every node's previous link names the sibling whose next is the node,
the last sibling for the first;
a root has no siblings and no previous link,
and a capability to a pool or an object is never one, nor a thread's process or watch or an `Irq`'s notification.
A node is derived from its parent:
the same object with no more rights, and for a notification no more bits to signal,
a range within the parent's range with no more rights,
a pool's own node on an Untyped, an installed region or a `Route`'s or a `Dma`'s end on a frame,
a thread's units on a capability to units that holds them, the first of them even for none,
a thread's process or watch or an `Irq`'s notification on a capability to that object,
the watch and the `Irq` with no more bits to signal, or on that object's pool or its pool's own node,
or a capability to a pool or an object below a capability to that pool or its own node.
A revoke, a delete or a destroy stopped for an interrupt leaves such a forest too,
since every step of theirs does; see "Bounded work".

**Structural soundness.**
Every pool's descriptor sits at its base and holds the pool's own node,
its objects tile the space from the descriptor to the used mark exactly,
each object records the pool it lies in and where the one before it lies,
the newest is the one the descriptor names,
and pools are pairwise disjoint.
A pool half destroyed is a pool like any other, with fewer objects.
The line table names exactly the bound `Irq`s, the channel tables exactly the bound `Route`s and `Dma`s,
and every node an object holds whose holder hears of its clearing names that holder:
an installed region its process, a thread's process and units the thread, an `Irq`'s notification the `Irq`,
and a `Route`'s or a `Dma`'s ends the `Route` or the `Dma`.
No capability in a table names a holder,
no node carries bits its type has no use for,
and every `Notification` capability names a block of bits.

**Thread state.**
Every thread is stopped, ready, or waiting,
and has one binding to units or none, a leaf of the derivation tree
made through a capability with the right to bind.
A thread's process is a leaf too, and a thread without one is stopped.
So is a thread's watch, set through a capability with the right to signal, and it signals some bit.
A thread with a fault to tell is stopped, where it faulted.
A waiting thread names a live notification and nothing else does,
and a notification's queue holds exactly the threads waiting on it.
A thread lends its time only while it waits, to another live thread, on whose ring of lenders it lies;
a thread's ring holds exactly the threads that lend to it, each linked back to the one before.
A place of a ready thread whose turn it is nowhere waits on exactly one of its core's queues and names it:
the run queue while it has time, or its lender's place on that core has, else the spare queue with spare time,
else the spent queue if it or its lender's place has units;
every other place names none and is linked into none, a place of a thread whose turn it is anywhere among them,
and the queues hold exactly the places that name them.
The queues a place waits on are those of the core whose time it holds;
a thread's places hold the time of one core each, and know the thread and which of its places they are.
While a thread runs it is its core's turn, and a core whose turn it is runs that thread;
a turn's payer, while it has one, is another live thread of that core, and waits on no queue,
and while it waits it is the first lender of the turn's thread.
The thread the kernel is running is one it could run:
it is a live object and it is ready,
though it may have lost its units during its turn, which it finishes.
A preempted thread stays ready and joins its queue,
so it runs again while it keeps its units or its spare time.

**Units and virtual times.**
Each of every core's `TIME_UNITS` units names the one thread bound to it, or none,
and a thread's units are all of one core.
Every place's virtual time lies at most the longest span ahead of the clock, since a request starts no later than the clock;
a thread has time exactly while it has a request open on its units, or its lender's, or their virtual time has come.
A place on the run queue has its request open on the units that pay for it.
A turn is charged up to a count the clock has reached, and began no later.
A thread with units that waits without time has its virtual time come no earlier than its core's release,
and so does the lender of a borrower that waits for lent time,
and every core's release lies ahead of the clock.

**Cores.**
A core's thread is a live thread or none, and a core whose turn it is runs it.
A core idles only while some core runs a thread or holds one waiting for a turn,
or something armed could make one runnable.
A core runs user mode only for a ready thread, and only with the regions its process has:
a call that takes either from it waits for the core to trap first.
A core whose turn's thread another core stopped is marked to drop the trap it takes next,
and a core holds a mark only from another core, and only while it has trapped;
under tracing a core has its turn for a thread the host cannot follow only while it is so marked.
The core a call runs on holds the regions of its thread's process, none waiting to be loaded again.

**Interrupts.**
Every `Irq` names a line there is: the log's, the controller's or a timer line.
Its notification is a leaf of the derivation tree,
bound through a capability with the right to signal,
and it is armed with no bit that capability may not signal;
an `Irq` without one is disarmed.
At most one `Irq` is bound to any line.
The controller forwards a line exactly while an `Irq` is armed on it,
read back from the controller itself:
an interrupt masks the line as it disarms the `Irq`,
a set masks or unmasks it with the bits,
and a destroy masks the lines of the `Irq`s it takes.
The count of armed sources is exactly the `Irq`s armed on a device's line or a timer line.

**Routes.**
Every `Route` names a channel the kernel lends, and at most one is bound to any channel.
Each end is a leaf of the derivation tree,
connected through a frame with the right to read for the event and to write for the task,
and names an end its board's router finds in that frame.
A `Route` is armed only with both its ends,
and the router connects a channel exactly while a `Route` armed on it is, from its event to its task,
read back from the router itself:
a set arms or disarms it, and a revoke above an end and the destroy of its pool disarm it.

**DMA.**
Every `Dma` names a channel the kernel lends, and at most one is bound to any channel.
Each end is a leaf of the derivation tree,
connected through a frame with the right to read for the source and to write for the sink,
and names an end its board's controller finds in that frame.
A channel that may still reach memory belongs to a `Dma` with both its ends,
reaches nothing beyond its memory end's frame and moves bytes to or from its device end's device alone,
read back from the controller and what it reads:
a start runs it, and a stop, a new end, a revoke above an end and the destroy of its pool halt it.

**Timer lines.**
An `Irq` armed on a timer line has its deadline ahead of the clock:
the timer fires every timer line that is due,
and a line that fires disarms its `Irq`.
Each core's nearest deadline lies ahead of the clock
and no later than the deadline of any `Irq` armed on a timer line from that core.

**The watchdog.**
An armed watchdog has its deadline ahead of the clock, since the timer that reaches it halts the machine,
and no further ahead than the longest delay a feed may name.
The core that fed it last has its nearest deadline no later than the watchdog's.

**The timer.**
The count the timer was last set for comes no later than the release, the core's nearest deadline,
the end of the turn's request unless its thread may go on alone on spare time,
and a span into a turn on spare time while another waits for spare time,
unless a change since marked it stale.

**The log.**
An `Irq` armed on the log's line has nothing untaken behind it:
the head is where the reader said it had taken to.
A set with bytes untaken signalled at once,
and a byte that made the line rise signalled as it landed,
so the same "armed and unmasked are one state" holds,
with the head for the controller.

**Memory safety.**
No sequence of system calls makes the kernel read or write
outside its own objects,
nor reach any undefined behaviour.
Any kernel panic reachable from user mode is a bug.

**Authority only flows.**
A call leaves no capability its caller could not have made.
Every node it fills or changes is covered by a capability
the caller's tables held before the call:
the same object, or a range within its range, with no more rights,
where an Untyped covers the frames it makes,
and a thread's units are covered as a range of units, with the rights it was bound with,
and its watch as a capability to its notification, with the rights it was set with,
each only on a thread the caller held a capability to with the right to control it,
and a table mounted in a process only in one the caller held a capability to with the right to control it.
Or it names an object the call built,
in a pool the caller could allocate from,
bound to what the caller could write,
and a pool on memory the caller held an Untyped to, with read and write.
So a thread earns units only through a capability to them with `RIGHT_W`,
and runs on spare time only through one with `RIGHT_X` besides.
This is goal 2 across a call.
It measures a copy against all the caller held, not against its source,
so a copy wider than its source but within another capability of the caller passes;
the derivation invariant checks it only against the parent it shares with its source.

**A call is as good as its capability.**
A call that goes through, returning or blocking, was made through a capability
with the rights its operation needs, as `include/rvuos/abi.h` lists them,
and a signal sets only bits its capability may signal,
in the notification or in the thread it wakes.

**A refusal says the first check it fails.**
A call that returns failed at the first of the checks the manual orders that it fails, if any does:
a slot that names nothing, a type that does not take the operation, a right the capability lacks,
or something nonzero in what the operation does not read, a rights mask's bits past `RIGHT_ALL` among it.
`host/history.c` works out each from `host/ops.h`, the kernel's `op_shapes` aside,
so the two tables are held to each other, and to `include/rvuos/abi.h`, through every refused call.

**Memory crosses zeroed.**
Every object that leaves the pools holds nothing but zeros,
whether its pool went or a destroy stopped half way,
and every object holds nothing of its memory from before the pool took it.
No byte of an object reaches a process when its pool is destroyed,
and no byte a process wrote becomes part of an object when its memory becomes a pool.
The rest of a pool's memory comes back as it went in; see "Zeroing goes with the object" in "Bounded work".

**The caller keeps what it runs on.**
No call begins to destroy the pool its thread, its process or a table its process mounts lives in.
A destroy stopped half way leaves the pool's threads running,
so this is what keeps a thread from making a call it cannot return from.

**A dying pool only gives back.**
No call builds an object in a pool whose destroy has begun,
so every step of the destroy takes something away.
That the destroy ends does not follow, and does not hold; see "What a holder gave, it takes back".

**A step charges.**
Under tracing, where the clock is the record's,
the thread the turn is charged to, its payer while it runs on lent time, pays `TIME_UNITS / u` counts of virtual time
for each count the turn ran, up to the end of its request, once the call charges the turn, which it need not;
a request it starts after brings its virtual time up to the clock, and nothing else moves it.

**A charge counts overruns.**
A thread's count of overruns moves only as a charge reaches the end of a request it holds.
Under tracing that is exactly where `charged_up_to` says the call's charge of the turn reaches it.

**A thread lends by its own wait.**
A thread comes to lend its time to another only by its own lending wait,
through a capability to that thread with the right to lend;
no other call, and no fault, makes one thread lend to another.

**A wait says how soon.**
A wait or a lending wait that goes through, returning or blocking, leaves its caller's requests on every core
due within the span it gave, and as they were if it gave none.

**A thread stops by its holder, where it stood.**
A thread ready or waiting before a call and stopped after it is the caller of a fault, one whose process the call took,
or the thread the call's `OP_THREAD_STOP` named, through a capability with the right to control it.
The caller of a call stands past it;
a waiter stands at the call it waited in, every register as it was, unless the call answered its wait first,
as a destroy does with `KERR_INVALID_CAP` and no bits;
any other stands where it stood, every register as it was.

**A move keeps a node's place.**
A move that succeeds leaves its source empty
and its destination holding the same capability,
below the same parent, or a root where the source was one,
and above the same children, in their order.
The tree's shape alone does not say so:
a copy beside the source and a delete of it leave a whole tree too.

**A carve takes what it names.**
A carve that succeeds puts in its destination a capability of its source's type and rights
and of exactly what the call names of the source:
a frame the block at the offset of the length, lines or units the range there,
and a `Notification` capability the bits of the mask its source may signal.
The self-check sees the frame as a block whatever the carve did, since a frame is one word,
so only the call's arguments tell a carve that skipped the alignment of its offset from one that did not.

**A description is the capability's.**
A describing call, `OP_CAP_INFO` on any slot and `OP_POOL_INFO` on a pool,
succeeds on a filled slot whatever the capability's rights,
unless it is given an argument it does not take, which it refuses,
and fails on a slot that names nothing, as every call does.
It returns the type, the rights and what `include/rvuos/abi.h` says the type tells, as the call found them,
no object's address among them,
and it changes no node, no object, no pool's descriptor and no table's header.
What the tail of every call and its trace line may move is left out:
a thread's state, queues, virtual time and frame, a notification's bits and waiters, an `Irq`'s bits.

**A pool's room is what an allocation may take.**
An allocation from a pool, `OP_POOL_ALLOC`'s or `OP_IRQ_BIND`'s, fails for want of room
only when what its type takes, rounded as the pool counts it, is more than the room as the attempt found it,
in a pool not being destroyed,
and one that succeeds takes exactly that; `OP_POOL_INFO` tells those bytes and that room.
Any other status comes from a check before the room's and says nothing of it.
The type is measured by the structures' own sizes, as `MANUAL.md` gives them, not by the kernel's `obj_type_size`,
so an allocator and a walk that agree on a wrong size show too.

These thirteen relate the state before a call to the state after it,
so the self-check, which sees one state, cannot check them,
and it gains no check for the last two: a description changes no state,
and the self-check's tiling holds the sizes the allocator takes, `obj_type_size`'s, to those the walk steps by, `obj_size`'s.
`host/history.c` checks them around every call of the host build, the untraced prologue's too,
and around every fault, which must change no more than a call that moves no time,
but a move's place, a carve's and a description only around a traced call,
since they follow the tree's links and only the self-check vets them.
It does not know what memory held before a call,
so an object handed out unzeroed shows only by what it holds:
the host's RAM starts out holding a pattern and keeps the objects of the input before,
and a new table's stale slots are capabilities its maker held nothing to cover.

**What a holder gave, it takes back.**
A walk whose thread makes it again whenever it stops ends, whatever other threads do between its steps:
a revoke, a pool's destroy, a delete below a root, and the revoke that begins a bind.
This is what a parent needs: what it lent comes back to it.
It is a promise over a run, which no check of a state or a call holds,
and steps that each keep the forest need not end.
It does not hold.
A walk keeps its progress in the tree alone,
and a thread with two capabilities where the walk goes puts back with one call what each step took:
a copy of the one left into the slot the step emptied,
or, below a root, a delete of the child made a root and then such a copy.
The seeds `revoke-outpaced`, `destroy-outpaced`, `delete-root-outpaced` and `destroy-root-outpaced` replay it,
`OP_TEST_PREEMPT` stopping the walk after every step, and `revoke_outpaced` shows it in the model.
On two cores a thread waiting for the kernel's lock stops a walk after one step too,
so a thread there that calls without a pause may keep pace; no run has shown it.
On one core the other thread runs between two steps only on time of its own.
Open decision 43 weighs the remedies.

**Exit to user mode.**
Every `mret` enters user mode,
at the running thread's saved program counter with the registers of its own frame,
with the PMP holding its process's image,
and with no reservation, which `trap_return` breaks.
The kernel writes `mstatus.MPP` once, to user mode, before the first `mret`;
a trap from user mode sets it to user mode again,
and a trap from machine mode halts.
`trap_handler` refuses a frame that is not the running thread's
and hands back the running thread's,
and under tracing the self-check compares the PMP CSRs with the running process's image.
The rest lives in `start.S`, which the host build does not have,
so only the demo and the replay under QEMU run it,
and no check says what it does wrong; see `TODO.md`.

## Verification

**Host fuzzing** (`make host-test`, `make fuzz`).
The kernel's logic compiles natively with a shim for `kputc`,
the PMP CSRs and halting, and physical addresses indexing a RAM buffer;
an address outside it breaks "Memory safety" and is reported as any invariant is.
So does an access inside it to anything but an object or the log:
the host poisons the rest of RAM for ASan, and the pools move the poison as objects come and go.
ASan thus sees the kernel reach a pool's free space, a destroyed pool, memory no pool was made of,
or the slot past a table's last, which begins in the table's padding,
but not one object reach into the next, right behind it as on the target; see `TODO.md`.
Where each input runs as if alone, ASan goes on after the report, so that the inputs after it still run,
and the input ends as the call returns.
An input is a sequence of events the kernel receives,
not the behaviour of one process:
each record is a system call with the thread that makes it,
and a step of the clock is a record too, `OP_TEST_TICK`,
which moves time by a millisecond of the counter's counts and fires the timer lines due,
so the host has a clock that moves when the input says;
a device interrupt is a record as well, `OP_TEST_IRQ`,
which does to an armed `Irq` what the controller would,
so the host has devices that fire when the input says.
libFuzzer feeds the records into the threads' registers
and the self-check runs after every call, under ASan and UBSan.
Around every call `host/history.c` compares the state before with the state after,
for the properties that relate them; see "Properties".
Since no system call takes a pointer,
the registers are the whole attack surface of a call;
a record may also load or store a word itself, as a process's own code would, see below.
The mutator in `host/mutator.c` works on whole records:
it inserts, deletes, swaps and moves them,
sets one field to a value in its range,
and splices two inputs on record boundaries,
so that a handoff between the driver's threads is one mutation, not a guess per byte.
Most records it draws are typed, by a table in `host/ops.h` of what each operation's arguments are
and a guess of what each slot holds, which follows the records from the prologue:
a record invokes a capability of the type its operation wants, takes its sources from slots of their types,
and may use what an earlier record made or come again with fresh slots,
so a chain of splits down to a small pool is a mutation or two.
A wrong entry in the table draws worse arguments and hides nothing,
and the untyped draws stay, for the refusals the types leave out.
An operation that describes is drawn apart, one typed draw in eight,
since every type takes `OP_CAP_INFO` and a pool `OP_POOL_INFO` besides,
which drawn among the rest would halve the draws of a pool's allocation, the operation that builds every object.
The coverage that guides it is the kernel's alone:
the self-check and the harness walk every object after every call,
so their edges would reward an input for the objects it leaves rather than for what the kernel did,
and the comparisons they traced for the fuzzer took most of a run.
So the walks over every object, which no system call takes, lie in `kernel/selfcheck.c`,
and what of the kernel's they call on every object, `obj_size` and `obj_nodes`, is inline,
so each has a copy of its own outside the coverage.
Nearly every edge of the kernel is reached,
so `make fuzz` lets the values its comparisons meet guide the search as well.
It fuzzes in a process per processor, each taking up what the others add to one working copy of the corpus.
The inputs replayed are checked in under `tests/seeds` and `tests/corpus`.
`make mutants` and `make qemu-replay` replay them in one harness process,
each input from RAM as at power-up and ending at its first report, as if it ran alone,
since starting the harness costs more than most inputs do.
A seed is written by hand for a scenario the kernel must handle,
is named after that scenario, and is kept for as long as the scenario exists.
It is text, a record per line as `tools/seed.py --text` writes it, comments aside,
the operations, the slots, the types and the rights by their names in the ABI's headers,
a slot the replay's setup leaves empty as `REPLAY_CAP_FREE+n`,
and `tools/seed.py` builds the records of every seed into `build/seeds` before a replay,
so that the ABI renumbering what a seed names leaves the seed meaning what it did.
`tools/seed.py --text` writes such text from records,
an input of the corpus a mutant needs, say,
and `build/host/fuzz --print` shows records a line each, by number.
The corpus is what the fuzzer found,
and `make corpus-merge` rebuilds it from scratch each time:
an input stays only while it reaches an edge on some harness
that the seeds and the inputs kept before it do not.
How often an edge is hit guides the fuzzer but keeps no input,
since `make qemu-replay` pays one QEMU boot per input.
Coverage is measured against the kernel as it is,
so an input that was unique for an earlier kernel
and covers nothing new today is dropped rather than kept for its history.
`make mutants` plants bugs in the kernel one at a time,
each a patch under `tests/mutants/` headed by the invariant it breaks,
and requires a seed on a host harness to catch each with an invariant report,
or, for a mutant of the stack, `tools/stack-depth.py` to refuse the link,
and for a mutant of the bounded work, `tools/loop-bounds.py`
or the counting harness `fuzz-work`, whose report is an invariant report too.
An input that some mutant needs thus belongs among the seeds under a name,
since the corpus keeps it only while coverage does.
The header also lists exactly the checks that catch the mutant
and the invariant reports the seeds give on it,
and the run fails where the checks disagree:
a check that stops catching a mutant shows, and so does one that starts to,
or a seed that catches it by another invariant than the one it plants.
The list of checks is also what shows a minimisation that lost something.
The header names a witness too, one seed on one harness that catches the mutant,
or the tool that refuses its link where no seed does,
and the run checks it as it checks the list.
`tests/mutants.sh -q` runs the witnesses alone, in a tenth of the time:
it says that every mutant is still caught, not that the list still holds.
The QEMU checks are in the list too, so that their strength is known,
but a mutant only they catch counts as missed:
`make test` runs one scenario with the self-check off
and catches a bug only when the transcript changes;
`make qemu-replay` catches one when the default machine's self-check reports it
or the two traces differ,
and since the list says only whether it does, `make mutants` stops it at the first input that fails.
QEMU's clock counts instructions, not the host's time,
so a run takes the same path however many run beside it, on two harts too.
What `make smp-test` catches hangs on the harts' turns, though,
which any change to what they run moves, so it is not in the list.
`make mutants-refresh` carries the patches over a change in the kernel
by the lines they change rather than by their context,
and `make mutants` checks by the headers the ones it had to move.
`git apply` uses line numbers only to break a tie,
and `make mutants` lets it drop the context to one line,
so a hunk that only moved keeps its old numbers
unless its lines with one line of context match in several places.
A refresh thus rewrites only the patches whose text changed.
A three-way merge does not help: the kernel's history shows the mutated lines or their neighbours
changing whenever the context alone was not enough.
A mutant whose own lines changed is planted again by hand.
A seed that catches a mutant says the checks are strong enough, not that the fuzzer would have found it.
`make mutants-fuzz` measures that: it fuzzes each mutant from the corpus alone, or from nothing,
for a budget of runs with libFuzzer's seed fixed, and prints the runs to the first report,
so two versions of the mutator compare by the numbers; `make check` leaves it out.
A seed repeats a run only with ASLR, `-rss_limit_mb`'s thread and `-reload` off and the environment the same,
as the measurement keeps them.
A change of a flag or of the harness moves the stack as another seed would,
and two seeds differ by a few mutants, so a change is judged over several.

**QEMU** (`make test`, `make qemu-replay`).
The kernel has no console, so a transcript reaches QEMU's UART two ways:
through a logger in user mode while the machine runs,
and through the halt, which writes the whole log out under a line saying so;
see "The kernel log".
`make test` boots the real kernel and checks the root task's transcript:
the root task starts a logger thread on the log's line and the UART's,
builds a second process, exchanges a word with it
through a shared region and two notifications,
hands its place over to a successor, which destroys the pool the root task lived in,
has a thread of its own fault on a region it took away and hears it through the thread's watch,
maps the region again and resumes the thread, whose load goes through then, and halts;
`tests/run.sh` checks that the logger carried the transcript out
before the halt did, by where the halt's line falls in it.

`make escape` boots the escape-attempt suite,
one root task per scenario, each attempting a single breach of its confinement.
A fault stops the thread that makes it, and a scenario's root task has one,
so the machine stops with nothing left to run, and a scenario tests one thing;
`tests/escape.sh` checks that the fault is the one expected
and that the scenario did not reach the line it prints only when the escape works.
It executes from the no-execute data region and jumps into the kernel, mcause 1,
reads a machine-mode CSR from user mode and runs `mret` there, mcause 2,
loads a misaligned word across the end of the data region, mcause 5,
stores one from a region it may write into one it may only read, mcause 7,
stores into the kernel, mcause 7 too,
stores to the clock's counter, a device, through no frame, mcause 7 as well,
and loads the word just past a frame of the smallest region's size, mcause 5,
which a kernel that took the grain for finer than it is lets through.
A core that raises misaligned exceptions instead, as RP2350's does, reports 4 and 6 for the misaligned two.
On ARM the same scenarios make their attempts in Thumb, `user/arch/<arch>/escape.h`, and the MPU's refusal is a MemManage.
In place of the CSR and `mret` ARM loads from the System Control Space, a BusFault,
and two scenarios try the frame the core keeps on the thread's stack, one it cannot stack and one it cannot unstack;
see "Architectures".
The user linker script says where the kernel starts, for these scenarios alone.
The fuzzer's whole attack surface is a system call's registers,
so this suite is what runs real instructions on the real core
to check that PMP or the MPU and the privilege boundary confine a process;
it runs on the target alone, like `make test`, never on the host.

The replay driver carries the log to the UART after every record,
by polling and with no system call, so the transcripts stay the same on both builds,
and the host writes the same mark into the header after every event;
the halt writes out what the last record left.
`make qemu-replay` boots the replay driver once per corpus input
with the input placed in RAM by QEMU's loader.
The driver runs two threads that take records from one cursor,
so a record that blocks one of them leaves the kernel
something else to run and the blocking paths are replayed too.
A third shares the second's process, lives in the first's pool, and starts stopped,
so a record can resume it when two threads should wait at once,
and a destroy of the second's pool leaves it stopped without a process.
All three earn no units and run on spare time, so they take turns as one queue
and passing a record round visits every runnable thread,
until a record binds one of them to units, and so to time the others do not have.
A record that names a thread is passed to it with `OP_TEST_TICK`
by the protocol in `include/rvuos/replay.h`,
which the host runs from the kernel's state;
the passing is system calls, so both transcripts carry it.
The second thread has a pool, a process and a table of its own,
so every switch between them reloads the PMP under the self-check's eye,
and memory of its own, an Untyped derived from a quarter of the free RAM, as the first thread has half of it,
so a pool it makes lies below the free RAM the first thread holds,
a record on the first thread can destroy both at once with a revoke,
and the cascade is replayed like any other path.
Each table holds the block the other thread's pool was made of,
so a record on the first thread destroys the second thread's pool,
one on the second destroys the boot pool and the first thread with it,
and either is refused its own.
The state both builds start from is in `include/rvuos/replay.h`
rather than written out twice.
`OP_TEST_TRACE` makes the kernel print one line per call,
and after a describing call that succeeded a line of what it returned,
and run the self-check after it, reading the PMP CSRs back.
So the replay holds the real kernel's answers to the host's as well as its statuses:
the probe of the PMP and the timer, which the host's shim stands in for, show in a description.
`tests/differential.py` requires the host and QEMU transcripts
to match line for line,
and fails an input whose transcript ends in an invariant report or a kernel panic,
even where the two agree.
The host's transcript takes in what `host/history.c` reports,
and one that crashes the host fails.

The replay is the only program that traces,
so only QEMU virt's kernel has the self-check and the tracing, `SELFCHECK=1`,
and every other board's leaves them out, the largest part of the kernel.
There `OP_TEST_TRACE` fails with `KERR_WRONG_TYPE` and `debug_trace` is a constant false,
so what the compiler drops is only what a kernel not tracing never runs,
and a board runs the sources the replay checks.
The host has no instruction fetch to fault,
so it makes a driver thread fault as soon as it runs
without its code or its data mapped with the needed rights,
or without the UART or the log when it drains the log, as it does after each record it performs,
and every driver thread that runs once the records are done fault on the breakpoint the driver ends with.
A record may also be a word the driver loads or stores itself, `REPLAY_OP_LOAD` and `REPLAY_OP_STORE`:
QEMU's PMP decides whether the access goes through,
and the host decides it from the process's regions, and faults the thread where they do not grant it,
so where the image and the slots disagree on an access a record makes, a fault shows on one side only,
whatever the kernel's own reading of the image says.
A store is how a process leaves in memory what it writes before the memory becomes a pool.
The log, the driver's code and data and the UART are left alone,
since a write there would change what the driver does on QEMU, which the host cannot follow.
These are the places where the host models rather than executes.
A fault stops its thread alone, so on both builds the next thread goes on with the records,
and one resumed where it faulted goes on too, making the access again;
the transcripts compare the line of each fault, but not its frame, whose addresses the host does not know.

The same gap bounds which threads a replay may cross.
The host can follow a thread only if it never has to guess
what that thread's code does,
which holds for the driver's own threads
and not for a thread the records configured.
So a thread carries `THREAD_UNTRACED`
unless its program counter was set before tracing began
and no register of it was written with `OP_THREAD_WRITE_REG` since,
and rather than run one while tracing,
the kernel stops the machine with `untraced thread`.
That is a constraint verification puts on the kernel,
and it costs nothing outside trace mode.

The timer is the other such constraint.
A preemption lands between two instructions,
and the host runs records rather than instructions,
so it cannot say where one would land.
While tracing is on, the timer therefore preempts nobody
and moves no time: the clock moves only through `OP_TEST_TICK`, a millisecond of the counter's counts a step,
and so do timer lines fire, virtual times come, and the watchdog halt; each step ends the turn, as a tick once did.
`OP_TEST_TRACE` starts the clock where the counter was and brings every virtual time to it,
so the host, which boots with them at its clock, starts from the same virtual times relative to the clock as QEMU,
where the driver ran untraced for a while before.
The clock that charges a turn is then the record's alone, on both builds:
a turn with time pays its units' part of what it ran at each `OP_TEST_TICK`, and nothing at a wait.
`OP_CLOCK_READ` tells the same clock, which the host, with no counter, tells too.
The charge at a change of turn is checked by the demo in `user/init/units.c`,
by a thread held to an eighth that waits between every round of its work;
see `TODO.md` for what nothing checks.
The interrupt is quieted under tracing on QEMU, its compare set for no count,
but a switch happens only when a thread waits
or when a record moves the clock through `OP_TEST_TICK`.
A traced run in which every thread waits, or waits for its virtual time to come, therefore stops
even with a timer line armed, since no record is left to fire it,
and the host build, which has no clock, agrees.
A timer pending in a revoke stops it on QEMU as anywhere,
but a stopped call is not traced, only the attempt that finishes,
so where the timer lands leaves the transcript alone,
and the host may stop every such call without a line of its own.
The stall in `wfi` for an armed timer line is checked by the demo in `user/init/time.c`,
and so are periods that keep pace with the clock,
bounded on both sides;
the host never stalls, so nothing else checks the stall's timer.
The demo ends in the watchdog's halt, untraced, on every board, after feeding it across sleeps longer than it waits;
that the stall wakes for the watchdog on time rests on the core's nearest deadline, which only the self-check holds.
That a thread alone is not interrupted is checked by the demo as well,
by a thread that spins alone and counts the gaps a trap leaves between two reads of the counter.
The host sets no timer, but the self-check holds the count the kernel would set the timer for to "The timer" in "Properties",
so a timer that would let a request's end, a release or a line pass shows on the host too;
and the host does after every call what a trap does on its way back,
so a change that brings that count forward without marking it stale shows there as well.
The demo also holds a spinning thread to an eighth of the processor with nothing else to run,
so the kernel stalls for its virtual time to come, and then lets it run on spare time;
the host replays virtual times running ahead and coming, but never that stall.
A device interrupt is delivered under tracing as it is otherwise,
because a line left claimed would storm
and one masked without its `Irq` disarmed would break an invariant;
the replay driver holds no device and enables none,
so none arrives during a replay, and `OP_TEST_IRQ` fires them by name.
The path from the controller to the `Irq` is checked by the demo in `user/init/logger.c`,
whose logger drives the UART's transmitter on its interrupt.
The kernel loses nothing by that:
it runs with interrupts off, so a timer lands only in user mode,
and every such landing is the same to it, a ready thread whose frame is saved.
A timer landing within a restartable call is a record too, `OP_TEST_PREEMPT`:
it arms a stop at the n-th place from now at which a call could stop between two steps,
counted whether an interrupt is pending or not, so both builds stop at the same place,
and does there what `OP_TEST_TICK` does, so another thread may run while the call is half done.
The host stops every restartable call after each step and makes it again at once,
which checks every restart alone;
a thread an armed stop left stands at its `ecall` until the processor comes back to it,
and faults on the `ecall` if its process no longer maps the driver's code.
So does a thread `OP_THREAD_STOP` took off its wait, back at the `ecall` whose registers the host loaded:
the host follows it by the stop's result, `THREAD_WAS_WAITING`, as it follows an armed stop,
and the wait made again prints its line on both builds.
What the demo in `user/init/` alone still checks
is the interrupt landing between two instructions.
The replay driver starts its second thread after turning tracing on,
so that no timer runs it before the records it takes are in place.
Preemption itself is checked by the demo in `user/init/root.c`:
the two processes take turns through a shared word and no notification,
which nothing but the timer at the end of a request can get them past.

**Cores.**
The harness `fuzz-smp2` builds the kernel for two cores and runs them in turns, one trap at a time, as the lock has them:
a record goes to the core whose turn its actor has, or the one it waits on for a turn,
and a core another one interrupted takes its trap once the trap that interrupted it is over,
so of the orders the cores' traps may come in it takes one.
A core that a call waits for in `core_shoot` traps at once, its registers saved,
while one only told of a change goes on in user mode until then,
so the self-check sees a core left running what the call took from it.
Such a core drops what its trap was for through the kernel's own `core_trapped`, as the target's trap does.
A core an `OP_THREAD_STOP` shot is slower: its trap waits, as the trap of a core slow to take its ticket of the lock would,
until a record comes for the thread whose turn it has or that waits there for one,
until no other core has a turn, until its turn goes with a destroy, or until the records end,
so the calls of other cores between, a resume, a write of a register, a destroy, come before it,
which is the case the stop's mark is for, and the one order of the traps in which the thread's state and the mark disagree.
The self-check holds there that a traced core has its turn for a thread the host cannot follow only while marked,
which a trap that went by the state breaks once a register was written,
and that a ready thread waits on its queue, which a trap that handed the processor on as a wait does breaks once the thread was resumed.
A shot for a lost process or a region traps at once, as before.
Each core has its own PMP CSRs there, and the host checks after every round of interrupts
that no core idles while a thread waits for a turn on it.
The seeds named `cores-` bind a driver thread to the second core's units and take things from it there,
`cores-thread-stopped`, `cores-waiter-stopped`, `cores-resume-before-trap`, `cores-resumed-thread-runs`
and `cores-stopped-thread-destroyed` among them, which stop it there,
or arm a timer line there that a step of the clock on the first core fires;
the other seeds and the corpus run on the first core alone, as on one.
`make smp-test` boots the demo on two harts of QEMU `virt`,
the only check of the lock, the software interrupt, the idle and the start of the second hart,
and of a shootdown that waits on real hardware rather than the host's.
`make arm-test` boots it on mps2-an521's two Cortex-M33s last, the only check of all that on ARM,
of its ticket taken with an exclusive pair, its waits in `wfe` and the line the kernel keeps for its cores;
`make mutants` runs none of these demos: it runs no ARM, and what `make smp-test` catches moves with the harts' turns.
QEMU runs the harts in turns too, so a hart that waits for the lock spins out its turn while the holder waits for its own,
which the demo allows for and nothing measures;
an ARM core waits in `wfe` instead, which QEMU 8.2 takes for a turn handed to the other core.
No check yet makes a core wait for the lock while another walks, or replays the corpus on two harts against the host.

**Cost** (`make bench`).
Goal 4 bounds what a call costs, and `user/bench/` measures what it does cost,
in the core's cycles, which do not differ with the clock as times do.
Under QEMU's `-icount` the clock counts instructions and a run repeats exactly,
so on QEMU's three boards `tests/bench.py` holds each cost to the board's record in `tests/bench/` within a tenth either way,
narrow enough for a step added to every call.
The band holds for one clang's code alone, the version the record names:
clang 18's missed clang 22's records by up to two fifths on the ARM boards,
though clang 22's kept within clang 23's.
A change that moves a cost further writes the record anew with `make bench-refresh`, and its commit says what it cost.
The last measure, the worst round trip beside a process that always wants the core, is the scheduler's; see open decision 9.

**Where the cost goes** (`make trap-profile`).
A round's cost says how much, not where, nor how much a measure weighs in a real program.
`tools/trap-profile.py` logs every instruction the kernel runs under QEMU and splits the run into traps,
each classed by what it ran: the operation, a wait or a signal, a switch, a load of regions, an idle core.
Each class's share of the kernel's work is its cost times how often it came,
so what to make cheaper is what weighs most in the program at hand:
`TRAP_PROGRAM=bench` is a made-up mix, `init` the demo's, which its console's interrupts fill, and `lab` the laboratory's.
A run repeats exactly, so `TRAP_SAVE` and `TRAP_AGAINST` say what a change moved, function by function,
and `TRAP_ANNOTATE` breaks a function down by source line.
It counts instructions, not cycles, which `make bench` measures on a board.

**Hardware.**
QEMU's PMP may differ from a real core in Smepmp behaviour,
and it has a four-byte grain, so a coarser grain is never seen there.
The host shim models the grain instead, reading back the address bits below it as hardware would,
and the corpus is replayed with a four-byte and a 32-byte grain.
That harness, `fuzz-rp2350`, also holds RP2350's three hardwired entries past eight,
so the self-check sees them open wherever the fence is missing.
On the ESP32-C6, `make test BOARD=esp32c6` boots the demo
and checks the same transcript as under QEMU;
that is the only check that reaches its timer, matrix and USB console,
and the user-mode CSRs the kernel sets back.
The replay records are transport-agnostic
and can be fed to the board once something loads them there;
the replay driver's second stack is QEMU's address, see `rvuos/replay.h`,
so it is not built for the board yet.

**What no check reaches.**
Every check above holds what it sees to a statement about one state or one call.
So none holds a promise over a run, that something ends or happens in time whatever other threads do,
since a run that never ends passes each of its steps;
nor one across two runs, that what a process sees is the same whatever a process it shares nothing with does.
"What a holder gave, it takes back" is of the first kind, and broken.
"What a request promises" and "How much work a thread gets", in "Scheduling", are of it too,
and only `make lab` measures them, in its own scenarios.
None of the second kind is stated yet.
`.claude/skills/run-promises/SKILL.md` says how such promises are found.

## The model

`model/` holds a model of the derivation tree in Lean 4,
the invariants of "Properties" its state can say,
proofs about it,
and `model-check`, which holds the host build to it and searches it.
The model maps a state and an event to every outcome the kernel may reach.
The state is what `host/model.c` writes of the kernel's memory:
every object with its array of nodes, and a pool its size,
every filled node's words by its address, unpacked:
a frame's base and size and a range's first and count in two words,
a `Notification` capability's bits whole, an Untyped's going in its second word,
and a node an object holds by a type of the model's own, with its index among its holder's slots or places,
and each node's children in the order of their ring; no link, no scheduler, no PMP.
With it comes what the machine fixes: the smallest region and pool, where a process's tables, a table's slots and a thread's process lie,
and the counter's block, which a clock gives out.
It models the operations that touch nothing but the tree:
`OP_CAP_COPY`, `OP_CAP_DERIVE`, `OP_CAP_MOVE`, `OP_CAP_DELETE`,
`OP_CAP_REVOKE` until it meets a pool's own node,
`OP_FRAME_CARVE`, `OP_UNTYPED_SPLIT` and `OP_UNTYPED_RETYPE` into a frame,
and the refusals dispatch makes before any handler,
but where an operation's own values decide what it reads.
A call that may stop part way has an outcome for each place it may stop.
Of the rest the model says nothing.

The model is written from the kernel's code,
so holding the one to the other finds a slip in either, never a mistake they share;
a fuzzing run that held them on every input it tried found none, at a tenth of the fuzzer's speed,
so the model runs on the corpus alone.
What it adds is what no run can:
every place a call may stop, all at once,
and statements of every state, not only of the states a run reaches.
A statement of a call is no statement of a state:
`delete_revoke_shrink` and `step_flows` below would hold of a delete that left an Untyped free over what became roots,
as it only takes away.
So each invariant of "Properties" the model's state can say goes through three stages.

1. It is stated from the self-check as a check of a state, in `invariants` of `model/Main.lean`:
   `State.wellFormed` the forest of "Derivation", `State.nestingOk` the nesting of "Isolation",
   `State.derivedOk` what the nesting needs of the rest of "Derivation" to follow from step to step,
   and `State.parentsOk` the rule of "Derivation" each node is derived from its parent by, `derived_from`,
   with what a copy and a delete need of it to follow: a table holds the ABI's types alone, none of the nodes an object holds.
   `model-check` holds it to every traced state the self-check passed,
   so a statement that says other than the kernel's check shows on the seeds and the corpus.
2. It is searched:
   from each state an input's records begin in, `model-check` follows the model `MODEL_SEARCH` calls deep, two by default,
   and gives the seed of the first state that breaks it; see `model/Rvuos/Search.lean`.
3. It is settled by a theorem: that every step keeps it,
   or, for a bug the kernel has, `Breaks` of it, a state and a call `decide` checks, which `invariants` names and `TODO.md` lists.
   A search that breaks an invariant fails the run unless `invariants` names such a theorem,
   and so does one that no longer breaks an invariant it names one for, so the fix takes the theorem out.
   A proof that every step keeps the invariant may first need the invariant to say more,
   as a state that breaks it may be one no step reaches.

`make model-test` builds the model, which checks its proofs,
replays the seeds and the corpus with `--model-trace`, each input alone,
and runs `model-check --search` over the trace:
every step the model speaks of must reach one of its outcomes, state and registers alike,
every state must hold every invariant,
and the search must break the invariants with a known bug and no other.
The model is held to the kernel on the steps the corpus makes, not proved of it.

What is proved is proved of the model:

- `blockWord_reads_back` and `split_halves`, in `model/Rvuos/Block.lean`:
  an Untyped's word reads back as its block,
  and a split's halves read back as theirs, cover the block and do not overlap,
  for every 32-bit word.
  `bv_decide` checks its SAT proof with code Lean compiles, so these two trust Lean's compiler too.
- `delete_revoke_shrink`, in `model/Rvuos/Shrink.lean`:
  a delete or a revoke leaves no capability that was not there before, with the same words,
  but for an Untyped a delete of it as a root marks going,
  in every outcome, stopped part way or not.
- `step_flows`, in `model/Rvuos/Flows.lean`:
  authority only flows, in every outcome of every step the model names.
  Every node a step fills or changes held the same words before, but such an Untyped,
  or a node the caller reached through its tables covers it:
  the same capability, or memory within a frame's or an Untyped's, with no more rights.
  It leans on `split_within`, proved with `bv_decide`, so it trusts Lean's compiler too.
- `step_forest`, in `model/Rvuos/Forest.lean`:
  every outcome of every step the model names is a forest where the state before it was one,
  so a delete or a revoke leaves a forest wherever it stops;
  and `wellFormed_forest`: every state `model-check` passes as a forest is one.
- `step_nested`, in `model/Rvuos/Nested.lean`:
  every outcome of every step the model names keeps the nesting, wherever a delete or a revoke stops,
  with what it needs of the derivation rule, `State.derivedOk`;
  and `checked_nested`: every state `model-check` passes holds both.
  Its lemmas of a split's halves are proved with `bv_decide`, so it trusts Lean's compiler too.
- `step_parented`, in `model/Rvuos/Parented.lean`:
  every outcome of every step the model names keeps every node derived from its parent, wherever a delete or a revoke stops,
  where the smallest region is two bytes or more, which `model-check` holds every machine to;
  and `checked_parented`: every state `model-check` passes holds it.
  So what a capability gave out stays below it, with no more rights, memory or bits, until a revoke of it takes it.
  `step_checked` puts it with `step_nested`: the four invariants hold together from step to step.
  It trusts Lean's compiler as `step_nested` does.
- `revoke_outpaced`, in `model/Rvuos/Outpaced.lean`, is a break, not a proof:
  from the forest `revoke-outpaced` reaches as its revoke begins,
  rounds of a stopped step of the revoke and a copy by another thread come back to a state they passed,
  so every step keeps the four invariants and the revoke never ends.
  It is shown by `native_decide`, which trusts Lean's compiler too,
  and goes once the model follows a remedy of open decision 43.
