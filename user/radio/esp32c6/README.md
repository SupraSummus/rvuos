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
its `NOTES.md` tells how the radio's code came to be its own, step by step,
and `PHYREGS.md` beside it what the PHY's registers are known to be.

`test/` holds the radio's tests on the host, which `make check` runs:
the MAC's and the baseband's bring-up replayed against the accesses of Espressif's libraries,
and the sending's choices of `txctl.c`.
