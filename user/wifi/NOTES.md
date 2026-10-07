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
the sections at the end say what the library and the kernel's changes did,
what moving the network's services into clients in processes of their own found,
and what a run that lasts took.

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

## Clients in processes of their own

The network process kept the IP stack and its own status port,
and its services moved out into clients, each a process of its own:
the echo, which answers on UDP port 7 as the network process did,
and the clock, new, which asks the network for the time and tells it on port 13.
Each has a channel from the network process, through which it uses datagram sockets, `sock.h`:
it binds ports, sends from them and receives what is sent to them, and asks for the network's addresses.
That took about 600 lines:
`sock.c`, the server's side, and `sntp.c`, the clock's messages, which make no system call, so `make wifi-test` tries every lie on them;
`echo.c` and `clock.c`; and about 50 more lines in the network process and 100 in the root task, which builds the clients again.
The library gained a hub, room for children's data, and the calls of an end that is connected again.
Nothing in the kernel changed.

**The parent's regions ran out first.**
A child's page lies in its data, which the parent had installed in a region for each child:
code, data, console, log and three children's pages fill the seven regions Hazard3 gives a process on RP2350,
and the fourth child, the clock, failed to connect.
`self_room` now gives a parent one frame for all its children's data, installed once, of which each child's is carved.
The driver still has a region of its own, since the firmware fills the free memory the room comes from until the chip runs.

**The server's regions would have run out next.**
A channel installed for each client costs the server a region each:
with its code, data and link the network process would serve four clients.
A hub is one frame of a channel for each client, which the server installs once, and each client is given its own, carved.
Both fixes are the same move: carving is how the system names less, and one region over many carves costs one entry.

**Connecting at run time is a handshake of three.**
The parent closes a client's end, the server lets go of it and says so, and only then may the parent connect another client:
before, the new client's rings could be emptied under the server, or its port refused as still held by the client that died.
The server's word is a count, `gen`, that the parent moves at each connection and close and the server echoes back, `seen`,
so neither a close nor a connection the server slept through is lost.

**A peer can make a ring look full.**
The rings keep memory safe from a lying peer, not time:
moving the counters makes a ring seem to hold billions of packets, and taking until it is empty never ends.
The network process takes a ring's worth from each channel for each wake,
and wakes itself for what it left, through its own inbox carved to a bit of its own,
a slot and a bit, since there is no wait that does not block.

**Asking and answering is a state machine, and a channel for each asker keeps it small.**
The clock asks four questions in a row, the network's addresses, a port, the server's address and the time,
each asked again every second until it is answered.
Answers come back in the order of the questions, each client on its own channel,
so no question needs a tag, and the guarantee that an answer reaches its asker is the channel's.
A client of two threads asking at once would need one or the other back, a tag or a channel each.

**What a client costs.**
A datagram handed to a client and its answer handed back, four system calls and two switches,
take about 50 microseconds on average and 37 at least on Hazard3, 46 and 40 on the Cortex-M33, both at 150 MHz,
by the counter read in the network process around the echo, code that was not kept.
A laptop's echo of 32 bytes took 4.0 to 5.5 ms at the median through the echo's process, from run to run on either kind of core,
as it took 4.05 and 5.24 when the network process answered itself: the radio's jitter is a hundred times the hop.
Each client costs the root task six slots and one for its channel, and the network process a slot and a bit;
a run ends with 12 of the root task's 46 slots unused, from 26.

**A client's fault costs the client.**
A datagram that reads `fault` makes the echo store where it has no region.
The root task closes its channel, takes it down, waits for the network process to let go, and builds it again;
the laptop has its echo back 5 to 7 ms later, and the root task ends the run with the bytes and slots it had before.
Every check that keeps the clients apart is the server's own code:
it reads a request's header once, since the client may rewrite it meanwhile,
holds each request to the length the ring says, and each client to the ports it holds.

So the kernel had what clients that do not trust each other need:
carving for regions, the notification's bits for who signalled, and a fault that stops one process.
The four system calls of a round trip and the answer with no kernel to carry it to its asker, open decision 5,
cost nothing a laptop can see; a server whose round trips were its time would be the next measure.


## The clock and the watchdog

