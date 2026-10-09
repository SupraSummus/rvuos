# The ESP32-C6's Wi-Fi driver: notes

`user/wifi/esp32c6/` is the Wi-Fi system's driver on the ESP32-C6:
Espressif's closed libraries around an adapter at first,
then step by step a MAC, a station and a bring-up of its own, toward leaving the libraries the PHY alone.
These notes say what each step took and what it found, in the order the steps were made;
what writing the Wi-Fi system as a program of several processes was like, on the Pico 2 W first,
is in `user/wifi/NOTES.md`.

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
are all the own station has to answer; the libraries' quirks around them stayed in `wpa.c`, until it went.

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

## The station's own SAE

The supplicant already made and checked SAE's messages for the libraries,
so WPA3's personal on the own station is the Authentication frames around them, `mgmt_sae`.
`supp_choose` takes SAE wherever the access point offers it and the station may protect management frames, and `sae=0` WPA2's.

The heap, not the stack, was what SAE wanted:
its products take a few kilobytes at once, in Mbed TLS's big numbers and hostap's buffers,
and with the station's buffers and the MAC's list taken, hash to element's first step found the heap empty.
The libraries' static receive buffers lie idle once the own station has the MAC's list, so they are two, ESP-IDF's least,
and the station's log says the fewest bytes the heap had free.
The stack goes hardly deeper than under WPA2, and stayed as it was.

The Pico 2 W's access point, offering WPA3 and WPA2 together, refuses WPA2's join, see `TODO.md`,
but takes the own station by SAE, as it takes the libraries'.

## The libraries' station gone

Once the own station joined both kinds of network, the libraries' station went:
their scan, join and data path in `main.c`, `wpa.c`, which bound the supplicant to them,
and the options that chose a path, `rx=own`, `tx=own` and `sta=own`, with the knobs only their station read.
What the libraries still do is bring the MAC up and stop it, and a little through `mac.c`;
`make BOARD=esp32c6 wifi-esp32c6-refs` counts it, and each step that drops a call can say how far it went.
They are given no supplicant now.
Their station's start and stop look for its table and go on without one;
some of their handling of a received management frame reads it unchecked,
which no run has reached, as the driver takes the MAC's receiving before anything is joined.
The libraries' station had joined what the own one does not:
the router's own WPA2 network, whose group key is TKIP, open networks, and a modem asleep between beacons.
So the own station now takes, of the network's access points, the strongest whose suites it takes,
as wpa_supplicant passes over a BSS whose security does not match,
where it took the strongest and failed if that one offered nothing it took.
The comparison the libraries gave, the same run on either path, went with them; the history keeps the runs that held it.

What `mac.c` still asked of the libraries came next, read in their code as the receiving and the sending were.
The station-mode receive policy their association programs is three small functions of `pp`,
read-modify-writes of interface 0's words, which `mac_receive` now makes itself.
Their promiscuous mode is more: a virtual interface for the sniffer, their own bookkeeping, and under it
`hal_sniffer_enable`, a filter word and the words for miscellaneous and control frames.
A read of the MAC's words before and after their calls, with their station started, showed the filter word alone changing,
so `mac_sniffer` writes what their hal writes and nothing of their bookkeeping;
a scan without it hears nothing, not even an interrupt.
What is left of theirs is the bring-up, and the lmac's block, whose state byte the sending clears for their way out.

## The libraries' bring-up, the driver's own

The bring-up the libraries still do is being taken from them, from the last entry point,
so that each step's state is read no more by what remains.
Those entry points are `esp_wifi_init_internal`, `esp_wifi_set_mode`, `esp_wifi_start` and `esp_wifi_stop`,
the four bring-up symbols the driver asks of the flash libraries, with their two of the log.
The stop is the driver's own now, `mac.c`'s `mac_stop`: it turns interface 0's receive off,
makes its addresses invalid, holds the MAC still, sets the coex PTI back,
and calls `drv_phy_disable`, which stays `libphy`'s.
`make BOARD=esp32c6 wifi-esp32c6-refs` fell from net80211 6 and pp 1 to net80211 5 and pp 0,
the last reference being `our_instances_ptr`, which the library's stop alone read.

