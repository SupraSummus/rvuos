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
- The clock asks the network's one DNS server again and again until it answers; one that never does leaves it without
  the time, and `tools/wifi-run.py` and `tools/wifi-share.py`, which wait for it, without their checks, as happened once.
- In access point mode nobody gives the network process an address, so no client is built, the logger among them,
  and the log reaches the host only at the halt, as much of it as the ring still holds.
- The driver polls the chip every millisecond.
  The chip raises the data line, GPIO24, when it has a packet and chip select is high,
  which IO_BANK0 can turn into IO_IRQ_BANK0_NS, an `Irq` the driver would wait on instead.
- The root task takes the blob's frames back once the chip runs, so the driver can no longer reset the chip;
  the firmware in flash would let it, and writing flash is the maintainer's to decide.
- The bus runs at 25 MHz; embassy runs it at 37.5 MHz with the faster of its programs, and DMA would free the core meanwhile.

## Thread

`user/thread/`, on an nRF52840, drives its radio in IEEE 802.15.4's mode, surveys the channels,
and exchanges frames with the ESP32-C6's driver on `i154=`, each acknowledging the other's; `tools/thread-pair.sh` runs both.
Both send after CSMA-CA, `mac154.h`'s, and again until acknowledged;
the nRF52840's link, `link.c`, counts each neighbour's ETX.
Thread, IPv6 over 802.15.4 in a mesh, needs more of it, step by step:

- The level the nRF52840's survey reports is the radio's own, 0 to 127;
  beside the C6's survey, which reads dBm, a level is the dBm above -94, to a few dB, but the two surveyed seconds apart.
  The assessment's threshold, -75 dBm, rests on that; the C6's frames at one moment would do as a known signal.
- The nRF52840's radio starts its next reception as the driver wakes for the last frame,
  so a frame that begins within that wake is lost; only an acknowledgement follows so closely, 192 us after its frame.
  A shortcut from END to START, with the buffers' turn kept, would close it.
- The nRF52840 acknowledges a frame two wakes past aTurnaroundTime rather than at it,
  as its driver learns of a frame's end only by waking for it, and its alarm comes a wake late.
  The C6 waits macAckWaitDuration, which holds that, but a receiver that waits less would not;
  Nordic's driver arms the radio's own turnaround, TIFS, while the frame still arrives, from its addresses.
- The nRF52840's backoffs draw from xorshift, seeded by the clock and the survey, where the C6's draw from its RNG;
  the chip's RNG, a device the boot does not grant, would keep two nodes booted together from drawing the same.
- The two use fixed short addresses in a PAN of mac154.h's own, until Thread gives them theirs.
- 6LoWPAN over the frames, and IPv6, ICMPv6 and UDP over it; `user/wifi/net.c` knows IPv4 alone.
  Both sides' exchange is a test of its own today, the C6's inside its driver;
  under IPv6 each radio serves a link to a network process instead, as the C6's Wi-Fi does.
- The mesh, RPL by default, see `design/decisions.md`, 30,
  and a border router, the ESP32-C6, between the mesh and the Wi-Fi network,
  which on one chip shares the radio between the two; its driver brings up one or the other today.

## Programs

`user/wifi/NOTES.md` says what writing a program of several processes was like, and `user/lib/` is what came of it;
what is left:

- The library's blocks never join again: the parent of a split is deleted to keep its slot,
  so free memory ends as blocks that each hold a slot,
  20 of the root task's 42 slots on QEMU once `user/libtest/` has given everything back.
  Keeping a split's parent while a half is taken would let two free halves join again,
  at a slot per split for as long as they are apart.
- The Wi-Fi system starts every child on the first core and holds all 64 of its units:
  a fourth client would take some of another's, and the second core, where there is one, runs none of it.
- A channel between two peers is connected once: a child taken down leaves the other end naming nothing,
  and the child built in its place needs a channel of its own, which the other end has to learn of.
  A hub's ends are connected again, with the server's word that it let go of the last client;
  a peer could be given the same, which the driver and the network process have not needed, as neither is built again.
