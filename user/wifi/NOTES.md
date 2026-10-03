# Writing the Wi-Fi system: notes from the program's side

`user/wifi/` was written as a program for rvuos is meant to be written:
from `MANUAL.md` and `user/rvuos.h`, reading the kernel only where the manual left a question.
It is a root task and two processes on a Pico 2 W:
a driver for the CYW43439 that loads its firmware, scans, joins or runs an access point,
and a network process with a small IPv4 stack, joined to the driver by rings of frames.
On the board it joins a WPA2 network, takes an address by DHCP and answers a laptop's ping, about 6 ms there and back,
and datagrams on UDP ports 7 and 7777.
These notes say what that took, what helped and what hurt,
so that the next decisions about the kernel and a library for programs can start from a program and not from a guess.
They were written before `user/lib/`, which came of them;
the last section says what the library changed.

## What it took

About 3,400 lines of C, of which the root task is the largest piece, 586 lines and 9.6 KiB of code.
Little of it is about Wi-Fi: most of it builds processes, hands out memory and slots, and copies logs.
Building one child takes about 18 calls in `child_new` and 6 in `child_start`, besides halving memory,
and the link between two children 8 more, with its conventions written down in `wifi.h`.
The kernel needed one operation, `OP_DEBUG_FRAME`, for the devices;
the boot now grants a frame over each device instead, open decision 25.

## What worked well

**The isolation is real and cheap.**
The driver holds PIO0's registers, 128 bytes of four pins' control, its own RAM and nothing else:
no `Debug`, no console, no other pin.
A 128-byte frame over sixteen pins' control registers is authority cut to the size of the job.

**PIO from user mode worked the first time,**
and IO_BANK0's output overrides drove chip select and power without SIO, which a process cannot reach.

**One wait for everything.**
A notification's bits let the driver wait on its timer, the root task and the network process at once,
and a wait with a timeout is a timer bit beside the others.
The rings' rule, wake the other side only when a ring goes from empty or from full,
is free of lost wakes because bits are sticky; it needed no lock and no kernel help.

**Revocation by memory did what it promised.**
Taking the firmware's 228 KiB back from the running driver was four revokes,
and the driver's window region went with them, cleanly.

**A fault is easy to find.**
The root task watches each child, and `OP_THREAD_FAULT` gives the cause, the pc and the address;
none came in this work, and `tools/no-globals.py` turns the likeliest, a global in a child, into a link error.

## What it found in the kernel

A program that sleeps a quarter of a second and drives devices the demo never touches found three things the checks had not:

- On ARM an idle kernel slept for good through any deadline further than one reload of SysTick reaches,
  112 ms on RP2350's Cortex-M33: SysTick left pending makes no second event for `wfe`.
  The driver's 250 ms wait for the chip to power up hung the machine; `kernel/arch/arm/trap.c` sets SysTick again,
  and the demo now sleeps past a reload.
- Listing the ADC among the devices hung every boot, since a device whose clock is off never leaves reset.
- The ARM link's stack check took two Thumb halfwords for `core_idle`'s address and saw a recursion;
  it now reads literals only where the assembler marked data.

## Where it hurt

### IPC: the plumbing, not the model

The model is right: shared memory with no copy, and notifications to say when.
What costs is everything around it, and it is the same every time:

- a frame, which takes halving free memory and a retype;
- a region for it in both processes, and regions are the scarcest thing there is, below;
- a notification capability each way, copied with the right narrowed, into slots both sides agree on in a header;
- a bit each way, agreed on in a header;
- the layout of what goes in the frame, initialised by whoever made it;
- the addresses, told through a page, since a process starts with one word in `a0`;
- fences, which the programmer must remember, since two cores read the same memory.

None of it is hard, and all of it is boilerplate that each program rewrites.
`user/wifi/` grew its own: `child_new`, `child_give`, `child_map`, `child_start`, `ring.h`, `struct plog`, `say`.
That is a library waiting to be extracted, and most of the pain goes with it, with no change to the kernel.

What a library cannot fix:

- **Who signalled.** Bits are a convention, open decision 6; fine for two peers,
  not for a server with many clients, which also cannot afford a region per client.
- **Asking and answering.** Every request between processes is a state machine over shared memory;
  the network process has no way to offer `send(socket, data)` to another process that is less work than a ring.
- **Connecting at run time.** Only the root task holds what a new connection needs,
  so every channel is made by it, before or while the processes run, as the link was.

### Processes without a loader

Every process runs the one image's code, so a child keeps no global.
The compiler does not know: a `static` in a child is a fault at run time, which `tools/no-globals.py` now turns into a link error.
State goes through a context pointer and callbacks find their owner with `offsetof`.
A thread starts with a pc and a stack and nothing else,
so the root task writes its `a0` with `OP_THREAD_WRITE_REG`, a debugger's operation, with the architecture's number for the register,
which also makes the thread one that tracing cannot run.
And no program may have initialised data, so `static uint32_t next = BOOT_CAP_COUNT;` fails to link,
in the root task as well, which has a data region.
There is no C library either: `lib.c` has the memory functions the compiler calls, on ARM its `__aeabi_` ones too,
which a zeroed struct needs, and a small `say` in place of printf.

### Memory and slots by hand

There is no allocator, as the design intends, so the root task has one: free blocks kept as Untypeds, halved down to size.
Each halving takes two slots, and the root task's table has 46 free past the boot's, 69 in all on RP2350;
with three processes it ran out, and needed slots given back: deleted, consumed by a bind, emptied by a revoke.
Reading 64 bytes of memory the root task holds took six calls:
retype all of free RAM into a frame, carve the bytes, install, read, uninstall, revoke.

### Regions: the scarce resource

