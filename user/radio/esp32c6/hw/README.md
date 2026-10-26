# The ESP32-C6's radio hardware

What is known of the ESP32-C6's modem, a file a block:

| File | Block |
|---|---|
| `phy.md` | the PHY: the front end, the RF and baseband PLLs, the receive and transmit basebands, the AGC, the analog I2C's blocks, the calibrations and `phy_param` |
| `wifi-mac.md` | the Wi-Fi MAC and its timer |
| `i154.md` | the IEEE 802.15.4 MAC |
| `btbb.md` | the BLE and IEEE 802.15.4 baseband |
| `dump.md` | the dump engine, which copies what the receiver hears into SRAM |
| `rftest.md` | Espressif's RF test tool and its firmware as a source: how it loads and talks, its commands and the blocks each reaches |

A fact says where it comes from and how sure it is:
**known**, a source names it, an ESP-IDF header, a `libphy` debug string or a library function's name;
**inferred**, the role of a library's loop says it;
**measured**, a run on the board answered it, the program and its keys named;
and **unknown**, or a hypothesis, said as one.
How the radio's code came to be its own, step by step, is history, and in the station's `NOTES.md`.

## The modem's frame

The kernel gives a program the modem as one frame, 0x600A0000 to 0x600B0000.
`prog/regmap.c` reads every word of it, twice in a row, in four passes,
cold, as the program finds the modem, up, once the Wi-Fi MAC and the PHY are up on a channel,
again, a moment later, and tuned, five channels above, and the analog I2C's blocks in the last three;
`user/radio/esp32c6/tools/regmap.py` puts the passes together:

    printf 'channel=1\nrun=20\n' > local/regmap.conf
    make BOARD=esp32c6 radio PROG=regmap RADIO_CONFIG=local/regmap.conf PORT=/dev/serial/by-id/usb-Espressif_...
    user/radio/esp32c6/tools/regmap.py build/esp32c6/radio-run.log            # the blocks, and each page's time
    user/radio/esp32c6/tools/regmap.py build/esp32c6/radio-run.log words tuned  # or up, drift, moving; and i2c, dump FROM TO

What it read, measured on one board in runs on channels 1 and 6 and on 8 and 13:

| From | Block | Its date word | Repeats every | Named by |
|---|---|---|---|---|
| 0x600A0000 | FECOEX | 0x3FC, 0x02206240 | | `libphy` |
| 0x600A0400 | FEDATA | 0x7FC, 0x02207011 | | `libphy` |
| 0x600A0800 | FECTRL | 0xBFC, 0x02206021 | | `libphy` |
| 0x600A1000 | btmac | 0x1FFC, 0x02207081 | | `libphy` |
| 0x600A2000 | bt_v3_2, the BT baseband, 0x15C bytes | 0x2158, 0x02206080 | 0x200 bytes, to 0x600A2800 | `libphy` |
| 0x600A2800 | btagc | 0x28FC, 0x02201240 | 0x100, to 0x600A2C00 | `libphy` |
| 0x600A2C00 | zbbb, the 802.15.4 receiver | 0x2CF8, 0x02206170 | 0x100, to 0x600A3000 | `libphy` |
| 0x600A3000 | the 802.15.4 MAC | 0x3184, 0x00220622 | | ESP-IDF's `ieee802154_reg.h` |
| 0x600A4000 | the Wi-Fi MAC, four blocks of 1 KiB | 0x3F8 of each, 0x02207040 | | `macregs.h` |
| 0x600A5000 | the Wi-Fi MAC, its key table at 0x5800 | 0x55B0, 0x02006110 | | `macregs.h`, `mac.c` |
| 0x600A7000 | agc, bbtx, nrx, of 1 KiB each | 0x73FC, 0x77FC, 0x7BFC: 0x02206241, 0x02206201, 0x02207080 | | `libphy` |
| 0x600A7C00 | bb | none found | | `libphy` |
| 0x600A8000 | brx | 0x8384, 0x02003181 | | `libphy` |
| 0x600A8800 | the modem's ETM | 0x88A0, 0x20220524 | 0x100, to 0x600A9000 | ESP-IDF's `ieee802154_reg.h`, `dump.md` |
| 0x600A9000 | the dump engine | 0x9020, 0x02202221 | 0x40, to 0x600A9400 | `dump.md` |
| 0x600A9400 | not known: one word, 0x000F83FC, at its start | 0x94CC, 0x02206100 | 0x100, to 0x600A9800 | |
| 0x600A9800 | MODEM_SYSCON | 0x9824, 0x02206300 | 0x40, to 0x600A9C00 | ESP-IDF's `modem_syscon_reg.h` |
| 0x600AD000 | the Wi-Fi MAC's timer, a block of 1 KiB | 0x3F8, 0x02207040 | 0x400, to 0x600AE000 | `mac.c` |
| 0x600AF000 | MODEM_LPCON | 0xF02C, 0x02206240 | 0x40, to 0x600AF400 | ESP-IDF's `modem_lpcon_reg.h` |
| 0x600AF400 | not named: a few words from 0xF49C, zero before the bring-up; two of its bits select the protocol of a forced radio, below | 0xF4E4, 0x02203141 | 0x100, to 0x600AF800 | `rftest.md`'s firmware |
| 0x600AF800 | i2c_mst, the analog I2C's master | 0xF834, 0x02201300 | 0x40, to 0x600AFC00 | `libphy`, `phy.c` |
| 0x600AFC00 | i2c_mst's command memory | | 0x100, to 0x600B0000 | `libphy`, `phy.c` |

