# The ESP32-C6's radio

This directory holds what the ESP32-C6's radio programs share:
the root task that runs them, the page it gives each of them, `drv.h`,
the adapter their threads run on, `osi.c`,
and the radio's own code, with no protocol above it:
the PHY of `phy.c`, the baseband of IEEE 802.15.4 and BLE of `btbb.c`,
the Wi-Fi MAC of `mac.c` and `macstart.c`, the IEEE 802.15.4 MAC of `i154.c`,
and the modem's dump engine, `dump.c`.

A program is an image of its own that runs as the root task's child from the window onto flash.
The Wi-Fi station of `user/wifi/esp32c6/` is one;
its `NOTES.md` tells how the radio's code came to be its own, step by step.
What is known of the hardware, block by block, is in `hw/`, whose `README.md` maps the modem's frame.

The other programs are the files of `prog/`, each an image of its own, and each runs with
`make BOARD=esp32c6 radio PROG=name RADIO_CONFIG=file`, the file's keys the program's own:

| Program | What it shows |
|---|---|
| `sampler` | what the receiver hears on a Wi-Fi channel, a capture a second through the modem's dump engine |
| `i154` | IEEE 802.15.4's frames exchanged with the nRF52840's `user/thread/`, which `tools/thread-pair.sh` runs |
| `i154listen` | everything heard on an 802.15.4 channel, and the BLE baseband's side of the dump engine beside it |
| `regmap` | every word of the modem's frame and the analog I2C's registers, cold, up, a moment later and on another channel, which `tools/regmap.py` reads |

The root task reads what a program needs from its image's header, `DRV_PROGRAM` in `drv.h`,
and gives it that and nothing more; `CLAUDE.md` here says what a change to a program runs.

`test/` holds the radio's tests on the host, which `make check` runs:
the MAC's and the baseband's bring-up replayed against the accesses of Espressif's libraries,
and the sending's choices of `txctl.c`.