No process of this system read the clock: a frame over the counter costs a region, and regions ran out first,
so the clock client counts its own timer's milliseconds.
`OP_CLOCK_READ` now tells the time in a call, 64 bits on every board, and the frame stays for a thread that cannot afford one.
A run that lasts wanted a watchdog the root task feeds, and a frame over RP2350's would have handed it the registers
that tell the bootrom what to run after the reset, in machine mode;
so the watchdog is the kernel's, fed through the clock with `RIGHT_W`, and it halts, which writes the log out, rather than resets.
The root task does not feed it yet: a run longer than seventeen seconds says nothing until it ends,
which waits for the log to leave by the network.

## A run that lasts

The system now runs as long as its configuration says, `run=0` for good, and tells what it does while it runs:
a third client, the logger, carries the kernel's log to a host over the network,
and the root task asks each child every second whether its loop still comes round, then feeds the watchdog.
`make BOARD=rp2350 wifi` follows the log through `tools/wifi-run.py`,
which learns the system's address from it and checks the clients from the host,
the echo built again after a fault and after a hang among them, which were tried by hand before.
That took 200 lines for the logger, 40 more in the root task, 20 in the library and a line in each other child's loop,
and 280 lines of Python; nothing in the kernel changed.

**The log's reader word was the transport's.**
The halt writes out what lies past the log's `taken`, so the logger moves it only as far as the host says it has the bytes:
what went over the network and what the halt writes out neither overlap nor leave a gap.
Nobody reads the log until the logger runs, and the ring held the boot's kilobyte with room to spare,
so a host sees the log from the boot.
The root task no longer copies the log to its console, which freed the regions of both: it holds four of its seven.

**A host finds the system by asking everyone.**
Only the log could tell the host the address DHCP gave, and the host cannot ask for the log without it,
so it asks by broadcast, and the answer comes from the address.

**A check is a word each way in the page.**
`child_check` counts the parent's questions and tells the child, which wakes it if it waits,
and `child_answer` copies the count back each time round the child's loop,
so a child that spins, or waits for what never comes, has not answered by the next check.
Every loop had to say so; the driver's lie all through the chip's code, and its sleep answers for all of them.
A wait in the library that answered by itself would have been one place,
but the driver's loop under load seldom waits, and would have gone unanswered.
The root task takes a client that did not answer down and builds it again, as after a fault,
and halts for the driver or the network process;
the echo set spinning by `hang` answers again 1.4 to 1.9 s later.

**The watchdog is left the root task's own loop.**
The root task feeds it after each check, so it halts the machine, with the log written out,
only if the root task's loop or the kernel's tick stops; the children's hangs the root task handles itself.
That is what open decision 26 needed to see: the kernel's watchdog watches what a userspace supervisor could not watch itself.

**Units ran out before slots or regions.**
The first core's 64 units were all held when the logger came, so each client now earns 4, not 6;
the echo's round trip stayed at about 4 ms on Hazard3.

**Half an hour on the home network.**
A run of 27 minutes on Hazard3, ended by unplugging the board, answered a host that asked every five seconds
for the status and ten echoes of 64 bytes:
3,250 echoes came back and none was lost, 4.0 ms at the median and 26 ms at worst,
and at the end the clock's time still agreed with the laptop's to the minute.
Nothing was restarted but what the checks at its start set out to restart.


## The ESP32-C6's driver as a child

On the ESP32-C6 the driver is Espressif's closed Wi-Fi libraries around an adapter, `user/wifi/esp32c6/`.
It ran in the root task's process until its frames were to reach the network process;
now it is a child, the network process its peer through the link,
and the board takes an address by DHCP and answers a laptop's ping, about 10 ms there and back.
The network process did not change but for the board its status line names.

**Eight regions, and the libraries want six.**
The root task's process held its code and data, the driver's RAM, the window onto flash, the ROM, the modem,
the SAR ADC and the random number generator: all eight, so the link had nowhere to go.
Carving the link out of the driver's RAM would have cost half its free heap;
a pager, a thread that installs the frame another faulted on and resumes it, works with the kernel as it is,
but the kernel logs every fault, so it suits frames touched seldom.
As a child the driver holds all eight too, its image in the code region in place of the root task's.

