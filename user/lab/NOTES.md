# The laboratory: notes

`user/lab/` is a made-up system of servers and clients,
to weigh ways of modelling communication between processes against one workload rather than argue them.
`make lab` prints how long each scenario's clients waited, and holds the numbers to nothing.
Under QEMU a run repeats exactly, but a small change to a role moves an answer by a tick here and there,
so scenarios are compared within one run.

## The cast

- **store**, a server whose answer costs the work the ask names, serving one ask from each client in turn,
  as the Wi-Fi system's network process does.
- **gate**, a server in the middle, passing asks on to the store, as the network process does to the radio's driver.
- **ui**, a client that every three ticks works up to half a tick of its own, drawn afresh, then asks for about 25 µs of work;
  the draw puts its asks anywhere in a tick, as events fall.
  `local` does the work itself, as a library linked into it would.
- **bulk**, a client that asks for half a tick of work, one ask after another.
- **hog**, a process that wants the core all the time, earning 32 of its 64 units wherever it runs.
- **quick** and **slow**, sharing a lock:
  quick, with 16 units, takes it every period for a little work;
  slow, with 2, holds it long and works as long between holds.
  The lock is the one of `lib/lock.h` in `lock`, `lock-hog` and `lock2-hog`,
  and its lock that names the holder in `lend`, `lend-hog` and `lend2-hog`, so that a taker that waits lends the holder its time.
  In `lock2-hog` and `lend2-hog` **quick2** takes it too, as quick does, with the 6 units the hog leaves.
- **burst**, a process that every seven ticks works about half a tick of its own, earning 32 units,
  so that its account stays full and its requests are due as soon as anyone's.
- **drivers** are two pairs of a ui and a store, each pair an interrupt and the driver that answers it, beside the burst,
  all with requests of 32 µs; `drivers-tick` is the same with requests of a tick, and the `-hog` ones are beside the hog instead.
- **lend-server** and **lend-rider** are `poor-server` and `free-rider` with a ui that lends the store its time
  while it waits for an answer.
- **driver**, woken every three ticks by its timer line, standing in for a device's, to do about 25 µs of work.
  It says how much later than its earliest each wake started, the earliest found alone, before the rest of the cast starts.
  `driver-hogs` puts a second hog beside it, **hog2**, with 16 units.

The root task keeps 8 units, the units nobody earns are spare time, and every member may run on it.

Threads run in requests, a tick long unless a member asks for another quantum, the one whose request is due first first;
the scenarios ending `-batch` give the hog requests of 64 ticks, and `lax-server` gives the store such requests.
In `net` a ui that lends asks a gate that lends, of no units, which passes the ask to a store of no units, beside the hog,
so only lent time that goes on through the gate answers it; `net-unpaid` is the same chain lending nothing.

## What it showed

`make lab` prints the numbers, which a run under QEMU repeats exactly;
the commits that changed the lab or how threads take turns say what they were.
A tick is a millisecond, and every shape below held on QEMU `virt` and on its ARM boards alike.

**Short requests shorten a driver's wait, while its account is full.**
With requests of 32 µs rather than a tick, the drivers' worst answer fell, to a fifth at best, or held,
beside the burst and beside the hog, on QEMU `virt` and its ARM boards, but for the AN521 beside the burst.
On the ARM boards their median halved too, and their p90 came to a few small asks,
where with requests of a tick one pair's was about a fifth of a tick and the other's a small ask.
On the AN521 beside the burst the worst rose instead, to about two fifths of a tick, both pairs at once.
By the accounts' arithmetic, which no trace has checked yet, that one is not the quantum's:
a request is due a quantum past the count at which its account would be full,
and accounts start empty at a bind and take a tenth of a second to fill,
so 126 ms into the window the ui, its account still filling, would be due several ticks later than the burst, whose account was full,
and the answer would wait for the ui's turn behind the burst's.
In `hog` the one slow answer, about half a tick, is the first of the window, while the ui's account still fills.

The paragraphs below were seen while threads with time took turns in order, before decision 39,
and the commits since say how each moved.

**Units do not buy latency.**
Beside the hog a small ask waits about a tick at the median and nearly two at p90,
and twice the units for the client, `rich-ui`, or for the server, `rich-server`, change neither.
The client that waits hands the rest of the tick to whoever is next on the run queue,
and the server, woken by the ask, joins the queue behind the hog.
Units buy a part of the processor, not a place in that queue.

