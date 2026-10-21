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
the adapter runs the MAC's interrupt in a thread, so the driver's handler takes the place of theirs,
and the MAC writes into RAM the driver allocates, which open decision 13 trusts it with.
`listen=` holds the two against each other, by the beacons each hears of an access point and those it misses,
counted by the access point's own clock: a count by sequence numbers took its bursts to other stations for losses.
A scan of the driver's own followed, passive as the libraries' now is too:
the radio retuned through `libphy.a` alone, as the libraries retune it, and each beacon read by hostap's parser,
which `make mgmt-test` runs on the host, under the sanitizers, against every cut of a beacon.

## The MAC's sending, the driver's own

Reading what the libraries wrote to a slot to send a frame let the sending be the driver's own:
the driver builds the frame's descriptor and programs the slot's PPDU words itself,
as the libraries' `lmacSetTxFrame` does, and arms the slot, as their `hal_mac_txq_enable` does.

The words were first copied from a probe to the group, and so they were wrong for a frame to the access point.
PLCP0's response field was 0, which has the MAC wait for no acknowledgement and end every try as acknowledged,
and the driver took the arm bits clearing for the frame's end, which they do at once:
the next frame was copied over one the MAC still read,
and the access point acknowledged frames it then dropped, a few in a hundred of the echo's answers.
The backoff was the one draw the probe left, 2 slots, under the libraries' voice queue's AIFSN,
so the station took the medium before stations that drew theirs.

Now a frame to the access point asks for the acknowledgement, as their `mac_tx_set_plcp0` does for one;
a frame to the access point's address on a channel it is not on showed the difference, never acknowledged.
A try is done when the MAC leaves its completion in the hardware txq state,
and the slot's completion word says whether it was acknowledged, as their `hal_mac_get_txq_complete` reads it.
A frame not acknowledged goes again, its Retry bit set, seven tries at the most.
Data go at OFDM's rates, management frames and frames to a group at 1 Mb/s, each at libphy's power for its rate;
`txctl.c` picks each try's rate, by AARF, and its backoff, from the best-effort window that doubles with the tries,
and `make txctl-test` holds both.
The slot's state is cleared just before each arm, since a bit from a frame before would end the new one at once.

Between its reads of the state the sending waits on a `lib/event.h` event,
which the interrupt's thread sets on the causes their `wDev_ProcessFiq` hands their completion's, timeout's and collisions' handlers.
It once spun through the try instead, and the receiving's threads waited behind it for their turn:
beside a Pico 2 W, where each try waits longer for the medium, the C6 then carried a sixth of what it carried alone,
and now carries about half of what the two carry.

Fairness is the board's to show: `tools/wifi-share.py` drives a Pico 2 W's echo and the C6's at one access point,
alone and together. Beside the C6 with the fixed backoff, the Pico kept about half what it kept beside the fair one.

## The station's own authentication

The station's own logic begins with the authentication.
With `sta=own` the driver scans for the network by its own code, takes the access point heard strongest, or the page's bssid,
retunes to it, and sends an open-system Authentication frame by `mac_tx`, reading the answer through `mac.c`;
the access point accepts it, and the libraries' station is never asked to connect.
The scan and the authentication are two takes of the receiving one after another, on the one list the start makes.
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
frames it receives and encrypts the ones it sends, and `ccmp.c`'s software CCMP keeps the robust management frames
and the EAPOL ones, which the driver leaves off the MAC; see `TODO.md`.

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
An install and the sending take one lock, `sta.key_lock`, which copies the key and reserves its packet number together;
the MAC's own sending holds it from the reservation to the frame's end, since the MAC takes the key from its entry then,
so a frame's packet number and the entry it is encrypted under belong together, a rekey's among them.
EAPOL goes out under the key in use once one is there, and in the clear until then.
A station's frame to the access point is addressed to it, so it goes under the pairwise key
however its own destination, Address 3, is addressed; the receiving picks the key by Address 1,
the group's for a group address.

**The frames.**
An Ethernet frame is laid into a Data frame, its CCMP header written and its MIC's room left for the MAC's cipher alone to fill, and sent.
A received protected frame comes to `read_frame` already decrypted by the MAC, its CCMP header left,
so the packet number is read back with `ccmp_head_read` for the replay check, one counter a priority,
802.11's traffic identifier, as the nonce and the additional data carry it, and the cipher is passed over;
the header is taken away and the frame handed to the supplicant if it is EAPOL, else made an Ethernet frame.
The header is taken away where the frame lies, the 802.11 header moved up onto it, and the Ethernet frame is built
straight into the link's slot, its payload copied by halfwords, as it lies two bytes off a word where the 802.11
payload lies on one: the library's `memcpy` goes by words only where both ends start on one, and three byte-wise
copies of each frame once cost the receiving more than the rest of it together.
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
The same entries and the engine word encrypt the sending: the driver lays a data frame out with its CCMP header
and a zeroed MIC's room (`ccmp_encap_hw`), arms it with the pairwise entry in `PLCP1`'s keyslot field, and the MAC
writes the ciphertext and the MIC. The engine word (`hal_crypto_enable(0, 3, 0, 0)`: `0x30103` at `KEY_CFG0`,
`0x600a4800`, its other select `0x600a4804`) covers the whole MAC, so `mac_tx_key` turns it on for the frame and off
again after; left on across frames it encrypts the software-CCMP frames too and the access point drops them.
`0x30000`, which the bring-up's `mac_crypto_init` leaves, is the engine off.

**The MAC's cipher fields, what is known of each.**
Confirmed by a run: the engine's bits that matter for the sending, 0, 1 and 8 of `0x30103`;
the valid bitmap `0x600a4814`, a bit an entry;
an entry's peer and temporary key, a wrong key losing every frame;
`PLCP1`'s bits 17 to 24, the entry the sending encrypts under.
From the disassembly alone: `KEY_CFG0`'s bit 17, which `hal_crypto_mgmt_rx_enabled` reads as management RX,
and bits 16 and 17, present with the engine off too;
an entry's control word, its bits 2 to 4 the cipher, 5 to 7 a class, 8 and 9 the interface, 11 and 14 and 15 the key id;
the entry's four words at `+0x18` to `+0x24`, which a 32-byte TKIP key reaches and a CCMP key does not;
and `0x4810`, whose bits 6 to 17 `hal_crypto_enable` sets for cipher 4 alone.
The transmit descriptor's bit 29, which `ppProcTxSecFrame` sets for a keyed frame, is not the gate:
the MAC encrypts without it.
Unknown: what each of `0x30103`'s bits 0, 1 and 8 does alone, what `0x4810` means,
and `0x4808` and `0x480c`, written by `hal_crypto_init` and read by nothing found.
The C6's TRM leaves the MAC's registers out, so none of this is from a datasheet.
A run with the group entry's temporal key one byte wrong had the access point's group frames not come at all,
its own relayed broadcast counting none, where the right key had them come, so the MAC hands over nothing
that failed its MIC, which the reading of a short frame rests on.
The MAC finds a group frame's entry by the key id in its CCMP header, not by the entry's place:
with a wrong key of the other id in the entry above the right one or below it,
or the right key in the other id's entry, the access point's group frames still came.

## What the link carries

Each direction is measured alone, and both together, from the host, the station joined:
datagrams to a UDP port of the station's that nothing serves, at a rate, the network process's count of frames in,
from its status port, before and after, saying how many arrived; `blast=`, seconds, has the station send a datagram
to its port 9 back to its sender as fast as the MAC lets it, the sending alone; and the echo, with a window of
datagrams in flight, every frame crossing the network process and its client too.
The log tells what each frame costs, `cost` lines in the core's cycles and the time on the air in microseconds,
which data rates the access point sends the station's frames at, by the MAC's header about each, and their signal.

At 54 Mb/s, the fastest of OFDM's, either direction alone carried about what single frames at that rate can,
each with its preamble, its acknowledgement and its backoff: the driver is not what bounds it.
The echo carried a quarter of that each way: its datagrams cross the link to the network process, the client and
back, every one through threads of three processes, and the link holds four frames each way, its 16 KiB split in two
and each ring's slots rounded down to a power of two, so a burst of more is dropped there.
More over the air wants more in each transmission: HT's rates, A-MSDU, several frames in one, once the station
associates for QoS, and A-MPDU from the access point, which wants the MAC's block acknowledgement; see `TODO.md`.

So the station associates for QoS, by WMM's information element, and once the access point's answer carries WMM's
parameters, sends the link's frames as A-MSDUs, as many as wait and fit 3839 bytes, which an access point that
takes HT takes: two of the link's largest, which then share a transmission's preamble, backoff and acknowledgement.
The access point here sent its own frames more slowly to a QoS station, about a tenth less, and the station's
A-MSDUs carried about a seventh more; `qos=0` associates without it, `amsdu=0` sends a frame to a transmission.
The frame is laid out straight into the buffer the MAC sends from (`mac_tx_begin`), its header written CCMP's header's
length in and moved down after, so that the room lies before the payload, which is copied once, by halfwords as it
lies two bytes off a word, where it was copied three times, two of them byte by byte.
HT, `ht=1`, has the access point send at MCS 7 with the short guard interval, yet less arrived, for a reason not found;
it offered no block acknowledgement, which would aggregate its frames, and the station declines an ADDBA Request,
as the MAC's block acknowledgement is not the driver's yet.

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

## The station's link, kept

Once joined, the station watches its access point.
The receiving's thread counts the access point's beacons, by a frame's subtype and addresses alone (`mgmt_beacon_of`),
and the station's thread looks once a second.
Two seconds without one, about twenty beacon intervals, and it asks the access point by a Null frame
whether it still hears the station, as Linux's mac80211 probes an access point whose beacons it lost;
three unacknowledged in a row end the link, and so do thirty seconds without a beacon, acknowledged or not,
since a station that hears nothing of its access point cannot be served by it.

A link that ends so, or that the access point ends -- a deauthentication heeded, a handshake of the supplicant's failing,
an SA Query unanswered -- no longer stops the driver: the station joins again,
the access point it last joined first, on its channel, else after a scan,
pausing half a second before, and twice as long after each join that fails, up to sixteen.
The network process keeps its address meanwhile, and its frames are dropped until the station serves the link again.
Only the first join failing fails the driver,
so that a wrong passphrase or a network not there still ends the run at its step.
A step that fails therefore returns, saying where in `ended`, up to the station's thread,
where it once stopped the thread for good:
it is called from inside hostap's supplicant too, which neither a stop nor a jump out of it would leave whole.
The supplicant keeps what the network's SSID and passphrase alone derive, WPA2's PMK and SAE's password element,
so that a join again derives neither; the PMK's PBKDF2 is most of a second on the C6.
The Pico 2 W's access point, its run let end and started again, showed both halves:
the station called the link lost a few seconds after the access point went, and joined again once it was back.

The PHY is kept to the chip's temperature once a second while the station is joined, as ESP-IDF's `phy_track_pll` keeps it:
libphy's `phy_param_track_tot`, Wi-Fi's part, under the sending's lock, so that no frame is on the air meanwhile,
and only while the RF is on.

## The station's power save

With `ps=1` the station sleeps between beacons while its link is idle, IEEE 802.11's power save (11.2.3),
the libraries' modem sleep made the driver's own:
the RF off, as ESP-IDF's `esp_phy_disable` turns it off once the PHY is calibrated, and on as `esp_phy_enable` turns it on,
`drv_phy_disable` and `drv_phy_enable`, under the sending's lock.
The access point holds the station's frames while the last frame of the station's it acknowledged had the
Power Management bit set: a Null frame with the bit, once the link has carried nothing of the station's own for 200 ms.
Every other frame the station sends has the bit clear, so the station is awake again whenever it sends;
`mac_tx_send` reads the bit off each frame it sends, and turns the RF on before.

The station wakes for the next DTIM beacon, or at the latest every listen interval's, the three its association
promised, a few milliseconds before the beacon is due. When that is, it reads from each beacon heard:
the receiving's thread's clock when the beacon came, less the beacon's time on the air at 1 Mb/s, the longest it takes,
less how late the access point's own clock says it went, its TSF modulo the interval.
The beacon's TIM says whether the access point holds frames for the station's AID,
whereupon the station wakes by a Null frame without the bit, as mac80211's dynamic power save does rather than poll,
and whether frames to the group follow a DTIM beacon, whereupon the RF stays on until the group's frame without More Data.
Four beacons missed in a row keep the RF on until one is heard, which sets the time again.

The beacons came within a few milliseconds of the time so read, so the driver's own clock serves;
the MAC's TSF and its TBTT interrupt, which the libraries' `pm` times its wakes by, would wake it nearer.
What the power save saves the log tells as the RF's time off, not as the chip's current, which nothing here measured.
`debug=ps` has the driver tell each sleep, each beacon that holds frames for the station, and every frame to it.

## The libraries' station gone

Once the own station joined both kinds of network, the libraries' station went:
their scan, join and data path in `main.c`, `wpa.c`, which bound the supplicant to them,
and the options that chose a path, `rx=own`, `tx=own` and `sta=own`, with the knobs only their station read.
What the libraries did was bring the MAC up and stop it, and a little through `mac.c`;
`make BOARD=esp32c6 wifi-esp32c6-refs` counted it, and each step that dropped a call could say how far it went.
They are given no supplicant now.
Their station's start and stop look for its table and go on without one;
some of their handling of a received management frame reads it unchecked,
which no run reaches, as their receive never runs: the driver's list and interrupt are the MAC's from its start.
The libraries' station had joined what the own one does not:
the router's own WPA2 network, whose group key is TKIP, open networks, and a modem asleep between beacons.
So the own station now takes, of the network's access points, the strongest whose suites it takes,
as wpa_supplicant passes over a BSS whose security does not match,
where it took the strongest and failed if that one offered nothing it took.
The comparison the libraries gave, the same run on either path, went with them; the history keeps the runs that held it.