**A child that builds threads.**
The libraries want tasks, which the adapter runs as threads, and the library knew only children of one thread.
`child_give_own` hands a child a pool, timer lines, units and slots, and `child_self` spends them
through the calls a root task uses, so the adapter changed only where its account comes from.
The threads' faults reach the root task as the driver's, but it holds no capability to them to ask which;
the kernel's own report in the log says.

**The ROM's delays counted on a counter the kernel stopped.**
`ets_delay_us` counts cycles in the user-mode performance counter, which the kernel stopped at every change of process,
so beside other processes a delay could have lasted for ever.
Open decision 22 was waiting for a program that wanted the counter kept: a process now keeps it.

**What the build and the board kept.**
The Makefile read no dependencies of the driver's files, so a changed header left the rest with the old layout.
And the board's RAM keeps what a longer configuration left, so one that named no network joined the last one named;
the loaders' `--text` now ends it with a NUL.

## Debugging the ESP32-C6's driver

A second network, of two access points, found what the first had hidden,
and finding it took tools the system lacked, which it keeps now.

**The log at the halt misled.**
The ESP32-C6's kernel has no console, so its log came out when the machine halted,
and a host that waited for the address in the log pinged a board that had already halted: the board seemed out of reach.
The root task now carries the log to the console as it comes, through the reader the boot grants it, woken by the log's line,
and the console brings the host's commands the other way, `end` and `stats`.

**A round follows events.**
`tools/wifi-run.py` acts on the log's lines: the address starts the checks, and their end ends the run with `end`;
the loader writes the driver into flash only when the flash's MD5 differs, on the connection that boots the kernel.
A round that joins and checks takes 8 s, from some 40, and each line comes with its time.

**A fault names its place.**
The adapter's threads are watched by one of its own, which writes the faulted thread's registers and the top of its stack
into the log, and the host names the functions their addresses fall in.
It found a jump to 0x20 in `wifi_hw_stop`, through an entry of the OS table that had changed since the start.
The heap now refuses to free what it did not hand out, and says who tried:
hostap's `wpa_sm_deinit` frees the context `wpa_sm_init` was given, and the supplicant had given it a static one,
which the first stop of the radio put among the heap's free blocks, and the next allocations handed out the driver's data.
The OS table lies in flash now, read only, so that a stray write faults where it is made.
And two of the adapter's threads read their records before `osi_thread` had stored them,
which the watcher, started first, brought out.

**Two details of the protocols.**
hostap orders a TKIP group key's Michael keys as Linux's drivers take them and the libraries take 802.11's,
so every frame to the group failed its check where the group used TKIP.
An access point that protects management frames keeps an association a halted run never left,
and refuses the next run's first join until the station fails its query, so the driver leaves at the end of a run.

## One root task's half for both boards, and fewer of Espressif's libraries

**What the image holds, by library.**
A map of the driver's link gave each closed library its share:
libnet80211.a 177 KB of code, libpp.a 102 KB, libphy.a 28 KB, libcore.a 310 bytes, libbtbb.a nothing at all;
libcoexist.a was fetched and never linked.
Three of the five are gone, and the driver defines the few words libcore.a gave.
Replacing the other two takes a MAC of one's own, as esp32-open-mac wrote for the plain ESP32, a project of months.

**Code that did not do what its comment said.**
Turning the RF off at the end stood commented out, while its comment and its commit said it ran:
called, it faulted on a register of the PMU, which the driver's frames leave to the kernel.
And `phymap`, run over the driver, laid the PMU's registers to functions that never reach them,
because a weakened library function keeps its code, without a symbol, in a section it shares with others.

**The root tasks' shared half.**
The two root tasks built the network process alike, and only the Pico 2 W's built clients.
`system.h` holds what they share; it lies in the image the children run, so it keeps no global,
and a root task hands it a `struct system`, as the library takes a `struct self`, which cost nothing to write.
The ESP32-C6 then had too little memory for the clients: the room for children's data is a power of two,
and the hub one frame for every client's channel, four of 16 KiB.
The hub now has as many channels as the clients wanted need, and the driver's data a block of its own,
which leaves the ESP32-C6's root task 8 KiB and 9 slots: enough, and a number to watch.
Its echo answers in 13 to 22 ms at the median, run to run, where the Pico 2 W's does in about 5.

## The network's addresses, published

