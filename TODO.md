# TODO

Open work only; an item leaves this file in the commit that finishes it.

## Wi-Fi system

`user/wifi/`, on a Pico 2 W: the driver brings the CYW43439's firmware up, scans,
runs an access point an ESP32-C6 joins by WPA3's SAE, or joins a network by WPA3's SAE or WPA2,
where DHCP gives the network process an address
and a laptop's ping and datagrams to UDP port 7777 are answered;
its clients, in processes of their own, answer on UDP ports 7, the echo, and 13, the clock,
and the logger carries the kernel's log to a host on port 7070 while the run lasts.
`WIFI_CONFIG`, see `user/wifi/build.mk`, says what to do and for how long, and `tools/wifi-run.py` follows the log and checks the clients.

- The access point's WPA3 transition mode refuses a station that joins it by WPA2 and checks the RSNXE,
  as hostap's supplicant does, the ESP32-C6's among them:
  the firmware, 7.95.61, the one embassy carries, names hash to element in an RSNXE in its beacons,
  but leaves the element out of the third message of WPA2's handshake, where IEEE 802.11 has it repeated.
  A station that joins by SAE is not refused, nor one at an access point of `sae=0`, which offers WPA2 alone.
  Of the firmware's variables tried, none turns hash to element off,
  and whether a later firmware repeats the element is not known.
- At half its lease the network process lets its address go and asks for one anew, as if it had none,
  so a run for good is unreachable for a moment every twelve hours with the router here, whose lease is a day.
  A renewal, a request to the server that gave the lease, would keep the address meanwhile.
- The clock asks the one server of pool.ntp.org it was given again and again; one that never answers,
  as happened once, leaves it without the time, and `tools/wifi-run.py`, which waits for the time, without its checks.
  Asking DNS for another server after a few tries would move on.
- In access point mode nobody gives the network process an address, so no client is built, the logger among them,
  and the log reaches the host only at the halt, as much of it as the ring still holds.
- The driver polls the chip every millisecond.
  The chip raises the data line, GPIO24, when it has a packet and chip select is high,
  which IO_BANK0 can turn into IO_IRQ_BANK0_NS, an `Irq` the driver would wait on instead.
- The root task takes the blob's frames back once the chip runs, so the driver can no longer reset the chip;
  the firmware in flash would let it, and writing flash is the maintainer's to decide.
- The bus runs at 25 MHz; embassy runs it at 37.5 MHz with the faster of its programs, and DMA would free the core meanwhile.

## Programs

`user/wifi/NOTES.md` says what writing a program of several processes was like, and `user/lib/` is what came of it;
what is left:

- The library's blocks never join again: the parent of a split is deleted to keep its slot,
  so free memory ends as blocks that each hold a slot,
  20 of the root task's 42 slots on QEMU once `user/libtest/` has given everything back.
  Keeping a split's parent while a half is taken would let two free halves join again,
  at a slot per split for as long as they are apart.
- `struct self` hands out the first core's units alone, so children run on the first core,
  and the Wi-Fi system holds all 64 of them: a fourth client would take some of another's.
- A channel between two peers is connected once: a child taken down leaves the other end naming nothing,
  and the child built in its place needs a channel of its own, which the other end has to learn of.
  A hub's ends are connected again, with the server's word that it let go of the last client;
  a peer could be given the same, which the driver and the network process have not needed, as neither is built again.
- A child's data comes from the parent's room whenever there is one, `self_room`,
  so a parent cannot keep some children in blocks of their own once it has made the room.
- A child's stack grows down onto its page, and nothing stops it there;
  a guard would cost a region, the scarcest thing a process has.
- `user/lab/` does not show yet a client of several threads asking at once, a driver whose work an interrupt starts,
  two cores, a client that makes its ring look full, or one that gives up on an answer.
  Nor several takers lending to one holder, or a holder that waits on a second lock.
  Nor a thin server, which only passes data on while its clients do the work,
  to tell what moving work into the client saves and what the hop still costs.
  Why its `chain`'s second hop costs no more than the first is not worked out.

## ESP32-C6

- Boot from flash, and with it whether the ROM can load the image
  straight from offset 0 or Espressif's second-stage bootloader must run first,
  and whether execute-in-place goes through a cache the kernel must control.
- What the ROM overwrites in RAM on a reset, for open decision 12.
- Try user mode on the rest of the user-level CSR space, here and on QEMU;
  only the CSRs the TRM lists and `0x800` to `0x802` were tried.
  `utval`, `0x043`, reads zero from user mode but was not written, so the kernel does not set it back.
