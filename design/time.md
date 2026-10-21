# rvuos design: time and scheduling

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Time

**There is no sleep.**
A thread that wants to stop for a while wants to stop until something happens,
and a notification is the one way to stop;
what is missing is only something that happens at a time.
So time reaches userspace as one more interrupt, on a timer line.
The kernel has `TIMER_LINES` of them, numbered after the controller's,
and the timer raises them where a device would raise its own.
An `Irq` is bound to a timer line exactly as to a device's line, with `OP_IRQ_BIND`,
armed with `OP_IRQ_SET` for a set of bits and a delay,
and signals those bits when the delay has passed.
Sleeping is arming a timer line and waiting on its notification.
A wait with a timeout is the same two calls,
because a notification is a word of bits:
a driver gives the device one bit and the timer line another.
No operation takes a deadline and no thread state is a sleep,
so "a waiting thread names a live notification" stays the whole story.
Yield is open decision 10.

**The contract is a lower bound.**
The delay is in microseconds, rounded up to whole counts of the counter,
and the line fires at the first count past the call by at least that much:
the timer is set for it, and the interrupt that follows signals the line.
An upper bound is not promised because the kernel could not keep one:
the woken thread is ready, not running, and waits for its turn,
and for its virtual time to come if it ran ahead of its units and has no spare time; see "Scheduling".
A timer line that fires disarms its `Irq`, as a device's line does,
and setting an armed one moves its deadline rather than adding a second.

**A period counts from the deadline.**
A periodic task arms the line again each time it wakes,
and a delay counted from the wake would put every wake's lateness into the period.
`OP_IRQ_SET` with `IRQ_SET_PERIOD` counts from the line's last deadline instead,
or from its bind if it never fired:
the line fires at the first count after the call
that lies a whole number of periods from that deadline,
and the call returns how many such counts had already passed.
The cases a task needs follow from that one rule.
Set on each wake, the line keeps its period without drift.
A task that woke late skips the periods it missed and learns how many,
so it can do their work at once, note the overrun, or ignore it.
Set again before it fired, the line keeps its deadline.
The deadline is never more than a period away, however stale the last one.
The period is rounded up to whole counts, as a delay is.

**Why not a periodic line.**
A line the kernel arms again on its own would save a task one call per period,
but periods fired before the thread wakes would merge into one bit,
and a task that wanted one wake would race the next to disarm the line.
It would also be the one line that does not mask itself as it signals.

**Why not an absolute deadline.**
A deadline in the clock's counts would be the simplest rule to state,
but every task that sleeps would need the clock as well as the line,
and under tracing, where time moves only by record,
the host's clock would have to start where QEMU's does.

**Why a fixed number of lines.**
The timer has to find the due lines without looking at the others.
Timers as objects in pools would have it walk every pool, which goal 4 rules out;
a sorted queue moves that walk to the set, and a wheel to the bucket that comes due.
A queue sorted in a balanced tree, as deep as the RAM allows, is open decision 38.
A limit per process, thread or pool bounds nothing, since anyone can make more of those.
A limit on the whole machine does: the timer looks at the `TIMER_LINES` lines and nothing else.
Lines make it a limit of authority rather than of who comes first:
the root task receives them all in `BOOT_CAP_TIMER_LINES`,
apart from the controller's so that no board's count shows,
and hands them out and takes them back as it does memory.
The count is a constant of the kernel, like the table slots of a process,
while the `Irq`s bound to the lines live in their holders' pools.
A program that needs more timers than it holds keeps its own deadlines
and arms one line for the nearest.

**Why an interrupt.**
A deadline argument on `OP_NOTIFY_WAIT` would be smaller,
but it would make time a special case of waiting
rather than a signal like an interrupt.
A time server in userspace, a driver holding an `Irq` for a second hardware timer,
would cost the kernel nothing and remains the right shape for a board that has one;
the timer lines are what make a sleep one system call
rather than a round trip to that server.

