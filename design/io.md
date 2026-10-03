# rvuos design: interrupts and the kernel log

Part of the design document; [`DESIGN.md`](../DESIGN.md) lists every section and the file that holds it.

## Interrupts

**An interrupt is a signal, as a tick is.**
The kernel owns the trap vector and the interrupt controller,
and no driver code runs in machine mode.
A driver holds an `IrqLine` capability naming one line,
binds it with `OP_IRQ_BIND` to a notification in a pool of its choosing,
which makes an `Irq` object there,
arms the `Irq` with `OP_IRQ_SET` for the bits it wants,
and waits on the notification.
When the line fires, the kernel masks it, disarms the `Irq`
and signals the bits, then returns to whatever was running;
the driver runs when its turn comes, as a thread a timer line woke does.
Having serviced the device, the driver arms the `Irq` again,
which is what unmasks the line.
A timer line takes the same `Irq`:
bound at creation to a notification,
armed with bits and a delay, disarmed by signalling,
so a driver that gives the device one bit and a timer line another
has a wait with a timeout for the same two calls; see "Time".
The machine timer is the one interrupt the kernel keeps for itself,
because the tick is its own; see "Scheduling".

**Armed and unmasked are one state.**
The controller forwards a line exactly while an `Irq` is armed on it,
which the self-check reads back from the controller line by line.
A line the kernel claims therefore always has an `Irq` to signal,
and a device whose level stays high, as a UART's does until it is serviced,
costs one trap and not a storm:
the mask outlasts the claim, and only the driver lifts it.
`OP_IRQ_SET` with no bits masks the line by hand,
and on a timer line cancels the delay.

**Lines are authority, and the root task holds them all.**
Interrupt lines are hardware, not kernel memory,
so an `IrqLine` capability names no object, as a `Frame` names none:
the root task receives every line of the controller at boot, and every timer line,
carves single lines out with `OP_IRQ_CARVE`, a table operation,
and hands them to drivers the way it hands out memory.
Binding consumes the invoked slot with everything derived from it,
and the `Irq` capability hangs below the Pool capability the `Irq` was allocated through,
as every object's does, so the pool's destroy finds it.
Copies of the line elsewhere are inert while the `Irq` exists,
because a line is bound at most once and a second bind fails with `KERR_OVERLAP`.
When the `Irq`'s pool is destroyed the kernel masks its line
and the object is gone, so those copies work again:
the lender of a line gets it back by revoking below the Untyped the borrower's pool was made of,
exactly as the lender of memory does.
Revoking below the line capability the lender derived the borrower's from
takes the line only while it is unbound;
once bound, the line is the `Irq`'s until its pool goes.

**An `Irq` holds its notification as a thread holds its process,**
by a capability in a slot of its own, below the `Notification` capability it was bound with,
and signals only the bits that capability may, whatever `OP_IRQ_SET` names.
A revoke above the slot, or the destroy of the notification's pool, clears it,
which disarms the `Irq` and masks its line.
`OP_IRQ_SET` refuses an `Irq` without a notification,
so it stays disarmed, its line bound, until its own pool goes.
It costs twenty-four bytes per `Irq` and a test on every set;
before, an `Irq` had to lie in its notification's pool so a pointer could not dangle.

**One `Irq` per line.**
Sharing a line between drivers is open decision 11;
what the kernel does today is refuse the second binding.
The kernel records the bound `Irq` of each line in a table of its own,
a word per line of the controller and per timer line,
so an interrupt or the tick finds its `Irq` without a walk.

**No driver in the kernel.**
The UART is the first device, and it is userspace's alone:
the root task is granted its registers and its line at boot,
and `user/init.c` drives the transmitter from user mode on its interrupt.
The kernel has no console to share it with.
What the kernel has to say goes into its log,
and the log raises a line of its own; see "The kernel log".

## The kernel log

**The kernel has no console.**
Where a kernel's messages go is the application's business:
a UART on one board, USB or a radio on another,
nowhere at all on a device in the field.
Each of those is a driver, and no driver runs in the kernel.
So the kernel writes into a ring of bytes in its own memory,
`KLOG_SIZE` of them behind a header at `KLOG_BASE`, and stops there.
`kputc` appends a byte and moves the head, a count of every byte ever written.
The kernel never waits for a reader:
one more than a ring behind loses the oldest bytes and can tell,
because the head has run past its own count by more than the size.
`OP_DEBUG_WRITE` appends up to twelve bytes to the same ring,
so a program's output and the kernel's keep their order.