- The counter rate is measured over one tick at boot,
  so it is only as exact as the polling loop in `timer_init`; a longer measurement would do better.
- A device interrupt wakes its driver but does not run it;
  the driver waits for its turn like a thread a timer line woke.
  Measure that latency on the board; it belongs to open decision 9.
- The chip's watchdogs stay off, so a kernel that stops altogether, and cannot halt, stays stopped here,
  where RP2350's resets the chip a second after the kernel's watchdog's deadline;
  `board_watchdog` could start the RTC watchdog the same way.
- The PHY tracer: the root task that watches the harness of `user/phyblob/` and carries out its device accesses,
  laid out as `user/phytrace.h` says, and a rule that boots it with the harness beside it;
  `make BOARD=esp32c6 phyblob` builds the harness, and nothing has run it.
- The tracer has to log and feign the six registers no frame covers, which `make BOARD=esp32c6 phymap` lists,
  in PCR, PMU and the LP domain; what their reads should answer is open.
  `phymap` sees only constant addresses, so a register reached through a table or a loop shows first in a trace.
  The harness reaches three of the board's frames, the SAR ADC, the modem and the eFuse,
  which with the tracer's own regions have to fit a process's eight region slots.
- A driver of the ESP32-C6's Wi-Fi that leaves Espressif's libraries the PHY alone, `libphy.a`:
  `pp` and `net80211` give way to the driver's own MAC and station, and `osi.c` shrinks to what `libphy.a` calls.
  The station is the driver's own already, on hostap; `user/wifi/NOTES.md` says why.
  It receives, scans and sends by `mac.c`, authenticates, by SAE where the access point offers WPA3's personal
  and by open system under WPA2's, associates, runs the supplicant's 4-way handshake over a link of its own, see `supp.h`,
  installs the keys it derives in the MAC's own cipher and in the station, and serves the link.
  What the driver still calls of `pp` and `net80211`, `make BOARD=esp32c6 wifi-esp32c6-refs` lists:
  the bring-up, and through `mac.c` their promiscuous mode, their receive policy, and their lmac's control block.
  The receiving is the MAC's: its own station address and the access point's are set before the authentication,
  the libraries' channel sniffer is left, and the MAC decrypts each protected frame, leaving its CCMP header
  for the replay check, so the receiving runs no software CCMP; `ccmp_decrypt` stays for `ccmp-test` alone.
  The sending is software CCMP still, `ccmp.c` on hostap's `aes-ccm.c`, under the pairwise key;
  with the engine word set the access point took none of the sending, so it is left unset;
  the MAC's own cipher is in `NOTES.md`.
  The station's data go at 1 Mb/s, the rate `mac_tx` programs for management frames.
  A rekey is checked on the host alone, by `keys-test`:
  showing a pairwise one live wants an access point that answers a station's rekey request,
  which the one here does not, or one that rekeys on its own;
  a group one, the old key read beside the new until the access point switches,
  wants one that rekeys its group inside a run, which the one here did not in 300 s;
  its group key interval, shortened for a run with the maintainer's leave, would do.
  The station's protected management frames — the leave policy and the SA Query — want a file of their own beside
  `sta.c`, which has grown to hold them.
  The MAC's bring-up stays the libraries': the own path runs between their `esp_wifi_start` and `esp_wifi_stop`,
  so what those do to the modem and the MAC is the last of `pp` to read before it can go.
  Its frame completion spins in the caller's thread while the slot is armed, and borrows the libraries' slot 0;
  an own slot, the driver's own interrupt to finish the frame, and the libraries' retry of a collision or a timeout,
  with their invalidation of the queue on a timeout, would do better.
  `mac.c` points a MAC that ran out of descriptors past the last it filled by `RX_BASE` alone,
  which the take found not always taken; no run has run out yet, `restarted` 0.
- The ESP32-C6's driver takes all its memory from one heap: the libraries' as they run,
  the own path's buffers, stacks and queues once, at its start, and the supplicant's and SAE's as they run, hostap's and Mbed TLS's way;
  SAE leaves a few kilobytes at the fewest.
  The own path's memory can be static,
  and the supplicant's a bounded arena of its own, or fixed buffers over a P-256 of fixed width.
- The ESP32-C6's Wi-Fi root task has 8 KiB of memory and 9 slots left once its clients run,
  so another client needs room found first, in the driver's block or a smaller `CLIENT_CHAN_SIZE`.