**Reading the time.**
A timer line says that a delay has passed, not what time it is.
The `Clock` capability gives the time:
`OP_CLOCK_READ` returns the counter's counts since boot, 64 bits on every board, and its rate.
The kernel counts a 32-bit counter's wraps, reading it at least once a wrap, so the MPS2 boards' time is whole too,
and under tracing the time is the record's, which `OP_TEST_TICK` moves a millisecond at a time, as every clock is there; see "Verification".
The rate is the board's, and on the ESP32-C6 measured at boot, so a program takes it from the clock.
The clock's description, `OP_CAP_INFO`, tells the rate and the default span without `RIGHT_R`,
since neither is the time: a program that may not read the time may still know the grain its timers and requests come in.

**And the counter's frame, for a reader that cannot trap.**
A call is a trap, which a program that measures what the kernel does to it cannot afford:
a thread spinning to see whether the timer or another core takes the processor would trap at every read.
So `OP_CLOCK_FRAME` derives a read-only `Frame` over the counter's low 32 bits, below the clock,
and a process with that region installed reads them with a load.
They wrap, and the rest of the counter lies where the board has it, the word below on RP2350 and nowhere on the MPS2 boards,
so the frame gives differences and the call the time.
The frame is the smallest block holding the word;
on a PMP grain coarser than eight bytes it also shows the timer registers beside it, read only.
A board whose counter cannot be shown so, or whose neighbours a read would change, would refuse the frame; none does yet.

**Why a capability.**
The time is authority, and goal 2 allows no ambient authority:
a process given no clock and no timer line cannot tell time passing,
unless it builds a clock from a second thread or learns the time from someone who has one.
On the ESP32-C6 it can time its own turns on the core's performance counter,
which the kernel keeps with the process and stops while another process's thread runs; see decision 22.
Gating `rdtime` would have needed `mcounteren` switched per process, and the ESP32-C6 has neither.
`RIGHT_R` on the clock reads the time, and `RIGHT_W` feeds the watchdog; see "The watchdog".
Neither needs an object, so the clock adds one type and no object.

## Scheduling

**The processor is authority, as memory is.**
A process that got a turn per ready thread could buy the processor with threads,
which cost a few hundred bytes of a pool each.
So the processor is a fixed number of units, handed out as capabilities as memory and lines are,
and each unit is earned by one thread at a time.

**A thread earns units of time.**
Each core is `TIME_UNITS` units, a constant of the kernel as the timer lines are,
numbered core by core across the machine,
and the root task receives them all in `BOOT_CAP_TIME`, its thread earning every one of the first core's.
A thread earns units in a place, which holds the time of one core, and runs on the cores its places hold the time of; see "Cores".
On one core a thread has one place, and what follows says "a thread's" of it.
A `Time` capability names a range of units and no object, as an `IrqLine` names lines,
and `OP_TIME_CARVE` takes a smaller range out of one.
`OP_TIME_BIND` binds a thread's place for a core to some of a capability's units of it, or to none,
and moves the place off the units it earned before.
The kernel records which thread earns each unit and refuses a bind that names a unit another thread earns,
so a capability may be copied, but each unit's time goes to one thread,
and the time of the units nobody earns is spare.
The binding is a node of the derivation tree held in the thread, below the capability it was bound through,
so revoking above that capability unbinds the thread, as it unmaps a region installed from a frame.
A thread with no units and no spare time keeps its state and does not run:
that is how a scheduler in userspace stops and starts a thread, decision 9,
or with `OP_THREAD_STOP`, which also takes a waiting thread off its notification.

**A thread has a virtual time.**
A place's virtual time is the count of the clock its units have paid for its thread's running up to:
each count the thread runs on `u` units moves it on `TIME_UNITS / u` counts,
and so does each count it runs on spare time, but never more than a span ahead of the clock.
So a thread that runs its units' part of the core keeps it at the clock, and one that runs more puts it ahead.
It is the one word of the scheduler's that a thread carries:
the kernel keeps no balance and no constant of how much a thread may save.
No call tells what it is, which would be a clock.

