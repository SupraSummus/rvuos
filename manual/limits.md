# rvuos user manual: limits and what is not there yet

This file holds chapters 10 and 11 of the user manual.
[`MANUAL.md`](../MANUAL.md) lists all chapters.

## 10. Limits

| Limit | Value | Where |
|---|---|---|
| region slots per process | 1 up to the number of PMP entries the kernel uses, as the creator chooses. That number is 16 on QEMU, the ESP32-C6 and the ESP32-C5, 8 on the ARM boards, and 7 on RP2350's Hazard3 | `OP_POOL_ALLOC`; `OP_CAP_INFO` on a process; `PROCESS_REGION_SLOTS_MAX` |
| root task's region slots | 8, or fewer where the core has fewer entries | `ROOT_REGION_SLOTS` |
| table slots per process | 2 | `PROCESS_TABLES` |
| places per thread | 1 up to the number of the machine's cores, as the creator chooses | `OP_POOL_ALLOC`; `OP_CAP_INFO` on a thread |
| PMP entries the kernel uses | 16, or `PMP_MAX_ENTRIES` | Makefile |
| minimum PMP entries to boot | 4 | `kernel/main.c` |
| slots per `CapTable` | 1 to 1024 | `CAPTABLE_MAX_SLOTS` |
| a slot number | `SLOT_IN(table slot, index)`. The table slot is in bit 10 and up, the index in the bits below | `SLOT_TABLE_SHIFT` |
| root task's table | 46 slots after the boot capabilities, 69 in all on QEMU | `ROOT_TABLE_SLOTS` |
| boot pool | 4 KiB | `BOOT_POOL_SIZE` in `kernel/layout.h` |
| smallest pool | 64 bytes | `POOL_MIN_SIZE` |
| object alignment | 16 bytes | `OBJ_ALIGN` |
| an object's size | see section 5.5 | `OP_POOL_INFO` |
| notification bits | 32 | the word size |
| timer lines | 16 for the whole machine | `TIMER_LINES` |
| a router's channels | 19 on the nRF52840. 4 on QEMU's boards, which connect nothing. None on the other boards, or with `ROUTES=0` | `OP_CAP_INFO` on `BOOT_CAP_ROUTES` |
| a DMA controller's channels | 3 on the ESP32-C6 and the ESP32-C5. 16 on RP2350. 4 on QEMU's boards, which move nothing. None on the nRF52840, or with `DMA=0` | `OP_CAP_INFO` on `BOOT_CAP_DMA` |
| a DMA transfer | Whole words. At most 32736 bytes on the ESP32-C6 and the ESP32-C5, the frame's length on RP2350, and 4096 bytes on QEMU's boards | `DMA_GRAIN` and `DMA_MOST` in the kernel's `board.h`; `OP_DMA_START` |
| units of time | 64 per core | `TIME_UNITS` |
| cores | 1, or 2 with `CORES=2` on QEMU `virt`, mps2-an521 and RP2350 | Makefile; `OP_CAP_INFO` on `BOOT_CAP_TIME`: its count divided by `TIME_UNITS` |
| timer delay or period per call | 2^32 - 1 µs | `OP_IRQ_SET` |
| watchdog's delay | 10 s | `WATCHDOG_US_MAX` |
| default span. It says how soon a request is due when no wait or bind gave a span. It is also the length of a turn on spare time while another thread waits for one | the longest power of two of counts that fits in a millisecond | `timer_default_span` in `kernel/timer.h`; `OP_CAP_INFO` on a `Clock` |
| span: how soon a request is due | the default span, or a power of two of counts up to 2^29 | `SPAN_MAX`; `OP_TIME_BIND`, `OP_NOTIFY_WAIT`, `OP_NOTIFY_LEND` |
| one step of the clock under tracing, `OP_TEST_TICK` | the counter's counts in one millisecond | `timer_step_counts` in `kernel/timer.h` |
| interrupt lines | 96 on QEMU, 77 on the ESP32-C6, 85 on the ESP32-C5, 52 on RP2350, 32 on mps2-an385, 48 on mps2-an521. Line 0 is the log's line | `IRQ_LINES` |
| smallest region | 8 bytes, or the PMP grain if that is larger. 32 bytes on ARM | `region_min_size` in `kernel/pmp.h`; `OP_CAP_INFO` on any frame |
| kernel log ring | 4 KiB minus a 32-byte header | `KLOG_SIZE` |
| replay records per input | 256 | `REPLAY_MAX_RECORDS` |

## 11. What is not there yet

These gaps are known and documented.
`TODO.md` lists the work items, and `DESIGN.md` lists the open decisions.

- **A watcher on ARM cannot set the flags.**
  It also cannot step a thread past an instruction inside an IT block.
  See `DESIGN.md`, open decision 23.
- **No priorities and no yield.**
  The whole policy is this:
  - the request that is due first runs first (earliest deadline first);
  - threads with time go before threads on spare time.

  A thread's units promise it their share of each span of the processor, and no more.
  The only extra is the time that a waiting thread lends to the thread it names.
  See `DESIGN.md`, open decisions 9 and 10.
- **No synchronous endpoints.**
  Shared memory and notifications carry everything.
  See open decision 5.
- **Lent time goes through at most 64 lenders, and only on a core whose time all of them hold.**
  A borrower runs on the time of the thread that has lent it time the longest.
  If that lender has no time for it,
  the borrower runs on the time of the thread that has lent to that lender the longest, and so on.
  Lent time never passes on through a borrower that waits without lending.
  It never runs on a core whose time one of the lenders does not hold.
  A lock that names its holder is fair only to takers that lend.
  See open decision 27.
- **One `Irq` per line.**
  See open decision 11.
- **Two cores at most.**
  Two cores run only on QEMU `virt`, mps2-an521 and RP2350.
  The first core takes every device interrupt.
  See `DESIGN.md`, open decision 24.
- **ARM with no replay yet.**
  Replay does not run on ARM yet, neither under QEMU nor on RP2350's Cortex-M33.
  See `TODO.md`.
- **No counter to load on the ESP32-C5.**
  Its core does not let user mode load from the CLINT.
  So a program there reads the time only through `OP_CLOCK_READ`.
  See `DESIGN.md`, decision 41.
- **Boot from flash only on the ESP32-C6 and the ESP32-C5.**
  RP2350 runs from RAM, and its bootrom loads it over USB.
  The nRF52840 also runs from RAM, and it is loaded over SWD.
- **No loader.**
  The image embeds one program, linked at fixed addresses.
  A second process runs code from the same code region.
- **The log across a reset is not reliable yet.**
  See open decision 12.