- The ESP32-C6's echo answers in about 7 ms at the median, the Pico 2 W's in about 5:
  where the time goes, the radio at 1 Mb/s, the software CCMP, or the driver's turn after its interrupt, open decision 9,
  is not measured.
- The Pico 2 W's run lasts its `run=` whatever its checks found, or until a reset by hand,
  since the host has no way to end it: its USB serial port, `kernel/board/rp2350/cdc.c`, is the halt's alone.
  That port kept up while the run lasts would let the host end it there, in either mode,
  where the network does not reach the lab's access point; closing the port already reboots the chip into BOOTSEL.
  Whether the kernel keeps it, or the root task is given the USB controller as the ESP32-C6's is given its console, is open.
- The ESP32-C6's driver stops once the access point lets the station go, and the root task ends the run:
  nothing joins again, as ESP-IDF leaves to the program too.
- The ESP32-C6's kernel moves the CPU to the SPLL from whatever source it finds, but has only found the SPLL:
  the crystal, which a power-on leaves when the flash holds no bootloader, is untried.
- The ESP32-C6's SAE takes a fifth of a second between the access point's commit and the station's confirm,
  two P-256 products in software, and its own commit a third of one by hash to element, or twice that by hunting and pecking.
  IEEE 802.11's default has the access point send its own again after 40 ms and give up after a few; the two here waited.
  The chip's ECC accelerator, which no frame lists yet, would shorten it.
- The ESP32-C6's station takes WPA2's PSK and WPA3's SAE on P-256, with CCMP alone, pairwise and for the group:
  SAE's extended key, of the larger groups, fast transition, PSK over SHA-256, and a TKIP group key it does not take,
  and of the network's access points it joins the strongest whose suites it takes.
  Of the management group ciphers it takes BIP-CMAC-128 alone, not the GMAC of the 192-bit and GCMP suites.
- The ESP32-C6's station keeps the radio on between beacons, and tells the access point nothing of power save:
  the modem's sleep until each DTIM beacon, which the libraries' station did, though not well over the adapter, is to be its own.
- ESP-IDF tracks the PHY's PLL every second while the radio runs, `phy_track_pll` of `esp_phy`;
  the adapter does not, which a run of hours may need.
- The ESP32-C6's radio stays on until the next boot when a thread of the driver's faults
  or the root task halts for the network process; the driver turns it off only on its own way out,
  and even then the RF's analog buses stay powered in the PMU, `PMU_RF_PWC`'s bits 28 to 31,
  which the kernel's `board_init` sets and the driver's `phy_xpd_rf` leaves alone.
  The kernel's halt could power the modem down, once what of the PMU does it is measured.
- At a MikroTik wAP ax here the ESP32-C6 misses frames to the group that the access point sends 10 µs after another,
  in its run after a DTIM beacon: of those after a frame with More Data set,
  from a third to nearly all in runs of half a minute, none in some short ones, and not even as broken frames.
  ESP-IDF's own stack misses them alike, and a Pico 2 W at the same access point at the same time heard them all:
  the loss lies in Espressif's libraries or the chip, reported in esp-idf #16096 with a sketch on the branch `esp32c6-group-loss`.
  `net.c` asks DHCP for unicast answers for it, but an ARP request for the station is a broadcast still,
  so a host may wait seconds to reach it.
  There the ping of `make BOARD=esp32c6 wifi-esp32c6` fails now and then:
  the station answers every request, the host hears fewer than four of five, and where the answers go is not known.
  The Pico 2 W's `debug=dhcp` and `bssid=` measure it; whether the own station, which receives through the MAC
  without the libraries, misses the same frames is not measured.
- The ESP32-C6's station refuses an open network: it takes an access point whose beacon has an RSN element,
  and a passphrase.
- With the APM units confining the modem to the driver's block, as measured for open decision 13,
  the modem reached `0x4082c7ac`, in the root task's code block past its image, once in each of two runs,
  before the driver had started, and the units refused it; five runs after showed nothing.
  What of the modem reaches memory before a driver programs it, and why there, is not known;
  with the units off, as they are, such an access lands.
- The driver's process holds all eight regions, so anything more it needs takes a region from something it has.
  A pager would lift that: a thread that, when another faults on a frame its process may have,
  installs the frame in place of one it is not using and resumes the thread, which runs the access again.
  The kernel allows it as it is, but writes a report of every fault into the log, so it suits frames touched seldom.
- Feed the replay corpus to the board.
  Something has to put each input where `BOOT_CAP_INPUT` points,
  below the ROM's buffers or over USB once the kernel runs,
  and the replay driver's second stack has to follow the board's data region.