**A thread runs in requests.**
A request is due a span past the virtual time it starts at, and is over once the virtual time reaches that deadline,
so a request is the thread's units' part of its span: `u * s / TIME_UNITS` counts of running.
The span is the one the thread's last wait gave, `OP_NOTIFY_WAIT` or `OP_NOTIFY_LEND`, or else its binding's, `OP_TIME_BIND`,
a power of two of counts; with none given, the default span, the longest power of two of counts within a millisecond.
A request starts as its thread is made ready or bound, and the next as one is over,
at the latest of the virtual time and the clock, and only once the virtual time lies at most a span ahead of the clock:
a thread that ran further ahead of its units waits for them to catch up, and one that waited comes back with nothing saved.
So a place lies at most a span and a request ahead of what its units paid for.
A thread that waits gives up the rest of its request,
and one whose wait returns at once with a new span keeps it only if it lies within that span, else starts the next.
A place has time while its thread has a request open on it, or while its virtual time lies within a span of the clock, so that the next may start.

**The queues are the policy: earliest deadline first, time before spare time.**
A ready thread that may run, and whose turn it is nowhere, waits on one of three queues of each core it holds the time of,
each a ring of places through the place's own `queue_next` and `queue_prev`,
as a notification's waiters are a ring of threads through the thread's:
the run queue while it has time, the spare queue without time if it may run on spare time,
and the spent queue otherwise, where a thread with units waits for its virtual time to come.
The thread whose request is due first on the run queue has the next turn, the oldest of those tied,
or with the run queue empty the oldest on the spare queue.
A deadline stands while its request lasts, so the order changes only as a thread is made ready,
which takes the processor from a turn whose request is due later as the trap ends,
and as the turn's request is over, which the timer marks while another thread waits:
a turn whose units give it the next request goes on with it if it is still due first, and one they give none ends.

**Spare time is the far end of the same order.**
Spare time is the turns no thread with time wants, so a turn on spare time ends as soon as one wants it:
a thread with time that becomes ready, woken, resumed, bound or given time again by its virtual time coming,
takes the processor as the trap that made it ready ends,
and a core that runs a turn on spare time is interrupted for one another core made ready there,
as a core in user mode is for any thread another core puts on its run queue, whose request may be due before the turn's,
and for any change another core makes to the thread whose turn it has or that pays for it.
Only a call stopped for an interrupt goes first: its thread makes it again, and the turn ends as the call does,
so that on its core no other thread runs between a call's steps.
While others wait for spare time, a turn on it lasts its thread's span, and then the next has its turn:
threads on spare time take turns by thread, each for as long as it says it may wait.
A thread bound through a capability with `RIGHT_X` runs on spare time, for free:
it costs it nothing of its units, though its virtual time moves on as it runs, up to a span ahead of the clock,
so that what it ran on spare time puts it behind the threads that waited, by its own span at most.
The boot grant has `RIGHT_X`, so a thread bound through it to no units runs whenever the threads with time let it.
A thread without `RIGHT_X` runs at most its units' part of each span, and two requests,
and its core sleeps in `wfi` for the rest: that is how the root task caps a thread's time.
A turn left with neither time nor spare time, by a bind, an unbind or its payer's destroy, ends as the trap that left it so does,
and another core that runs it is interrupted the same way.

**What a request promises.**
The units of a core add up to its time, which is all earliest deadline first needs to meet every deadline:
a thread with `u` units made ready at `t0` while its virtual time is at or behind the clock, its span `s`,
has run `u * s / TIME_UNITS` counts by `t0 + s`, or waits again before,
whatever the other threads do, besides the kernel's longest step and the switches.
A thread that ran ahead of its units is late by what it ran ahead, two of its spans at most.
So a driver that must do work of `c` within `D` of its interrupt waits within a span of `D` at most,
the longest power of two of counts that `D` holds, and needs `TIME_UNITS * c / s` units at least.
A span costs nobody but its thread:
a short one makes its requests as short, so that its thread takes no more than its units' part of any span,
and a long one puts its requests behind shorter ones and costs it nothing of its units.
A short span costs its thread switches: while another thread waits, the timer ends each of its requests.
The promise asks besides only that the thread ran no more than its units' part before the wake,
so a wake comes at most a span after the last for it to hold each time; nothing in it depends on what the thread did longer ago.

