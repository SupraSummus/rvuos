# rvuos design: time and scheduling

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Time

**There is no sleep.**
A thread that wants to stop for a while wants to stop until something happens,
and a notification is the one way to stop;
what is missing is only something that happens at a time.
So time reaches userspace as one more interrupt, on a timer line.
The kernel has `TIMER_LINES` of them, numbered after the controller's,
and the tick raises them where a device would raise its own.
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
The delay is in microseconds.
The call lands anywhere within a tick,
so the kernel fires at the first tick that surely lies past the delay:
the delay rounded up to whole ticks, plus one.
The tick's period is the board's business and not part of the ABI.
An upper bound is not promised because the kernel could not keep one:
the woken thread is ready, not running, and waits for its turn,
and for its account to reach a tick if it spent it and has no spare time.
A timer line that fires disarms its `Irq`, as a device's line does,
and setting an armed one moves its deadline rather than adding a second.

**A period counts from the deadline.**
A periodic task arms the line again each time it wakes,
and a delay counted from the wake would put every wake's lateness into the period.
`OP_IRQ_SET` with `IRQ_SET_PERIOD` counts from the line's last deadline instead,
or from its bind if it never fired:
the line fires at the first tick after the call
that lies a whole number of periods from that deadline,
and the call returns how many such ticks had already passed.
The cases a task needs follow from that one rule.
Set on each wake, the line keeps its period without drift.
A task that woke late skips the periods it missed and learns how many,
so it can do their work at once, note the overrun, or ignore it.
Set again before it fired, the line keeps its deadline.
The deadline is never more than a period away, however stale the last one.
The period is rounded up to whole ticks, as a delay is.

**Why not a periodic line.**
A line the kernel arms again on its own would save a task one call per period,
but periods fired before the thread wakes would merge into one bit,
and a task that wanted one wake would race the next to disarm the line.
It would also be the one line that does not mask itself as it signals.

**Why not an absolute deadline.**
A deadline in the clock's counts would be the simplest rule to state,
but every task that sleeps would need the clock as well as the line,
and under tracing, where time moves only by record,
the host's tick count would have to start where QEMU's does.

**Why a fixed number of lines.**
The tick has to find the due timers without looking at the others.
Timers as objects in pools would have it walk every pool, which goal 4 rules out;
a sorted queue moves that walk to the set, and a wheel to the bucket that comes due.
A limit per process, thread or pool bounds nothing, since anyone can make more of those.
A limit on the whole machine does: the tick looks at the `TIMER_LINES` lines and nothing else.
Lines make it a limit of authority rather than of who comes first:
the root task receives them all in `BOOT_CAP_TIMER_LINES`,
apart from the controller's so that no board's count shows,
and hands them out and takes them back as it does memory.
The count is a constant of the kernel, like the region slots,
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
The kernel counts a 32-bit counter's wraps for its tick anyway, so the MPS2 boards' time is whole too,
and under tracing the time is the tick count's, as every clock is there; see "Verification".
The rate is the board's, and on the ESP32-C6 measured at boot, so a program takes it from the clock.

**And the counter's frame, for a reader that cannot trap.**
A call is a trap, which a program that measures what the kernel does to it cannot afford:
a thread spinning to see whether the tick or another core takes the processor would trap at every read.
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
On the ESP32-C6 it can time its own turn on the core's performance counter,
which the kernel stops at zero whenever another process's thread runs; see open decision 22.
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
A thread runs on the core its units are of, and only there; see "Cores".
A `Time` capability names a range of units and no object, as an `IrqLine` names lines,
and `OP_TIME_CARVE` takes a smaller range out of one.
`OP_TIME_BIND` binds a thread to some of a capability's units, or to none,
and moves it off the units it earned before.
The kernel records which thread earns each unit and refuses a bind that names a unit another thread earns,
so a capability may be copied, but each unit's time goes to one thread,
and the time of the units nobody earns is spare.
The binding is a node of the derivation tree held in the thread, below the capability it was bound through,
so revoking above that capability unbinds the thread, as it unmaps a region installed from a frame.
A thread with no units and no spare time keeps its state and does not run:
that is how a scheduler in userspace stops and starts a thread, open decision 9.
A thread that loses its units while it runs finishes the turn it had,
so only a wait, the tick, a fault, or a revoke that takes its own process takes the processor from a running thread,
and a call stopped for an interrupt is made again before anything else runs.

