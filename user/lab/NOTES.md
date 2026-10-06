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
  The lock is one of `lib/lock.h` in `lock` and `lock-hog`, the kernel's mutex in `mutex` and `mutex-hog`,
  and the mutex taken with `MUTEX_LEND` in `lend` and `lend-hog`, so that a waiter lends the holder its time.

The root task keeps 8 units, the units nobody earns are spare time, and every member may run on it.

## What it showed

`make lab` prints the numbers, which a run under QEMU repeats exactly;
the commits that changed the lab or how threads take turns say what they were.
A tick is a millisecond, and every shape below held on QEMU `virt` and on its ARM boards alike.

**Units do not buy latency.**
Beside the hog a small ask waits about a tick at the median and nearly two at p90,
and twice the units for the client, `rich-ui`, or for the server, `rich-server`, change neither.
The client that waits hands the rest of the tick to whoever is next on the run queue,
and the server, woken by the ask, joins the queue behind the hog.
Units buy a part of the processor, not a place in that queue.

**A library does not wait.**
`local-hog` takes as long as `local`.
The hop to a server costs a few microseconds alone and about a tick beside the hog.

**The client's time does not reach its server.**
With no units the store runs on spare time alone, `poor-server`,
and its client waits over a tick and a half at the median though it earns a quarter of the core.

**Who asks is not who pays.**
In `free-rider` a client that earns nothing has its big asks answered, about a sixth of the core,
out of the store's units and spare time, and the ui, which earns its own, waits two thirds longer than beside the hog alone.
The store cannot tell an ask that brings time from one that does not.

**A small ask waits behind big ones.**
In `bulk`, with no hog, the ui waits for the big ask the store is on, ten times as long as alone.
That is the store's own order, which no kernel would change.

**A second hop cost no more than the first** beside the hog; why is not worked out yet.

**A lock waits for its holder's turns.**
Quick earns eight times slow's units, and still waits nearly two ticks at p90 for slow to give the lock back, three beside the hog.
The kernel does not know who holds the lock, so quick's units cannot help slow finish,
and a give wakes quick without handing it the lock, so slow may take it again before quick's turn comes.

**A mutex hands the lock on in order.**
On the kernel's mutex, `mutex`, quick's p90 is a third of the lock's:
slow's give hands the mutex to quick, so slow no longer takes it again before quick's turn.

**But a mutex handed to a thread that waits for its turn is a convoy.**
Beside the hog, `mutex-hog`, quick waits over a tick and a half at the median, where on the lock it mostly waits for nothing,
and slow, which did not wait on the lock, waits over a tick:
the mutex goes to quick, which waits behind the hog's turn holding it, and slow, wanting it again, waits for quick.

**A waiter that lends its time takes its holder's wait away.**
With `MUTEX_LEND`, `lend-hog`, quick waits under a fifth of a tick at p90 beside the hog,
about what is left of slow's hold:
quick's wait hands slow the rest of quick's turn at once, slow runs on quick's account,
and the unlock that hands quick the mutex hands it the rest of the turn too.
Slow takes the mutex about as often as on the lock, and the time it ran so was quick's to spend.
It is the one scenario where units buy latency, because the kernel knows whom the waiter waits for;
a client waiting on a server through a notification lends nothing, `poor-server`.

**What it costs**, `make bench`: a hold nobody else wants is two calls on the mutex, and none on the lock of `lib/lock.h`.
One the waiter waits for costs a tenth less when it lends, since the turn goes to the holder and back without a queue,
and the lending's checks cost every switch a few per cent.