**What programs ask of time.**
Six kinds of program ask the processor for something different,
and the interface is what they need to say and no more:

| Who | Asks for | Which is |
|---|---|---|
| a driver, a server that answers | work `c` done within `D` of an event | an answer to an event: units and a span at the wait |
| a control loop | work `c` every period `P` | the same, its event a timer line's, `IRQ_SET_PERIOD` |
| batch work | a part of the processor over time, however late | units alone, and a long span |
| best effort | the time nobody wants | spare time, `RIGHT_X` |
| a server for its clients | its client's time | lent time, with the client's units and span |
| a parent over a child it does not trust | at most so much power, and so much energy | units without `RIGHT_X`, and fuel, decision 39 |

Since time reaches a thread as an interrupt, a periodic task is an answer to an event too,
so only an answer asks for anything but a part of the processor, and it asks for one number besides: how soon.
How often the event may come is not the kernel's to know:
the virtual time holds a thread to its units whatever the rate, so a thread whose events come faster than its span waits behind its own.

**Why the span is the wait's.**
How soon a thread must answer belongs to what it answers, so a thread says it as it waits:
a driver gives its interrupt a short span and its housekeeping timer a long one, at one wait each,
and a client that lends gives its server the span it gives itself, so a server answers each client within the client's.
The span takes nothing from anyone, so it needs no capability, as units do;
a wait that gives none keeps the span its thread had, and a thread that never waits has its binding's.
A wait that gives a span the request in progress cannot hold to ends that request as the core next looks,
and the next starts no earlier than the virtual time, so no span brings a request before what its units have paid for.
A thread that wants more than its units waits on the spent queue for its virtual time, or runs on spare time,
and delays no other thread's requests.

**Why no account.**
A balance its units filled and a cap kept it from overfilling would let a thread save by waiting and spend it in a burst,
so a thread whose account was full would be due before every thread whose was not, whatever their spans; decision 39.
A thread whose bursts outrun its units waits for them, by its own span at most,
and one that needs a burst is given the units for it, or spare time.

**A bind keeps the virtual time, and so do the units.**
A place bound to other units keeps its virtual time, and takes the latest of its own and the units':
each unit keeps the virtual time of the place that earned it last, `unit_vt`, as the place leaves it,
so a thread that ran ahead cannot shed it by handing its units to another thread, or to a new one, and back.
Units nobody earned since boot keep none, so a thread bound to them starts at the clock and is due at once.
This is a word for each unit of the machine, where the units are a fixed few, as the timer lines are.
`OP_TEST_TRACE` brings every virtual time to the clock as tracing begins, the units' too, and a second one brings none.

**A turn is charged by the counter.**
A change of turn reads the clock and charges the place paying for the turn leaving for the counts since its turn began or was last charged,
`TIME_UNITS / u` counts of virtual time a count, rounded up, up to the end of the turn's request,
past which a turn runs on spare time, as one alone on the core does once nobody wants the core,
which moves its own place's virtual time on up to a span past the clock and costs a lender nothing.
What would change a charge charges the turn first, at what it ran on:
a bind or unbind of the units it runs on, its payer's or its own on lent time, on any core;
a wait's span, which bounds a charge on spare time; a lend, whose borrower may start a request on the units that pay for the turn;
and a turn handed back to its lender, which the borrower ran on the lender's request.
The divisions are a word wide, since the kernel divides no wider.
Whether a turn is paid is decided as it begins and as its request ends, and holds until the next.
Only the running thread's place is charged; the others' virtual times stand while they wait.
A thread with units that waits without time has time again once its virtual time comes within a span of the clock,
and the kernel keeps each core's release, the earliest count at which that happens for any such thread of the core,
and none once no thread waits on the core's spare and spent queues, so the timer does not wake for a thread that has had its turn since.
Only at the release does it look at the core's `TIME_UNITS` units, as the timer looks at the timer lines;
every thread with units earns one of them, so the look finds them all,
and a thread's units are a run of them, so the look steps over a thread's at once.

