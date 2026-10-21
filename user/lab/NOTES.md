# The laboratory: notes

`user/lab/` is a made-up system of servers and clients,
to weigh ways of modelling communication between processes against one workload rather than argue them.
`make lab` prints how long each scenario's clients waited, and holds the numbers to nothing.
Under QEMU a run repeats exactly, but a small change to a role moves an answer by a request here and there,
so scenarios are compared within one run.

## The cast

- **store**, a server whose answer costs the work the ask names, serving one ask from each client in turn,
  as the Wi-Fi system's network process does.
- **gate**, a server in the middle, passing asks on to the store, as the network process does to the radio's driver.
- **ui**, a client that every three milliseconds works up to half a millisecond of its own, drawn afresh, then asks for about 25 µs of work;
  the draw puts its asks anywhere in another thread's request, as events fall.
  `local` does the work itself, as a library linked into it would.
- **bulk**, a client that asks for half a millisecond of work, one ask after another.
- **hog**, a process that wants the core all the time, earning 32 of its 64 units wherever it runs.
- **quick** and **slow**, sharing a lock:
  quick, with 16 units, takes it every period for a little work;
  slow, with 2, holds it long and works as long between holds.
  The lock is the one of `lib/lock.h` in `lock`, `lock-hog` and `lock2-hog`,
  and its lock that names the holder in `lend`, `lend-hog` and `lend2-hog`, so that a taker that waits lends the holder its time.
  In `lock2-hog` and `lend2-hog` **quick2** takes it too, as quick does, with the 6 units the hog leaves.
- **burst**, a process that every seven milliseconds works about half a millisecond of its own, earning 32 units,
  so that it runs within its units and its requests are due as soon as anyone's.
- **drivers** are two pairs of a ui and a store, each pair an interrupt and the driver that answers it, beside the burst:
  each store waits for its asks within a millisecond and each ui for its answers within half of one,
  which is as soon as requests of 32 µs of work at their units are due;
  `drivers-tick` is the same with the default span, given no span, and the `-hog` ones are beside the hog instead.
- **lend-server** and **lend-rider** are `poor-server` and `free-rider` with a ui that lends the store its time
  while it waits for an answer.
- **driver**, woken every three milliseconds by its timer line, standing in for a device's, to do about 25 µs of work.
  It says how much later than its earliest each wake started, the earliest found alone, before the rest of the cast starts.
  `driver-hogs` puts a second hog beside it, **hog2**, with 16 units.

The root task keeps 8 units, the units nobody earns are spare time, and every member may run on it.

Threads run in requests, the one whose request is due first first,
each due within the span a member's wait gives, or the default span, the longest power of two of counts within a millisecond, for none;
the scenarios ending `-batch` bind the hog, which never waits, to requests due within a quarter of a second,
and `lax-server`'s store waits within a second, each the longest power of two of counts within it.
In `net` a ui that lends asks a gate that lends, of no units, which passes the ask to a store of no units, beside the hog,
so only lent time that goes on through the gate answers it; `net-unpaid` is the same chain lending nothing.

## What it showed

`make lab` prints the numbers, which a run under QEMU repeats exactly;
the commits that changed the lab or how threads take turns say what they were.

**A thread within its units is due first; one past them waits for them.**
A thread that wants the core all along runs about a span ahead of the clock,
so its requests are due after those of a thread that wakes with a span no longer than its own:
beside the hog the median answer is as fast as alone, in `hog`, `chain` and `net`.
The p90 is about one of the hog's requests:
the ui's draws, up to half a millisecond of work at 8 units, outrun its units, and it waits for them as any thread does,
for its asks in `hog` and for its own work in `local-hog`.
With twice the units, `rich-ui`, it stays within them, and its p90 is as alone.
Units buy latency for a thread that stays within them.

**Spare time keeps no urgency.**
Where every member wants more than its units, `poor-server`, `bulk-hog`, `free-rider`, `lax-server`,
a thread that ran on spare time is a span ahead, so nobody is due before the others for long,
and the waits are a millisecond or two.

**The client's time reaches its server only lent.**
With no units the store runs on spare time alone, `poor-server`, and its client waits about the hog's request at the median.
A client that lends the store its time while it waits, `lend-server`, is answered as fast as alone at the median:
its wait hands the store the rest of its turn, and the answer's signal hands the turn back.

**A lender pays for whatever its borrower does.**
In `lend-rider` the ui lends to a store that is mostly on the bulk's big asks,
so the ui pays for the rest of the ask in progress and waits for it, about a big ask at the median.
The store cannot tell an ask that brings time from one that does not.

**A small ask waits behind big ones.**
In `bulk`, with no hog, the ui waits for the big ask the store is on, ten times as long as alone.
That is the store's own order, which no kernel would change.

**A second hop costs no more than the first** beside the hog, `chain` against `hog`.

**A lock's holder past its units holds the lock at its units' pace.**
Slow, at 2 units, works as long between holds as it holds, far more than its units' part,
so beside the hog quick waits for slow's units even at the median, `lock-hog`.
A taker that lends, `lend-hog`, pays for the hold with its own time and takes about a third off that wait;
a holder that holds longer than its units pay for needs more of them.
Without the hog, quick's p90 is a little longer when it lends, `lend` against `lock`; why is not worked out yet.

**A driver waits for a request due before its own.**
In `drivers` the spans are no shorter than the default,
so a wake during the burst's request is due after it and waits for its rest, up to about half a millisecond;
a driver that must answer sooner gives a shorter span.
A driver its timer line wakes starts at its deadline beside one hog or two, `driver-hog` and `driver-hogs`,
and on spare time alone, `driver-spare`, waits for the hog's request in progress, the same each time.

**A mutex in the kernel got the same from the lending, and a convoy from the ownership.**
The kernel's mutex of open decision 27, which the lending wait replaced, handed itself to the waiter that waited longest:
that helped without the hog, but beside it the mutex went to quick, which waited behind the hog's turn holding it,
and slow, wanting it again, waited for quick, over a millisecond at the median both.
With its waiters lending, it waited as `lend-hog` did, at two calls for a hold nobody else wanted.
It was measured under the tick of decision 18, before decision 39.

**What it costs**, `make bench`: a hold nobody else wants is no call on either lock of `lib/lock.h`,
where the kernel's mutex made two.
One the waiter waits for costs about a seventh more when it lends, `lock-lend` against `lock-wait`.