**A library does not wait.**
`local-hog` takes as long as `local`.
The hop to a server costs a few microseconds alone and about a tick beside the hog.

**The client's time does not reach its server, unless it lends it.**
With no units the store runs on spare time alone, `poor-server`,
and its client waits about a tick at the median and nearly two at p90 though it earns a quarter of the core:
the store runs only on turns no thread with time wants, and beside the hog those come when the hog's account runs dry.
A client that lends the store its time while it waits, `lend-server`, is answered as fast as `alone`:
its wait hands the store the rest of its turn, and the answer's signal hands the turn back.

**Who asks is not who pays.**
In `free-rider` a client that earns nothing has its big asks answered, about a sixth of the core,
out of the store's units and spare time, and the ui, which earns its own, waits two thirds longer than beside the hog alone.
The store cannot tell an ask that brings time from one that does not.
A ui that lends, `lend-rider`, is answered as fast as `alone`, and the bulk waits about a tenth longer at the median:
the store answers the ui first, on the ui's time, and the answer hands the ui its turn back.
A lender pays for whatever its borrower does until the borrower wakes it, so a ui that asked while the store was on a big ask
would pay for the rest of that ask; the run's draws did not put one there.

**A small ask waits behind big ones.**
In `bulk`, with no hog, the ui waits for the big ask the store is on, ten times as long as alone.
That is the store's own order, which no kernel would change.

**A second hop cost no more than the first** beside the hog; why is not worked out yet.

**A driver its timer line wakes starts at its tick, behind the threads with time already waiting.**
Beside the hog, `driver-hog`, it started as early as alone every time:
the tick signals the lines before it ends the hog's turn, so the driver joins the run queue ahead of the hog.
Beside a second hog, `driver-hogs`, it waited a whole tick at worst, for the hog with time on the queue ahead of it,
and on spare time alone, `driver-spare`, a tick at p90, whenever the hog had time.
A timer line fires only on a tick, where the hog's turn ends anyway, so this is a device's interrupt at its best:
one that comes between ticks would wait for the rest of the hog's turn.
No board the lab runs on lists a device whose interrupt it could time, so the lab does not show that yet.

**A lock waits for its holder's turns.**
Quick earns eight times slow's units, and still waits a third of a tick at p90 for slow to give the lock back,
nearly two ticks beside the hog.
The kernel does not know who holds the lock, so quick's units cannot help slow finish,
and a give wakes quick without handing it the lock, so slow may take it again before quick's turn comes,
while slow has time of its own:
once slow has spent it and runs on spare time, the give that wakes quick, which has time, hands quick the processor at once.

**A taker that lends its time takes its holder's wait away.**
On the lock that names its holder, `lend-hog`, quick waits under a fifth of a tick at p90 beside the hog,
about what is left of slow's hold:
quick's wait hands slow the rest of quick's turn at once, slow runs on quick's account,
and slow's give, which signals quick, hands quick the rest of the turn back, so quick takes the lock before slow can again.
Slow takes the lock about as often as on the plain lock, and the time it ran so was quick's to spend.
It is where units buy latency, because quick names whom it waits for.
Without the hog, `lend`, quick waits a seventh of the plain lock's p90 or less, as the turn back puts quick first.

**Two takers that lend never wait at once.**
In `lend2-hog` quick waits as in `lend-hog`, and quick2 a third of a tick at most,
where on the plain lock, `lock2-hog`, both wait a tick or more at p90.
Quick's wait hands slow its turn and slow's give hands it back, so the hold ends before quick2 runs.
Only a hold that outlasts a tick would let quick2 wait while quick lends.

**A mutex in the kernel got the same from the lending, and a convoy from the ownership.**
The kernel's mutex of open decision 27, which the lending wait replaced, handed itself to the waiter that waited longest:
that helped without the hog, but beside it the mutex went to quick, which waited behind the hog's turn holding it,
and slow, wanting it again, waited for quick, over a tick at the median both.
With its waiters lending, it waited as `lend-hog` does now, at two calls for a hold nobody else wanted.

**What it costs**, `make bench`: a hold nobody else wants is no call on either lock of `lib/lock.h`,
where the kernel's mutex made two.
One the waiter waits for costs a little less when it lends, since the turn goes to the holder and back without a queue,
and the lending's checks cost every switch a few per cent.