- Every child's objects get a pool of `CHILD_POOL_SIZE`, 4 KiB, whatever its table,
  where the ESP32-C6's clients fill 936 bytes and its network process 1128,
  and the driver's own pool, `DRV_OWN_POOL`, is 8 KiB for 1544.
  Sized to fit, they would leave the ESP32-C6's root task 8 KiB more,
  and 14 with the driver's own, at a slot for each block that halving it leaves;
  `OP_POOL_INFO` tells what each object takes, so `user/lib/child.c` could size them by asking.
- A child's data comes from the parent's room whenever there is one, `self_room`,
  so a parent cannot keep some children in blocks of their own once it has made the room.
- A child's stack grows down onto its page, and nothing stops it there;
  a guard would cost a region, the scarcest thing a process has.
- `child_stop` in `user/lib/child.h` reports and then waits for good, since a child holds no capability to its own thread:
  the convention giving each child its thread, which a self-stop takes, would let a stopped child be used again by a resume,
  but `RIGHT_W` on it also sets its watch, so it waits for the narrower right open decision 32 weighs.
- `lib/bulk.h` has no user but its test and the bench.
  The Wi-Fi system's link and sockets would be the first:
  the driver lending the network process the MAC's received frames to read, where they lie, a buffer kept back for its own,
  the network process copying each datagram once into its client's buffers lent to read,
  and lending each client buffers to fill for what it sends, as the driver would lend it buffers to fill for frames;
  `DESIGN.md`, "Bulk data", counts the copies that leaves.
  The ESP32-C6's MAC reading a frame from a chain of descriptors, untried,
  would let the driver send the network process's frame where it lies, its own header in front, and spare the copy out.
- A borrower cannot lend on what it was lent:
  a stack passing a driver's frames uncopied to a client allowed to read them all, `DESIGN.md`, "Bulk data",
  would need a lender's end over a block of another lender's frame,
  the stack keeping what it lent on until the client hands it back.
- `bulk_close` takes a connection's blocks from its borrower at once, so a borrower that is to live on faults at its next touch;
  one whose lender is built again, a network process whose driver is,
  would want a close that tells it first and waits for its word, as the lender's.
- A hub keeps a slot of its parent's for each client connected, its channel's carve, which `chan_hub_close` revokes;
  `lib/bulk.h` deletes its carves once installed and uninstalls them at a close, which would spare a hub's parent those slots too.
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
  The access is decoded by `user/tracer/trace.c`, the decoder the Wi-Fi tracer uses too;
  the harness's ebreak requests and that decoder want one protocol, in `user/tracer/`.
- The tracer has to log and feign the six registers no frame covers, which `make BOARD=esp32c6 phymap` lists,
  in PCR, PMU and the LP domain; what their reads should answer is open.
  `phymap` sees only constant addresses, so a register reached through a table or a loop shows first in a trace.
  The harness reaches three of the board's frames, the SAR ADC, the modem and the eFuse,
  which with the tracer's own regions have to fit the root task's eight region slots, or a process of its own with more.
- The kernel writes two lines of its own for every user fault, which any tracer's log drowns in,
  and the host's check symbolizes each;
  whether a thread that has a watch, `OP_THREAD_WATCH`, should still be logged is a decision of `DESIGN.md`'s.
  A tracer works around it in the root task meanwhile.
- `tools/wifi-run.py` spawns `llvm-symbolizer` for every line it names,
  which is slow on a run that faults tens of thousands of times;
  one long-lived symbolizer, or a cache of an address's name, would keep up,
  and would take the root task's own filter away.
