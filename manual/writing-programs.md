# rvuos user manual: writing and debugging a program

Chapters 8 and 9 of the user manual; [`MANUAL.md`](../MANUAL.md) has its contents.

## 8. Writing a program

### 8.1 Toolchain and layout

A user program is a flat binary linked against `user/user.ld.S`,
with the board's addresses, see "Boards",
and embedded into the kernel image by the Makefile.
Constraints of the current linker script:

- code and read-only data live in the read-execute region, at `0x80100000` on QEMU,
- zero-initialised data and the stack live in the read-write region, at `0x80110000` on QEMU,
- **initialised data is forbidden**: nobody would copy it into the data region,
  and the link fails if `.data` is not empty,
- `user/arch/<arch>/start.S` zeroes `.bss`, calls `main`,
  and halts with code 1 through `BOOT_CAP_DEBUG` if `main` returns.

Compile flags are `-march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding -nostdlib`,
or `--target=thumbv7m-none-eabi -mcpu=cortex-m3 -mfloat-abi=soft -ffreestanding -nostdlib` on ARMv7-M,
with `--target=thumbv8m.main-none-eabi -mcpu=cortex-m33+nodsp+nofp` in place of the first two on ARMv8-M.
There is no libc; `user/rvuos.h` provides the system call wrappers:

| Wrapper | Operation |
|---|---|
| `rv_invoke(op, cap, a1, a2, a3)` | any |
| `rv_frame_info(cap, &base, &size)` | `OP_FRAME_INFO` |
| `rv_frame_min_size(cap, &min)` | `OP_FRAME_INFO`, reading `a4` |
| `rv_untyped_info(cap, &base, &size, &made)` | `OP_UNTYPED_INFO` |
| `rv_retype(cap, type, dst, &base)` | `OP_UNTYPED_RETYPE` |
| `rv_split(cap, lower, upper)` | `OP_UNTYPED_SPLIT` |
| `rv_signal(cap, bits)` | `OP_NOTIFY_SIGNAL` |
| `rv_wait(cap, &bits)` | `OP_NOTIFY_WAIT` |
| `rv_timer_set(cap, bits, us)` | `OP_IRQ_SET` on a timer line, with the delay |
| `rv_timer_period(cap, bits, us, &skipped)` | `OP_IRQ_SET` on a timer line, with `IRQ_SET_PERIOD` |
| `rv_irq_set(cap, bits)` | `OP_IRQ_SET` |
| `rv_putc(cap, c)`, `rv_puts(cap, s)`, `rv_put_hex(cap, v)` | `OP_DEBUG_PUTC` |
| `rv_halt(cap, code)` | `OP_DEBUG_HALT` |

To replace the demo, edit `user/init.c` or add a program to `USER_PROGRAMS` in the Makefile;
each program becomes its own kernel image.

### 8.2 Building a second process

Today one program is embedded, and a second process runs code
from the same code region with its own data and stack.
Loading a separate binary is a userspace job the root task does not do yet;
`DESIGN.md`, open decision 3.
The steps below are what `user/init.c` does.

1. **Take memory** out of `BOOT_CAP_FREE_RAM`:
   a frame to share, a frame for the child's data and stack,
   and a pool for its kernel objects.
   The demo keeps no allocator: `take_untyped` splits what is left of the free RAM,
   keeps the lower half as the block and the upper as what is left,
   so its blocks halve one after another, section 5.4.
   It deletes the Untypeds in between, so every block hangs right below the free RAM
   and three working slots do for all of them.
   `take_frame` makes the block a frame and deletes its Untyped, since revoking below a frame takes it back;
   the pool's Untyped stays, since revoking below it is what destroys the pool.

   ```c
   take_frame(SLOT_SHARED, &shared_base, &shared_size);
   take_frame(SLOT_CHILD_DATA, &child_data_base, &child_data_size);
   take_untyped(SLOT_POOL_MEMORY, &pool_base, &pool_size);
   rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base);
   ```

2. **Allocate** the child's objects from the pool,
   table first, then the process that uses it, then a thread in it.

   ```c
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_PROCESS, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0);
   rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0);
   ```

3. **Install regions** into the child: code, data, and the shared chunk.

   ```c
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 0, BOOT_CAP_CODE, RIGHT_R | RIGHT_X);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 1, SLOT_CHILD_DATA, RIGHT_R | RIGHT_W);
   rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, 2, SLOT_SHARED, RIGHT_R | RIGHT_W);
   ```

4. **Fill the child's table** with exactly the authority it should have,
   narrowing rights as you go.

   ```c
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG,  BOOT_CAP_DEBUG, RIGHT_ALL);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED,    RIGHT_R | RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP,     SLOT_UP,        RIGHT_W);
   rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN,   SLOT_DOWN,      RIGHT_R);
   ```