A thread that waits hands the processor to the next, and the timer ends the turn whose request is over while another thread waits:
the thread goes back to the queue its virtual time and binding now put it on, with the next request if its units give it one.
A thread that becomes ready, resumed, woken or bound, joins its queue with a new request.
Finding the next thread walks the run queue, at most `TIME_UNITS` places,
and the scheduler's state is the three queues, each place's virtual time and its request's deadline,
the place that earns each unit and the virtual time each unit nobody earns was left at,
the release, the running thread, and the count its turn is charged from.
With every thread on spare time alone this is the plain round-robin over threads,
which is how the replay driver runs; see "Verification".

**A waiter may lend its time to a thread it names**, with `OP_NOTIFY_LEND`; see "Communication and synchronisation".
A borrower without time of its own has the run queue while its lender's units give it time,
or the spent queue while they will, and the release looks at the lender's virtual time as at its own.
Its lender is the thread that has lent it time longest, the first on its ring of lenders.
Such a turn is charged to the lender, the turn's payer, as it would be to the turn's thread,
and each request decides afresh who pays, so a turn on lent time ends with its request:
a request in progress goes on, on the units it was made on, while it is open there,
and the next is made on the borrower's own units if they give it time, else on its lender's.
A request is open only on the units it was made on, which a bit beside the request's mark says,
so a borrower does not carry a deadline made at its own span onto its lender's units.
A turn whose payer has no request of its thread open on its units, their request over or the units gone, runs on spare time.
A lender whose own time pays for its turn, and that the borrower now borrows from,
hands the rest of the turn and of its request to the borrower as it begins to wait, if the borrower waits for a turn;
and a signal of the turn's thread that wakes the payer hands it the rest of the turn back.
A payer stopped, or woken from another core or by an interrupt, pays on until the turn ends, at its request's end or the borrower's wait,
since whether a turn is paid is decided as it begins and as its request ends;
resumed meanwhile, it joins its queue as the turn ends.
A lender lends only on a core whose time both it and its borrower hold, its place's virtual time there,
since units are a part of one core and pay for no other's time.
A first lender with no time for its borrower lends on what it borrows, up to `LEND_DEPTH`, 64, lenders up.
Every lender waits, so of a chain only the thread at its end may wait for a turn, and what settles a chain settles that end:
the release looks down from a waiting lender to the end, and the end's settle looks up for the first lender with time,
`TIME_UNITS` times twice `LEND_DEPTH` steps at a release at most.

**What it promises.**
Threads with time run by their requests' deadlines, and then threads on spare time take turns.
A thread that wants the processor all along gets its units' part of it, give or take a request,
and without spare time no more.
A thread a process adds earns only units split off the process's own, or runs on spare time,
so it takes nothing another thread earns, and a child gets time only through units its creator lends it.
Spare time goes round by thread, so more threads there get more of it:
it is the time nobody earned, and a thread that needs a part of the processor is given units.
A borrower on lent time has the turns its lender would have had, charged to the lender.