Hazard3 gives a process seven regions; the root task uses all seven and the driver six.
A region is a power-of-two block, so the 228 KiB firmware is four frames,
more than the driver could install beside its devices; it installs them one at a time in a window.
Every shared frame costs a region on both sides, so the region budget,
not the kernel's calls, is what bounds how many peers a process can have.
A server shares one frame carved into a block per client, which works while the clients are known when it is made.

### Devices

The boot gave the root task one device, the console.
Everything else came through `OP_DEBUG_FRAME`, which needed the capability that halts the machine,
and only for devices the kernel took out of reset and opened in ACCESSCTRL at boot;
the first try listed the ADC, whose clock is off, and the boot never finished.
Since open decision 25 the boot grants a frame over each device the board lists, which the root task carves as it does RAM,
and `devices.h` names their slots; the kernel still has to list a device and start it before a program can have it.

### Running for longer than a test

A run ends at the halt, by seventeen seconds at the latest, and the console reaches the host only then.
That is right for tests and wrong for a system that is meant to be pinged.
`rv_putc` is a system call per byte, and the kernel's log keeps 4 KiB.

## What would change it

A library for programs, in the tree beside `user/rvuos.h`, would take most of the above away:

- a process builder: the child's objects, its table filled, its regions, its time, its watch, its start, its argument;
- a slot allocator that takes slots back;
- an allocator over Untypeds, by halving, with a peek that does not take six calls;
- a channel: a frame, two rings, a bit each way, set up from both ends;
- a log ring a child writes and its parent copies, so that no child needs `Debug`;
- typed wrappers for every operation, as there are for a third of them now.

In the kernel, by what they would save:

1. **An argument for a thread**, `OP_THREAD_CONFIGURE` taking `a0` too, so that starting a child needs no debugger's call.
2. **Initialised data**, copied by `start.S` from the code region, which costs a loop and lets a program write C as C is written.
3. **Devices granted at boot**, open decision 25, so that drivers need not come through `Debug`.
4. **A capability that only prints**, or the log as a frame a child may be given, so that no child needs `Debug` to say anything.
5. **Badges**, open decision 6, once a server has clients that do not trust each other.
6. **A run that lasts**: a watchdog the root task feeds through a frame, or the kernel through its tick,
   and a console that reaches the host while the system runs, which with the network up could be the network.

## After the library

`user/lib/` is the library the list above asked for, taken out of this program, see `MANUAL.md`, section 8.4,
and the system was written again on it; it joins the same network and answers the same ping on both kinds of core.

- **The root task** is 342 lines and 7.0 KiB of code, from 586 and 9.6 KiB.
  A child is `child_new` and `child_start`, and the link `chan_new` and `chan_connect`.
- **`wifi.h`** is 114 lines, from 188, and names no slot, region or bit:
  the library hands each child's out and the root task writes which into the child's page,
  so the pages' types are all the processes agree on.
- **Slots.** A child costs the root task six slots, from twelve:
  what is needed only to build it, its pool, its data's frame, its timer's `Irq` and its units, is deleted once used,
  which the derivation tree allows, since what hung below a deleted capability goes to its parent.
  A station's run ends with 26 of the 46 unused, and nothing gives a slot back by hand.
  What the library cannot help is the free memory: halves never join again, so each free block holds a slot.
- **Regions.** A child's page lies at the base of its data, so the page costs no region of its own:
  the network process holds three, the driver six of seven, with the link no longer in the window's place,
  and the root task six, a seventh for a moment.
- **The link is safer.** The rings' shape and place lie in each end, in a page the other process cannot write;
  the rings of `ring.h` kept theirs in the frame both write, so either side could have steered the other's writes past the ring.
- **Timer lines** come from the kernel's answer, `KERR_OVERLAP` for one bound already, so nothing keeps them.
- **The state of a child** is on its stack, in its main's frame, which never returns,
  rather than at a base address it is told; `offsetof` remains only for the callbacks of the chip's layer.

What a library cannot fix is as it was: who signalled, asking and answering, and connecting at run time.
In the kernel, the list above stands, the devices excepted, which the boot grants now:
a thread's argument, initialised data, a capability that only prints, badges, and a run that lasts.
`make lib-test` checks the library on every board, and `make check` under QEMU on both architectures.

## After the kernel's changes

Four of the five changes the list above asks of the kernel are made, each the way the kernel already did something else,
and the library and this system moved onto them:

- **A thread's argument.** `OP_THREAD_CONFIGURE` takes the word a thread finds in `a0` and zeroes the other registers,
  so `child_start` makes no debugger's call, and a thread started again starts from nothing its last run left.
- **Initialised data.** `start.S` copies `.data` from behind the code, so the root task may write `static uint32_t next = BOOT_CAP_COUNT;`.
  A child still keeps no global: it runs the root task's code without its data, and `tools/no-globals.py` still says so at the link.
- **A capability that only prints.** `Debug` writes into the log with `RIGHT_W` and halts or drives the machine with `RIGHT_X`.
  A child gets `CHILD_LOG` with `RIGHT_W` alone, so its lines reach the log in order with the kernel's, its faults' reports among them,
  and the ring in its page and the root task's copying of it are gone; `OP_DEBUG_WRITE` carries twelve bytes a call, not one.
- **Badges.** A notification capability carries the bits it may signal, and `OP_NOTIFY_CARVE` narrows them as a frame is carved.
  The library gives every signaller the other side's inbox carved to its own bit,
  so who signalled is the kernel's answer rather than a convention, and neither a child's page nor a channel's end names a bit to signal.

What is left of the list is a run that lasts:
a watchdog someone feeds and a console that reaches the host while the system runs, decisions about the board more than the kernel.
Of what a library cannot fix, asking and answering and connecting at run time remain,
which a server with clients in processes of their own, a socket over a channel to the network process, would measure.