The clock asked the network process for its addresses, a round trip asked again every second until DHCP had given them.
The network process now publishes them through `lib/seqlock.h`, in 64 bytes of its page,
which the root task carves out of its room and gives the clock read only, and the clock reads them at every wake,
so it sees an address that changes too, and the sockets lost `SOCK_CONFIG`.
It costs the root task a slot and the clock a region.
A frame of their own cost the Pico 2 W's root task its last slots first, since free memory is halved down to a block's size,
a slot for each half, and the third client was not built.

## The MAC's receiving, the driver's own

The driver is to leave Espressif's libraries the PHY alone, see `TODO.md`, and nothing open drives the ESP32-C6's MAC:
esp-wifi-hal, in Rust, drives the plain ESP32's and the S2's, with the C3's and S3's under review,
and esp32c6-open-mac sends a beacon from the C6, with no licence to take code from.
So the MAC is the driver's own, read from the libraries' code, whose symbols name every function.
The station's logic is to be its own too, on hostap, which already runs the handshakes and SAE:
OpenBSD's or FreeBSD's net80211 would want a kernel's mbufs and timers emulated, as `osi.c` emulates FreeRTOS,
and Linux's mac80211 is GPL, which an image holding `libphy.a` cannot carry.

Receiving came easily, for rvuos's part:
the adapter runs the libraries' interrupt in a thread, so taking it is an exchange of a handler,
and the MAC writes into RAM the driver allocates, which open decision 13 trusts it with.
`listen=` holds the two against each other, by the beacons each hears of an access point and those it misses,
counted by the access point's own clock: a count by sequence numbers took its bursts to other stations for losses.
A scan of the driver's own followed, passive as the libraries' now is too:
the radio retuned through `libphy.a` alone, as the libraries retune it, and each beacon read by hostap's parser,
which `make mgmt-test` runs on the host, under the sanitizers, against every cut of a beacon.

## The MAC's sending, the driver's own

Reading what the libraries wrote to a slot to send a frame let the sending be the driver's own:
with `tx=own` the driver builds the frame's descriptor and programs the slot's PPDU words itself,
as the libraries' `lmacSetTxFrame` does, arms the slot, as their `hal_mac_txq_enable` does,
and programs a slot the libraries' lmac still names, for a legacy frame at one Mbit;
a probe sent so is answered by the access point.

The completion is the driver's too.
Arming alone left the frame unfinished when the driver held the interrupt, which `rx=own` does:
the MAC leaves a bit in its hardware txq's state when the frame is done,
and the libraries' `lmacProcessTxComplete`, which their interrupt posts, is what cleared it, and the slot's arm bits with it.
The driver clears that bit now, as `hal_mac_clr_txq_state(2, slot)` does, while it waits for the arm bits to clear,
so a probe sent with both `rx=own` and `tx=own` is answered the same, and their interrupt is needed for neither.
The state's other two groups are a timeout and a collision,
on which the driver disarms the slot, as their `hal_mac_txq_disable` does, and fails;
their `lmacProcessTxTimeout` also invalidates the queue first, by `lmacDisableTransmit`, which the driver does not.
A state bit of the libraries' slot may lie there from before the driver took the interrupt,
so the driver clears the slot's state just before it arms a frame;
without that, the first frame read a stale timeout bit and was failed though the access point answered it.
An own slot of the driver's, rather than the libraries' slot 0, would keep their completions out of its way,
and the libraries' pp, which retries a collision or a timeout, does more than failing and letting the caller send again.
It still clears the queue's own state byte, which `lmac_stop_hw_txq` reads to leave the slot alone
and `lmacProcessTxComplete` reads to skip a queue it is not finishing,
and reads none of the details `hal_mac_get_txq_complete` reads of a completion;
`pp` and `net80211` stay for the slot the driver borrows and the station's own logic.

## The station's own authentication

The station's own logic begins with the authentication.
With `sta=own` the driver scans for the network by its own code, takes the access point heard strongest, or the page's bssid,
retunes to it, and sends an open-system Authentication frame by `mac_tx`, reading the answer through `mac.c`;
the access point accepts it, and the libraries' station is never asked to connect.
The scan and the authentication are two takes of the receiving one after another,
so `mac_rx_take` makes its list whole again at each take, its descriptors and buffers made once.
The association, the keys and so the join are to follow the same way; `TODO.md` says what is left.