**How long a thread with time waits for its turn** is bounded by its request and by the kernel's steps:
made ready with its virtual time at or behind the clock, it has run `u * s / TIME_UNITS` counts by a span `s` later, as above,
and later by what it ran ahead of its units before, a span of its last request at most.
Only requests due before its own run first, each paid by units of the core,
or by the one thread that lends to their thread first, which waits and so runs no request itself;
handing a turn to a borrower, or back to its lender, hands on the request in progress,
and a turn on spare time ends as the thread becomes ready.
A call stopped for an interrupt, which its thread makes again first, delays it by the rest of that call.
The thread's span sets the bound, against the switches a shorter span costs it while threads compete.
On top come the kernel's steps: the longest it takes without looking for an interrupt,
and on several cores one such step of each other core, which the lock waits for,
and the trap that ends the last request ahead, which looks at the timer lines and the releases.
They add to the wait once, and once more for each request ahead whose end falls in such a step;
the longest today, zeroing a table of `CAPTABLE_MAX_SLOTS` slots, `TODO.md`.
A thread made ready from another core waits besides for the rest of that core's trap, the interrupt between them, and the lock.
A timer line's thread starts within those steps and its span of its deadline, so a periodic line keeps its deadlines and its thread starts within as much of each;
a device's driver within them of the trap that took its interrupt.
It all holds while tracing is off, since under tracing only a record moves the clock,
and while the thread keeps its binding: a bind, even to more units, starts its request afresh at the back of its queue.
A walk that wakes the thread, a pool destroy's, stops at its next step for the timer to be set and another core to be told,
so it holds the thread no longer than a step; see "Bounded work".

**How much work a thread gets** is bounded by its units, not by its turns.
Over any stretch the other threads of its core run at most their units' part of it and a request each,
so a thread with u units that wants the processor all along gets at least u sixty-fourths of the stretch,
less a request of each other thread, and the kernel's steps.
Requests bound it closer: work of `c` counts begun with the virtual time at the clock is done within `TIME_UNITS * c / u` counts,
rounded up to whole requests, whatever the other threads did before.
A periodic task of cost C and period T has time at every release when its units earn the cost, `u * T / TIME_UNITS` at least C,
and its requests due within its period, a span of T at most;
C counts what the kernel charges to the task's turns too.
On several cores a driver on another core whose arms each interrupt the first, `TODO.md`, takes the kernel's share of the first core's turns too.
No other latency is promised;
what a real-time promise would take is open decision 36.
A thread with no ready work spends nothing, so an idle holder of units costs the others nothing.

**Why a fixed number of units.**
Units made of memory would buy time with memory again, unless each were conserved on its own.
A limit per process or per thread bounds nothing, since anyone can make more of those.
A fixed count is conserved for free, and it bounds the release:
only a thread with units waits for its virtual time, and each earns a unit,
so the release looks at the units and at nothing else,
where virtual times on objects made of memory would need a queue of releases sorted by time,
as seL4's scheduling contexts have.
Sixty-four makes a unit a fine enough part of the processor and the release a look at 64 words at most,
one for each thread with units and each unit nobody earns.

**Why threads earn, and not shares.**
Before the units, the processor was 32 shares: kernel objects threads were bound to,
each with a budget moved by a call of its own, and an account its threads spent together.
More shares bought more turns and more budget more time, two knobs for one thing, and units are one.
The price is that threads cannot spend one virtual time together:
a process splits its units among its threads, or runs them on spare time.

**There is no tick.**
A thread runs until it waits on a notification, until it faults,
or until the timer takes the processor from it at the end of its request, or a thread due first becomes ready.
The kernel has one clock, the counter's counts, and sets each core's timer for the first count that could change what runs:
the earliest of the deadline of a timer line armed on the core or of the watchdog it fed, the release,
the end of the turn's request unless its thread may go on alone on spare time, where its virtual time changes nothing that runs,
and a span into a turn on spare time while another waits for spare time.
So a thread alone on the processor is not interrupted at all, and an idle core sleeps until one of those comes.
Whatever could change one of them happens in a trap and marks the timer stale:
settling a thread, binding or unbinding one, a switch, the timer, an arm, the stall.
A trap sets the timer again only then, and choosing the count costs no walk,
since an arm brings its core's nearest deadline forward and the timer's look at the timer lines makes it exact.
A trap that begins with the timer's interrupt pending does what is due first, before anything reads a virtual time,
so what is due is done by whichever trap comes first.
Under tracing the clock moves only by record, a millisecond at a time, and each record ends the turn, as a tick did;
see "Verification".
Interrupts are taken in user mode only:
machine mode runs with `MIE` clear from the trap to the `mret`,
so a system call is never interrupted, and on one core the kernel needs no locks;
several cores share one, see "Cores".