- Whether the rare whole stops of the trace stream can still happen, now that the console writer is fixed,
  wants a longer series; write each stop down with its sequence number and time.
  On 2026-10-09 a `trace=1 libstart=1` run stopped after its window's last record, 73573, at 28.07 s,
  short of its halt line, and waited for it 24 minutes;
  `tools/esp32c6-gate.py` now stops a board's run after 180 s and makes a traced one again.
  Both that stop and the next came at a gate's fourth or fifth traced run,
  where a later gate read the chip at its warmest, 65 C against the first run's 56,
  so `losses.txt` now carries the chip's temperature with each loss, to tell heat from chance.
  The stream still loses bytes within a line, a piece of it whole and the line's ends kept:
  on 2026-10-08 record 23018's `pc 0x...` field, 14 bytes, and a `trace: req ` line's front, 11 bytes,
  and on 2026-10-09 record 25300's `pc 0x... t3 R4 `, 20 bytes; about one traced run in eight.
  The root task writes each line whole, from one thread, in one `console_send`, so the piece goes after it:
  in the USB Serial/JTAG FIFO, written while the packet before is still in it, or on the host.
  `tools/mac-trace.py` refuses such a log, and `tools/esp32c6-gate.py` makes the run again
  and writes each loss into `build/esp32c6/gate/losses.txt`, the series to read the cause from.
- A driver of the ESP32-C6's Wi-Fi that leaves Espressif's libraries the PHY alone, `libphy.a`:
  `pp` and `net80211` give way to the driver's own MAC and station, and `osi.c` shrinks to what `libphy.a` calls.
  The station is the driver's own already, on hostap; `user/wifi/esp32c6/NOTES.md` says why.
  It receives, scans and sends by `mac.c`, authenticates, by SAE where the access point offers WPA3's personal
  and by open system under WPA2's, associates, runs the supplicant's 4-way handshake over a link of its own, see `supp.h`,
  installs the keys it derives in the MAC's own cipher and in the station, and serves the link.
  What the driver still calls of `pp` and `net80211`, `make BOARD=esp32c6 wifi-esp32c6-refs` lists:
  the bring-up, and through `mac.c` their lmac's control block.
  The receiving is the MAC's: its own station address and the access point's are set before the authentication,
  the libraries' channel sniffer is left, and the MAC decrypts each protected frame, leaving its CCMP header
  for the replay check, so the receiving runs no software CCMP; `ccmp_decrypt` stays for `ccmp-test` alone.
  The sending is the MAC's cipher too: the driver lays the frame out with its CCMP header (`ccmp_encap_hw`) and
  arms it with the pairwise entry in `PLCP1`'s keyslot field, and the MAC writes the ciphertext and the MIC;
  the engine word covers the whole MAC, so it is turned on for the frame; the field table is in `NOTES.md`.
  A rekey's key and packet number move together under `sta.key_lock`, which the sending holds to the frame's end,
  the MAC taking its key from the entry then; the transitions are `keys-test`'s on the host,
  a live one waiting for an access point that rekeys, as below.
  A rekey is checked on the host alone, by `keys-test`:
  showing a pairwise one live wants an access point that answers a station's rekey request,
  which the one here does not, or one that rekeys on its own;
  a group one, the old key read beside the new until the access point switches,
  wants one that rekeys its group inside a run, which the one here did not in 300 s;
  its group key interval, shortened for a run with the maintainer's leave, would do.
  The station's protected management frames — the leave policy and the SA Query — want a file of their own beside
  `sta.c`, which has grown to hold them.
  The MAC's bring-up is being taken from the libraries, from the last entry point.
  The stop is the driver's own (`mac_stop`), and the start is being taken over: the driver's own `esp_wifi_start` runs
  the libraries' bring-up's body itself, and the HAL's functions go over one at a time.
  The method, the order and the groups are in `user/wifi/esp32c6/NOTES.md`.
  `libstart=1` in `WIFI_CONFIG` runs the libraries' start instead of the driver's own, for a traced comparison of
  the two from one image, in a traced window alone, the driver's receive being its own from its start;
  it goes when the own start is proven and no comparison is wanted.
  The four bring-up symbols and the two of the log that the driver once asked of the flash libraries are taken one
  by one, over `libphy`, `mac.c` and `macstart.c`; `make BOARD=esp32c6 wifi-esp32c6-refs` counts what is left of `pp`
  and `net80211`.
  The MAC's configuration, their `hal_init`, is the driver's own, every group transcribed into `macstart.c`'s
  `mac_config`.
  A link without the two archives leaves only what the driver's own code names: `libphy.a` and the ROM reach nothing
  of theirs.
  What is left is the `libstart=1` comparison's, `esp_wifi_init_internal`, `esp_wifi_set_mode` and `esp_wifi_start`,
  the driver's own start calling none of theirs.
  The comparison's tools -- `libstart=1`, the diff, the compare across images and their expected files -- go with
  them, and the archives with the tools, `osi.c` shrinking to what `libphy.a` calls;
  `seq` and the compare of the own start's dry runs, which hold the own start to itself, stay.
  Gathering the driver's address macros into `macregs.h`, ordered by address, would show a word two files name
  beside its twin.
  A write that does nothing can be left out, on a role rather than on the bit.
  `mac-trace.py seq` holds `RX_BASE`'s value, the address of the driver's descriptor list in its heap,
  which any change to the driver's `.bss` moves; holding that word to the driver's block would spare such a change.
  `mac.c` points a MAC that ran out of descriptors past the last it filled by `RX_BASE` alone,
  which a move off the libraries' list found not always taken; no run has run out yet, `restarted` 0.