**Writing is a right of its own.**
A `Debug` capability writes into the log with `RIGHT_W`,
and halts, traces, ticks, interrupts or preempts the machine with `RIGHT_X`,
so a child given it with `RIGHT_W` alone prints, its lines among the kernel's and its own faults' reports,
and can stop nothing.
Before, one capability did all of it, so a child printed into a ring its parent copied out,
and reached the log only while its parent ran.
The bytes travel in the call's registers, as everything does, twelve a call, so a line is a few calls;
a writer that floods the ring costs the reader the oldest bytes, the kernel's among them,
the price of one log every writer shares.

**The reader is granted the ring, and a word in its header.**
`BOOT_CAP_LOG` names the header and the ring,
so the root task installs it as it installs a device's registers
and reads the head and the bytes without a system call.
The header also holds `taken`, the reader's count of what it has read,
which is the one word of kernel memory user mode writes:
the kernel reads it only to tell whether the line is high
and how much of the ring a halt has left to write out,
and a count past the head reads as the head.
It is kernel memory that user mode can see,
which the isolation properties otherwise forbid;
it holds no object and no capability, only what the kernel chose to say,
and it can never become a pool, since the kernel writes there on its own:
`BOOT_CAP_LOG` is a frame, and no Untyped covers it.
The header and the ring are one 4 KiB block at the top of the kernel's RAM,
right below the root task's code,
so the ring holds the block less its header.

**The log is a device, and its line is line 0.**
A reader that has taken everything wants to stop until there is more,
and stopping is a notification, which a device reaches through an `Irq`.
So the log has an interrupt line, `LOG_IRQ_LINE`, the number the controller does not use,
and the root task receives it in `BOOT_CAP_IRQ_LINES` with the controller's lines,
carves it out, binds it and arms it as it would a UART's.
The line is level: high while the head lies past `taken`,
so the reader acknowledges by writing the header, as it would a device's register.
Arming the `Irq` while the line is high signals at once,
so a byte written between the reader's last look and its arm is not missed,
and a byte written while the `Irq` is armed signals as it lands.
Either way the `Irq` is disarmed by signalling, as a device's is,
and the rest of a burst wakes nobody until the reader arms again.
Nothing else is new: binding, the pool rules, revocation by pool destroy
and one `Irq` per line hold for line 0 as for line 10,
and `klog.c` does for the log's line what the controller does for a device's,
with the head for the level and no enable bit to write.

**Two things the log's line is not.**
It is not a source that can wake the machine:
only the kernel raises it, and the kernel runs only when a thread,
the tick or a device makes it,
so a kernel with nothing runnable and only the log's `Irq` armed
stops with `no runnable thread` rather than stall in `wfi`.
And it is not a line a replay can fire by name:
`OP_DEBUG_IRQ` refuses it, since its level is the log's and not the record's.
A replay reaches it all the same, because the trace is bytes into the log
and the trace of the very call that arms the `Irq` is what raises the line;
`tests/seeds/log-wakes-reader` rests on that.

**What a halt does with the log is the board's business.**
A board that resets leaves the ring in RAM,
where a logger after the reset can find the last words;
what it needs to trust them is open decision 12.
QEMU virt exits on a halt and keeps no RAM,
so its `khalt` in `kernel/board/qemu/halt.c` is the post-mortem reader:
it writes what no reader has taken to the UART under a line that says the log follows,
so that a transcript tells what a logger carried out from what the halt salvaged.
That is the one place the kernel touches the UART.
The ESP32-C6 parks its core on a halt, which keeps RAM,
and its `khalt` writes the same dump by polling to the USB Serial/JTAG console,
then a line with the code a simulator would have exited with,
so that the same transcript check runs on the board;
a console no host reads is given up on rather than waited for.
RP2350 has no serial port on USB, and its bootrom wipes RAM on entering BOOTSEL,
so there the logger's console is a block of RAM,
and the halt makes a USB CDC-ACM port of the chip's controller, `kernel/board/rp2350/cdc.c`,
writes the block and then the dump to it, and reboots into BOOTSEL once the host has read them.
It is the one USB stack in the kernel, and it runs only once the kernel has stopped.
The ring is just under 4 KiB, a burst's worth between two looks by a reader,
and a reader that looks less often loses the oldest bytes and can tell.