## A footing for the station

A review before the association found the station growing in `main.c` on bare offsets,
which is how the probe's wildcard SSID got through,
and two things from the air reaching too far:
a beacon's channel went to the radio unchecked, so one naming channel 14 would have tuned it out of Europe's band,
and a frame's length came from the PHY's header, never held against what the MAC wrote.
`mac_channel` now refuses any channel but 1 to 13,
`mac.c` hands a reader the frame alone, its length within what was written,
and the station's frames are `mgmt.c`'s, on hostap's definitions, which `make mgmt-test` reads back on the host.
The bound wanted a measurement first: the MAC writes a frame without its FCS, padded to a whole word,
and the first bound, which counted the FCS, dropped every frame of the first run.

The station then got a thread of its own, `sta.c`, waiting for events in one of the adapter's queues.
Two things shaped it.
The receiving runs in the interrupt's thread and may not wait, so it copies the station's frames into buffers a second queue hands out.
And only the first thread may wait on the driver's inbox,
so it stays the root task's side and passes the ask to leave on as an event,
and a step that fails in another thread stops that thread in a wait of the adapter's, not in `child_stop`.
No run asks the station to leave yet: the root task asks only a driver that serves.
`mac_tx` holds a lock, since EAPOL and the link's frames will come from two threads.

Two seams followed.
Once the libraries have brought the MAC up, the own path asks them for nothing but through `mac.c`,
which still has their promiscuous mode filter the frames.
And the supplicant, which was one with the libraries' table, is hostap's over a link, `supp.h`,
whose five asks, an EAPOL frame sent, a key installed, the handshakes done, a deauthentication and a timeout run,
are all the own station has to answer; the libraries' quirks around them stay in `wpa.c`.

## The station's own association and handshake

The association and WPA2's 4-way handshake took the footing as it was.
The station reads its access point's beacon for the RSN elements, which the supplicant holds message 3/4 against,
associates with the elements the supplicant writes, and hands it the EAPOL frames;
those come as data frames to the station's address, which the MAC passed with only management frames heard promiscuously.
The supplicant runs in the station's thread, and no stack has a guard,
so the adapter now paints a thread's stack, and the station's log says how deep the handshakes went.
The one surprise was the lab's, and `TODO.md` knew it already:
the Pico 2 W's access point, offering WPA3 too, leaves an element hostap checks out of message 3/4,
so the own station's runs there need that access point configured with `sae=0`, which offers WPA2 alone.

## The station's own data path

Once the handshake has installed the keys, the own station serves the link: the MAC's cipher takes the protected
frames it receives, and its sending is software CCMP, `ccmp.c` on hostap's CCM; see `TODO.md`.

**The keys.**
A pairwise key the handshake derives arrives twice, for receiving alone before the 4/4 and for sending too after,
so `link_set_key` stages the first and promotes the second, `keys.c`'s state machine, host-tested in `keys-test`;
the station keeps sending under the key in use in between, the old one at a rekey and none at the first join.
A group key is installed at once into the slot of its id, 1 or 2, which an access point alternates at each rekey,
so the old key stays beside the new and is read until the access point switches.
One installed again with the same bytes and id is left alone, its counters kept.
The management group key keeps its ids, 4 and 5, alike.
A key is written into the MAC's own entry, `mac_key_set` (`STA_KEY_ENTRY` 4 for the pairwise,
`STA_GRP_ENTRY` 0 and 1 for the group's ids 1 and 2, as the libraries' `esp_wifi_get_sta_hw_key_idx_internal`
places them), its valid bit cleared first, so a rekey never leaves half an entry live.
A group key of id 0 or 3 is refused: no access point here gave one, and the libraries would put 3 in entry 4, the pairwise key's.
An install and the sending take one lock, `sta.key_lock`, which copies the key and reserves its packet number together.
EAPOL goes out under the key in use once one is there, and in the clear until then.
A station's frame to the access point is addressed to it, so it goes under the pairwise key
however its own destination, Address 3, is addressed; the receiving picks the key by Address 1,
the group's for a group address.