## RP2350

- Hazard3's `mtval` reads zero, so a fault report names no address there.
- Hazard3 reads a NAPOT address's bits below the grain as zeros, the fence's as `0x1ffffffc`,
  though it matches the region written; the specification reads them as ones.
  So the self-check's reading of the CSRs, which runs only under tracing and never on the board yet,
  would take every region there for eight bytes; `pmp_get` could set those bits, as it swaps R and X.
- The logger drives no device here: its console is RAM, which only the halt carries out.
  A root task with a driver for the USB controller would let the log out while the machine runs.
  The demo on two cores fills about 7.7 KiB of its 8, so a few more lines overflow it, which `tests/run.sh` fails.
- The watchdog armed at boot resets CLOCKS without SYSCFG's `AUXCTRL` set,
  which the datasheet asks for when POWMAN runs from `clk_ref`, so a hang it rescues may corrupt POWMAN;
  the halt's reboot sets it, as the bootrom's does.
  Setting it at boot keeps POWMAN's watchdog-reset input asserted for the whole run, which is untried.
- Boot from flash, and feed the replay corpus to the board.
- The escape suite runs on the first core alone, with `CORES=2` too, on either kind of core,
  so nothing tries the second Hazard3's fence over the hardwired PMP entries or its shut counters,
  nor the second Cortex-M33's MPU;
  a scenario bound to the second core's units would.

## ARM

- The escape suite's two scenarios of the frame on the thread's stack have one thread each, which leaves two holes.
  `stack_takes` saying yes to anything passes `escape-unstack`,
  since unstacking from memory the thread lost faults as it does from the guard;
  a second thread that makes the first's stack read only while it waits would show it.
  And `frame_take` leaving the dropped call pending passes `escape-stack-call`, since no thread runs next to take it.
  The one-boot suite under "Verification" would have the second thread.
- QEMU 11 makes a pending exception a `wfe` wakeup only for the NVIC's lines, not for SysTick,
  since its `SEVONPEND` looks at external interrupts alone, where the architecture says any exception;
  so `make arm-test` waits in `intr_wait` for ever there, from the first sleep of the demo.
  QEMU 10.2 treats `wfe` as a hint and passes, and the Cortex-M33 wakes as the architecture says.
  Report it to QEMU, or make the compare a board timer on an NVIC line where one is free.
  Under QEMU 11 that would also let the MPS2 boards sleep through what their idle kernel now polls,
  most of each ARM demo's half minute, the longest part of `make check`.
- The Cortex-M33 has no `DISDEFWBUF`, so a bus fault on a thread's store may come imprecise,
  after the kernel has changed threads, and stop the thread that runs next;
  a device region is nGnRnE there, and whether that makes the fault precise is unmeasured.
- A host harness with ARM's frame, so the fuzzer reaches `frame.c` and the call registers;
  the host build knows RISC-V's alone.
- The demo's console never waits on UART0's line under QEMU, whose transmitter sends at once,
  so nothing runs `irq_enable` unmasking a line of the NVIC with its level still high.
- `kernel/board/mps2/timer.c` keeps the counter's wraps with an exclusive pair on memory the MPU leaves Non-shareable,
  which reaches the other core under QEMU alone; on an MPS2 board it would need the region `arch_ticket_take` borrows.
- The halt of the MPS2 boards leaves QEMU through semihosting, which on a board without a debugger is a fault in the kernel.
- Nothing arms or disarms a device line from a second core, mps2-an521's or RP2350's of either kind,
  so the first core catching its controller up with a change made on the other, `irq_sync`,
  and a claim that passes over a line no `Irq` is armed on any more, run in no check;
  the bind's refusal of the cores' line runs in the demo alone, since the host's board has no such line.
  A logger on the second core, whose UART's line the first core takes, would run the first of them.

## Verification

- No seed reaches a payer dropped as the turn's thread is destroyed:
  the turn's thread is the caller on its core, which keeps what it runs on, so only a call on another core can destroy it.
- No check sees who pays for a turn on lent time, only that the payer is charged:
  a kernel that chose no payer, or let a waiter without time hand its turn on, would run the borrower for free,
  and only `make lab` would show it.
  Nor does one see that a signal wakes the thread paying for the signaller's turn first, and hands it the turn back:
  a kernel that did neither would be slower, which only `make lab` shows, `lend-hog` and `lend-server`.