- The ESP32-C6's driver aims at a station alone, the profile its work is held to:
  WPA2's PSK and WPA3's SAE with CCMP, OFDM and HT rates with a simple rate control and retries,
  and the modem's sleep until the DTIM beacon with the PHY's PLL tracked, as the items below say;
  no soft AP, no HE or OFDMA, no coexistence with Bluetooth.
  So the start's HE and beamforming groups, which `mac_config` writes out, can go, each a step held by the runs,
  and the rest of 802.11ax is not written.
- The ESP32-C6's heap is left to hostap and Mbed TLS as a station joins, see `user/wifi/esp32c6/NOTES.md`,
  and SAE's arithmetic is most of what they take: Mbed TLS's numbers grow and shrink some thousands of times a join.
  `ec.c` over a P-256 of fixed width, its numbers and points in slots of its own, would leave hostap's few.
- The ESP32-C6's Wi-Fi root task has 8 KiB of memory and 9 slots left once its clients run,
  so another client needs room found first, in the driver's block or a smaller `CLIENT_CHAN_SIZE`.
- The ESP32-C6's link carries about what OFDM's 54 Mb/s can, up in A-MSDUs of two frames and down in single frames,
  see `user/wifi/esp32c6/NOTES.md`; more wants HT's rates, which only the libraries' code knows yet,
  to be read as `mac.c`'s sending and receiving were.
  Up, OFDM's frame holds 4095 bytes; an HT frame at MCS 7 would carry an A-MSDU of 7935 where the access point takes one.
  `pp`'s `hal_mac_tx_set_ppdu`, `mac_tx_set_plcp0` to `2`, `mac_tx_set_htsig` and `mac_tx_set_len`
  write the slot's words for one, which `mac.c` writes for OFDM's.
  A second buffer, laid out while the frame before is on the air, carried no more.
- The ESP32-C6's rate control fell to 1 Mb/s and stayed there twice in ten minutes at -74 dBm, `blast=` then sending
  a fiftieth of its usual: an A-MSDU of two frames is 24 ms on the air at that rate, and was almost never acknowledged,
  so the ten first tries in a row that raise the rate never came. Linux sends no A-MSDU at the legacy rates;
  A-MSDUs held to the faster rates, or a long frame's tries kept above 1 Mb/s, would bound it.
- The ESP32-C6's station declines the access point's block acknowledgement, so the access point sends no A-MPDU:
  `pp`'s `hal_agreement_add_rx_ba` and `_del_rx_ba` write the MAC's side of an agreement the station would accept,
  and the station would reorder a window's frames before its replay check.
  With `ht=1` it also promises to take an A-MSDU, which `mgmt_data_read` refuses.
