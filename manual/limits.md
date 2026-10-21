# rvuos user manual: limits and what is not there yet

Chapters 10 and 11 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 10. Limits

| Limit | Value | Where |
|---|---|---|
| region slots per process | 1 to the PMP entries the kernel uses, as its creator asks: 16 on QEMU and the ESP32-C6, 8 on the ARM boards, 7 on RP2350's Hazard3 | `OP_POOL_ALLOC`; `OP_CAP_INFO` on a process; `PROCESS_REGION_SLOTS_MAX` |
| root task's region slots | 8, or fewer where the core has fewer entries | `ROOT_REGION_SLOTS` |
| table slots per process | 2 | `PROCESS_TABLES` |
| places per thread | 1 to the machine's cores, as its creator asks | `OP_POOL_ALLOC`; `OP_CAP_INFO` on a thread |
| PMP entries the kernel uses | 16, or `PMP_MAX_ENTRIES` | Makefile |
| minimum PMP entries to boot | 4 | `kernel/main.c` |
| slots per `CapTable` | 1 to 1024 | `CAPTABLE_MAX_SLOTS` |
| a slot number | `SLOT_IN(table slot, index)`: the table slot from bit 10 up, the index below | `SLOT_TABLE_SHIFT` |
| root task's table | 46 slots past the boot capabilities, 67 in all on QEMU | `ROOT_TABLE_SLOTS` |
| boot pool | 4 KiB | `BOOT_POOL_SIZE` in `kernel/layout.h` |
| smallest pool | 64 bytes | `POOL_MIN_SIZE` |
| object alignment | 16 bytes | `OBJ_ALIGN` |
| an object's size | section 5.5 | `OP_POOL_INFO` |
| notification bits | 32 | the word size |
| timer lines | 16, on the whole machine | `TIMER_LINES` |
| a router's channels | 19 on the nRF52840, 4 that wire nothing on QEMU's boards, none elsewhere | `OP_CAP_INFO` on `BOOT_CAP_ROUTES` |
| units of time | 64 of each core | `TIME_UNITS` |
| cores | 1, or 2 on QEMU `virt`, mps2-an521 and RP2350 with `CORES=2` | Makefile; `OP_CAP_INFO` on `BOOT_CAP_TIME`, its count over `TIME_UNITS` |
| timer delay or period per call | 2^32 - 1 µs | `OP_IRQ_SET` |
| watchdog's delay | 10 s | `WATCHDOG_US_MAX` |
| default span, how soon a request is due that no wait or bind gave a span for, and a turn on spare time while another waits | the longest power of two of counts within a millisecond | `timer_default_span` in `kernel/timer.h`; `OP_CAP_INFO` on a `Clock` |
| span, how soon a request is due | the default span, or a power of two of counts up to 2^29 | `SPAN_MAX`; `OP_TIME_BIND`, `OP_NOTIFY_WAIT`, `OP_NOTIFY_LEND` |
| a step of the clock under tracing, `OP_TEST_TICK` | a millisecond of the counter's counts | `timer_step_counts` in `kernel/timer.h` |
| interrupt lines | 96 on QEMU, 77 on the ESP32-C6, 52 on RP2350, 32 on mps2-an385, 48 on mps2-an521, line 0 the log's | `IRQ_LINES` |
| smallest region | 8 bytes, or the PMP grain if coarser, 32 bytes on ARM | `region_min_size` in `kernel/pmp.h`; `OP_CAP_INFO` on any frame |
| kernel log ring | 4 KiB less a 32-byte header | `KLOG_SIZE` |
| replay records per input | 256 | `REPLAY_MAX_RECORDS` |

## 11. What is not there yet

These are documented gaps, not surprises;
`TODO.md` carries the items and `DESIGN.md` the open decisions.

- **A watcher on ARM cannot set the flags**, nor step a thread past an instruction inside an IT block;
  `DESIGN.md`, open decision 23.
- **No priorities and no yield.**
  Earliest deadline first over requests, threads with time before threads on spare time,
  is the whole policy, and units promise their part of each span of the processor, no more,
  but for the time a waiter lends the thread it names;
  `DESIGN.md`, open decisions 9 and 10.
- **No synchronous endpoints.**
  Shared memory and notifications carry everything; open decision 5.
- **Lent time goes through up to 64 lenders, on a core all of them hold.**
  A thread borrows from the thread that has lent it time longest,
  and, while that one has no time for it, from that one's own first lender, and on;
  never through a borrower that waits without lending, nor on a core whose time a lender does not hold;
  a lock that names its holder is fair only to takers that lend; open decision 27.
- **One `Irq` per line**; open decision 11.
- **Two cores at most**, on QEMU `virt`, mps2-an521 and RP2350;
  the first core takes every device interrupt;
  `DESIGN.md`, open decision 24.
- **ARM with no replay yet**, under QEMU and on RP2350's Cortex-M33; `TODO.md`.
- **No boot from flash.**
  The ESP32-C6 and RP2350 run from RAM, loaded by their ROMs over USB.
- **No loader.**
  The image embeds one program, linked at fixed addresses;
  a second process runs code from the same region.
- **The log across a reset** is not yet trustworthy; open decision 12.