- Feedback on the state beyond the kernel's edges, libFuzzer's extra counters
  over the objects of each type, the threads waiting and stopped, the pools dying and the `Irq`s armed,
  found no more mutants than edges alone in 5000 runs each, and was left out;
  a finer picture, such as the derivation tree's depth, is untried.
- From nothing, `-len_control=100` keeps the inputs to one record for tens of thousands of runs,
  since libFuzzer grows the limit by bytes and a record is sixteen;
  `-len_control=20` reached more edges in a short run.
  Measure it with `make mutants-fuzz MUTANTS_FUZZ=-e` before `make fuzz` takes it.
- Two cores are checked by `fuzz-smp2`, which takes the cores' traps one after another, and by the demo,
  which on mps2-an521 and RP2350's Cortex-M33 is the only check of ARM's lock, its waits in `wfe`
  and the start of the second core;
  only RP2350's runs, outside `make check`, would show the ticket's borrowed region missing.
  Nothing makes a core wait for the lock while another walks, so a walk stopping for a waiting core runs only in the host's yes to everything;
  nothing replays the corpus on two harts against the host, which needs the replay driver to order its threads' records across cores;
  and the self-check never runs on two harts, since the demo does not trace.
  The demo's shootdown reaches the reload of the regions as the lock is taken only when the trap there ends no turn,
  which QEMU's turns decide; `fuzz-smp2` always reaches it.
  Nothing measures what the demo's two-core checks catch, since `make mutants` leaves `smp-test` out.
- What the PMP or the host decides of a load or store record is no edge of the kernel,
  so the corpus keeps such a record only where the fault it makes leads somewhere new,
  and nothing keeps one for the decision itself.
- `make mutants-fuzz` finds neither `alloc-in-dying-pool` nor `bind-in-dying-pool`,
  from the corpus or from nothing, in 5000 runs:
  a stop armed at the right place, a pool dying and an allocation from another thread
  are three records the mutator must line up.
  A longer run lines them up now and then:
  about 664 000 runs of `make fuzz` made 28 inputs that catch `alloc-in-dying-pool` and 8 `bind-in-dying-pool`,
  all with `OP_DEBUG_PREEMPT`, where 186 000 made none.
  None reaches an edge the others do not, so the corpus keeps none of them.
  A mutation that inserts the three together is untried.
- `make mutants-fuzz` finds neither `borrower-destroy-keeps-lenders`, `borrower-destroy-keeps-waiting`
  nor `lend-ignores-right` from the corpus in 20000 runs, and only the seeds catch them:
  a thread that lends to another of a pool a revoke destroys after,
  and a lend through a capability without `RIGHT_X` from a caller that holds none with it,
  are each a few records that must name the same thread.
- The self-check is most of what a fuzzing run costs, and grows with the square of what the machine holds:
  `check_node` walks each node's ring up to its parent and the parent's ring back to it,
  and `check_nesting` walks every node for every Untyped and every pool.
  Walking each ring once from its parent, and holding the rings' lengths to the nodes that are not roots,
  would make the tree check linear,
  but a broken ring would give other reports, and the mutants' headers would change with them.
- An input of one record costs about a fifth of what the average input does:
  every input boots the kernel and makes the 48 calls of the prologue in `rvuos/replay.h`,
  each checked by `host/history.c`.
  A snapshot of the state after the prologue would spare that,
  if it held every global `host_boot` resets and the ASan poison over RAM.
- `fuzz-work` counts the loop annotations' claims only where the corpus reaches.
  The host's `irq_claim` never returns a line,
  so nothing counts the claims on the way from an interrupt through `sched_claim_interrupts`,
  its `IRQ_LINES` bound among them.
- That an object holds nothing from before its pool took the memory
  shows only where the garbage is a capability or makes a call fail;
  `host/history.c` does not know what the memory held before the call,
  so what a process leaves in memory it turns into a pool
  is checked where a store record wrote it, as in `tests/seeds/dirty-frame-becomes-pool`,
  and elsewhere by the pattern the host's RAM starts out holding.
- `tools/loop-bounds.py` knows clang's jump tables by their shape;
  any other jump through a register may make up a loop, which fails the link, never passes it,
  as a table whose base clang keeps on the stack does.
- The link checks and `make bench` read what clang makes, and a cloud session's clang is not the local one, 22:
  a tree that linked in the cloud once failed `loop-bounds` locally,
  and the ARM boards' costs in the cloud miss the records, which 22 wrote, by more than a tenth.
  Pin one clang in `.claude/hooks/session-start.sh`, or run the checks under both.