What `mac.c` asked of the libraries came next, read in their code as the receiving and the sending were.
The station-mode receive policy their association programs is three small functions of `pp`,
read-modify-writes of interface 0's words, which `mac_receive` now makes itself.
Their promiscuous mode is more: a virtual interface for the sniffer, their own bookkeeping, and under it
`hal_sniffer_enable`, a filter word and the words for miscellaneous and control frames.
A read of the MAC's words before and after their calls, with their station started, showed the filter word alone changing,
so `mac_sniffer` writes what their hal writes and nothing of their bookkeeping;
a scan without it hears nothing, not even an interrupt.
What was left of theirs was the bring-up, which the next section takes.

## The own start

The start `esp_wifi_start` once was is the driver's own: `drv_wifi_start` runs their `wifi_start_process` written out for the station, and `mac_stop`, their `esp_wifi_stop`.
Their `esp_wifi_init_internal` and `esp_wifi_set_mode` are called no more, and net80211 and pp are out of the link.
`make BOARD=esp32c6 wifi-esp32c6-refs` counts the driver's own calls of `libphy` alone, the RF it keeps.

Its calls, in order: `drv_wifi_clock_enable`;
`drv_phy_enable`, libphy's calibration, most of a traced window, which stays libphy's;
`osi_coex_enable`;
their `wifi_reset_mac`, the reset pulse, then `mac_tx_block_clear` and `mac_home_channel`;
`mac_config`, the MAC's configuration, which `macstart.c` writes out group by group -- their `mac_txrx_init`, `hal_init` and `hal_he_init`: the RX filter, the RX buffers, the TX power and rate tables, the antenna table, the cipher and the PTI, with libphy brought in for the channel and the gain -- along with `osi_slowclk_cal`, their `hal_timer_update_by_rtc` and the PTI;
then `osi_intr_set`, `osi_isr_set` and `osi_ints_on`, the handler, the address, the RX enable and the STA TSF;
and `osi_event_post` STA_START.
`mac_station_start` makes their `wifi_mode_set` and `_do_wifi_start` after that:
the low-rate disable, the STA's TSF, interface 0's address and its access point's, its receive policy with its own flags set and its queue policy left clear, the receive enable, and the cipher's entry clear.
None of net80211's own state is written, and the runs hold that: the scan, `listen=`, `listen=` with `probe=`, and the joins.

The trace window is the oracle:
`trace=1` has the tracer fault on every device access and records, on the same request counter, the start's own steps and its calls into `osi.c`, and `trace=2` runs the same window with the devices mapped and nothing faulting.
`tools/mac-trace.py` attributes each access to its library and function, `--segments` splits the log at the `trace: req` lines, and `snapshot` and `compare` hold the window's end against the start's own dry runs.

Left out of their start:
their association's call, `ieee80211_update_phy_country`, which wrote the power registers again through `hal_init_tx_pwr`, is not made, since their association never runs;
`mac_config`'s receive policy for interface 1 is not made either, since the driver never brings that interface up, and three traced runs without it took no interrupt in the window, whose accesses `test/replay-expected.txt` names;
their `chm_init`'s channel-manager state is not written, only its frame, `mac_home_channel`, which programs channel 1 of the 2.4 GHz band, 2412 MHz, through their `chm_phy_change_channel` -- the MAC held, the PHY tuned, its width word's bit 8 cleared, the MAC released;
and their `pm_noise_check_enable` and `pm_disconnected_start` go as well, the first's byte read only by their `pm_noise_check`, a path the own station does not run, and the second's PM state read by no path the driver runs.
The ordinary runs hold all of it, the joins among them.

The receive base's write stays and points at the driver's own list, `mac.c`'s.
Read in the write's place, before it, the word held 0x00800000 in three own runs, after their `wifi_reset_mac`;
how the MAC makes an address of it is not known, and read as their 0x4086e138 is, without bit 30, it would be 0x40800000, `__ram_start`, the kernel's `_start`.

A MAC register is named for the library function that reaches it;
one that only the start's own group reaches, or several with no one role, carries the offset it was reached at, and one address has one name, `macregs.h`'s when two files share it.
### The MAC words named only by their offset

Some of the words the own start writes are named by nothing but the address they were reached at.
Their writers and readers were found by scanning `libpp.a`, `libnet80211.a` and the ROM's ELF for the constant address, each a load or store whose address a register's constant gives.
The scan cannot follow a read through a base and an offset it does not know, so "no reader" below means none found, not that none exists.
A writer's bits are its `andi`, `ori`, `and` and `lui` constants, computed, and a field is named only where a writer's name covers it.
No word below is reached by a ROM function in the scan; every access is in `libpp.a`.

Their `mac_txrx_init`, which `macstart.c`'s `mac_queues_init` writes out:

| word | address | their writer, beyond it | their reader | what is known |
|---|---|---|---|---|
| `TXRX_MAC_110` | 0x600a4110 | -- | -- | nothing |
| `TXRX_MAC_114` | 0x600a4114 | -- | -- | nothing |
| `TXRX_HAL_C20` | 0x600a4c20 | -- | -- | nothing |
| `TXRX_HAL_C24` | 0x600a4c24 | -- | -- | nothing |
| `TXRX_HAL_C60` | 0x600a4c60 | -- | -- | nothing |
| `TXRX_HAL_C98` | 0x600a4c98 | `hal_he_init` | -- | nothing |
| `TXRX_HAL_C9C` | 0x600a4c9c | -- | -- | nothing |
| `TXRX_HAL_CA4` | 0x600a4ca4 | -- | -- | nothing |
| `HAL_CTRL` | 0x600a4308 | `hal_deinit`, `esp_test_clr_hw_statistics` | -- | bit 0 the hardware statistics, which `esp_test_clr_hw_statistics` clears and sets and `hal_deinit` sets; bit 1 `mac_txrx_init`'s |
| `HAL_WORD` | 0x600a4c8c | `hal_init` | `dbg_read_bsr_info` | nothing; a debug dump reads it |

Their `hal_init` and `hal_deinit`, which `mac_config_start` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HAL_CFG` | 0x600a4ddc | `hal_init`, `hal_deinit` | -- | the handshake `mac_config_start` polls for bit 0 |
| `MAC_ENABLE` | 0x600a4c40 | `hal_init`, `hal_enable_mac`, `hal_disable_mac`, `hal_deinit`, `hal_enable/disable_sta_beacon_filter`, `hal_sniffer_rx_set_promis` | `print_isr_regs` | the MAC's enable mask: 0 is it disabled and a set bit lets that function run, so `hal_enable_mac` writes its argument and `hal_disable_mac` 0, `hal_init` leaves 0x19a879e0, and the driver's start and stop write 0; bit 2 the sniffer's promiscuous mode, bit 15 the station's beacon filter |

Their `mac_rxbuf_init`, which `mac_rx_base_init` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `RXBUF_MAC_7C` | 0x600a407c | `mac_rxbuf_init` | -- | nothing |

Their `hal_he_init`, which `mac_he_init` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HE_CTRL` | 0x600a4c80 | `hal_he_init`, `hal_he_bsr_init`, `hal_he_set_default_pe`, `hal_he_set_ul_mu` | `dbg_read_ax_diag`, `dbg_read_bsr_info`, `dbg_read_rx_misc` | bits 0-2 the default packet extension (`hal_he_set_default_pe`, from its argument), bits 3-11 `hal_he_init`'s, bit 27 the buffer status report (`hal_he_bsr_init`), bits 30-31 uplink multi-user (`hal_he_set_ul_mu`) |
| `HE_MAC_10C` | 0x600a410c | `hal_he_init` | `dbg_read_rx_misc` | nothing; a debug dump reads it |
| `HE_HAL_C88` | 0x600a4c88 | `hal_he_init` | -- | nothing |
| `HE_HAL_CBC` | 0x600a4cbc | `hal_he_init` | -- | nothing |

