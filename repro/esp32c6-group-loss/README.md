# ESP32-C6: group frames lost right after another

A reproducer for Espressif, see [esp-idf #16096](https://github.com/espressif/esp-idf/issues/16096).

After a DTIM beacon an access point sends the frames it buffered for the group back to back.
The ESP32-C6 often does not receive a frame that follows one with More Data set,
with power save off, and not even as a frame with a bad FCS.
A DHCP server's broadcast answer follows the access point's copy of the client's own broadcast in such a run,
so DHCP with the broadcast flag fails.

## Setup

- ESP32-C6FH4 (QFN32) revision v0.2, on a Seeed XIAO ESP32C6.
- arduino-esp32 3.3.11, that is ESP-IDF v5.5's libraries, FQBN `esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc`.
- MikroTik wAP ax, RouterOS 7.16.2, `wifi-qcom`, 2.4 GHz, DTIM 1, WPA2/WPA3; the station at −75 to −78 dBm.

## Running

Set `ssid`, `pass`, `bssid` and `channel` at the top of `esp32c6_group_loss.ino`,
build and flash it, and read its console.
It joins with `WIFI_PS_NONE` and sends DHCP DISCOVERs with the broadcast flag through `esp_wifi_internal_tx`,
a REQUEST for each OFFER, in three phases of 25:
A and C as they are, B with promiscuous reception logging each frame the access point sends to the group.

## What to look for

Each phase ends with a line such as `phase A: offers 3 of 25 discovers, acks 0 of 3 requests`.
In phase B a frame missing after one with `more 1` shows as a gap in `seq`:

```
air 59893987 group from this-sta len 380 seq 2420 rssi -78 format 0 rate 0 more 1
air 60098811 group from this-sta len 80 seq 2422 rssi -78 format 0 rate 0 more 0
```

In `run.log`, a run of this sketch, the frame after `more 1` is missing 46 times in 47, and after `more 0` never in 108;
no frame with a bad FCS was logged.
In other runs the loss was lower, and in some short ones there was none.
A Pico 2 W (CYW43439) joined to the same access point at the same time received every frame the C6 missed,
in runs where the C6 ran the same Wi-Fi libraries under another system.
The MAC addresses in `run.log` are replaced by names.