- That the count of armed sources leaves the log's line out is checked by reading `irq_set_bits`.
  Under tracing each call's line reaches the log before the self-check runs,
  so an `Irq` armed on the log's line has always signalled by then.
- That a signal orders memory for the wait that takes its bits, `MANUAL.md` section 5.7, is checked by nothing:
  QEMU under icount runs the harts one at a time, and the library's children all run on the first core.
  Only a writer and a reader on RP2350's two cores could show the kernel's lock missing its acquire or release.
- The exit to user mode is checked by nothing but runs under QEMU;
  `start.S` is not in the host build.
  QEMU breaks a reservation on every trap by itself,
  so nothing there would show the `sc.w` in `trap_return` missing.
  The trap path is small enough to prove against the Sail model of RISC-V.
- The PMP image is checked against `pmp_napot_range`, the kernel's own reading of NAPOT,
  so a misreading the encoder and the decoder share passes the self-check.
  A decoder written from the specification alone, or the Sail model, would catch it.
- ASan sees the kernel reach RAM outside its objects, but not one object reach into the next,
  which lies right behind it as on the target.
  Such a write shows only if the self-check or `host/history.c` sees what it damaged.
  A red zone between objects in the host build alone would show it,
  but would change which allocations fit, and with them the transcripts the host and QEMU compare.
- The self-check sees structure, not semantics.
  That a signal wakes a thread waiting on *that* notification,
  and hands it the bits that were set,
  is checked only by the demo in `user/init/`,
  and so is that a fault signals the watch of the thread that faulted and stops it where it faulted;
  the host and QEMU run the same kernel code, so their transcripts agree on a fault that signals nothing.
- The log's line is replayed through the trace itself,
  `tests/seeds/log-signals-untaken` and `log-wakes-reader`;
  a reader falling a whole ring behind, and the logger's byte-per-interrupt path,
  are checked by the demo's logger in `user/init/logger.c` alone,
  and the loss only by reading its code.
  A record that writes garbage into the log's `taken` is beyond the fuzzer,
  since a store record leaves the log alone, which the driver carries out on QEMU;
  the clamp in `klog.c` is checked by reading it.
- `KERR_STATE` for a thread destroying the pool its table or its process lies in, but not the thread,
  is checked only for the two together:
  the driver's third thread lies apart from its process, but the process's table lies with it,
  and the demo's root task lives with both.
- The replay driver has three threads, and the third shares the second's process and lies in the first's pool.
  More of them, each with its process, pool and Untyped as the second has,
  would give the derivation below the free RAM more branches;
  the prologue in `include/rvuos/replay.h` and `host_boot` grow with them.
- No harness reaches `KERR_LIMIT` in `process_install`.
  Since regions are NAPOT blocks, each costs one PMP entry,
  and a process has eight region slots,
  so a budget of eight, `fuzz-pmp8`'s, is never short:
  the eighth install finds seven regions installed.
  `fuzz-rp2350`'s seven are short for an eighth region, which no seed and no input of the corpus installs.
  The replay driver's processes hold four regions each once set up,
  so a harness with a budget of six would reach the limit on the third install
  and still run the prologue.
- Escape-attempt suite: one boot for the suite.
  `make escape` runs one root task per scenario, each expected to fault a specific way;
  see `user/escape-*.c` and `tests/escape.sh`.
  Since a fault stops only its thread, one root task could run every scenario in a thread of its own
  and hear each fault through its watch, one boot for the suite;
  it would read the cause through `OP_THREAD_FAULT`.
- The ESP32-C6's install rule, `PMP_SPLIT_STORE_AS_READ`, is checked only by `escape-misaligned-store` on the board.
  The host shares QEMU's layout, where the log touches the code and the data the input,
  so no harness can have the flag, no mutant reaches the rule,
  and its self-check runs only under tracing, which nothing does on the board yet.
  A host machine with the flag needs a layout that keeps those apart.
- CBMC on the halves `OP_UNTYPED_SPLIT` makes, `OP_FRAME_CARVE` and the NAPOT encoding.
- One layout header consumed by C, the linker scripts and
  `tests/differential.py`.
  C and the linker scripts share `kernel/layout.h` and the board's `board.h` now;
  the replay driver's second stack in `rvuos/replay.h`
  and `tests/differential.py` still carry QEMU's addresses of their own.
- A preemptible call that takes its caller's own code or data mapping,
  a revoke below the frame or the Untyped they came from,
  and that stops for a tick on QEMU leaves its thread to fault on the `ecall` it would make again,
  while the host, which makes a stopped call again at once, finishes it and traces one line more.
  The corpus has no such input; the fuzzer made one within minutes,
  which `make qemu-replay` fails by that line.
