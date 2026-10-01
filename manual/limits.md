# rvuos user manual: limits and what is not there yet

Chapters 10 and 11 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 10. Limits

| Limit | Value | Where |
|---|---|---|
| region slots per process | 8 | `PROCESS_REGION_SLOTS` |
| PMP entries the kernel uses | 16, or `PMP_MAX_ENTRIES` | Makefile |
| minimum PMP entries to boot | 4 | `kernel/main.c` |
| slots per `CapTable` | 1 to 1024 | `CAPTABLE_MAX_SLOTS` |
| root task's table | 64 slots | `ROOT_TABLE_SLOTS` |
| boot pool | 4 KiB | `BOOT_POOL_SIZE` in `kernel/layout.h` |
| smallest pool | 64 bytes | `POOL_MIN_SIZE` |
| object alignment | 8 bytes | `OBJ_ALIGN` |
| notification bits | 32 | the word size |
| timer lines | 16, on the whole machine | `TIMER_LINES` |
| units of the processor's time | 64, on the whole machine | `TIME_UNITS` |
| most of its units an account holds | 100 ticks' worth, 100 ms (`ACCOUNT_TICKS`) | `kernel/timer.h` |
| timer delay or period per call | 2^32 - 1 µs | `OP_IRQ_SET` |
| tick | 1 ms (`TIMER_HZ` 1000) | `kernel/timer.h` |
| interrupt lines | 96 on QEMU, 77 on the ESP32-C6, 52 on RP2350, 32 on mps2-an385, line 0 the log's | `IRQ_LINES` |
| smallest region | 8 bytes, or the PMP grain if coarser, 32 bytes on ARM | `region_min_size` in `kernel/pmp.h` |
| kernel log ring | 4 KiB less a 32-byte header | `KLOG_SIZE` |
| replay records per input | 256 | `REPLAY_MAX_RECORDS` |

## 11. What is not there yet

These are documented gaps, not surprises;
`TODO.md` carries the items and `DESIGN.md` the open decisions.

- **A watcher on ARM cannot set the flags**, nor step a thread past an instruction inside an IT block;
  `DESIGN.md`, open decision 23.
- **No priorities and no yield.**
  Round-robin on a tick, threads with time before threads on spare time,
  is the whole policy, and units promise their part of the processor and a turn within 64 ticks, no more;
  `DESIGN.md`, open decisions 9 and 10.
- **No badged notifications.**
  A client's identity to a server is a convention, not kernel-enforced;
  open decision 6.
- **No synchronous endpoints.**
  Shared memory and notifications carry everything; open decision 5.
- **One `Irq` per line**; open decision 11.
- **ARM with no escape suite and no replay yet**, under QEMU and on RP2350's Cortex-M33; `TODO.md`.
- **No boot from flash.**
  The ESP32-C6 and RP2350 run from RAM, loaded by their ROMs over USB.
- **No loader.**
  The image embeds one program, linked at fixed addresses;
  a second process runs code from the same region.
- **No initialised data** in user programs, until something copies it.
- **The log across a reset** is not yet trustworthy; open decision 12.