**A thread has an account.**
An account counts in parts of a count of the counter, `COUNT_PARTS` of them to a count.
Every tick a thread's account gains a tick's counts of parts for each unit it earns,
so a thread that earns the whole processor gains a tick every tick.
It holds at most `ACCOUNT_TICKS` ticks' worth per unit, a tenth of a second's,
so a thread that idled comes back with that much at most.
The ESP32-C6 measures the counter's rate at boot, so the timer starts before the root thread's account is filled.
A new thread's account is empty, and the root thread's starts full.
A thread bound to other units keeps what its account held, up to what the new units hold,
and an unbound one loses it, so no bind puts time into an account.
`OP_DEBUG_TRACE` fills every account as tracing begins, and a second one fills none;
no call tells what one holds, which would be a clock.

**A thread has time while its account holds a tick.**
A turn lasts at most to the next tick, and every count of a turn the thread began with time costs it `COUNT_PARTS`,
while it earns its units as in any tick.
A turn with time began with a tick in the account, so it never costs the account more than it holds.
A thread with less than a tick has no time until its account reaches a tick again.
Spare time is the turns no thread with time wants.
A thread bound through a capability with `RIGHT_X` runs on it, for free, while its account fills as usual.
The boot grant has `RIGHT_X`, so a thread bound through it to no units runs whenever the threads with time let it.
A thread without `RIGHT_X` runs at most its units' part and a full account,
and its core sleeps in `wfi` for the rest: that is how the root task caps a thread's time.

**The queues are the policy: round-robin, time before spare time.**
A ready thread that may run, and whose turn it is not, waits on one of three queues,
each a ring of threads, oldest first, through the thread's own `queue_next` and `queue_prev`,
which a waiting thread uses for its notification's waiters instead:
the run queue while it has time, the spare queue without time if it may run on spare time,
and the spent queue otherwise, where a thread with units waits for its account.
The oldest thread on the run queue has the next turn, or with the run queue empty the oldest on the spare queue.
A thread that waits hands the rest of the tick to the next, and the tick ends the turn:
the thread goes to the back of the queue its account and binding now put it on.
A thread that becomes ready, resumed, woken or bound, joins the back of its queue.
Finding the next thread takes constant time however many objects exist,
and the scheduler's state is the three queues, each thread's account and the tick it was counted to,
the thread that earns each unit, the release, the running thread, and how far into the tick its turn is charged.
With every thread on spare time alone this is the plain round-robin over threads,
which is how the replay driver runs; see "Verification".

**A turn is charged by the counter.**
A change of turn reads how far into the tick the counter is
and charges the thread leaving for the counts since its turn began or since the tick, whichever is later;
a tick charges the thread whose turn it is for the rest of the tick and earns it its units,
whether the timer interrupted for it or a later trap counted it.
Whether a turn is paid is decided as it begins and at each tick, and holds until the next.
Only the running thread's account is counted at each tick;
the others are brought up to the count when the kernel looks at them.
A thread with units that waits without time has time again once its account reaches a tick,
and the kernel keeps each core's release, the nearest tick at which that happens for any such thread of the core.
Only at the release does it look at the core's `TIME_UNITS` units, as the tick looks at the timer lines;
every thread with units earns one of them, so the look finds them all.
A trap that counts several ticks at once charges them as if each had been taken, in constant time;
`account_after` in `kernel/sched.c` says how.
So a thread that waits just before every tick pays for its time all the same,
and one whose turn begins late in a tick pays only for the rest of it.

**What it promises.**
Threads with time take equal turns, and then threads on spare time do.
A thread that wants the processor all along gets its units' part of it,
give or take a tenth of a second's worth of the accounts, and without spare time no more.
A thread a process adds earns only units split off the process's own, or runs on spare time,
so it takes nothing another thread earns, and a child gets time only through units its creator lends it.
Spare time goes round by thread, so more threads there get more of it:
it is the time nobody earned, and a thread that needs a part of the processor is given units.
A thread with time waits for its turn at most `TIME_UNITS` ticks,
since only threads with units of its core have time there and each takes a tick.
No other latency is promised.
A thread with no ready work spends nothing, so an idle holder of units costs the others nothing.

**Why a fixed number of units.**
Units made of memory would buy time with memory again, unless each were conserved on its own.
A limit per process or per thread bounds nothing, since anyone can make more of those.
A fixed count is conserved for free, and it bounds the release:
only a thread with units waits for its account, and each earns a unit,
so the release looks at the units and at nothing else,
where accounts on objects made of memory would need a queue of refills sorted by time,
as seL4's scheduling contexts have.
Sixty-four makes a unit a fine enough part of the processor and the release a look at 64 words.