- A call that stops without putting its thread back on the `ecall`
  the host takes as finished, and no host check sees it unless `OP_DEBUG_PREEMPT` armed the stop;
  `make qemu-replay` does, by the trace line the host then lacks.
- The host moves time a tick at a time, and under tracing every turn begins at a tick,
  so what a turn begun within a tick pays at the tick, `turn_next`,
  and the ticks a trap counts at once, `account_after`, run only in the demo, whose bounds are loose.
  A demo thread held to its units that always takes over half way into the tick
  would run its units' part only if it paid for no more than that half.
- Once a record binds a driver thread to units, and so to time the others do not have,
  passing a record round may come back before it visited every runnable thread,
  and a record for a thread that could run is performed by another.
- No harness reaches a device's frame from the boot: QEMU and the host list no devices,
  and only RP2350's Wi-Fi system carves one.
  Listing one on QEMU, its RTC say, would let the fuzzer carve and install it,
  at the price of renumbering the seeds and the corpus and a frame more in the mutator's boot table.
- The seeds under `tests/seeds` are binary and were written by hand.
  Renumbering a `BOOT_CAP_*` or `REPLAY_CAP_*` slot takes a one-off script
  that knows which argument of which operation is a slot,
  run over the seeds and the corpus,
  and checked by replaying every seed on the kernels before and after and comparing the statuses;
  an operation whose arguments change leaves the seeds that use it to be written anew.
  `build/host/fuzz --print` shows a seed a record per line, by the names of `host/ops.h`;
  a generator in the repository reading such lines back
  would make the seeds readable and the next renumbering a rebuild;
  replay slots that start a few above `BOOT_CAP_COUNT` would spare the next one.
  A seed written before a renumbering and merged after it keeps passing and tests nothing:
  three seeds once did, and only a mutant nothing caught showed it.
- `make mutants` plants with `git apply -C1`,
  which, when the context has changed and one line of it matches in several places,
  silently takes the place nearest the patch's line numbers,
  and a refresh leaves those numbers old wherever they broke no tie.
  A refresh calls such a patch stale;
  planting through the refresh's `carry` would make the two agree.

## Code

- The four bytes little-endian, `le32`, is written three times, in `user/wifi/esp32c6/mac.c`, `main.c` and `user/wifi/wlan.c`;
  one in `user/lib/` would serve all three.

- The runs that check the ESP32-C6's `mac.c` and the station's own code are `WIFI_CONFIG` variants run by hand,
  named in `CLAUDE.md`; a target that runs them all, `make BOARD=esp32c6 wifi-esp32c6-mac`, would hold them in one place.
- The replay driver drains the log by polling after each record,
  so the host models it with one store into the header per event.
  A logger thread in the driver would make traced calls the host would have to follow.
- The UART driver in `user/init/logger.c` transmits only,
  because `make test` feeds the UART nothing to receive.
- Revoking below a line capability takes the line back only while it is unbound.
  The `Irq` could hold its line as it holds its notification, by a node below the line capability,
  whose clearing disarms the `Irq` and frees the line; `DESIGN.md`, "Interrupts".
- A pool whose every capability was deleted or revoked stays until the Untyped above it is revoked,
  and nothing tells the holder of that Untyped what is left below it:
  `OP_UNTYPED_INFO` says only that something is.
- A signal and a wait in one system call, the shape of seL4's `ReplyRecv`.
  Today it saves one trap per round trip and no context switch,
  because a signal does not take the processor away from the signaller.
  Should a woken thread ever preempt the signaller,
  which open decision 9 in `DESIGN.md` leaves out,
  it saves switches as well; that is when to add it.
- `kernel/arch/arm/nvic.c` and RP2350's `riscv/irq.c` each keep the lines the first core is to enable
  and catch the first core's controller up in `irq_sync`;
  one copy over a controller's enable and its read-back would serve both, and the next controller of each core's own.
- `pmp_init` stops counting at the first hardwired entry.
  A core with writable entries above a hardwired one loses them;
  have the image skip such entries if one turns up.
- `selfcheck.c` is always compiled in; make it a build-time option
  before any board with little RAM.
- The root task is granted one block of free RAM,
  so what lies between the blocks is unused:
  about 2.8 MiB on QEMU, where it does not matter, and 28 KiB on the ESP32-C6.
  So is the root task's own memory past the boot pool, 60 KiB on QEMU and 20 KiB on the ESP32-C6,
  until the root task is gone; open decision 20 in `DESIGN.md`.
  Grant the rest as further blocks, or lay the board out afresh,
  when a board's RAM gets tight.