- **The date words.** ESP-IDF's headers name a block's word DATE at MODEM_SYSCON's 0x24, MODEM_LPCON's 0x2C
  and the 802.15.4 MAC's 0x184, and each of the others reads as a date of 2020 to 2022, 0x0YYMMDDn,
  at or near its block's end; so a date word marks a block and its extent, as a hypothesis.
  Two blocks no source names show so, at 0x600A9400 and 0x600AF400.
- **The block at 0x600AF400.** No source names it, but Espressif's RF test firmware, see `rftest.md`, writes two of its bits (known, the functions named):
  - `0x600AF4B4` bit 0: librftest's `force_rx_gain` and `force_txon_mode` write it to the AND of their Bluetooth and force arguments, after `phy_dis_hw_set_freq_1`.
    So it selects Bluetooth's or 802.15.4's side over Wi-Fi's for a forced receive gain or transmit (inferred).
  - `0x600AF4B8` bit 0: the image's `zb_tx` sets it for its whole run and clears it at the end, inside the modem clock mode 3 of `rftest.md`.
    What it gates is not known.
  Nothing was run that touches either.
- **The copies.** A block that decodes fewer of the address's bits than its window spans repeats in it,
  and a read of a copy answers the block's word; whether a write to a copy reaches the block is not tried.
- **Nothing there.** 0x600AA000 to 0x600AD000 reads zero, each read answered at once;
  0x600AE000 to 0x600AF000 reads zero, each read only after about 0.9 ms, the bus giving up on it,
  so a program that reads it waits.
- **Before the bring-up.** The timer's block, i2c_mst and the block at 0x600AF400 read zero until the clocks are on,
  and the page from 0x600AF000 took 0.45 s to read, where it takes 1 ms once they are, some of its reads waiting;
  every other block reads what the image before left there:
  the board's reset does not reset the modem's words, and the second run's cold pass read the channel the first ended on.
- **The modem's resets.** ESP-IDF's restart pulses `MODEM_SYSCON`'s `MODEM_RST_CONF` 0x600A9810, all ones then 0,
  and `MODEM_LPCON`'s `RST_CONF` 0x600AF024, 0xF then 0, `modem_syscon_ll_reset_all` and `modem_lpcon_ll_reset_all`,
  which esp-sdr-zephyr does at every start against what an image before left.
  Pulsed before the bring-up (measured, `prog/txdc.c` with `modemreset=1` and `modemreset=2`, after an 802.15.4 run),
  it put back the Wi-Fi MAC's 0x600A4400, 0xc1cb3754 to 0xc1c93735, the 802.15.4 MAC's 0x600A3004, 0x02080080 to 0x12080002,
  `FEDATA` 0x600A0450 to 0 and 0x600A0414's 0xc8007 to 0xc8003,
  and left the dump block's words 0x9004, 0x900C and 0x9010 as written, and the AGC's 0x600A702C;
  the bring-up after it went as without, the loopback's estimate the same.
  So the pulse clears what 802.15.4 leaves in the MACs, but not a player left playing, which `dump_known` stops.
- **What moves.** Between two reads in a row, 0x600A4E4C's low 12 bits, and 0x600A4E54 once;
  between two passes, the counters of the Wi-Fi MAC, the 802.15.4 MAC's 0x600A30BC, and words of the receiver's.
  What the channel sets is in `phy.md`, "The channel's words, measured".

## The analog I2C's blocks

The ROM's `rom_i2c_readReg` reads a block's registers, a byte each, once the PHY's bring-up has powered the bus;
`libphy`'s `phy_i2c_check` names the blocks, and `phy.md`, "The analog I2C's blocks", lists them with their hosts.
`user/radio/esp32c6/tools/regmap.py LOG i2c` tells their first 32 registers in the passes after the bring-up;
those of 0x62, 0x63 and 0x6B that the channel moves are in `phy.md`.