- The ESP32-C6's station keeps the best-effort EDCA, not the access point's parameters for it,
  which WMM's parameter element in the association's answer carries.
- The ESP32-C6's `sta.c` holds the join, the link's frames both ways, its supervision, the power save and `blast=`,
  two thousand lines over one struct of state; the power save, whose state is already its own, `struct sta_ps`,
  would leave it first, a file of its own given the access point and the AID.
- The ESP32-C6's echo carries about a quarter of what its link does each way, bound by the system:
  each datagram crosses three processes' threads twice, and the link, whose 16 KiB `LINK_SIZE` gives each way
  four slots of `LINK_SLOT`, drops a burst of more; eight would take 32 KiB, which the root task's memory,
  see below, has no room for yet.
- The ESP32-C6's driver locks its own sending and keys, `tx_lock` and `key_lock`, with the adapter's mutexes,
  which exist for the libraries and go through the one lock all the adapter's objects share;
  `lib/lock.h`'s would serve them alone.
- The Pico 2 W's run lasts its `run=` whatever its checks found, or until a reset by hand,
  since the host has no way to end it: its USB serial port, `kernel/board/rp2350/cdc.c`, is the halt's alone.
  That port kept up while the run lasts would let the host end it there, in either mode,
  where the network does not reach the lab's access point; closing the port already reboots the chip into BOOTSEL.
  Whether the kernel keeps it, or the root task is given the USB controller as the ESP32-C6's is given its console, is open.
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
- The ESP32-C6's station sleeps between beacons with `ps=1`, the RF off, see `user/wifi/esp32c6/NOTES.md`;
  it is off by default until longer runs, and more access points than the three of two networks it ran at, have held it:
  an hour at the MikroTik, probed every half minute, lost one echo of its probes and no ping.
  It wakes by its own clock, a few milliseconds before a beacon is due, and is on some milliseconds a beacon:
  the MAC's TSF and its TBTT interrupt, which the libraries' `pm` reads, would wake it nearer.
  The core stays at its clock and waits in `wfi` meanwhile, and the modem's power domain stays on:
  a lower clock while the link is idle, and light sleep, would save more.
  What it saves the log tells as the RF's time off alone; a meter between the board and its supply would tell the current.
  One run of `make BOARD=esp32c6 wifi-esp32c6` with `ps=1` got none of the host's unicast frames through for its
  whole length, and the five after it, at both networks, passed; the cause is not known.
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
  The sending once copied a frame over the one the MAC still read, which lost answers just so;
  whether that was all of it wants a longer series.
  The Pico 2 W's `debug=dhcp` and `bssid=` measure it.
  The own receive, through the MAC without the libraries, misses them too:
  `listen=` counts the access point's frames to the group and whether the next by number is heard after each,
  and at the access point here heard at about -77 dBm it missed the next after every frame with More Data,
  at the one heard at about -56 dBm after none.
  So the loss lies not in the libraries' receive but in the chip, or in what `libphy.a` and the MAC's configuration,
  the same in the own start as in theirs, set; esp-idf #16096 can be told so.
- The ESP32-C6's station refuses an open network: it takes an access point whose beacon has an RSN element,
  and a passphrase.
- With the APM units confining the modem to the driver's block, as measured for open decision 13,
  the modem reached `0x4082c7ac`, in the root task's code block past its image, once in each of two runs,
  before the driver had started, and the units refused it; five runs after showed nothing.
  What of the modem reaches memory before a driver programs it, and why there, is not known;
  with the units off, as they are, such an access lands.
- The driver's process holds all eight regions, so anything more it needs takes a region from something it has.
  The C6 has sixteen entries, so a child made with more region slots than `CHILD_REGIONS`, which `child_new` could take, would lift that;
  so would a pager: a thread that, when another faults on a frame its process may have,
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

## nRF52840

- The library's test does not fit the root task's 32 KiB code region, so `make BOARD=nrf52840 lib-test` fails to link.
  A kernel run from flash would leave its RAM to a larger region, and boot without the probe.