Their `hal_init_bf`, which `mac_init_bf` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HE_HAL_C78` | 0x600a4c78 | `hal_init_bf`, `esp_test_set_csi_dump_cfg`, `dbg_disable_report_cbf` | `esp_test_get_csi_dump_cfg`, `dbg_read_rx_misc` | `esp_test_set_csi_dump_cfg` clears bits 0-7 and sets bits 1-8 from its argument, the CSI dump configuration; `hal_init_bf` writes 0x20 into bits 0-7 and 0x71 into bits 8-15, which overlap it, 5 into bits 16-18, sets bit 19, and clears bits 21 and 23; `dbg_disable_report_cbf` clears bit 20 |
| `HE_HAL_DE0` | 0x600a4de0 | `hal_init_bf` | -- | nothing |
| `HE_HAL_7128` | 0x600a7128 | `hal_init_bf` | -- | nothing |
| `HE_MAC_470` | 0x600a4470 | `hal_init_bf` | -- | nothing |
| `HE_MAC_474` | 0x600a4474 | `hal_init_bf` | -- | nothing |

Their `hal_timer_update_by_rtc`, which `mac_timer_update_by_rtc` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `TIMER_30` | 0x600ad030 | `hal_timer_update_by_rtc`, `pwr_hal_set_lpclk_sync_enable`, `pwr_hal_set_lpclk_sync_disable`, `pwr_hal_set_modem_state_interface`, `pwr_hal_set_modem_state_rxend_exit_code`, `tsf_hal_map_tbtt_target_to_rx_frame`, `tsf_hal_unmap_tbtt_target_to_rx_frame` | -- | bit 16-17 the modem state's rxend exit code, bits 18-19 the modem state interface, bit 27 the low-power clock's sync, bit 28 the TBTT target to RX frame |
| `LPCLK_CYCLE` | 0x600ad070 | `hal_timer_update_by_rtc`, `pwr_hal_set_lpclk_cycle_time` | -- | bits 0-17 the low-power clock's cycle, which both write as a Q13.19 period |

Their names come from a library function, but their fields are not known; the same scan gives their writers and
readers, and `macstart.c`'s own transcription gives the fields:

| word | address | their writers (fields) | their readers | what is known |
|---|---|---|---|---|
| `BBRXHUNG_TIME` | 0x600a4c1c | `hal_he_set_bbrxhung_time` (bits 0-11), `mac_txrx_init` (bits 30-31) | -- | their name says a receive-hung timeout, bits 0-11; the field's scale is not known |
| `HE_MAC_DELAY0` | 0x600a4c54 | `hal_he_set_mac_delay` (bits 10-19, 21-29) | `dbg_read_wdevdelay` | one of the delay pair their mac-delay body writes; its two `env_is_chip` paths write different values, and the driver's the chip's |
| `HE_MAC_DELAY1` | 0x600a4c58 | `hal_he_set_mac_delay` (bits 0-9, 10-19, 21-29) | `dbg_read_wdevdelay` | as `HE_MAC_DELAY0` |
| `HE_TB_TX` | 0x600a4df8 | `hal_init_tb_tx` (bits 14, 15) | `esp_test_get_hw_tb_statistics` | the trigger-based TX word; the reader names it a TB statistics source |
| `HE_TB_POWER0` | 0x600a4430 | `hal_init_tb_power` (bits 0-5, 8-13, 16-21, 24-29) | -- | four 6-bit power indices, from their power table |
| `HE_TB_POWER1` | 0x600a4434 | `hal_init_tb_power` (the same four) | -- | as `HE_TB_POWER0` |
| `HE_TB_POWER2` | 0x600a4438 | `hal_init_tb_power` (the same four, the last two from libphy's `phy_get_max_pwr`) | -- | as `HE_TB_POWER0` |
| `OFDMA_PTI_W0` | 0x600a4dd0 | `hal_set_tb_pti`, `hal_set_beamf_pti` | `hal_set_ofdma_sequence_pti` | a coex PTI word, which their sequence function reads; the fields' layout is not known |
| `OFDMA_PTI_W1` | 0x600a4dd4 | `hal_set_beamf_pti`, `hal_set_beamf_mt_pti` | `hal_set_ofdma_sequence_pti` | as `OFDMA_PTI_W0` |
| `ANTENNA_MAC_2CC` | 0x600a42cc | `hal_attenna_init` (bits 0-2, bit 5) | -- | their antenna init's other word; what it holds is not known |
| `HE_ERSU` | 0x600a4c7c | `hal_he_set_ersu` (bit 10), `hal_he_bsr_init` (bits 0, 4-8, 15), `hal_he_bsr_ctrl` (bit 15), `hal_he_set_bsr_acihigh_comp_method`, `dbg_tb_ignore_cca_enable` (bit 12) | `dbg_read_bsr_info`, `dbg_read_rx_misc`, `hal_he_get_bsr_acihigh_comp_method` | bit 10 their extended-range single-user, bits 0 and 4-8 the buffer status report's, bit 12 their test's "ignore CCA"; the rest is not known |
| `HE_UORA` | 0x600a4c84 | `hal_he_set_uora_parameter` (bits 18-24, 25-31) | -- | uplink-OFDMA random access: 2^OCWmin - 1 and 2^OCWmax - 1, from their start's byte; the word's other bits are not known |
| `HE_BF_REPORT_RATE` | 0x600a4458 | `hal_he_set_bf_report_rate` (bits 0-6, 7-13, 14-20, 21-27) | `dbg_read_bfr_rate` | four 7-bit report-rate fields, their name and the reader's |
| `RX_POLICY` | 0x600a40d8 | `hal_init` (bits 0, 2, 7, 9 set; bits 6, 10, 13 cleared), the driver's own `mac_receive` (bits 4, 6, 10 cleared; bits 1, 8 set) | -- | their `hal_mac_rx_set_policy(0, 1, 1, 1)` clears bits 4, 6 and 10 and their `hal_mac_set_rxq_policy(0, 1)` sets bits 1 and 8; the scan cannot follow those two, which reach the word through its interface's number |

`mac.c` names every word of the first group's kind that it writes, so none of its own is above.

## IEEE 802.15.4 beside Wi-Fi

`i154=` brings the chip's 802.15.4 radio up instead of Wi-Fi's, as ESP-IDF's open driver does; `i154.c` says how.
Its MAC is a whole one in hardware, unlike Wi-Fi's, but for CSMA-CA's backoffs and the retries,
`mac154.h`'s, as the nRF52840's link has them, which the driver times on the MAC's own timer0.
It needs the BT baseband, now the driver's own `btbb.c` in place of `libbtbb.a`; the baseband's data still lies in the
driver's RAM, so Wi-Fi's runs see their own data further on, and a traced start held to one from before the data was
there differed only in the address the receive list is written at.

The bring-up has a trace window of its own, `trace=1` with `i154=`: the driver runs the radio's bring-up and nothing
else, before its survey and the exchange, and with the root task mapping no device every access faults to the watcher,
which carries it out and logs it, as Wi-Fi's window's does; `trace=2` runs the same window with the devices mapped.
It was what the driver's own baseband, `btbb.c`, was held to as Espressif's `libbtbb.a` was taken from it one function
at a time, the archive's `bt_bb_v2.o` its source: the traced run's device accesses were held to the archive's, and,
with `snap=` names in the configuration, `tools/mac-trace.py compare` held the two runs' final state.

A run of 802.15.4 leaves the modem in a state the runner's reset does not clear:
a traced Wi-Fi start straight after one read `0x600A4400` at its third access as `0xc1c93734`,
where runs after a Wi-Fi run read `0xc1cb3754`, as the gate's did.
The baseband's own registers survive it too, so a traced run's reads of them answer what the image before left there,
which a Wi-Fi run does not put back: a traced run's board values are held by the last write to each address,
and the full sequence, reads and all, by the host replay over the recorded base.
A traced run, or a gate, goes after a Wi-Fi run.

## The 802.15.4 MAC's registers, named

ESP-IDF 5.5 (Arduino esp32 3.3.11) names the whole 0x600a3000 block in
`soc/esp32c6/register/soc/ieee802154_reg.h`, SPDX Apache-2.0, a copy of whose headers sits in
`build/esp/idf-esp32c6-3.3.11/`.
i154.c's names to the header's; a row says what the driver writes or reads there, its source
the header unless it names another. A word whose role the header does not give keeps the offset it was reached at.

| i154.c | ESP-IDF | the driver | field |
|---|---|---|---|
| COMMAND 0x000 | COMMAND_REG | the MAC's command, a byte | opcode, low 8; the values are the driver's CMD_*, named in the header's sibling ieee802154_ll.h, not its register header |
| CTRL_CFG 0x004 | CTRL_CFG_REG | the receive's filter and the acknowledgements | bit 0 our AUTO_ACK_TX = HW_AUTO_ACK_TX_EN, bit 3 our AUTO_ACK_RX = HW_AUTO_ACK_RX_EN, bit 6 our COORDINATOR = PAN_COORDINATOR, bit 7 our PROMISCUOUS = PROMISCUOUS_MODE, bits 28 to 31 our MULTIPAN = MAC_INF0..3_ENABLE |
| INF0_SHORT_ADDR 0x008 | INF0_SHORT_ADDR_REG | the node's short address | MAC_INF0_SHORT_ADDR, low 16 |
| INF0_PAN_ID 0x00c | INF0_PAN_ID_REG | the PAN | MAC_INF0_PAN_ID, low 16 |
| INF0_EXT_ADDR0/1 0x010, 0x014 | INF0_EXTEND_ADDR0/1_REG | the node's extended address | the whole word each |
| CHANNEL 0x048 | CHANNEL_REG | the radio's frequency | HOP, low 7; the driver writes 2402's MHz offset, i154.c's FREQ, from IEEE 802.15.4's channel numbering |
| TX_POWER 0x04c | TX_POWER_REG | libbtbb's power-table index | TX_POWER, low 5 |
| ED_DURATION 0x050 | ED_SCAN_DURATION_REG | how long the energy detection samples | ED_SCAN_DURATION, low 24, the driver's count of 16 us symbols, i154.c; bits 24 to 27 ED_SCAN_WAIT_DLY |
| ED_CFG 0x054 | ED_SCAN_CFG_REG | the assessment's threshold and the last sample | CCA_ED_THRESHOLD bits 0 to 7, ED_SAMPLE_RATE bits 11 to 12, ED_SAMPLE_MODE bit 13, i154.c's ED_SAMPLE_AVG, see below, CCA_MODE bits 14 to 15, ED_RSS bits 16 to 23 read, CCA_BUSY bit 24 read |
| ACK_TIMEOUT 0x05c | ACK_TIMEOUT_REG | how long an acknowledgement is waited for | ACK_TIMEOUT, low 16, the driver's 54 symbols, macAckWaitDuration, IEEE 802.15.4's and mac154.h's |
| EVENT_EN 0x060 | EVENT_EN_REG | which events raise the line | EVENT_EN, low 13 |
| EVENT_STATUS 0x064 | EVENT_STATUS_REG | the events since the last, written back to clear | EVENT_STATUS, low 13 |
| RX_ABORT_EN 0x068 | RX_ABORT_INTR_CTRL_REG | the receive aborts that raise the line | one bit a reason |
| COEX_PTI 0x070 | COEX_PTI_REG | the coexistence priority, 3 with no coexistence | COEX_PTI bits 0 to 3, COEX_ACK_PTI bits 4 to 7, CLOSE_RF_SEL bit 8 |
| TX_ABORT_EN 0x078 | TX_ABORT_INTERRUPT_CONTROL_REG | the sending aborts that raise the line | one bit a reason |
| RX_STATUS 0x080 | RX_STATUS_REG | the receive's state and its last failure | RX_STATE bits 16 to 18, the driver's SFD test; RX_ABORT_STATUS bits 4 to 8; FILTER_FAIL_STATUS bits 0 to 3 |
| TX_STATUS 0x084 | TX_STATUS_REG | the sending's state and its abort's reason | TX_STATE bits 0 to 3, TX_ABORT_STATUS bits 4 to 8, the driver's ABORT_REASON; TX_SEC_ERROR_CODE bits 16 to 19 |
| TXDMA_ADDR 0x0d0 | TXDMA_ADDR_REG | the frame to send | |
| RXDMA_ADDR 0x0e0 | RXDMA_ADDR_REG | where a received frame is written | |
| TIMER0_THRESHOLD 0x0a8 | TIME0_THRESHOLD_REG | the backoff's microseconds | TIMER0_THRESHOLD, whole |
| CRC_ERROR_CNT 0x148 | CRC_ERROR_CNT_REG | frames whose FCS was bad | read |
| RX_FILTER_FAIL_CNT 0x154 | RX_FILTER_FAIL_CNT_REG | frames the address filter refused | read |
| — 0x100 | PAON_DELAY_REG | unused by the driver; the name beside TXON_DELAY for a PA-on delay, the MAC's; the baseband's pa_on_bt_delay is another word, 0x600a08fc, no claim the two are alike | PAON_DELAY, low 10 |
| btbb.c 0x104 | TXON_DELAY_REG | the transmit-on delay | TXON_DELAY, low 10; ieee802154_txon_delay_set writes 0x23 |
| btbb.c 0x10c | TXOFF_DELAY_REG | the transmit-off delay | TXOFF_DELAY, low 6; the same function clears it |
| btbb.c 0x110 | RXON_DELAY_REG | the receive-on delay | RXON_DELAY, low 11; it writes 0x32 |
| btbb.c 0x114 | TXRX_SWITCH_DELAY_REG | the transmit/receive switch's delay | TXRX_SWITCH_DELAY, low 10; it writes 0x7f |

### The energy scan's sample mode, bit 13

ESP-IDF's two headers name `ED_CFG`'s bit 13 differently. Its register header,
`soc/esp32c6/register/soc/ieee802154_reg.h`, gives bits 11 to 12 to `ED_SAMPLE_MODE` and bit 13 to
`DIS_ED_POWER_SEL`; its structure header, `ieee802154_struct.h`, the one the HAL writes through, gives bits 11 to 12
to `ed_sample_rate` (the samples each microsecond) and bit 13 to `ed_sample_mode`, the choice between the most and the
average sample, after `cca_threshold`:8, `reserved8`:3 and `ed_sample_rate`:2.
IDF's driver settles which it is: `ieee802154_mac_init`, in `components/ieee802154/driver/esp_ieee802154_dev.c`, calls
`ieee802154_ll_set_ed_sample_mode(IEEE802154_ED_SAMPLE_AVG)`, and `ieee802154_ll_set_ed_sample_mode` in
`components/esp_hal_ieee802154/include/hal/ieee802154_common_ll.h` writes `ed_cfg.ed_sample_mode`, bit 13. So bit 13 is
the sample mode's average, not the register header's `DIS_ED_POWER_SEL`; i154.c's `ED_SAMPLE_AVG`, bit 13, is IDF's
own choice, and it is not renamed. IDF leaves the sample mode at the average; i154.c clears it for the survey and
sets it again. Both headers are from ESP-IDF 5.5 (Arduino esp32 3.3.11).

### Where i154.c differs from ESP-IDF's driver

The register table above names the fields; these are places i154.c uses a word otherwise, or with another value, or in
another order than ESP-IDF's driver does, each with its source, no more:
- The events: `ieee802154_mac_init` enables every event and then clears `TIMER0_OVERFLOW` (`EVENT_EN`'s bit 8) unless
  `CONFIG_IEEE802154_TEST`; i154.c enables every event including bit 8, its CSMA-CA backoff timed by timer0.
- The sample mode: the same init sets `ED_SAMPLE_AVG` and leaves it; i154.c clears it in `survey()` and sets it again.
- The statistics: the init clears the counters through `ERROR_CNT_CLEAR_REG`, 0x180; i154.c never writes it, and reads
  `CRC_ERROR_CNT` and `RX_FILTER_FAIL_CNT` at the end of its run.
- The address filter: IDF's PIB starts with promiscuous true; i154.c clears `PROMISCUOUS_MODE` so the PAN and the
  short address filter what the MAC takes.
- The acknowledgements: IDF's PIB starts with `enhance_ack_tx` true; i154.c clears `AUTO_ENHACK`.
- The interfaces: IDF sets the multipan enables its `CONFIG_IEEE802154_INTERFACE_NUM` names and the pending mode;
  i154.c sets `MAC_INF0_ENABLE` alone.
- The CCA: IDF's mode and threshold are its Kconfig's `CONFIG_IEEE802154_CCA_MODE` and `CONFIG_IEEE802154_CCA_THRESHOLD`;
  i154.c sets `CCA_MODE_ED` and -75 dBm.
- The acknowledgement timeout: IDF defines `ieee802154_ll_set_ack_timeout` and never calls it; i154.c writes
  `MAC154_ACK_WAIT_SYMBOLS`, 54 symbols.
- The energy scan's length: IDF's `ieee802154_energy_detect` takes its duration from the caller; i154.c writes
  `ENERGY_SYMBOLS`, 6250, 100 ms. Both set the assessment's duration to 8 symbols, IDF's `CCA_DETECTION_TIME` and
  i154.c's `CCA_SYMBOLS`.
- Timer0: IDF fires it against a clock time for `transmit_at` and for a 200 ms wait on a received acknowledgement;
  i154.c writes `TIMER0_THRESHOLD` with a backoff in microseconds and starts it with `TIMER0_START`.
Both set the channel the same way, `(channel - 11) * 5 + 3`, IDF's `ieee802154_channel_to_freq` and i154.c's `FREQ`,
and the TX power through the same table and the same nearest-below index.

## The baseband's words, what is known

The 0x600a2000 block the IDF register header does not name, only dividing it in btbb_retention_reg.h.
Its fields come from weaker sources, strongest first: the field names ble_tx_config_check prints, which its own
disassembly pairs with each read, checked there; then a getter's own cut, or a setter's write whose bits its
disassembly gives and whose name the function's; then the archive's function names alone, which name a group and not a
field; constants are leads, called hypotheses.
The archive is libbtbb e4897a5.

Its disassembly wants llvm-objdump with --mattr=+m, or every divide by 80 vanishes as unknown, as ble_tx_config_check's two did.

| address | field | what the driver knows | source |
|---|---|---|---|
| 0x600a08fc bits 0..7 | rf_on_bt_delay | a delay; the print divides the byte by 80, hypothesis: 80 MHz ticks, the print in us | ble_tx_config_check .LC3, its disassembly (divu by 80) |
| 0x600a08fc bits 8..15 | pa_on_bt_delay | a delay, divided by 80 in the print, the same hypothesis; btbb_init sets it 0xc8, 2.5 us by it | ble_tx_config_check .LC4 |
| 0x600a1650 bits 0..7 | lc_tx_on_delay | a delay; constant_DC_time is this less bb_tx_on_delay, tx_ramp_delay and tx_rampup_delay, and a check fails when it is 9 or less | ble_tx_config_check .LC7 and .LC8 |
| 0x600a2010 bits 5..12 | bb_tx_on_delay | the BB transmit-on delay; bt_bb_set_le_tx_on_delay writes it, bt_bb_get_le_tx_on_delay reads it and adds 5 | ble_tx_config_check .LC2, and the setter's and getter's names |
| 0x600a2010 bits 24..31 | zb_tx_on_delay | the ZB transmit-on delay, a field of its own beside bb_tx_on_delay's; bt_bb_set_zb_tx_on_delay writes it, bt_bb_get_zb_tx_on_delay reads it and adds 5 | the setter's and getter's names |
| 0x600a2018 bits 3..7 | tx_ramp_delay | a delay | ble_tx_config_check .LC5 |
| 0x600a2018 bits 19..23 | tx_rampup_delay | a delay | ble_tx_config_check .LC6 |
| 0x600a20b8 bits 12..19 | tx_cca_end_ts | the CCA window's end, as the print reads it; bt_bb_tx_cca_period writes bits 9 to 19 there, eleven, so the setter's field and the print's do not agree yet: it writes (first - 5) * 8 - 1 in bit 9's units, hypothesis a finer field the print shows divided by 8 | the setter's disassembly and ble_tx_config_check .LC1, kept apart |
| 0x600a20b8 bits 23..30 | tx_cca_start_ts | the CCA window's start; bt_bb_tx_cca_period writes bits 23 to 30 there too, so the two agree | ble_tx_config_check .LC0, and the setter |

The archive's other functions, the getters and the setters outside the bring-up,
each field the bits its own disassembly cuts, its name from the function's:

| address | field | what the driver knows | source |
|---|---|---|---|
| 0x600a20b0 whole | cur_rx_info | the receive's info word, cut by nothing | bt_bb_get_cur_rx_info's name |
| 0x600a20b0 bits 0..7, signed | channel_pwr | the same word's low byte, sign-extended; hypothesis RSSI in dBm, the only signed level here | bt_get_channel_pwr's name and its sign extension |
| 0x600a20d4 bit 20 | cca fifo reset | written 1 to reset the fifo | bt_bb_tx_cca_fifo_reset's disassembly |
| 0x600a20d4 bits 25..28 | cca fifo count | how many entries wait | bt_bb_tx_cca_fifo_count's disassembly |
| 0x600a20d4 bit 29 | cca fifo empty | | bt_bb_tx_cca_fifo_empty's disassembly |
| 0x600a20d4 bit 30 | cca fifo full | | bt_bb_tx_cca_fifo_full's disassembly |
| 0x600a20d8 whole | cca fifo read | the front entry, one word a read | bt_bb_tx_cca_fifo_read's disassembly |
| 0x600a2064 bits 20..25 | corr thresh LE, argument 0 | bt_bb_set_corr_thresh_le writes its first argument there; bt_bb_rx_correlator_set writes 3 there | the two disassemblies |
| 0x600a2064 bits 14..19 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 4 there | the same |
| 0x600a2060 bits 26..31 | corr thresh LE, argument 2 | the same setter writes its third there; bt_bb_rx_correlator_set writes 4 there, bit 28 | the same |
| 0x600a20f8 bits 5..10 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 3 there | the same |
| 0x600a20f8 bits 19..24 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 2 there, and bit 25 beside it | the same |
| 0x600a28a0 bits 16..23 | rx_sense | the receive sense; bt_bb_set_rx_sense writes its third argument there when its first is nonzero and its second is 1 or less; bt_agc_rssi_thresh writes 0x9c there | bt_bb_set_rx_sense and bt_agc_rssi_thresh |
| 0x600a2004 bits 8..9 | channel_pwr's enable | cleared by bt_get_channel_pwr_unset | its name |
| 0x600a00c0 bit 25, bits 4..10 | channel_pwr's own word | bt_get_channel_pwr_set sets bit 25, clears bit 15 and writes its argument, 7 bits, in bits 4 to 10; unset clears bit 25; a MODEM word, not the baseband's | the two names |
| 0x600a207c bits 28..31 and 22..23 | bbmac_idle_for_swrst's state | a predicate: the function returns 1 when these, 0x600a1550's low 4, 0x600a3088's low 4 and 0x600a2c38's low 3 are all idle | the function's name and its read of each |
| 0x600a20dc bit 31, bits 15..30, bits 13..14 | wdt | bt_bb_wdt_init(a0, a1) sets bit 31, a0 in bits 15 to 30 and a1 in bits 13 to 14; hypothesis the timeout and a prescaler, a0 16 bits and a1 2 | the function's name and its two arguments' widths |

The blocks of words each group writes, computed from the archive's disassembly and, for `bt_bb_rx_set`, whose call to
`bt_agc_v2_set` hides the words after it, from `btbb.c`, which the replay holds to the archive:

| group | the words it writes |
|---|---|
| bt_bb_v2_tx_set | 0x600a2008, 0x600a200c, 0x600a2018 |
| set_gauss_coeff_1m | 0x600a20e0, 0x600a20e4, 0x600a20e8, 0x600a20ec |
| set_gauss_coeff_2m | 0x600a20f0, 0x600a20f4 |
| bt_bb_set_le_tx_on_delay, bt_bb_set_zb_tx_on_delay | 0x600a2010 |
| bt_bb_tx_cca_period | 0x600a20b8 |
| ieee802154_txon_delay_set | 0x600a3104, 0x600a310c, 0x600a3110, 0x600a3114 |
| bt_bb_v2_rx_set | none of its own; it calls the groups below |
| bt_bb_rx_set | 0x600a2000, 0x600a2008, 0x600a200c, 0x600a204c, 0x600a2050, 0x600a2054, 0x600a20bc, 0x600a20c0, 0x600a2800, 0x600a284c, 0x600a2888, 0x600a288c, 0x600a2c00, 0x600a2c04, 0x600a2c0c, 0x600a4c24, and bt_agc_v2_set |
| bt_agc_v2_set | 0x600a2058, 0x600a2824, 0x600a2828, 0x600a282c, 0x600a2854, 0x600a28a0, 0x600a28c8, 0x600a28d0, 0x600a28d4 |
| bt_bb_rx_correlator_set | 0x600a2060, 0x600a2064, 0x600a2068, 0x600a20f8, 0x600a20fc, 0x600a2100 |
| bt_bb_rx_dpo_set | 0x600a2020, 0x600a2024, 0x600a2028, 0x600a202c, 0x600a2030, 0x600a2034, 0x600a2038, 0x600a28f4 |
| bt_agc_restart_set | 0x600a2884, 0x600a2888, 0x600a28bc, 0x600a28dc, 0x600a28e0, 0x600a28e4, 0x600a28e8, 0x600a28ec, 0x600a28f0 |
| bt_agc_target_set | 0x600a2840, 0x600a2894, 0x600a2898, 0x600a289c, 0x600a28b0, 0x600a28b4 |
| bt_agc_detect_set | 0x600a28a0, 0x600a28a8, 0x600a28ac, 0x600a28b8, 0x600a28c0, 0x600a28c4 |
| bt_agc_recorrect_set | 0x600a2868, 0x600a286c, 0x600a28b0, 0x600a28b4, 0x600a28bc |
| bt_agc_rssi_thresh | 0x600a28a0, 0x600a28ac, 0x600a28b8 |
| bt_agc_gain_offset | 0x600a2848, 0x600a2868 |
| bt_agc_gain_max | 0x600a2840, 0x600a2850 |
| bt_agc_gain_set | 0x600a2854, 0x600a7cd0, and bt_agc_gain_max |
| bt_bb_rx_filter_sel | 0x600a20fc |
| bt_set_rx_comp | 0x600a2844 |
| zb_rx_core_set | 0x600a2c04, 0x600a2c08, 0x600a2c20, 0x600a2c24 |
| zb_rx_v0_0_set | 0x600a2c0c, 0x600a2c10, 0x600a2c14, 0x600a2c18, 0x600a2c2c, 0x600a2c30, 0x600a2c34 |
| zb_rx_auto_sel | 0x600a2c48 |

The constants a read gives a number that says something, each a lead and a hypothesis, computed from the disassembly:

| constant | where | hypothesis |
|---|---|---|
| 0x7a120, 500000 | zb_rx_core_set's 0x600a2c20 bits 8 to 26 and zb_rx_v0_0_set's 0x600a2c2c bits 8 to 26 | 500000, a count or a duration |
| 0xf85edf, -500001 | zb_rx_core_set's 0x600a2c24 bits 8 to 31 and zb_rx_v0_0_set's 0x600a2c30 bits 8 to 31 | the negative beside the 500000, a range about ±500000; -500001 as a signed 24-bit, one from -500000, so the magnitude is the lead |
| 0xbb8, 3000 | zb_rx_core_set's 0x600a2c04 bits 16 to 31 and 0x600a2c08 bits 0 to 15 | 3000, a duration or a count |
| 0x1770, 6000 | zb_rx_core_set's 0x600a2c08 bits 16 to 31 | 6000, twice the 3000 or a second count |
| 0x199a, 6554 | bt_bb_rx_set's 0x600a200c bits 12 to 27 and 0x600a2c00 bits 12 to 27 | 0.1 in Q16, 6554 of 65536 |

## The driver's memory, and whom it trusts

The driver's own code takes no memory as it runs.
The MAC's list and its sending's frame, the queues and locks, and every thread's stack
are declared where they are used, so the image says what the driver needs and a run cannot fail for want of it;
`osi.h`'s objects are ready zeroed, but for a timer, which names its function, and a queue, which `OSI_QUEUE` declares with its slots.
The heap is left to the code that is not the driver's and is written around one:
hostap's supplicant and Mbed TLS as a station joins, about 12 KB at the most under WPA3 and 6 KB under WPA2.
`libphy.a` takes nothing from it in the driver's own start, as a heap that named each block's caller found.
The link fails if any other of the driver's objects calls an allocator, `WIFI_ESP_HEAP` in `build.mk`.

Inside the driver nothing crosses a boundary of trust, so a frame is read where the MAC wrote it:
the receiving's thread keeps the MAC's buffer of a frame for the station and queues it, `MAC_RX_KEPT` at most,
and the station's thread gives each back once read, `mac_rx_release`; the MAC's list has room for them besides its own.
That was a copy of every frame in the receiving's thread, half of what it spent on one.

The driver holds the modem, a bus master that reaches all of RAM, so it is trusted as the root task is;
see open decision 13. The network process trusts the driver, as it cannot do otherwise,
and the driver does not trust the network process, nor the network process its clients.
So the driver lends the link, `lib/bulk.h`:
the network process reads each frame the driver received in a buffer lent to read, which it cannot write,
and fills the buffers it was lent to fill with the frames it sends,
of which the driver reads each once, into the frame the MAC sends;
and the driver takes no more of the link for each wake than its ring held as the turn began,
so a network process that kept handing references would not keep the first thread from the root task's checks.
The station's thread lends what it receives and the first thread sends what comes back filled, two threads on one end,
so each call on the end holds a lock of `lib/lock.h`, `sta.link_lock`;
the adapter's mutex, which takes the lock the receiving's interrupt takes too, doubled what a frame cost to hand on.
The copies stay: the MAC writes into the driver's own buffers, so a frame is copied once into a buffer lent to read,
about 4,800 cycles a kilobyte here.
Reading in place across the boundary would want the MAC's list to hold the link's own buffers,
a block the root task's free memory cannot spare yet,
and sending in place would want the MAC to read a frame from a chain of descriptors, which is untried; see `TODO.md`.

## Taking the PHY from `libphy.a`

`libphy.a` is taken a function at a time,
each the driver's own in `phy.c` and held against the library's before the library's symbol is weakened
in the copy `build.mk` makes, until nothing of it is linked.
The driver keeps to 2.4 GHz channels 1 to 13 and never above the power `libphy.a` sets for a channel and rate.
The registers and `phy_param` fields each step touches are named in `PHYREGS.md`, with their source and how sure it is.

The first is `phy_wifi_enable_set`: one read-modify-write of bit 1 of `MODEM_SYSCON_WIFI_BB_CFG`, 0x600A981C,
`lui 0x600aa; lw -0x7e4(a4); ori 2` or `andi ~2; sw`, and nothing else,
so the driver's own and the library's are equal by their disassembly alone.
The library's own `phy_wakeup_init_` and `bb_init` call it with 0 under `phy_param[0x19e]`,
and after the weakening they call the driver's.
`phy_bbpll_en_usb`, which the driver called once before its calibration, is an empty return on this chip,
so its call went rather than a replacement.
`make BOARD=esp32c6 wifi-esp32c6-refs` counts the driver's own calls of the library, 17 before and 15 after.

Next, the temperature sensor's power: `phy_set_tsens_power`, bit 22, `APB_SARADC_TSENS_PU`,
of `APB_SARADC_APB_TSENS_CTRL_REG`, 0x6000E058, one read-modify-write from the argument's low bit,
and `phy_xpd_tsens`, which the library's writes as: while `phy_param`'s byte 0x19D is clear,
`phy_set_tsens_power(0)`, then set the flag.
Both are their disassemblies' own, with nothing else in them;
a scan on the board read the calibration and left the radio off at a clean halt,
and the count is 15 before and 13 after.

Next the ROM's own two, batched into one board run.
`phy_disable_low_rate`, which `mac_low_rate_disable` called, is `phy.c`'s `drv_phy_low_rate_disable`:
three stores, 0x600A8060's bit 10, then its bit 11, then 0x600A807C's bit 11, each a read-modify-write.
Its masks come from the ROM's instructions run by a script, not read by eye,
after the first reading took `lui 0xfffff; addi 0x7ff` for a `~0x7ff` that it is not.
`get_data_sat`, which the baseband's power table asked, is `btbb.c`'s `data_sat`:
span where span is below index, else index, or bias where index lies below it, so index held between them.
A scan and a join on the board each ended at a clean halt, and the count is 13 before and 11 after.

Then `phy_get_max_pwr`, the first whose result depends on the calibration: the driver's own `rate_to_index`,
`wifi_get_target_power_new`, `get_chan_target_power_new` and `get_target_pwr` in `phy.c`, and the ROM's
`get_rate_fcc_index` written out there too, held to the library's in one run for every channel 1 to 13 and every
rate the driver's table is read at, 0 to 0x1b: 364 pairs, none different and none above the library's.
The library's `rate_to_index` runs past its own 0x12-byte table above rate 0x1f, reading its stack, and those
entries reached no register: `mac_init_tx_pwr` keeps the 0x1a entries its readers ask for, and our
`phy_get_max_pwr` refuses a rate above 0x1f. The count is 11 before and 10 after.

Then `wait_i2c_sdm_stable`, which `open_i2c_xpd_new` calls: our own reads the analog I2C's block 0x63, host 1,
register 0 through the ROM's table until it answers 0x5b, or until the MAC's microsecond counter at 0x600AD000
has moved more than 0x270f, as libphy's disassembly does. The count is 10 before and 9 after.

Then `phy_close_rf`, which `drv_phy_off` calls: the library's `phy_close_rf_` written out, and the driver's own.
While the temperature sensor still has its power it reads the temperature through the ROM's table,
its DAC code naming one of the library's five entries and the ROM's `code_to_temp` turning the sensor's code,
with the DAC corrected where the temperature lies outside its entry's range;
a print the library makes when the front end's setting does not settle is left out, a print not being a device write.
Then, only once the library's bring-up has marked the PHY up, it brings the front end's frequency setting down,
forces the TX and RX off, takes the RF down through the driver's own `phy_xpd_rf`, calibrates the RF PLL with the ROM,
resets the analog bias, and marks the RF off, all outside the library's critical section,
which on this chip is a pair of empty returns.
An analog I2C log of the library's window and the driver's, taken once each the same way, is the same eight entries
to one hash; the temperature fell in its entry's range, so the correction's write was not exercised.
Off the board, `phy-ab.py` runs both functions from one state in an emulator and compares their device accesses:
the same for 12 seeds and for each of the four combinations of the sensor read's and the PHY-up flag's branches.
It found two differences the board had not reached -- the frequency setting's enable is bit 26, not bit 30,
and the sensor table's offset byte is a signed load, its 0xfe and 0xff being -2 and -1 -- both fixed.
The PMU's word 0x600b0154 is ignored, the one the driver's `phy_xpd_rf` leaves to the kernel.
The count is 9 before and 12 after: the library's own call went, and the driver's own now names four of the ROM's,
which the ROM holds, not the library. Of the library's own symbols alone it is 7 before and 6 after.

Then the channel, the widest tree: the library's `phy_change_channel` and `chip_v7_set_chan`, `set_chanfreq` between them,
and the leaves of `chip_v7_set_chan_misc` the driver's own in `phy.c` --
`set_freq_i2c` and `phy_i2c_master_mem_txcap`, `wifi_get_tx_tab` with the two transmit-gain tables it builds,
`wifi_set_tx_gain` and its gain-memory write, `set_tx_gain_mem`, `phy_set_most_tpw`, `chan14_mic_cfg`,
`wr_rx_gain_mem` with the receive-gain index `rfrx_gain_index_new`, `set_rx_comp_new` and `write_gain_mem_new`.
The driver's own channel is refused outside 2.4 GHz channels 1 to 13 by a fault in `chip_v7_set_chan`,
after the frequency is turned into a channel, and the library's tracking functions' current channel lies inside them.
Off the board, `phy-ab.py` held `chip_v7_set_chan` to the library's for channels 1 to 13, twenty states,
both second arguments, each gain-run length and the 11p and channel-14 branches: 2080 calls, every access the same;
it found two differences the reading had missed -- `write_gain_mem_new` adds its 0x80000 rather than ors it,
and the eighth length byte of `wr_rx_gain_mem` is the zero the library's stack holds past the seven -- both fixed.
On the board a scan and a join ended clean, and the count is 12 before and 20 after, of the library's own 6 before and 4 after.

Then the wakeup, which `drv_phy_on` calls when the PHY was calibrated before, as the station's power save turns
the RF on and off: the library's `phy_wakeup_init_` written out, and the twenty leaves it runs through --
`i2c_clk_sel`, `i2c_bbpll_set`, `reg_set_new`, `freq_reg_init`, `bias_reg_set`, `pbus_clear_reg`, `phy_i2c_init2`,
`filter_dcap_set`, the frequency chain `freq_i2c_mem_write_new`, `freq_get_i2c_data` and `freq_i2c_write_set_new`
under `freq_i2c_data_write_new`, and `phy_reg_init` with `rx_11b_opt`, `tx_paon_set_new`, `agc_reg_init_new`,
`bb_reg_init_new`, `btbb_wifi_bb_cfg2`, `noise_floor_auto_set_new` and `bt_filter_reg_new`.
It powers the baseband PLL and the analog bias, brings the front end, the baseband and the frequency setting up,
clears the RF-off and sensor flags in `phy_param`, and clears Wi-Fi's baseband where `phy_param` 0x19e asks;
the library runs it inside the ROM's I2C critical section, which the driver's own leaves out.
Four device words the library's own leaves write are left to the kernel's `board_init`, as the earlier steps left them:
PCR's 0x60096130 from `phy_get_xtal_freq`, the PMU's 0x600b00cc and 0x600b0154 from `open_i2c_xpd_new`,
and LP_AON's 0x600b1054 from `pwdet_reg_init_new`; `phy-ab.py` ignores those four and the wakeup is the same
as the library's for twelve states.
On the board a scan heard four access points, and a join with `ps=1` turned the RF off and on seventeen times
through the driver's own `phy_close_rf` and `phy_wakeup_init`, answered the echo and five pings, and halted clean;
the count is 20 before and 35 after -- the ROM's own PHY functions the driver's new code names --
of the library's own symbols 4 before and 3 after.

Then the transmitter's IQ correction, which the library's bring-up and its second tracking block both run:
the library's `txiq_cal_init_new` and the two halves under it, `rfcal_txiq_new` and `txiq_cover_new`, the
driver's own, with the five ROM leaves they call -- `txiq_set_reg`, `txiq_get_mis_pwr`, `get_power_atten`,
`txcal_work_mode`, and the tone through `get_tone_sar_dout`.
Each half raises its bit in the front end's word `FEDATA_REG_SET3`, lowers its done bit, puts the transmitter
into the mode its last argument names -- a pbus register forced where the loopback is not used, the loopback on
and the transmit DC pair settled where it is -- and measures a tone's power through `txiq_get_mis_pwr` at the
attenuation and the temperature it is given.
Over seven rounds each round's mismatch moves the correction pair, held between -0x1f and 0x1f and its second
byte between -0x3f and 0x3f; the forward pair is packed into `phy_param`'s `TXIQ` at 0xd0, the loopback pair
into `LB_TXIQ` at 0xe6, and `param_flag`'s bit 14 marks the correction done, so the second call does nothing.
The tone generator's word at 0x600a0420 is kept and put back around each half, and the two start bytes
`phy_param` 0x18 and 0x2b are read as they are, no label naming either.
Off the board, `phy-ab.py` held all three to the library's, the tone's reads given in turn by `--seq` so the
branches that turn on a measurement are exercised: `txiq_cover_new` the same for every sequence tried, with
seven different sequences giving seven different pairs, `rfcal_txiq_new` the same for each of its three modes
and clear and negative temperatures, and `txiq_cal_init_new` the same for the whole correction, its done bit
clear and set.
It found two differences the reading had missed -- the tone generator's word is at 0x600a0420, the front end's
window offset 0x20 and not the modem's, and the pair's first byte is packed into the caller's halfword with the
mask 0x1f80, from `lui 0x2; addi -0x80`, not 0x180 -- both fixed.
The count is 47 before and 51 after -- the four new ROM leaves the driver's own names -- and of the library's
own symbols 4, as it was before.

Then the tracking that reruns what has drifted: the library's `phy_cal_param_track`, the driver's own, which
the library's `phy_param_track_tot` calls once a second while the station is joined.
Two temperatures are held against the current one, the receive rows' at `phy_param` 0x198 and the transmit
calibration's at 0x48, each against its own step -- 0x1e and 0x28, or the byte at 0x1a6 where bit 1 of 0x1a5
is set, which `phy_track_temp_debug` writes.
A block whose temperature has moved past its step brings the frequency setting down, forces TX and RX off and
clears the pbus; the first then wipes the receive rows' flags, rebuilds the rows and retunes the channel,
disables the baseband around it and puts its enable bit back as it was; the second takes the baseband down,
runs the IQ and transmit-DC calibrations and the transmit gains, and puts the baseband back.
Either way it releases TX and RX, brings the setting up and sets the block's temperature byte to the current
one; the library's prints, under its first argument, are left out.
`force_txrx_off`, which this tree reached in the library, is the driver's own here -- a global in `phy.c`,
called as before -- so `build.mk` weakens the library's with it.
Off the board, `phy-ab.py` held it to the library's for both blocks taken and skipped, the debug threshold
both ways, channels 1 to 13 with both second arguments, and the parameter flags: 23 runs, every access the
same.
It found three differences the reading had missed -- the row flags' mask clears bits 7 and 9, so `-0x281` is
`& ~0x280` and not `& ~0x281`; the two temperatures are signed halfwords, so each is sign-extended before the
subtraction; and the row flags' write, from an earlier step, puts the flag into the word as it then is rather
than as it was read at the function's start -- all three fixed.
The map's line for the PMU words left to the kernel went with a dead library function the linker had kept
beside the library's `force_txrx_off`, which this step weakens; the only entry outside the frames is the
LP_WDT word, as before.
The count is 51 before and after -- no new named leaf -- of the library's own symbols 4 either way.

Then the whole tracking, the library's `phy_param_track_tot`, the driver's own, which `drv_phy_track` runs once a
second while the station is joined: inside the ROM's critical section, and only while the RF-off and the
sensor's power flags are clear, the RFPLL capacitance where `phy_param` 0x0a is set, Bluetooth's transmit power
where the caller asks, and Wi-Fi's -- the I2C track and the transmit power, and the parameter calibration where
0x19a is clear -- and the sensor's temperature is read last. The library's prints are left out.
Off the board, `phy-ab.py` held it to the library's for both arguments, each gate off, and the temperatures near
and far: 12 runs, every access the same.
The count is 51 before and 50 after -- the library's own `phy_param_track_tot` went -- of the library's own
symbols 4 before and 3 after.

On the board, a join with `ps=1` and forty-five seconds joined the access point on channel 9, answered the echo
and five pings, slept three times and turned the RF off and on thirty times through the driver's own
`phy_close_rf` and `phy_wakeup_init`, ran the tracking eight times through the driver's own `phy_param_track_tot`
-- the whole of the step above on the radio -- and halted clean.

Then the ROM's PHY function table and its slot fillers, the whole of package 5b. The ROM keeps forty-four function slots at 0x4087f954, which its own calls reach through `phy_get_romfuncs()` and the library's through `g_phyFuns`, and the library's `phy_get_romfunc_addr` fills twenty-three of them with the library's own. The driver's own `phy_get_romfunc_addr` hands the ROM the driver's `phy_param` through the ROM's `phy_param_addr` and fills the same twenty-three slots, first with the library's fillers by name and then, in this package's second commit, with the driver's own. Three of those are the ROM's own byte for byte -- `ram_set_chan_cal_interp`, `ram_set_chan_freq_sw_start` and `ram_chip_i2c_readReg_org`, which the driver's calls by name -- and twenty are the driver's, taken over with their disassembly: the analog I2C's masks and host id, the pbus's force mode and address and shift windows, the receiver's and transmitter's and loopback's bring-up, the transmit tone generator's run and stop, the transmit calibration's debug mode, the power detector's tone and idle wait and SAR read, the RF PLL's frequency, and the baseband's gain index. Two differences the library keeps against the ROM's own show why they are its own: `ram_get_i2c_hostid` masks 0x223 and writes 0x1f00 where the ROM masks 0x227 and writes 0x1fe00, and `ram_rfpll_set_freq` divides the parameter's offset by ten where `phy_param` 0x1a7 is set and signs and widens the division to 64 bits where the ROM's is 32. `g_phyFuns` stays the library's, as `phy_param` does, since the library's own functions read it; both move to the driver when the last object that needs them goes.

Held to the library's off the board, by phy-ab.py: each of the twenty-three fillers alone, from the same state, for its arguments and gates and seeds -- including `ram_pbus_force_mode` with the syscon's bit 1 both ways, `ram_pwdet_tone_start` with `phy_param` 0x1a1 both ways, `ram_rfpll_set_freq` with 0x1a7 both ways and its five bytes compared, `ram_bb_gain_index` over its seven entries, and `ram_read_sar_dout`'s eight halfwords compared -- 27 runs, every access, every return and every written byte the same. The two `phy_get_romfunc_addr` are compared slot by slot, the same twenty-three offsets and the same function names. The whole of `register_chipv7_phy`, still the library's and now calling the driver's own table and fillers everywhere, is the same as the library's for the same 22 runs as the point of reference: eight seeds, seven tone sequences, the four calibration modes and the gates, every access, `phy_param`, the calibration data and the return the same, and the log identical to the reference. The A/B found two mis-readings -- `lui`'s immediate shifted left twelve, so the pbus force mode's bit is 27 and not 31 and the SAR mask is thirteen bits -- and one negative `andi -0xd`, which clears bits 2 and 3 and not bit 0. The count is 63 before the package and 72 after, its first commit 88 -- the driver's own table names the library's twenty-three fillers -- and its second 72, the driver's own table and fillers naming the ROM's helpers instead; of the library's own symbols 11 before, 35 after the first commit and 13 after the second.

Then the calibration's saved blob, which `register_chipv7_phy` carries in and holds across a restart:
the library's `phy_get_rf_cal_version`, `phy_set_mac_data` and `phy_get_mac_addr`, `phy_rfcal_data_sub`,
`rf_cal_data_backup` and `rf_cal_data_recovery`, `phy_rfcal_data_check` and `phy_rfcal_data_check_value`,
the driver's own. Its head is the version, 0x159, and the factory MAC; from 0x0C it holds `phy_param`[0..0x1A8)
word for word, the two SAR codes `rf_cal_data_recovery` restores by hand at 0x26 and 0x28, and its last word at
0x1B4 is the negation of the sum of every word before it. `phy_rfcal_data_check` puts the version and MAC at the
head and either stores that sum or reports the blob stale against it; `phy_rfcal_data_check_value` with mode
zero restores the blob into `phy_param` and copies its fields into the caller's 0x66-byte buffer, and with mode
set holds that buffer against `phy_param` field by field and counts each that moved by more than four -- the
receive IQ pairs going through `get_iq_value`'s unpacking first, which the blob's comparison calls, so the
driver takes it here although it is the receive-IQ package's.
The front end's `esp_phy_efuse_get_mac`, which the driver reads the factory MAC through, is kept a call rather
than inlined: `phy_get_mac_addr` reverses its bytes, and the off-board control replaces both the driver's read
and the library's with a stub, which a folded-away call would escape. The blob's layout is in `PHYREGS.md`.
Off the board, `phy-ab.py` held the whole of `register_chipv7_phy` -- still the library's, calling the driver's
own blob by then -- to the library's from one state for eight seeds, seven tone sequences, the four calibration
modes and the gates: 22 runs, every access, `phy_param`, the calibration data and the return the same.
Held apart, the blob's own eight functions are the same for the pointer arguments and five states, and
`get_iq_value` for all 8192 pairs it reads.
The count is 50 before and 51 after -- the ROM's `phy_byte_to_word`, which the blob's own sums words with --
of the library's own symbols 3 either way.

Then the RC calibration, which the library's `rf_init` runs in its bring-up:
the library's `rc_cal_init`, `rc_cal_new` and `i2c_rc_cal_set`, the driver's own, with the ROM's `get_rc_dout`
and `get_data_sat` called as before. `i2c_rc_cal_set` writes three of `i2c_txrf`'s fields -- register 0x0B's
bits 4..5, 0x08's bits 3..7 and 0x0D's bits 2..5 -- through the ROM's `rom_i2c_writeReg_Mask`; `rc_cal_init`
hands `rc_cal_new` the library's three byte tables, its constants 0x141E1428, 0x2814 and 0x20163224; and
`rc_cal_new`, once `param_flag`'s bit 23 says it has not run, takes the ROM's `get_rc_dout` -- which settles the
oscillator through those same registers and answers its register 0x0E -- and turns it into `phy_param`'s
`rc_dout` bytes: one raw, two for Wi-Fi and four for Bluetooth, each held between the ROM's `get_data_sat`
bounds, before it sets bit 23. Which value selects what is not known; the field names stay by register.
Off the board, `phy-ab.py` held each of the three to the library's: `rc_cal_new` for eight seeds that read the
oscillator from 0x09 to 0xF8, its bit set and clear, and a second table; `rc_cal_init` the same; and
`i2c_rc_cal_set` for both its arguments' 0 to 255; 23 runs, every access and `phy_param` the same. A mutation
of the 0x38 offset was told apart at `phy_param` 0xE9, so the arithmetic is under the comparison, not only the
accesses. The whole of `register_chipv7_phy`, still the library's, now calling the driver's own RC calibration
by way of its `rf_init`, is the same as the library's for the same 22 runs.
The count is 51 before and 52 after -- the ROM's `get_rc_dout`, which the driver's own names now --
of the library's own symbols 3 either way.

Then the frequency memory's write and the pbus's save, which the library's `rf_init` and `bb_init` call:
the library's `wr_rf_freq_mem_new` and `save_pbus_reg`, the driver's own. `wr_rf_freq_mem_new` writes the
frequency memory's two words for one index through the ROM's `get_freq_mem_param` and `get_freq_mem_addr` and
the driver's own `freq_i2c_mem_write_new`, the second with the flag word 3, as the library does. `save_pbus_reg`
copies the six pbus force words in FECTRL, 0x8E0 to 0x8F4, into `phy_param` 0x30 to 0x48, word for word; no
header names them and only `save_pbus_reg` and `write_pbus_mem` touch them, so the names stay by register.
Off the board, `phy-ab.py` held both to the library's: `wr_rf_freq_mem_new` for all 0x55 indices and for four
seeds, `save_pbus_reg` for its six words forced to distinct values and for three seeds, every access and
`phy_param` the same. The whole of `register_chipv7_phy`, still the library's, calling them through its
`rf_init` and `bb_init`, is the same as the library's for the same 22 runs.
The count is 52 before and after -- no new named leaf, the ROM's `get_freq_mem_param` and `get_freq_mem_addr`
already named -- of the library's own symbols 3 either way.

Then the pbus's program, which the library's `bb_init` runs: the library's `write_pbus_mem` and `set_pbus_mem`,
the driver's own. `write_pbus_mem` takes twelve word arrays and writes them into the front end's gain-memory
words, one entry at a time through `FECTRL_GAIN_MEM0` 0x8CC under a countdown in `FECTRL_GAIN_MEM_CONF` 0x8C8,
and forces each array's range of entries into one half of one of the six `FECTRL_PBUS_FORCE` words 0x8E0 to
0x8F4, its first and second array taking the low and the high half. `set_pbus_mem` builds the twelve arrays from
four of the library's own tables and two of `phy_param`'s bytes (2 and 0x14), hands them over and then saves the
force words; two of the copies it hands have a word put back by hand, which is why the arrays are not six and
their copies. Off the board, `phy-ab.py` held `set_pbus_mem` to the library's -- and so `write_pbus_mem` under
it, from the library's own buffers -- for eight states of those two bytes, every access the same. A mutation of
the first table's first word was told apart, so the tables are under the comparison, not only the accesses. The
whole of `register_chipv7_phy`, still the library's, now calling the driver's own pbus by way of its `bb_init`,
is the same as the library's for the same 22 runs. The count is 52 before and after -- no new named leaf -- of
the library's own symbols 3 either way.

Then the RF PLL's charge pump, which the library's `rf_init` calibrates: the library's `rfpll_chgp_cal`, the
driver's own. The analog block `i2c_rfpll` 0x62 is armed through its register 0x0f's bits 6 and 5, and
register 0x0e's bit 7 is read after a 20 us wait -- up to a hundred times -- until it rises; then register
0x0e's bits 4..0, the measurement, is turned into the pump's setting, times 7 over 6 plus 9 held at 0x1f,
written to register 0x0f's bits 4..0 with its bit 6 set, and the register read back into `phy_param` 0x196,
which `phy_i2c_init2` and the library's `rf_init` read. The driver's own reads the register through a new
named ROM leaf, `rom_i2c_readReg_Mask`, its table's slot 23, which `rfpll_chgp_cal` is the first to reach.
Off the board, `phy-ab.py` held it to the library's for eleven oscillator values, seven for which the done bit
rises at once (14 events) and four for which the loop runs its hundred (212): every access and `phy_param` the
same. A mutation of the `9` was told apart on both paths. The whole of `register_chipv7_phy`, still the
library's, now calling the driver's own pump by way of its `rf_init`, is the same as the library's for the same
22 runs. The count is 52 before and after -- the new ROM leaf is reached through the table, not by name, so
nothing is added to the count -- of the library's own symbols 3 either way.

Then the frequency memory's build, which the library's `rf_init` runs at its end: the library's
`get_rf_freq_init_new` and `set_chan_freq_hw_init`, the driver's own. `set_chan_freq_hw_init` writes the
frequency module's words through the driver's own `freq_reg_init`, builds the memory for the 0x55 channels
`set_rfpll_freq` can reach, and writes it. `get_rf_freq_init_new` runs once, marked by `param_flag`'s bit 5:
the PLL's capacitance is written to 0xc8, the analog block 0x62's register 2 is armed, three settings at 0x985,
0x960 and 0x9a0 MHz go through the ROM's `set_rfpll_freq`, the last two are read back as the capacitance's two
ends, and their difference steps a running value over the channels: each channel is retuned through the
library's `ram_rfpll_set_freq` (the table's slot 12), the running value turns into a baseband word through the
ROM's `get_data_sat`, and the two words are packed and written by the driver's own `wr_rf_freq_mem_new`.
Off the board, `phy-ab.py` held `get_rf_freq_init_new` to the library's for eight seeds, the gate set and
clear, and five counts and parameters, every access and `phy_param` the same; `set_chan_freq_hw_init` for
three pairs of its arguments, the same. A mutation of the 0x985 was told apart at the block's I2C writes, so
the constants are under the comparison, not only the accesses. The whole of `register_chipv7_phy`, still the
library's, now calling the driver's own memory by way of its `rf_init`, is the same as the library's for the
same 22 runs. The count is 52 before and 53 after -- the ROM's `set_rfpll_freq`, which the driver's own names
now -- of the library's own symbols 3 either way.

Then the temperature sensor's read and its calibration, which the library's `register_chipv7_phy` starts the
calibrations from: the library's `tsens_dac_to_index`, `tsens_dac_cal_new`, `ram_tsens_temp_read_new`, its weak
`phy_get_tsens_value`, `tsens_temp_read_new` and `get_temp_init_new`, the driver's own. The sensor's DAC code is
read from the analog I2C's block 0x69, host 0, register 6, and names one of five ranges, each a DAC code, an
offset and a temperature span; the sensor's own code, through the ROM's `tsens_code_read`, is turned into a
temperature by the ROM's `code_to_temp` with that range's offset, and a temperature outside the range writes a
corrected DAC code back to the same register. The index is kept at `phy_param` 0x16 and the temperature at
`phy_param` 0x00. `get_temp_init_new` then hands the temperature to the blocks that start from it: the transmit
calibration's `temp_code_init` and its own kept temperature where the caller asks, the RFPLL capacitance
tracking's kept temperature where the caller asks, and always the transmit power tracking's. The library's
`register_chipv7_phy` asks for both while `param_flag`'s bits 5 and 20 are clear, and bit 20 is read there alone,
no step of the library setting it. The library's `phy_set_tsens_range` is left out, nothing calling it. A DAC
code the five ranges do not name takes index 5, and the library's `tsens_dac_cal_new` then reads a sixth entry
past its table -- `low` and `high` both 256, `dac` 0 -- answering 0 for a temperature of exactly 256 and
correcting otherwise; the driver's own corrects for that index too, so the two differ for that one input, which
no sensor code through the ROM's `code_to_temp` reaches, and the comparison leaves it out.
Off the board, `phy-ab.py` held the read and its calibration to the library's: `tsens_dac_to_index` for the
seventeen codes its five ranges and the unknown ones name, `tsens_dac_cal_new` for every temperature from -40 to
125 against three indices and for every index against the ranges' bounds, `ram_tsens_temp_read_new` and
`phy_get_tsens_value` and `tsens_temp_read_new` for twenty-four seeds, and `get_temp_init_new` for its four
argument pairs by eight seeds; every access and `phy_param` the same. A mutation of the corrected DAC was told
apart at the argument, so the arithmetic is under the comparison, not only the accesses. The whole of
`register_chipv7_phy`, still the library's, now starting its calibrations through the driver's own read, is the
same as the library's for the same 22 runs. The count is 53 before and after -- no new named leaf -- of the
library's own symbols 3 either way.

Then the analog bring-up, which the library's `register_chipv7_phy` runs between its calibration-data check
and its temperature init: the library's `rf_init` and `bb_init` and the two analog I2C initialisations,
`phy_i2c_init1` and `phy_i2c_master_cmd_mem_init`, the driver's own. `rf_init` brings the baseband PLL, the
analog bias, the analog I2C, the I2C master, the power detector, the front end, the baseband registers and the
temperature sensor up, sets the I2C clock and the baseband PLL, runs the RC calibration and the filter's DCAP,
reads the RF PLL's charge pump word into `phy_param` 0x196, opens the analog I2C's blocks through the twelve
writes of `phy_i2c_init1` -- the transmit block 0x6b's registers 2 to 8, the RF PLL 0x62's registers 0, 4, 0x0f
and 0x0b, the baseband top 0x67's register 2 -- calibrates the charge pump and programs the I2C master's
command memory, thirty words from 0x600afc00 that `phy_i2c_master_cmd_mem_init` writes with the RF PLL's charge
pump byte and the RC calibration's bytes, and ends tuning the frequency module with the two arguments 2 and 4
the library leaves in the registers. `bb_init` raises the SAR ADC's power detector drive and the frequency
setting's bit 28, runs the baseband's calibrations once -- the transmit DC and IQ, the power detector's, the
transmit capacitor's and the transmit power's, marked in `param_flag` bit 3 -- then Bluetooth's gain, the pbus
program, the receive IQ calibration, the receive table, the gain table for 0x985, the front end's registers and
channel 11, and lowers the drive and the setting's bit 28 again, clearing Wi-Fi's baseband where `phy_param`
0x19e asks. `param_flag` bit 3 was not named before; the map gains the bring-up's blocks, and the library's
`i2c_mst` command memory now has its name.
Off the board, `phy-ab.py` held each to the library's: `phy_i2c_init1` and `phy_i2c_master_cmd_mem_init` for
twelve seeds each, `rf_init` for eight seeds, and `bb_init` for three states of `param_flag` -- its calibration
bit clear, set and with another flag -- by six seeds; every access and `phy_param` the same. A mutation of the
command memory's first word was told apart at the first event, so the thirty words are under the comparison.
The whole of `register_chipv7_phy`, still the library's, now calling the driver's own bring-up, is the same as
the library's for the same 22 runs. The count is 53 before and 63 after, and of the library's own symbols 3
before and 11 after: the driver's own bring-up names the library's own calibrations it runs until their
packages -- `txdc_cal_init_new`, `tx_cap_init_loop`, `tx_pwctrl_init_new`, `bt_tx_gain_init`, `rxiq_cal_init`,
`phy_rx_table_init`, `rfrx_sat_rst` and `phy_check_rx_sat` -- which the count will give back as packages 6 to 9
and 11 and 12 take them over.

Then the receive table and the transmit channel's offset, package 6:
the library's `phy_rx_table_init`, `chip_v7_set_chan_offset` and `mac_tx_chan_offset_new`, the driver's own.
`phy_rx_table_init`, which the driver's own `bb_init` runs before the gain rows are built, writes `phy_param`'s
halfword 0x4f4f at 0x120 -- its two receive counts -- and the front end's gain memory 0x4f times through
`write_gain_mem_new`, then the front end's registers and the AGC. `mac_tx_chan_offset_new` sets the transmit
channel's two offset bits in the transmit minimum power's word, `MAC_TX_MIN_PWR` 0x600a4400, 2 for channel 2, 1 for
channel 3 and 0 for every other -- the word `macstart.c`'s own start also writes, its bits 4 to 9 for the minimum
power and bit 17 for the HE group, so the PHY and the MAC share it, an inference from the two functions and not a
stated purpose; its ROM counterpart `mac_tx_chan_offset` [0x40004dbe] also sets bit 0 for channel 2, which is how
the library's own differs. `chip_v7_set_chan_offset` builds the channel's frequency offset -- the channel plus two,
its low two bits dropped -- adds `phy_param` 0xA2's signed byte in eighths where 0xA1 is set, and hands it to the
ROM's `phy_freq_correct` and `set_channel_rfpll_freq`; the library's `register_chipv7_phy` calls it only where
`phy_param` 0x25 is clear and 0xA1 is set. The branch is dead on the board -- nothing in the library writes 0xA1 or
0xA2 and the driver's `phy_init_data` leaves 0xA1 zero, so the recorded run did not reach it -- and its check is
the off-board comparison with the gate forced both ways. The library runs it inside the ROM's critical section, the
same pair of empty returns left out as elsewhere. The names stay by register and `phy_param` offset for 0x20, 0xA1
and 0xA2, which have no label and whose meaning is not known, in `PHYREGS.md` with the open questions.
Off the board, `phy-ab.py` held each to the library's: `mac_tx_chan_offset_new` over channels 0 to 5 and with the
word's other bits set, `chip_v7_set_chan_offset` over channels 1 to 13 with the gate clear and set, 0xA2 from -1
up to 127, and the boundary channels, and `phy_rx_table_init` for seeds and both `param_flag` states with
`phy_param` compared; 18 runs, every access and `phy_param` the same. The gate was forced both ways and shown to
move the offset and the I2C bytes it reaches. A mutation of the eighths in `chip_v7_set_chan_offset` was told
apart at `phy_param` 0x20, one of `mac_tx_chan_offset_new`'s channel-2 bits at the write, and one of
`phy_rx_table_init`'s 0x4f4f at `phy_param` 0x120. The whole of `register_chipv7_phy`, still the library's and now
calling the driver's own three, is the same as the library's for the same 22 runs, the log identical to the
reference.
The count is 72 before and 71 after -- the ROM's `phy_freq_correct`, which the driver's own now names, against the
library's `phy_rx_table_init` and `mac_tx_chan_offset_new` -- of the library's own symbols 13 before and 11 after.
The map gains one function in FE and one in WIFI_MAC and one indirect call; `phymap` is unchanged.

Then the receive IQ mismatch and the saturation checks, package 7a:
the library's `rfrx_sat_rst`, `phy_check_rx_sat`, `dc_iq_est_high` and `rxiq_get_mis_new`, the driver's own.
`rfrx_sat_rst` writes the front end's `AGC_4` 0x7068 with 0x404 and moves `AGC_RFRX_SAT` 0x705C's bits 31, 30, 28,
24 and 19 with its argument -- raised with it, cleared without -- raises its bit 11 or 10 and clears its bits
0..18. `phy_check_rx_sat` brings the pbus to its debug mode, powers the receiver through the ROM's
`ram_pbus_xpd_rx_on`, hands the ROM's `pbus_set_dco` the DCO pair 0x01000100, 0x01000100 -- the library keeps the
four halfwords 0x0100 -- raises the receive gain's trigger, waits five microseconds and reads the receive gain's
SAR `AGC_GAIN_SAR` 0x708C a hundred times, its field at bits 12..18 counted where it is 0x45 or below; where any
counted, `phy_param`'s `rx_sat` 0x1A4 is set, and the pbus's work mode is put back. `dc_iq_est_high` and
`rxiq_get_mis_new` take the estimate through the ROM's `rom_iq_est_enable` (the table's slot 40): the four words
`FEDATA_RX_IQ_EST2..5` 0x478..0x484 are shifted by the gain less two and the two pairs' sum and difference crossed
into the receive IQ mismatch's two bytes, and the accumulators `FEDATA_IQ_EST_I/Q/MAG` are turned into decibels.
The names stay by register: 0x478 to 0x484, 0x705C and 0x7068 have no label, and what they mean is not known;
they are in `PHYREGS.md` with the open questions.
The first `pbus_set_dco` told the package's bug apart: with the pair written the other way round, 0x00010001, the
front end's word differed at bit 14 alone. Off the board, `phy-ab.py` held each to the library's: `rfrx_sat_rst`
with the argument clear and set, `phy_check_rx_sat` for seeds and with the SAR field either side of 0x45,
`dc_iq_est_high` over gains and divisors, and `rxiq_get_mis_new` over gains 0 to 8 -- 9 runs, every access,
`phy_param` and the output buffers the same. Three mutations were told apart: the SAR field's 0x45 at `phy_param`
0x1A4, one of `rfrx_sat_rst`'s high bits at the write to 0x705C, and `rxiq_get_mis_new`'s shift at its two output
bytes. Its third argument asks the library for its own print of the mismatch; the whole driver leaves the PHY
prints out, so the covered path is that argument clear. The whole of `register_chipv7_phy`, still the library's and
now calling the driver's own four, is the same as the library's for the same 22 runs, the log identical to the
reference.
The count is 71 before and 69 after -- the driver's own `phy_check_rx_sat` and `rfrx_sat_rst` no longer reached by
name -- of the library's own symbols 11 before and 9 after; `dc_iq_est_high` and `rxiq_get_mis_new` the library
calls alone, so they leave the count in place. The map gains one function in FE and one in WIFI_BB; `phymap` is
unchanged.

Then the receive IQ mismatch's correction, package 7b:
the library's `rxiq_cover_mg_mp_new`, `rfcal_rxiq_new` and `get_rfcal_rxiq_data_new`, the driver's own.
`rxiq_cover_mg_mp_new` runs the correction over two rounds: each hands the ROM's `rxiq_set_reg` the running pair --
the high byte then the low -- takes its answer, asks `rxiq_get_mis_new` for the mismatch, and folds the answer and
the two bytes into the pair; the second round's pair is held to the ROM's `get_data_sat` bounds (0x1f and 0x3f),
handed back to `rxiq_set_reg`, and left in the caller's two bytes. `rfcal_rxiq_new` raises `FEDATA_REG_SET3`'s bit
29 and lowers its bit 30, starts a tone through the table's `ram_start_tx_tone_step`, takes the correction, stops
the tone, and writes the pair out. `get_rfcal_rxiq_data_new` repeats that up to four times, accumulating the two
bytes as signed halves, stopping early once a reading is within one of the last on both -- the last two averaged --
else summing the four and dividing by four; the pair is again held to the bounds and packed high byte first into
the answer's halfword. The names stay by register: `FEDATA_REG_SET3`'s bits 29 and 30 and its two correction fields
have no label, and what they correct is not known; they are in `PHYREGS.md` with the open questions.
Off the board, `phy-ab.py` held each to the library's: `rxiq_cover_mg_mp_new` over gains 0 to 8, `rfcal_rxiq_new`
over gains and tone arguments, and `get_rfcal_rxiq_data_new` over tone arguments with the readings converging
early and with the estimate walked so all four are summed -- 8 runs, every access and the output buffers the same.
Three mutations were told apart: one gain in `rxiq_cover_mg_mp_new` at the estimate's word, one of
`rfcal_rxiq_new`'s `FEDATA_REG_SET3` bits at the write to 0x450, and the four readings' count in
`get_rfcal_rxiq_data_new` at the reading with the estimate walked, the answer and the event count both moved. Its
third argument asks the library for its own print, which the whole driver leaves out, so the covered path is that
argument clear. The whole of `register_chipv7_phy`, still the library's and now calling the driver's own seven of
the receive IQ's, is the same as the library's for the same 22 runs, the log identical to the reference.
The count is 69 before and 70 after -- the ROM's `rxiq_set_reg`, which the driver's own now names -- of the
library's own symbols 9 before and 9 after: the library calls the three alone, so they leave the count in place.
The map gains two functions in FE; `phymap` is unchanged.

Then the receive IQ calibration's gain search and the block that runs it, package 7c:
the library's `set_rx_gain_cal_iq_new` [0x4200699a] and `rxiq_cal_init` [0x420075c8], the driver's own.
`set_rx_gain_cal_iq_new` saves the analog block 0x67's register 3 bits 2..2 where its first argument asks, sets the
loopback gain and the loopback transmit IQ pair `phy_param` 0xD0, runs the ROM's `pbus_rx_dco_cal`, then steps the
receive gain a sixteenth at a time over two passes -- the estimate's `FEDATA_IQ_EST_MAG` shifted seven decides, below
0x1000 the gain falls and above 0x20000 it rises and the loopback side is dropped, the pbus's receive DC taken again
each step and the gain held between 0 and 0x78 -- and hands the second pass's gain and the tone to
`get_rfcal_rxiq_data_new`, whose halfword lands in the caller's buffer, before the saved analog bits go back.
`rxiq_cal_init` returns at once where `param_flag` 0xA4's bit 10 is set, else sets the channel's analog side to 6
through the ROM's `chip_v7_set_chan_ana`, writes the six-byte table at `phy_param` 0xDC through the ROM's
`rom_set_txcap_reg`, rises `FECTRL_TXRX_OFF` 0x600A0910's bits 14 and 15 -- the disassembly's `lui 0x4` and `lui 0x8`,
not bits 2 and 3 -- enters the pbus's debug mode, powers the receiver and the loopback, and where its first argument
is set reads the pbus's word and raises its bit 1. Around the gain search it rises `FEDATA_REG_SET3`'s bit 29 and
bit 13 and lowers bit 30 and bit 14, repacks the four halfwords 0xD4..0xDA through `(v >> 1 & 0x1F80) | (v & 0x7F)`,
and where its third argument is clear clears `FECTRL_TXRX_OFF`'s bit 15, lowers the loopback, powers the transmitter
down through the table's slot 34 `rom_pbus_xpd_tx_off` -- not slot 32, `rom_pbus_xpd_rx_off`, which its name misled
the first reading to -- and restores the pbus's work mode, rising `FEDATA_REG_SET3`'s bit 30 and bit 14 and lowering
bit 29; then it sets `param_flag`'s bit 10.
The first run of seed 4 told the block's bug apart: the driver cleared `FEDATA_REG_SET3`'s bit 18, `0xfffbffff`,
where the library clears bit 14, `0xffffbfff`, and the write to 0x450 differed.
The names stay by register: `FECTRL_TXRX_OFF`'s bits 14 and 15 and `FEDATA_REG_SET3`'s bits 13, 14, 29 and 30 have
no label, and what they gate is not known; they are in `PHYREGS.md` with the open questions.
Off the board, `phy-ab.py` held each to the library's: `set_rx_gain_cal_iq_new` with its first argument clear and
set, two tones and seeds 1 and 5, and `rxiq_cal_init` for its first and third arguments clear and set by seed 1 and
by seed 4, the seed that reaches the bit-14 write, with `phy_param` 0xA4 zeroed, its early return with bit 10 set,
and the output buffers -- 5 runs, every access and `phy_param` the same. Three mutations were told apart: one of
`rxiq_cal_init`'s `FEDATA_REG_SET3` bits at the write to 0x450, one of `set_rx_gain_cal_iq_new`'s gain steps at the
estimate's word, and the early return's bit at `phy_param` 0xA4. The whole of `register_chipv7_phy`, still the
library's and now calling the driver's own nine of the receive IQ's, is the same as the library's for the same 22
runs, the log identical to the reference.
The count is 70 before and 71 after -- the ROM's `chip_v7_set_chan_ana`, which the driver's own `rxiq_cal_init` now
names -- of the library's own symbols 9 before and 9 after. The map is unchanged; `phymap` is unchanged.

Then the transmit capacitor calibration, package 8:
the library's `tx_cap_init_loop` [0x42009578] and `rfcal_txcap_loop` [0x4200946c], the driver's own.
`tx_cap_init_loop` calibrates over the three channels 1, 6 and 11 -- the frequency retuned to each by the ROM's
`set_channel_rfpll_freq` -- and for each raises `FECTRL_TXRX_OFF`'s bits 14 and 15, enters the pbus's debug mode,
powers the receiver, raises the loopback's clocks, sets the gain, takes the pbus's receive DC with the pair
0x01000100, and sets the analog I2C's `i2c_txrf` 0x6B register 2's low nibble to 1 and its high nibble to 5. A
tone at 0x80 is then started and its gain stepped by 0x18 while the estimate's third word sits at 31 or below and
down while it sits above 46, twice at most, before `rfcal_txcap_loop` runs into the byte `phy_param` 0xDC holds for
the channel -- 0xDC, 0xDE and 0xE0, so the three channels' `DCAP_STG1` pairs; the tone stopped, the loopback
dropped, the transmitter powered down, the pbus's work mode put back and `FECTRL_TXRX_OFF`'s two bits cleared.
`rfcal_txcap_loop` scans one byte at a time: the first pass walks `i2c_txrf` 0x6B register 2's bits 3..0 from 7
down to 0 and the second its bits 7..4 from 13 down to 1 by twos, each value written, the estimate's magnitude read
back through `dc_iq_est_high`, the largest kept and written at the pass's end, the caller's byte advanced -- so
0xDC's pair then 0xDE's -- and the second pass run for that byte. The names stay by register: `i2c_txrf`
register 2's two fields, `phy_param`'s `DCAP_STG` pairs and halfword 0x10 bit 7 (the library's print argument) have
no label, and what the capacitor's fields correct is not known; they are in `PHYREGS.md` with the open questions.
Off the board, `phy-ab.py` held each to the library's: `tx_cap_init_loop` for seeds and with `phy_param` 0x51's
byte, its halfword 0x10 bit 7 set and the estimate forced and walked through `--seq`, and `rfcal_txcap_loop` for
seeds and with the estimate forced and walked -- 13 runs, every access, `phy_param` and the `--out` buffers the
same. Three mutations were told apart: `rfcal_txcap_loop`'s second pass's first value (13 to 11) at its write, its
bits 7..4 field at its write, and `tx_cap_init_loop`'s upward gain step (0x18 to 0x10) at the tone. The whole of
`register_chipv7_phy`, still the library's and now calling the driver's own nine of the receive IQ's and its
transmit capacitor calibration, is the same as the library's for the same 22 runs, the log identical to the
reference.
The count is 71 before and 71 after -- the ROM's `dc_iq_est`, which the driver's own now names, against the
library's `tx_cap_init_loop`, which it no longer reaches -- of the library's own symbols 9 before and 8 after. The
map gains one function in FE, and `phymap` loses one reached function (1033 to 1032, 141 to 140 of the ROM);
WIFI_BB, WIFI_MAC and the pointer calls are unchanged.

Then the transmit DC calibration's two bring-ups and the loopback transmit IQ set, package 9:
the library's `txdc_cal_init_new` [0x42008d1e], `bt_txdc_cal_new` [0x42008c7a] and `set_lb_txiq_new` [0x42006962],
the driver's own. `txdc_cal_init_new` enters the pbus's debug mode, powers the transmitter with the caller's
second and third arguments, and where its fourth is set reads the pbus's word and raises its bit 1 first; then
five gains -- `index_to_txbbgain`'s -- are forced into the pbus's window 1 place 2 and one transmit DC pair
settled each into the caller's twenty halfwords by `txdc_cal_new`, the receiver powered down and the pbus's work
mode put back. `bt_txdc_cal_new` does the same for Bluetooth's pair where `param_flag`'s bit 12 is clear: the
transmitter powered with channel 0xf, the receiver's pbus window 4 place 2 forced from `phy_param`'s unlabelled
byte 0x14 shifted three, the pair settled into `BT_TXDC` 0x104 by `txdc_cal_new`, and the bit set. `set_lb_txiq_new`
splits its argument's packed pair through `get_iq_value` and hands the first byte signed and the second less four,
signed, to the ROM's `txiq_set_reg`'s forward and loopback halves. The names stay by register: `param_flag`'s bit
12 and `phy_param`'s byte 0x14 have no label, and how the five pairs relate to `TXDC` 0xA8/0xAA is not known; they
are in `PHYREGS.md` with the open questions. The library's print flag, `phy_param`'s halfword 0x10 bit 4 here, is
left out.
Off the board, `phy-ab.py` held each to the library's: `set_lb_txiq_new` over four packed pairs, `txdc_cal_init_new`
for its second and third arguments and its fourth both ways along with the print bit, and `bt_txdc_cal_new` with its
gate clear and set by three seeds -- 9 runs, every access, `phy_param` and the `--out` buffers the same. Three
mutations were told apart: `set_lb_txiq_new`'s second byte's four at its `txiq_set_reg` write, `txdc_cal_init_new`'s
pair stride at the caller's buffer, and `bt_txdc_cal_new`'s marking bit in `param_flag`. The whole of
`register_chipv7_phy`, still the library's and now calling the driver's own transmit DC bring-ups and loopback set
beside its nine receive IQ's, is the same as the library's for the same 22 runs, the log identical to the
reference.
The count is 71 before and 69 after -- the library's `txdc_cal_init_new` and `set_lb_txiq_new`, which the driver's
own no longer reaches -- of the library's own symbols 8 before and 6 after. The map and `phymap` are unchanged.

Then the head's init-parameter spread, package 10a:
the library's `register_chipv7_phy_init_param` [0x420037aa], the driver's own.
It copies the caller's init data -- the 0x4c bytes it uses of the 0x80 -- into `phy_param`: its byte 0 to 0x50, its
bytes 2..0x13 to 0x52..0x63, its byte 0x18 to 0x66, its bytes 0x43..0x4b to 0x67..0x6f, its bytes 0x19..0x26 to
0x70..0x7d, its bytes 0x27..0x34 to 0x7e..0x8b and its bytes 0x35..0x42 to 0x8c..0x99, leaving 0x51, 0x64 and 0x65
as it found them. The mapping is in `PHYREGS.md`.
Off the board, `phy-ab.py` held it to the library's over seeds and with the init data filled, `phy_param` compared
in full. Three mutations were told apart: its first byte's source, the eighteen-byte copy's count and the last
copy's start. The whole of `register_chipv7_phy`, still the library's and now calling the driver's own spread, is
the same as the library's for the same 22 runs, the log identical to the reference.
The count is 69 before and 69 after -- the function is the library's own leaf, which the driver's own now defines in
its place -- of the library's own symbols 6 either way. The map and `phymap` are unchanged.

Then the head itself, package 10b:
the library's `register_chipv7_phy` [0x42003ea0], the driver's own, with `phy_param` and `g_phyFuns` left the
library's until the step's end.
It hands the ROM the PHY table (`phy_get_romfunc_addr`), raises MODEM_SYSCON's Wi-Fi and modem clocks (`CLK_CONF1`
0x404), clears the baseband's bit 1, forces the front end's transmit and receive off, resets the frequency module
and re-enables the frequency setting, and, where `phy_param` 0x4E asks, applies the library's `phy_set_23dbm` to the
built-in default init data it builds there. On a fresh chip (`phy_param` 0x25 clear) it spreads the caller's init
data -- or the built-in default where the caller brings none -- into `phy_param`, and checks the saved calibration
blob: in mode 1 a blob that stands is recovered and `param_flag`'s bits 5, 7, 9 and 16 cleared (the mask
0xfffefd5f), one that will not do clears the whole flag and moves the mode to 2, and any other mode restores the
blob into `phy_param` and clears the flag. `rf_init` and `bb_init` run, `get_temp_init_new`'s two gates come from
`param_flag`'s bits 5 and 20, and, unless the blob was recovered in mode 1, the restored block is held against the
chip, saved back and its negated sum rewritten. Where `phy_param` 0xA1 asks, `chip_v7_set_chan_offset` runs; the
baseband PLL is taken down, the RF PLL's SDM register read through the ROM's table, the chip marked up, the
frequency setting enabled, the front end's forcing released and the Wi-Fi clock put back as it was found. Nothing
calls `phy_set_23dbm_en` and libphy's `phy_param` starts with 0 at 0x4E, so the 23 dBm branch never runs on the
board and the transmit power stays within the library's; the driver transcribes `phy_set_23dbm` word for word and
leaves the gate as it is, and the off-board comparison covers the branch by forcing 0x4E in the model. They are in
`PHYREGS.md` with the open questions.
Off the board, `phy-ab.py` held the whole function to the library's for the same 22 runs, the log identical to the
reference, `phy_param` compared in full beside the accesses and the blob; the mode-1 recovery path was reached with
a blob whose checksum and `param_flag` words were written to stand, and the gates `phy_param` 0x25, 0x4E and 0xA1
were forced both ways. Four mutations were told apart: the Wi-Fi clock's 0x404, the recovery's flag mask at its bit
0, the chip-up byte `phy_param` 0x25, and the mode-1 backup gate. The count is 69 before and 69 after -- the
library's `register_chipv7_phy` gone and `phy_set_23dbm`, which the driver's own head now reaches, come -- of the
library's own symbols 6 either way. The map drops from 1032 to 1026 functions reached (the library's head and the
functions only it reached are gone): FE 89 to 86 and WIFI_MAC_TSF 16 to 14, with the ranges, addresses and the
writes outside the frames unchanged; `phymap` is unchanged.

Then the transmit power control, package 11:
the library's `rfcal_pwrctrl_new`, `tx_pwctrl_init_cal_new`, `tx_pwctrl_init_new`, `bt_tx_pwctrl_init_new`
and `phy_set_23dbm`, the driver's own.
`tx_pwctrl_init_new` and `bt_tx_pwctrl_init_new` run the calibration for Wi-Fi and Bluetooth, each gated and
marked by a bit of `param_flag` 0xA4 -- Wi-Fi's bit 20 and Bluetooth's bit 15 -- and returning at once where
it is set; each tunes the frequency, writes the transmit capacitor's table and hands the side's three fields
-- `wifi_pwctrl_atten` 0xF1, `wifi_cal_power` 0xF7 and `wifi_cal_atten` 0xF4 for Wi-Fi, `bt_pwctrl_atten`
0xFB, `bt_cal_power` 0xFE and `bt_cal_atten` 0xF8 for Bluetooth -- to `tx_pwctrl_init_cal_new`.
That walks the channels 1, 6 and 11 and, for each, runs `rfcal_pwrctrl_new`, which starts a tone and searches
its step between 0 and 0x64 over up to ten rounds, the power detector's read through the ROM's `get_power_db`
at `phy_param` 0x0E's halfword forming the error against the source byte, with a running temperature in
`PHY_PARAM_TXIQ_TEMP` 0x18 and a running accumulator over the source bytes; each round's step less the side's
span is the attenuation left in the caller's buffer and copied to the attenuation field beside it.
Bluetooth's, before its walk, saves and forces the analog I2C's block 0x67 registers 0x1C, 0x1D, 0x1E's
bits 5..0 and 0x1F, forces the pbus's windows from `phy_param` 0x13 and 0x14, sets its transmit DC pair and
clears `BT_TXIQ` 0x100.
`phy_set_23dbm` is the library's 23 dBm setting, transcribed word for word: where its gate, `phy_param` 0x4E,
is set, it puts 0x5c into the init data's bytes 2 and 3 and into `phy_param`'s byte 6, and the gate is left
exactly as the library leaves it.
Nothing calls `phy_set_23dbm_en` and libphy's `phy_param` starts with zero at 0x4E, so on the board the branch
never runs and the transmit power stays within the library's, under the 20 dBm EIRP limit; the off-board
comparison covers it by forcing 0x4E in the model, which transmits nothing.
Off the board, `phy-ab.py` held each of the five to the library's: `rfcal_pwrctrl_new` for one and several
source bytes and their walking accumulator, `tx_pwctrl_init_cal_new` for Wi-Fi and Bluetooth by seeds,
`tx_pwctrl_init_new` and `bt_tx_pwctrl_init_new` for their gates both ways by seeds, and `phy_set_23dbm` for
its gate both ways, every access, `phy_param` and the output buffers the same.
Four mutations were told apart: the search's accumulator mask, the calibration's step byte, Bluetooth's first
forced I2C byte and the 23 dBm setting's 0x5c.
The whole of `register_chipv7_phy`, the driver's own since package 10b and now calling the driver's own five,
is the same as the library's for the same 22 runs, the log identical to the reference, and `phy_param` is
compared in full beside the accesses.
The power limit `phy_get_max_pwr` over the same 364 pairs of channels 1 to 13 and rates 0 to 0x1b is the
same as the library's, none above.
The count is 69 before and 70 after -- the ROM's `get_power_db`, `pwdet_ref_code` and `bt_bb_to_index`, which
the driver's own now names -- of the library's own symbols 6 before and 4 after: `phy_set_23dbm` and
`tx_pwctrl_init_new` gone.
The map loses one indirect call, 170 to 169, and `phymap` is unchanged.

Then the transmit gains' Bluetooth side, package 12:
the library's `bt_get_tx_gain_new`, `bt_get_tx_tab`, `bt_set_tx_gain` and `bt_tx_gain_init`, the driver's own --
beside the Wi-Fi side's `wifi_get_tx_tab`/`wifi_set_tx_gain` and the leaves the two share, `get_tx_gain_value_new`
and `set_tx_gain_mem`, already the driver's since packages 2b1/2b2.
`bt_get_tx_gain_new` walks sixteen subbands: a running byte from -0x60 stepping by four times the caller's
second byte, each held to [-0x3c, 0x50] through the ROM's `get_data_sat`, and the value is that plus the
caller's difference; `get_tx_gain_value_new` finds the subband and the three gains over the two fourteen-word
tables -- the library's rodata at 0x4200AE64 and 0x4200AE80, the middle buffer the library's own fourteen zero
words -- and the third, less the calibration byte, is held to [-0x3c, 0x18] and becomes the subband's byte.
`bt_get_tx_tab` builds the three buffers from `bt_correct_power` 0x124 less the gain byte 0x08 and `bt_cal_power`
0xFE, and `bt_set_tx_gain` writes them into the front end with `set_tx_gain_mem`, its `BT_TXDC` table at 0x104
and `TXIQ` 0xD0. `bt_tx_gain_init` tunes to 0x985, writes the transmit capacitor's table for index 6, runs
Bluetooth's transmit DC and power calibrations, and sets the gains for zero.
Off the board, `phy-ab.py` held the Bluetooth side to the library's: `bt_get_tx_tab` over seeds and with its
power fields poked both ways, its three buffers compared (it makes no device access of its own), `bt_set_tx_gain`
over seeds, the gain-memory writes the same at 93 events each, and `bt_tx_gain_init` with each of its two gates
clear and set and a tone walked, 110 to 1360 events, every access, `phy_param` and the buffers the same; the
Wi-Fi chain's `wifi_get_tx_tab` and `wifi_set_tx_gain` guard the shared leaves.
Four mutations were told apart: `bt_get_tx_gain_new`'s step multiplier, the gain byte, `bt_tx_gain_init`'s
frequency and `bt_set_tx_gain`'s `BT_TXDC`/`TXIQ` tables swapped.
`MAC_TX_MIN_PWR` 0x600a4400, the check package 11 left here: none of the transmit power functions -- the gains'
or Wi-Fi's `wifi_set_tx_gain` -- touches the word in any run, so the whole transmit power path the driver took
over writes only `phy_param`, the analog I2C and the front end.
The whole of `register_chipv7_phy` is the same as the library's for the same 22 runs, the log identical to the
reference and `phy_param` compared in full, and the same again with `--state phy_param`: the Bluetooth gains'
bring-up runs inside it.
The count is 70 before and 68 after, of the library's own symbols 4 before and 2 after: `bt_set_tx_gain` and
`bt_tx_gain_init` gone, `g_phyFuns` and `phy_param` left for the end of the step.
The map drops from 1026 to 1024 functions reached -- the library's `phy_tx_gain.o`, no longer linked now that
its gains are the driver's own, taking `wifi_get_target_power_new` and `get_chan_target_power_new` -- FE 86 to
85, the ranges, addresses and writes outside the frames unchanged; `phymap` is unchanged.