5. **Configure, bind and start** the thread.
   The stack grows down from the top of the child's data frame.
   The thread earns half the first core, units carved out of the boot grant.
   The root task's thread earns every unit of the first core at boot, so it keeps the first half and leaves the rest;
   binding the child through `BOOT_CAP_TIME` with no units would give it spare time alone,
   which it gets only while the root task does not want the processor.

   ```c
   rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
             child_data_base + child_data_size, 0);
   rv_invoke(OP_TIME_BIND, BOOT_CAP_TIME, BOOT_CAP_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, TIME_UNITS / 2, TIME_UNITS / 2, SLOT_CHILD_TIME);
   rv_invoke(OP_TIME_BIND, SLOT_CHILD_TIME, SLOT_CHILD_THREAD, 0, TIME_UNITS / 2);
   rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0);
   ```

   Revoking below `SLOT_CHILD_TIME` stops the child's thread; binding it again starts it.
   The demo gives the child no watch; section 5.6 says how a parent hears its child fault.

6. **Talk** through the shared region and the two notifications.
   The child writes a word, signals `SLOT_UP`; the parent waits, answers, signals `SLOT_DOWN`.

7. **Tear down** by revoking below the pool's Untyped, which destroys the pool.
   The child's thread, process, table and notifications go with it,
   every capability to them is cleared in every table,
   and the Untyped is free again, to be retyped into something else.

   ```c
   rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_POOL_MEMORY, 0, 0);
   ```

The child must touch no global variable, since it shares no data region with the parent:
everything it needs is on its stack or behind a capability in its table.

### 8.3 Writing a driver

A driver needs a frame for the device's registers and one interrupt line.

```c
rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, UART_SLOT, BOOT_CAP_UART, RIGHT_R | RIGHT_W);
rv_invoke(OP_POOL_ALLOC, POOL, CAP_NOTIFICATION, NTFN, 0);
rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, UART_IRQ, 1, LINE);
rv_invoke(OP_IRQ_BIND, LINE, POOL, NTFN, IRQ);

for (;;) {
    rv_irq_set(IRQ, BIT_UART);      /* arm: unmask the line */
    rv_wait(NTFN, &bits);           /* the interrupt masked it and signalled */
    /* service the device */
}
```

Arming again after servicing is the acknowledgement.
Give a timer line a second bit on the same notification for a timeout.

## 9. Debugging and testing interfaces

**Tracing.**
`OP_DEBUG_TRACE` makes the kernel print one line per system call,

```
trace: op=0x00000007 slot=0x00000004 a1=0x00000007 a2=0x0000000d a3=0x00000000 -> 0x00000000
```

with `blocked` in place of a status when the call blocked,
and `trace: wake <bits>` after a call that woke a waiter,
or after the `user fault` of a thread whose watch woke one.
A call stopped where `OP_DEBUG_PREEMPT` armed prints `trace: preempt` instead of its line,
followed by what the tick woke,
and its line comes once the call is made again and finishes.
After every traced call the kernel runs the self-check,
the executable form of the properties in `DESIGN.md`,
and halts with code 3 on the first violation.
While tracing is on the tick preempts nobody and moves no time;
only `OP_DEBUG_TICK` does,
so that the host build can reproduce the transcript.
The clock that charges turns is then the tick count alone:
a turn with time pays a whole tick at each `OP_DEBUG_TICK` and nothing at a wait.
A thread whose program counter was set while tracing was on,
or any of whose registers `OP_THREAD_WRITE_REG` wrote then,
cannot be run under tracing; the kernel halts with code 6 instead.

**The self-check** (`kernel/selfcheck.c`) verifies, among other things:
no installed region or frame overlaps a pool,
and what one Untyped made lies within it and overlaps nothing else it made,
every process's PMP image matches its region slots,
every capability names a live object of its own type or an in-bounds frame, Untyped or line range,
and every capability to a pool or an object lies below the pool's own node,
pools tile their memory exactly,
every waiting thread names a live notification,
every ready thread but the running one that may run is on the queue its account says,
and each unit of time is earned by the thread bound to it and by no other,
every `Irq` names a live notification, or none and is disarmed,
every thread's watch names a live notification, through a capability that could signal it,
no `Irq` armed on a timer line is past its deadline,
and the controller forwards exactly the lines an `Irq` is armed on.

**Replay input.**
`BOOT_CAP_INPUT` maps 64 KiB where QEMU's loader places a file:

```c
struct replay_header { uint32_t magic; uint32_t count; };  /* magic "RVFZ", 0x5a465652 */
struct replay_record { uint8_t op; uint8_t actor; uint16_t slot; uint32_t a1, a2, a3; };
```

Each record is one system call and the thread that makes it:
`actor` 0 is whichever thread runs, 1 the driver's root thread, 2 its second thread,
3 a third that starts stopped until a record resumes it,
and lies in the root thread's pool while it runs in the second's process.
The replay driver in `user/fuzzdrv.c` performs them in order,
the host build in `host/` performs the same records natively under ASan and UBSan,
and `tests/differential.py` requires the two transcripts to match line for line.
A driver thread the records unmapped faults, and the next goes on with the records;
every driver thread ends in a breakpoint, a fault too, once the records are done,
so a transcript ends in `no runnable thread` unless something stopped it first.
Hand-written scenarios live in `tests/seeds`,
what the fuzzer found in `tests/corpus`,
and `tests/mutants/` holds one planted bug per invariant.