- The idle kernel polls, since SysTick wakes `wfe` only under a debugger;
  a compare on a TIMER's CC register, raising its line, would let it sleep, as the item under "ARM" says for QEMU 11.
- Code runs from RAM at its system-bus address; the alias at `0x00800000` fetches over the core's code bus,
  which may cost a call fewer of the cycles `make BOARD=nrf52840 bench` counts, unmeasured.

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
  So nothing checks the rule that a stop which made another core trap reports a frame that trap could not stack, `sched_stop`:
  the host's frame loses nothing, and no demo stops a thread whose sp lies where it may not write.
  A two-core scenario of the escape suite, a thread stopped from the first core with its sp below its stack's region, would.
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

- The host shim states what `pmp_init` finds rather than running it,
  so its search for hardwired entries, past the budget too, runs only on an RP2350, and no mutant can stand in it.
  Running `pmp.c` over the shim's CSRs, which already model the grain and RP2350's three, would let `fuzz-rp2350` hold it;
  the CSR accessors are on the path of a switch, so they stay inline on the target.
- The mutator's draws of the describing operations apart, one typed draw in eight, are not measured yet:
  `make mutants-fuzz` before and after, from the corpus and from nothing, over a few seeds,
  would say whether the allocations, a pool's other operation, kept their share, and what fuzzing finds of the new mutants.
- No seed reaches a payer dropped as the turn's thread is destroyed:
  the turn's thread is the caller on its core, which keeps what it runs on, so only a call on another core can destroy it.
  Nor one destroyed itself from another core, which `core_thread_gone` tells its core of, as the turn may be left on spare time:
  the borrower, its lender, a thread with time beside them and the destroyer are four threads, and the replay driver has three.
- No check sees who pays for a turn on lent time, only that the payer is charged:
  a kernel that chose no payer, or let a waiter without time hand its turn on, would run the borrower for free,
  and only `make lab` would show it.
  Nor does one see that a signal wakes the thread paying for the signaller's turn first, and hands it the turn back:
  a kernel that did neither would be slower, which only `make lab` shows, `lend-hog` and `lend-server`.
- Feedback on the state beyond the kernel's edges, libFuzzer's extra counters
  over the objects of each type, the threads waiting and stopped, the pools dying and the `Irq`s armed,
  found no more mutants than edges alone in 5000 runs each, and was left out;
  a finer picture, such as the derivation tree's depth, is untried.
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
  all with `OP_TEST_PREEMPT`, where 186 000 made none.
  None reaches an edge the others do not, so the corpus keeps none of them.
  A mutation that inserts the three together is untried.
- `use_made` and `repeat`'s chains in `host/mutator.c` follow only what a record puts in an `ARG_DST` slot,
  so a call through what a copy or a derive made comes only by a typed record's draw,
  and nearly half the mutants `make mutants-fuzz` misses from the corpus ignore a right such a copy would take away.
- The mutator's guess of what each slot holds, in `host/mutator.c`, knows one table a thread and nothing of mounts,
  so a record names a slot of a table mounted second only by `draw_slot`'s chance,
  and only the seeds move a process to another table.
  A guess kept per process, of which table each table slot holds, would let a typed record pick from the second,
  and a mutation could insert a whole move as it repeats a record.
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
- `KERR_STATE` for a thread destroying the pool its process or its first table lies in, but not the thread,
  is checked only for the two together, and alone only for a table mounted second, `tests/seeds/revoke-keeps-mounted-table`:
  the driver's third thread lies apart from its process, but the process's first table lies with it,
  and the demo's root task lives with both.
- That a call which fails changes nothing is checked by no invariant,
  so a call that tells two slots apart by their numbers, where a table mounted twice gives one slot two,
  could make half of what it makes and fail, and nothing but `tests/seeds/table-mounted-twice`'s status would show it,
  which the host and QEMU agree on;
  `OP_UNTYPED_SPLIT`'s and `OP_IRQ_BIND`'s comparisons are checked by reading them.
  A host check that a failed call left every node as it was would cover all of them.
