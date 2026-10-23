# The CYW43439's radio

This directory holds what is ours of the CYW43439's radio, beneath the Wi-Fi system's driver in `user/wifi/`:
the chip's map, the code we run on the chip's own processors, and the host's tools for both.
What is known of the hardware, core by core, is in `hw/`, whose `README.md` lists its files and the tools.

The chip runs the vendor's firmware, which the driver loads, so unlike the ESP32-C6's radio no program here is an image of its own:
an experiment reaches the chip through the Wi-Fi system's bridge, `make BOARD=rp2350 WIFI_BRIDGE=1`, see `user/wifi/bridge.h`,
whose host side, `user/wifi/tools/cyw43bridge.py`, stays beside it.
`user/wifi/BT.md` tells how the Bluetooth core was taken over and what its experiments showed,
as `user/wifi/esp32c6/NOTES.md` tells how the ESP32-C6's radio came to be its own.

| Directory | What it holds |
|---|---|
| `hw/` | the map: the backplane's cores, the Bluetooth core's registers, D11, the 802.11 PHY and its radio, the 20692 |
| `btcore/` | our code for the Bluetooth core, a Cortex-M3: `bt-stub.S`, which the driver's `WIFI_BT` experiment uploads through `user/wifi/bt.c`, and `bt-exec.c`, the executor of `tools/cyw43-exec.py`, both placed by `bt-map.h`; and the records `bt-tx.rec` and `bt-rx.rec`, the transmitter's and the receiver's setups |
| `wlancore/` | our code for the WLAN's processor, another Cortex-M3: `iq-cap.c`, the receiver's samples of `tools/cyw43-iq.py` |
| `tools/` | the host's tools: the map read off the chip, the firmware's code in Ghidra, the Bluetooth core parked, rehosted, replayed and driven by the executor, the receiver's samples, and D11's microcode assembler |

`build.mk` builds the code for the chip's processors into `build/cyw43/`, and the tools ask make for what they load.
`make check` builds none of it.
