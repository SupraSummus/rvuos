# The ESP32-C5's radio

The ESP32-C5's radio programs, the way to Wi-Fi on both of its bands, 2.4 and 5 GHz.
The chip's modem is of the ESP32-C6's family, `hw/README.md` says how far,
so the programs share what the two chips' radios have in common with `user/radio/esp32c6/`,
each file parameterised by the board's `radio.h` and `devices.h` where the chips differ,
and this directory holds what is the ESP32-C5's alone.

A program is an image of its own that runs as the root task's child from the window onto flash, as on the ESP32-C6:
the root task is `user/radio/esp32c6/root.c` built against this board's headers,
the image's frame `drv.h` and `radio.c`, and its threads the adapter's, `osi.c`.
Each program is a file of `prog/` and runs with
`make BOARD=esp32c5 radio PROG=name RADIO_CONFIG=file`, the file's keys the program's own:

| Program | What it shows |
|---|---|
| `frame` | every word of the modem's frame as user mode reads it, a line a 1 KiB page |
| `phyup` | the PHY calibrated and tuned through `libphy.a`, a channel of each band in turn, `channels=` 1 and 36 by default, and the writes feigned for it |
| `listen` | the MAC brought up as the station's, its sniffer on, the frames and access points heard on each channel of `channels=`, of both bands; it sends nothing |

`delay.c` is the ROM's `ets_delay_us` over the kernel's clock,
since the ROM's counts in `mcycle`, which user mode cannot read on this chip.
`phy.c` is the PHY's half of `osi.h`, the clocks, the calibration and the channel, over `libphy.a` for now.
The library's accesses to PCR, the PMU and LP_AON, which the kernel sets at boot, the adapter feigns,
and its reads of the eFuse it answers from the words the root task copies into the page; see the board's `radio.h`.

## The way there

As on the ESP32-C6, Espressif's closed libraries start the radio, and the radio's own code takes their place step by step,
each step held to what the libraries did on the chip:

1. The PHY up on a channel of each band through `libphy.a`,
   its accesses to PCR, the PMU and LP_AON feigned for it, since the kernel sets them at boot,
   and traced through the root task as the ESP32-C6's start was.
2. The Wi-Fi MAC of `user/radio/esp32c6/`, `mac.c` and `macstart.c`, with the ESP32-C5's registers,
   which match the ESP32-C6's but for a few runs of them that moved and the MAC's timer, a block of its own further on,
   as the same functions of the two chips' `libpp.a` and ROMs show.
   It listens first: the beacons heard on a channel of each band.

## The MAC's start, held to a library that never ran

On the ESP32-C6 the own start of `macstart.c` is held, by `make mac-replay-test`, to the accesses the libraries made
in a traced run on the chip. The ESP32-C5's radio never ran them under rvuos, so its file of their accesses,
`test/replay/config.txt`, comes of running `libpp.a`'s `hal_init` itself, under unicorn: `tools/hal-emu.py`.
It links the library's function alone with the ROM, stubs what it lacks, and answers the device accesses from a model
of the registers. How far it may be trusted is measured on the ESP32-C6: run on that chip's library, given the reads
the chip gave, it makes the chip's 664 accesses, value for value, but for the receive base, which their control block's
place set; `make BOARD=esp32c5 wifi-esp32c5-replay` checks that before it writes the ESP32-C5's file again.
`macstart.c` built for the ESP32-C5, `RADIO_MAC_C5` in the board's `radio.h`, makes that file's accesses,
`make mac-replay-test-c5` in `make check`, but for interface 1's, the soft AP's, which `test/replay-expected.txt` names.
The decompiled bodies the C5's branches were read from are Ghidra's, through the scripts of `user/radio/cyw43439/tools/ghidra/`.
3. The station of `user/wifi/esp32c6/` over it, its scan, join and link, and the network process of `user/wifi/`.
4. The PHY of its own, `libphy.a`'s functions taken one by one, as `user/radio/esp32c6/phy.c` took the ESP32-C6's.

## Sending within the law

The station sends only what a station sends, from its own address, to the maintainer's own access points,
on the channels the EU lets a station use without detecting radar itself:
2.4 GHz's channels 1 to 13, and 5 GHz's 36 to 48, 5150 to 5250 MHz, indoors.
It hears the channels of DFS, 52 to 140, without sending, and nothing from 149 up.
Its power stays at or below what `libphy.a` would set for the channel and rate, 23 dBm EIRP at most on 5 GHz,
and the limits are checked in the code, not only kept by care.