- A change of process writes every PMP entry the core has, sixteen on QEMU and the ESP32-C6,
  each through a read, a change and a write of its `pmpcfg` register:
  `make bench` measures the load at about 1,000 cycles a switch on the ESP32-C6 and 400 on Hazard3,
  beside 800 to 900 for the rest of a switch.
  Writing the image's four `pmpcfg` words whole, and `pmpaddr` only where an entry is in use, would save most of it;
  mutants stand in `process.c` and `pmp.c`, and the records of `tests/bench/` would move.
- `PROCESS_REGION_SLOTS` is fixed at 8, whatever the PMP budget;
  size it per process from the budget.
- `pmp.h` names the protection unit's interface after PMP, and `kernel/arch/arm/mpu.c` implements it too;
  the image keeps PMP's encoding on both, but `pmp_set` and `pmp_entry_count` read as RISC-V's.
  Rename it with the next change to the image in `process.c`, where mutants stand,
  and `kernel/arch/arm/armv7m.h` with it, which serves ARMv8-M too.

## Scheduling

- A thread whose account holds little more than a tick loses its time when its turn ends a few counts past a tick,
  as a trap that counts the tick late ends it, and spends the rest of that tick on spare time, or waiting without it.
  One that earns its whole core keeps its account where it is while it runs,
  so one bound with an empty account stays there; the demo's successor sleeps first.
  Time judged against what the rest of the tick costs, as `turn_owes` charges it, rather than a whole tick,
  would keep its turn, and change nothing under tracing, where every turn begins at a tick.
- A turn is one tick while another thread could run, so the timer skips ticks only for a thread alone,
  and a turn that begins mid-tick gets only the rest of one.
  A turn of a few ticks, or of what the thread's account holds, would skip them while threads compete too,
  and give turns the length the scheduler chooses: `sched_wake_ticks` would return the turn's end instead of one.
  It changes what `OP_DEBUG_TICK` means to the replay, which passes records with it,
  and what the demo's "root: units ok" measures; decide it with open decision 9.
- Threads with time take equal turns,
  so a thread with more units gets its larger part only by running on
  after the others have spent their accounts.
  A turn of as many ticks as the thread's units weigh would spread that over time;
  decide with the first workload that wants it, and with the turn counted from the switch below.
- A trap reads the counter on entry whenever the timer is set past the next tick,
  which QEMU counts at a sixth of the cheapest system call.
  Counting only where the count or an account is read, the timer lines' calls and a change of turn among them,
  which reads the counter already to charge the turn, would spare it,
  at the price of a rule every such place has to keep.
- Threads cannot spend one account together, and spare time goes round by thread,
  so a group of threads shares a part of the processor only by splitting units, a sixty-fourth at least each.
  An account several threads are bound to would bring the share back; decide with the first workload that wants one.
- An account holds at most a tenth of a second's worth of its units.
  More lets an idle thread bring back a longer burst, which the others then wait out;
  less holds a thread nearer its units over short stretches;
  decide when a workload measures the difference.
- A thread is stopped by revoking its units and started again by binding it,
  which is what a userspace scheduler, open decision 9 in `DESIGN.md`, needs.
  A thread waiting on a notification stays on it meanwhile, and cannot be taken off it today.
- A thread that waits hands the rest of its turn to the next on the run queue,
  so a server woken by an ask waits for the turn ahead of it, a tick beside a busy process, whatever units either side earns;
  see `user/lab/NOTES.md`.
  Handing it to the thread the waiter just woke, and the answer's back, might take that tick out with no change to the ABI;
  try it against `make lab`'s `hog`, `rich-ui`, `rich-server` and `chain`.
  A waiter that lends does hand its turn so, and has it back with the signal that wakes it:
  `lend-server` against `poor-server`, `lend-hog` against `lock-hog`.
- A server runs on the time of the client that has lent it time longest, not of the client whose ask it serves,
  so a client that lends pays for whatever the server does until the server wakes it, other clients' asks among it.
  A server that ran on the time of the client it serves would have to name it, a call more for each ask;
  `make lab`'s `free-rider` and `lend-rider`, and open decisions 5 and 27.
- A thread lends only on its own core.
  Running the borrower on the lender's core for the turn, as Linux's proxy execution does, would lend across cores
  and keep each core's time to its units; open decision 27.