**Why threads earn, and not shares.**
Before the units, the processor was 32 shares: kernel objects threads were bound to,
each with a budget moved by a call of its own, and an account its threads spent together.
More shares bought more turns and more budget more time, two knobs for one thing, and units are one.
The price is that threads cannot spend one account together:
a process splits its units among its threads, or runs them on spare time.
A share had time again only once its account was full, which kept a capped share's idle time in one stretch;
a thread has time again at a tick, which bounds how long it waits; open decision 18.

**The machine timer provides the tick.**
A thread runs until it waits on a notification, until it faults,
or until the tick takes the processor from it.
The tick has a fixed period, `TIMER_HZ` in `kernel/timer.h`,
and one tick is one turn: a preempted thread goes to the back of its queue.
A turn that begins when a thread waits mid-tick lasts only to the next tick;
`TODO.md` carries a turn counted from the switch.
Interrupts are taken in user mode only:
machine mode runs with `MIE` clear from the trap to the `mret`,
so a system call is never interrupted, and on one core the kernel needs no locks;
several cores share one, see "Cores".

When a thread waits and nothing is runnable,
only an `Irq` armed on a timer line or a device's line can make one runnable again,
because only a running thread can signal otherwise,
or the release, when a thread on the spent queue waits for its account.
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

**The timer interrupts only for a tick that could change what runs.**
A tick that ends a turn only to hand the processor back to the same thread moves the counts and nothing else.
So while another thread could have the next turn, `mtimecmp` is set for the next tick,
and otherwise for the nearest of the deadline of a timer line armed on the core or of the watchdog it fed, the release,
and the tick the running thread's account drains, unless it may go on alone on spare time,
where its account changes nothing that runs.
The account drains only at a tick, since a turn with time never outruns it before the next tick,
so the timer stays on the tick grid.
In the stall it is the nearer of the deadline and the release.
A deferral reaches at most 2^31 counts, several seconds on either board,
so that `timer_next` does not take it for a debugger's stop;
a longer one wakes there and is set again.

**Every trap counts the ticks the timer let pass.**
On entry, before anything reads the count or an account, a trap charges and counts them as the tick would have,
those other cores counted since among them,
and a trap whose count reaches the tick the timer was set for ends the turn as it returns.
It reads the counter only while the timer is set past the next tick or its interrupt is pending;
a change of turn reads it besides, to charge the turn.
Whatever could call for another tick happens in a trap and marks the timer's tick stale:
settling a thread, binding or unbinding one, a switch, a tick, an arm, the stall.
A trap sets the timer again only then, and choosing the tick costs no walk,
since an arm brings its core's nearest deadline forward and the tick's look at the timer lines makes it exact.
The counts, the charges, the deadlines and the turns come out as if every tick had been taken, a late tick aside,
while a thread alone on the processor is not interrupted every tick.
The price is a few loads on every trap: on QEMU a tenth of the cheapest system call,
a sixth while the counter is read.

**Nothing more lives in the kernel.**
Round-robin on a tick is the least policy that makes
a spinning thread harmless and a woken thread eventually run,
units each earned by one thread are the least that make a thread cost nobody but whoever gave it units,
and an account per thread is the least that caps its time without keeping the others busy.
Fixed priorities were the intended end state of this section
and are now open decision 9.

## The watchdog

**It halts a machine that stopped working.**
A system whose threads all wait for what never comes runs on and does nothing.
The watchdog halts the machine unless someone says, often enough, that all is well.
It halts rather than resets, so the log is written out where the board has a way to,
and what follows is the board's: BOOTSEL on RP2350.

**One watchdog, fed through the clock.**
`OP_CLOCK_WATCHDOG` on a clock with `RIGHT_W` arms the machine's one watchdog, or moves its deadline, nearer or further:
the machine halts with code 7 unless the call comes again within the delay it names, at most `WATCHDOG_US_MAX`, ten seconds.
The deadline is the one a timer line would have, the first tick that surely lies past the delay,
and the tick looks at it as at the timer lines, so it costs no walk.
The core that fed it last wakes for it, as the core that armed a timer line does,
so a machine whose threads all wait halts on time.
Nothing disarms it, since a watchdog that can be stopped is one a broken system may have stopped.
The bound keeps a deadline far inside the tick count's half range, by which deadlines compare,
and below what a board's own watchdog can count.

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

