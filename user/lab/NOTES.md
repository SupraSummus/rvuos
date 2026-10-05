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
- **quick** and **slow**, sharing a lock of `lib/lock.h`:
  quick, with 16 units, takes it every period for a little work;
  slow, with 2, holds it long and works as long between holds.

The root task keeps 8 units, the units nobody earns are spare time, and every member may run on it.

## What it showed

QEMU `virt`, one RV32 hart, in µs; `n` counts the answers in the window of 0.2 s, or quick's takes of the lock.

| scenario | member | n | median | p90 | worst |
|---|---|---:|---:|---:|---:|
| alone | ui | 67 | 29 | 29 | 29 |
| local | ui | 67 | 24 | 24 | 24 |
| hog | ui | 67 | 942 | 1692 | 1884 |
| local-hog | ui | 67 | 24 | 24 | 24 |
| rich-ui | ui | 67 | 942 | 1692 | 1884 |
| rich-server | ui | 67 | 942 | 1692 | 1884 |
| poor-server | ui | 67 | 1619 | 1875 | 2003 |
| bulk | ui | 67 | 316 | 449 | 512 |
| bulk | bulk | 373 | 485 | 823 | 995 |
| bulk-hog | ui | 67 | 922 | 1915 | 2311 |
| bulk-hog | bulk | 100 | 1990 | 2518 | 3026 |
| free-rider | ui | 67 | 1565 | 2312 | 2464 |
| free-rider | bulk | 67 | 2690 | 4630 | 4999 |
| chain | ui | 68 | 945 | 1824 | 2596 |
| lock | quick | 67 | 592 | 1930 | 2702 |
| lock-hog | quick | 67 | 0 | 2900 | 4884 |

QEMU's ARM boards run the work in fewer instructions and agree in every shape.

**Units do not buy latency.**
Beside the hog an ask of 29 µs takes 0.9 ms at the median and 1.7 at p90,
and twice the units for the client, `rich-ui`, or for the server, `rich-server`, match it within a fiftieth.
The client that waits hands the rest of the tick to whoever is next on the run queue,
and the server, woken by the ask, joins the queue behind the hog.
Units buy a part of the processor, not a place in that queue.

**A library does not wait.**
`local-hog` takes 24 µs, as `local` does.
The hop to a server costs 5 µs alone and about a tick beside the hog.

**The client's time does not reach its server.**
With no units the store runs on spare time alone, `poor-server`,
and its client waits 1.6 ms at the median though it earns a quarter of the core.

**Who asks is not who pays.**
In `free-rider` a client that earns nothing has 67 big asks answered, about a sixth of the core,
out of the store's units and spare time, and the ui, which earns its own, waits two thirds longer than beside the hog alone.
The store cannot tell an ask that brings time from one that does not.

**A small ask waits behind big ones.**
In `bulk`, with no hog, the ui waits for the big ask the store is on: 316 µs rather than 29.
That is the store's own order, which no kernel would change.

**A second hop cost no more than the first** beside the hog, in this run; why is not worked out yet.

**A lock waits for its holder's turns.**
Quick earns eight times slow's units, and still waits 1.9 ms at p90 for slow to give the lock back, 2.9 beside the hog.
The kernel does not know who holds the lock, so quick's units cannot help slow finish,
and a give wakes quick without handing it the lock, so slow may take it again before quick's turn comes.