The oracle is the trace window: `trace=1` has the tracer fault on the device accesses and records,
on the same request counter, the driver's own steps and the calls the libraries make into `osi.c`;
`trace=2` runs the same window with the devices mapped and nothing faulting.
`tools/mac-trace.py` attributes each access to its library and function,
and with `--segments` splits the log at the `trace: req` lines, so the adapter calls frame each group of accesses;
`snapshot` and `compare` hold the window's end against two dry runs;
and the ordinary runs, scan, `listen=`, `probe=`, the WPA3 join and `sae=0`, hold what the window cannot show.

The start is next. Its adapter calls, in order, are the frame the own `esp_wifi_start` must build;
they are not device accesses, so the trace shows the call beside the accesses it made:
`wifi_clock_enable`, the driver's clock enable;
`phy_enable`, libphy's calibration, most of the window, which stays `libphy`'s;
`coex_enable` and `wifi_reset_mac`, the MAC's reset pulse, then `hal_mac_init` sets the pm-txblock bits;
the MAC config, pp's `mac_txrx_init`, `hal_init` and `hal_he_init`, the RX filter, the RX buffers,
the TX power and rate tables, the antenna table, the cipher and the PTI,
with pp calling phy for the channel and the gain;
`slowclk_cal_get`, `hal_timer_update_by_rtc` and the PTI;
`set_intr`, `set_isr` and `ints_on`, the interrupt handler, the address, the RX enable and the STA TSF;
and `event_post` STA_START, where the start reports.

The start's last call, their `ieee80211_update_phy_country`, comes just after the event:
written out, it is their `hal_init_tx_pwr`, which fills their power table, the one their association reads,
and writes the power registers again by `hal_init_tb_power` and `hal_init_imrsp_power`
and reads libphy's `rate_to_index`, with no timer armed.
The driver's start calls it now, and the transmitting runs and the snapshot are its oracle.
Whether the HE, beamforming and antenna groups can be left out is the own start's to find as it runs,
each dropped on its own and held to the snapshot and the runs.

Most of the start's HAL functions write registers alone, their objects carrying no relocation to any library's data,
so the own start can call them as they are, given the right arguments, and take them over one at a time.
`mac_rxbuf_init`, which reads the interface's control block, waits for the init to be the driver's too.
The MAC's configuration, their `hal_init`, is the driver's own sequence now:
the register writes it makes about HAL_CFG, HAL_HOLD and HAL_MISC and its receive-policy words are `macstart.c`'s,
and the groups between them go over one at a time,
each named with `osi_trace`, so that a traced run's segments show its accesses.
A step's diff holds the device accesses; the writes a step makes to the libraries' own memory are not in the trace, so only the runs hold those.
Of the start's calls, one that only writes the libraries' own memory -- their pm or their control block -- is left a call:
taking it over moves no device work to the driver, and would carry their structures' layout into its code.
A write to their memory that a transcribed body brings -- their wdev's flag, the masks by their control block -- is named where it is written,
and goes whole when the reader of that state is the driver's own.

A MAC register is named for the library function that reaches it, by constant address in their code or in the trace;
one that only the start's own group reaches, or several with no one role, carries the offset it was reached at.
One address has one name, in `macregs.h` when two files share it.

`mac_config`'s receive policy for interface 1 is not inert -- left out, the libraries' receive interrupt ran in the
window where it did not with it -- so it stays, though the driver never brings that interface up.
The receive base's write stays as well.
Their receive reads the word: `wdev_record_rx_linked_list` read it in base 2, where their receive interrupt ran in
the window.
Read in the write's place, before it, the word held 0x00800000 in three own runs, after their `wifi_reset_mac`.
How the MAC makes an address of it is not known;
read back as their 0x4086e138 is, as 0x0086e138, without bit 30, it would be 0x40800000, `__ram_start`, the kernel's
`_start`.

Of the start's calls after the hardware's, their `wifi_mode_set` and `_do_wifi_start` are not written out:
they carry net80211's state -- offsets into their control block, and a static of `ieee80211_sta.o` this driver cannot
name -- for little device work, one TSF word and `ic_set_vif`, so they stay calls.
Whether the driver wants net80211's state after the start at all is open.