When a thread waits and nothing is runnable,
only an `Irq` armed on a timer line or a device's line can make one runnable again,
because only a running thread can signal otherwise,
or the release, when a thread on the spent queue waits for its virtual time.
The kernel counts those `Irq`s as they are armed and disarmed,
so it knows without a walk, and the spent queue it looks at.
With any of them the core idles once the trap is over, `sched_idle`, and stalls in `wfi`
until the nearest deadline of a timer line armed on the core or of the watchdog it fed, the release,
or a device interrupt is pending,
takes it by hand since machine mode runs with `MIE` clear,
and looks for a runnable thread again;
with none it says `no runnable thread` and stops the machine, watchdog or not.
On several cores another core running a thread can make one runnable too, and interrupts the core for it,
so the machine stops only once no core runs a thread or has one waiting; see "Cores".

**Nothing more lives in the kernel.**
Earliest deadline first over requests is the least policy that makes
a spinning thread harmless and a woken thread run by the span its own wait sets, within its units,
units each earned by one thread are the least that make a thread cost nobody but whoever gave it units,
and a virtual time per place is the least that caps its time without keeping the others busy.
Priorities, a period per thread and a calendar were weighed against it; see decisions 9 and 39.

## The watchdog

**It halts a machine that stopped working.**
A system whose threads all wait for what never comes runs on and does nothing.
The watchdog halts the machine unless someone says, often enough, that all is well.
It halts rather than resets, so the log is written out where the board has a way to,
and what follows is the board's: BOOTSEL on RP2350.

**One watchdog, fed through the clock.**
`OP_CLOCK_WATCHDOG` on a clock with `RIGHT_W` arms the machine's one watchdog, or moves its deadline, nearer or further:
the machine halts with code 7 unless the call comes again within the delay it names, at most `WATCHDOG_US_MAX`, ten seconds.
The deadline is the one a timer line would have, the first count past the call by at least the delay,
and the timer looks at it as at the timer lines, so it costs no walk.
The core that fed it last wakes for it, as the core that armed a timer line does,
so a machine whose threads all wait halts on time.
Nothing disarms it, since a watchdog that can be stopped is one a broken system may have stopped.
The bound keeps a deadline below what a board's own watchdog can count.

**Whether all is well is the holder's to judge.**
The kernel learns only that the holder fed it.
A root task that feeds it from its loop says that it runs, not that its children do;
one that feeds only once each child has said it is well, by a word in its page or an answer on its channel, says more.
A child given `RIGHT_W` can keep the machine running whatever the root task does, or halt it by never feeding.
A watchdog for each process would be a timer line:
what a parent does about a child that hangs, take it down and build it again, is not a halt.

**A board's own watchdog is the kernel's.**
A frame over one would be the machine's, as a bus master's would:
RP2350's watchdog keeps, beside its count, the registers that tell the bootrom what to run after the reset,
which its holder could point at code of its own, run in machine mode.
So no board lists a watchdog among its devices, and the kernel keeps the board's for when the kernel itself stops:
each feed sets RP2350's to reset the chip a second after the kernel's deadline, which only a kernel that cannot halt reaches.
Until the first feed it counts as the boot set it, so a run that never feeds still ends within seventeen seconds.
The ESP32-C6's stay off, and QEMU's boards start none; see "Boards".

**Why on the clock.**
The watchdog is a deadline in the machine's time, so a right on the clock names it,
with no new type and no boot slot, which would have renumbered the seeds and the corpus.
The rights keep the two apart: a copy with `RIGHT_W` alone feeds the watchdog and cannot read the time,
and one with `RIGHT_R` alone reads the time and cannot feed it.
Whether the kernel should keep a watchdog at all is open decision 26.