- The replay driver has three threads, and the third shares the second's process and lies in the first's pool.
  More of them, each with its process, pool and Untyped as the second has,
  would give the derivation below the free RAM more branches;
  the prologue in `include/rvuos/replay.h` and `host_boot` grow with them.
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
  the host takes as finished, and no host check sees it unless `OP_TEST_PREEMPT` armed the stop;
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
  at the price of renumbering the corpus and a frame more in the mutator's boot table.
- The corpus under `tests/corpus` is records, as the fuzzer wrote them,
  so renumbering a slot or an operation leaves its inputs calling other things than they were kept for:
  they still replay, and cover less until `make fuzz` and `make corpus-merge` find their like again.
  `tools/seed.py --text` run with the headers before the change, and the text built with those after,
  would carry them over, as the seeds are carried by being text.
- `make mutants` plants with `git apply -C1`,
  which, when the context has changed and one line of it matches in several places,
  silently takes the place nearest the patch's line numbers,
  and a refresh leaves those numbers old wherever they broke no tie.
  A refresh calls such a patch stale;
  planting through the refresh's `carry` would make the two agree.

- The model in `model/` stops at a pool:
  a retype into a pool, an allocation, and a revoke through a pool's destroy are unmodelled,
  and so is most of what the corpus revokes.
  Mount, unmount, install, uninstall, the binds and the watch fill or clear nodes too.
- `model-check` checks that every state is a forest; a proof that every modelled operation keeps one would replace the check.
  Authority only flows is proved in the model of a delete and a revoke alone.
- `model-check` sees only the corpus; run under `make fuzz` it would see every input the fuzzer tries,
  and `make mutants` does not run it.

## System calls

What open decision 31 in `DESIGN.md` asks for before the ABI is held still.

- `a4` to `a6` are read by no operation and checked by none, so none can be given a meaning later
  without changing what a program that left something there does, as the unread of `a1` to `a3` now can.
  Holding them to zero, as `dispatch` does `a1` to `a3`, has every wrapper of `user/rvuos.h` set them:
  on ARM, where `r4` to `r6` are the callee's to keep, that is three moves and their saving at every call,
  which grew the nRF52840's bench by 2.7 KiB, past its 32 KiB code region, and a call by a fortieth on the MPS2 boards.
  A wrapper out of line on ARM would hold them at the price of a call within the call.
  The count of arguments a call sets, in the bits of `a7` above the code, would hold them at no price:
  a wrapper loads its code as a constant either way, and `dispatch` would take a register past the count as zero,
  so an operation given `a4` later reads it only from a program that says it set it.
  `a7` past the code is refused today, so the bits are free until the ABI is held still, and not after;
  the replay's record would carry the count with the code.
- The breaking changes decision 31 lists, each in a change of its own.

## Code

- `PMP_MAX_ENTRIES=8 make check` stops building `fuzz-rp2350`, whose eight writable and three hardwired entries need eleven,
  though `README.md` says it runs everything; build that harness only with sixteen, or say so there.
- A change of process writes only the PMP entries the two images use, measured under QEMU alone:
  `make BOARD=rp2350 bench` and `BOARD=esp32c6` would say what it saves on silicon, open decision 5's cycles with it,
  and `make BOARD=rp2350 test escape` is the only run of the fence's byte it keeps in `pmpcfg1`.
- `cap_lookup` copies the capability twice, into its own and then the caller's;
  copying it once would spare about a dozen of every call's 450 instructions on QEMU virt, `make trap-profile`.
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
  and `OP_CAP_INFO` on that Untyped says only that a pool lies first below it:
  not the bytes the pool holds, nor whether anything outside it still holds its objects; open decision 33 in `DESIGN.md`.
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
- `pmp.h` names the protection unit's interface after PMP, and `kernel/arch/arm/mpu.c` implements it too;
  the image keeps PMP's encoding on both, but `pmp_set` and `pmp_entry_count` read as RISC-V's.
  Rename it in a change of its own, with the mutants that stand on its names in `process.c`,
  and `kernel/arch/arm/armv7m.h` with it, which serves ARMv8-M too.