**The frames.**
An Ethernet frame is laid into a Data frame, encrypted in software and sent.
A received protected frame comes to `read_frame` already decrypted by the MAC, its CCMP header left,
so the packet number is read back with `ccmp_head_read` for the replay check, one counter a priority,
802.11's traffic identifier, as the nonce and the additional data carry it, and the cipher is passed over;
the header is taken away and the frame handed to the supplicant if it is EAPOL, else made an Ethernet frame.
A protected frame the MAC did not decrypt, and a data frame left in the clear once the pairwise key is set,
are dropped. The access point sends a station's group frame to the group again, the station included;
the station counts its own and hands them no further, as FreeBSD's `net80211` drops them.

**The receiving's mode.**
A scan hears through the libraries' channel sniffer; the station leaves it and takes the receiving in station
mode, setting its own address, the access point's and the AID through `mac_receive`.
The MAC then hands up its own frames alone, decrypting them, where the sniffer passed every BSS's.

**What the board showed.**
Against the Pico 2 W's own access point, configured with `sae=0`, the station associated, installed the pairwise and group keys,
and served the link.
Each DHCP discover it sent under the pairwise key came back from the access point to the group, under the group key,
so both keys worked one way each; the access point's own discover came in too.
Nothing unicast came from the access point, so the pairwise key's receiving is not shown, nor an address:
that access point serves none.
The home network's WPA2 access point offers the group cipher TKIP, which the station refuses.
Its WPA2/WPA3 one takes the station by WPA2, which the station picks wherever it is offered,
and there too its broadcasts come back and the network's group frames come in.
No frame to the station itself did until the own path set the MAC's own station address and the BSSID,
which the libraries' association writes (`hal_mac_set_addr` and `hal_mac_set_bssid` of `hal_mac.o`,
at MAC + `0x5c`/`0x60` and + `0x00`/`0x04`), and took the receiving in station mode, its address set before
the authentication, with the libraries' channel sniffer left; `mac.c`'s `mac_station` sets the two addresses
and `mac_addr_restore` gives the libraries their MAC back at the leave.
`mac_key_set` writes the key into the MAC's own entry, the one `hal_crypto_set_key_entry` writes:
`0x600a5800 + entry * 0x28`, `+0x00` the peer address's low four bytes, `+0x04` its high two and a control word
above bit 16 (`0x086c` for a pairwise key, `0x88cc` for a group key), `+0x08` the temporal key,
and the entry's valid bit in a bit of `0x600a4814`.
The MAC decrypts each protected frame then, leaving its CCMP header, so `read_frame` keeps the packet number
and skips the cipher itself: with it the station takes its DHCP offer and an address and its echo answers.
The engine word the libraries' `hal_crypto_enable(0, 3, 0, 0)` writes, `0x30103` at `KEY_CFG0` (`0x600a4800`,
its other select `0x600a4804`), is left unset: with it set the access point took none of the station's sending
at all, and the sending stays `ccmp.c`'s.
A run with the group entry's temporal key one byte wrong had the access point's group frames not come at all,
its own relayed broadcast counting none, where the right key had them come, so the MAC hands over nothing
that failed its MIC, which the reading of a short frame rests on.
The MAC finds a group frame's entry by the key id in its CCMP header, not by the entry's place:
with a wrong key of the other id in the entry above the right one or below it,
or the right key in the other id's entry, the access point's group frames still came.

## The station's protected management frames

WPA2's PMF is taken where the access point offers it (`supp_choose`), the station advertising MFPC in the
association and keeping the IGTK message 3/4 carries, in software, since the MAC's cipher decrypts data frames
alone. Its own deauthentication goes under the pairwise key, `ccmp.c` protecting a robust management frame:
a 24-byte header, the Frame Control's subtype kept in the additional data where a data frame's is masked, and
the nonce's Management bit. An association answered status 30 is waited out by its Timeout Interval element and
asked again, from the authentication, a few times.
A deauthentication or disassociation is heeded only protected, its MIC and replay check under the pairwise key,
on the counter reserved for management (`ccmp.c`'s index 16). An unprotected one is a forgery, ignored, counted.
After one the station sends an SA Query of its own, its transaction id random, and leaves the link when the
access point does not answer within the whole time. An SA Query the access point sends is answered.
A group-addressed one is taken only with a valid BIP MIC over the IGTK, which `mgmt.c` checks in software.