## Scheduling

What the wait for a turn and the work a thread gets are bound by is in `DESIGN.md`, "Scheduling";
the first five below are what does not keep to it yet, each a constant-time change:

- A pool destroy stops only for a pending interrupt, `cap_stop_here`,
  so a thread with time its walk wakes waits for the rest of the walk while the core's timer lies past the next tick,
  and on another core for all of it, since the interrupt between cores goes out as the lock is given up.
  Stopping also where the trap made ready a thread the core's timer does not allow for,
  or owes another core an interrupt, would bound that by a tick;
  the transcripts would not move, as the host stops at every step.
- `pool_alloc` zeroes an object a byte a store with interrupts off,
  and a table of `CAPTABLE_MAX_SLOTS` slots is 24,596 stores, longer than a tick at 10 kHz on every board,
  where the wait's bound needs the kernel's longest step shorter than a tick.
  Zeroing a word a store, or a table in steps that stop, and measuring the longest step on each board,
  would say how short a tick each board can take.
- On ARM and RP2350's Hazard3 an `OP_IRQ_SET` on a core other than the first interrupts the first core each time,
  so a driver there that arms its line in a loop takes what it likes of the first core's turns, which its threads pay for.
  Interrupting it only as `core_notify` does, or routing a line to its waiter's core, open decision 24, would bound it.
- `OP_TEST_TICK` and a stop `OP_TEST_PREEMPT` armed move the tick count without time passing,
  and untraced the count stays ahead of the counter for good, so every deadline set before fires early, the watchdog's among them.
  Only the replay driver calls them, under tracing, so refusing them untraced with `KERR_STATE` would cost nothing.
- A bind of a ready thread, even to more units of its core, puts it at the back of its queue,
  since the old binding goes through `sched_unbind`'s settle; keeping its place would keep its wait.
- A thread whose account holds little more than a tick loses its time when its turn ends a few counts past a tick,
  as a trap that counts the tick late ends it, and spends the rest of that tick on spare time, or waiting without it.
  One that earns its whole core keeps its account where it is while it runs,
  so one bound with an empty account stays there; the demo's successor sleeps first.
  Time judged against what the rest of the tick costs, as `turn_owes` charges it, rather than a whole tick,
  would keep its turn, and change nothing under tracing, where every turn begins at a tick.
- A turn is one tick while another thread could run, so the timer skips ticks only for a thread alone,
  and a turn that begins mid-tick gets only the rest of one,
  so threads that wait just before every tick can leave a thread with time the end of each,
  and its work is bounded by its units, `DESIGN.md`, "Scheduling", where turns of a whole quantum would bound it by its turns too.
  A turn of a few ticks, or of what the thread's account holds, would skip them while threads compete too,
  and give turns the length the scheduler chooses: `sched_wake_ticks` would return the turn's end instead of one.
  It changes what `OP_TEST_TICK` means to the replay, which passes records with it,
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
  It bounds the work a thread gets beside threads that save up their time, (64 - u) / u tenths of a second at worst, whatever the tick;
  a cap in ticks, `COUNT_PARTS` and one at the least, or one the granter sets with the units, would shorten that with the tick.
- A fault hands the rest of the turn to the next thread as a wait does, though its watch's signal wakes the watcher
  whose time pays for the turn, if it lends the thread its time: that watcher waits on the run queue for its own turn.
  Handing it the rest of the turn, as a signal from the borrower does, would not;
  a thread that reports and then stops itself, `OP_THREAD_STOP`, has the report's signal hand it back already.
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
- A thread lends only on a core whose time both it and its borrower hold, a place each.
  Running the borrower on the lender's core for the turn, as Linux's proxy execution does, would lend to a borrower without a place there
  and keep each core's time to its units; open decision 27.
