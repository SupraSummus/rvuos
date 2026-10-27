# The ESP32-C5's radio hardware

What is known of the ESP32-C5's modem, a file a subject.
`phy.md`, `rftest.md` and `regs.md` gather what Espressif's libraries say of the chip, all of it static;
this file's sections from "The modem's frame, measured" on say what runs on the board answered.

| File | Subject |
|---|---|
| `phy.md` | the PHY: the band switch, the analog I2C and its blocks, the PA's current trim, per-channel quirks |
| `dump.md` | the dump engine as the C5's `adctrig` drives it, the DPD path's loopback, the FFT and EVM dump, and no player |
| `rftest.md` | the C5's side of Espressif's RF test firmware: its commands, its stubs, its init parameters |
| `regs.md` | every MMIO word the libraries touch with a constant address, by block, with the functions that touch it |

The ESP32-C6's files, `../../esp32c6/hw/`, hold most of what the two chips share;
these say where the C5 differs, and what only the C5 has.

## How sure a fact is

A fact says where it comes from and how sure it is, with the labels of the C6's `README.md`:
**known**, a source names it, an ESP-IDF header, a `libphy` debug string or a library function's name,
or a named function writes the value;
**inferred**, the role of a library's loop, a print string or a name says it;
**measured**, a run on the board answered it;
and **unknown**, or a hypothesis, said as one.
A value said to be written by a function is that function's, read from its code;
what the value means is **inferred** unless a source names it.

## Sources

- esp-phy-lib, commit 20f1db053a0e6cb9f1c09d255c43bf42483041d0, `esp32c5/`:
  - `librftest.a`, sha256 0f9680b41612762d854accf2334412e3e01b206a3ec290bbbb232842fdaec7ba,
    the RF test firmware's command handlers, its `print_ver` "1.9, Mar 10 2026";
  - `libphy.a`, sha256 dbf33c418c8d408d4005c849d12a1432deea82e2e5e57de3c8ddf914d104fffb, the PHY, which names itself "chip752";
  - `libbtbb.a`, sha256 9cbaf5bce18e6190dfab3213d2ecc65e2a5dd8bbde48bf5218dbbf47cc23da11, the BLE and 802.15.4 baseband.
  All three keep their symbols.
- The ROM ELFs `esp32c5_rev0_rom.elf` and `esp32c5_rev100_rom.elf` of esp-rom-elfs' release 20260528,
  and ESP-IDF's `esp32c5.rom.phy.ld`:
  the C5's ROM exports three PHY symbols and the table `phy_rom_phyFuns`, so, unlike the C6's,
  the PHY's register work and the analog I2C master's code are all in `libphy.a`.
- ESP-IDF's C5 `reg_base.h`, `modem/reg_base.h`, `modem_syscon_reg.h`, `modem_lpcon_reg.h`, `i2c_ana_mst_reg.h`,
  and its `esp_efuse_table.csv`, which names the PA trim's eFuse fields.
- Espressif's `EspRFTestTool_v5.2_Manual.zip`, https://dl.espressif.com/RF/EspRFTestTool_v5.2_Manual.zip,
  sha256 0b836ca247076ff98d12a00c432398c2b179300535f4897963e4384d01d53142:
  the C5's init-parameter spreadsheet and the host tool's C5 paths.

How they were read:
every object was disassembled with `llvm-objdump -dr`,
the relocations resolved to strings and data and the `lui`/`addi` pairs to absolute addresses;
then every function was decompiled to pseudo-C with Ghidra 11.4.2's headless analyser,
and the two were held to each other, address by address, in `regs.md`.
What Ghidra adds and what it drops is said there.

## The modem's frame

The C5's modem keeps the C6's base, 0x600A0000, and most of its layout;
`../../esp32c6/hw/README.md` has the C6's table, which holds where nothing here says otherwise.
`libphy.a`'s `phy_reg_check` dumps the blocks by name, `pmu`, `i2c_mst`, `FECTRL`, `FECOEX`, `FEDATA`, `FEDATA_WIFI`, `FEDATA_BLE`, `PCR`,
`MODEM_SYSCON`, `MODEM_LPCON`, `agc`, `bb`, `bbtx`, `brx`, `nrx`, `btagc`, `bt_v3_2`, `btmac`, `apb` and `i2c_ana`,
each over a range its loop bounds, which on the C6 ends at about the block's last register (**known** names, **inferred** extents).

| Block | C5, words dumped | C6, words dumped | What changed |
|---|---|---|---|
| FECOEX | 0x600A0000–0x0054 | 0x600A0000–0x00E4 | shrunk; its words move, the table below |
| FEDATA | 0x600A0400–0x0488 | 0x600A0400–0x04A0 | the tone, gain and IQ estimator's words move down |
| FECTRL | 0x600A0800–0x08E0 | 0x600A0800–0x09C8 | shrunk; its words move |
| FEDATA_WIFI | 0x600A0C00–0x0C54 | none | **new**: the per-band IQ swap at 0x0C08, a tone word at 0x0C04 |
| FEDATA_BLE | 0x600A0C00–0x0C1C | none | **new** name; its loop starts at the same 0x600A0C00 |
| btmac | 0x600A1000–0x1B2C, and 0x1FF0 | 0x600A1000–0x1B2C | one word more |
| bt_v3_2 | 0x600A2000–0x21FC | 0x600A2000–0x2154 | grown by 0xA8 bytes; 0x600A2170 is BLE's frequency offset (`ble_freq_offset`) |
| btagc | 0x600A2800–0x2908 | 0x600A2800–0x28F8 | 4 words more |
| zbbb, the 802.15.4 MAC | not dumped | 0x600A2C00, 0x600A3000 | still there: `zb_rx_v0_0_set` writes 0x600A2C0C–0x2C34, `ieee802154_txon_delay_set` 0x600A3104–0x3114 |
| Wi-Fi MAC | 0x600A4000–0x5FFF | the same | a TX queue's PLCP1 group is 0x78 bytes, the C6's 0x74, the table below |
| agc | 0x600A7000–0x71F4 | 0x600A7000–0x71E0 | 5 words more |
| bbtx | 0x600A7400–0x744C | 0x600A7400–0x7438 | 5 words more |
| nrx | 0x600A7800–0x7A30 | 0x600A7800–0x7A28 | 2 words more; 0x600A7848 is the band word, `phy.md` |
| bb | 0x600A7C00–0x7CF8 | 0x600A7C00–0x7CEC | 3 words more |
| brx | 0x600A8000–0x8098 | the same | |
| the dump engine | 0x600A9000 | the same | `adctrig` writes 0x9004, 0x9008 and 0x9018, the C6's 0x9014 |
| MODEM_SYSCON | **0x600A9C00**–0x9C28 | 0x600A9800–0x9824 | moved by 0x400, the table below |
| the Wi-Fi MAC's timer | **0x600AD800** | 0x600AD000 | moved by 0x800; every microsecond read moves with it |
| MODEM_LPCON | 0x600AF000–0xF04C | 0x600AF000–0xF02C | grown; the XTAL's frequency is read at 0x600AF028, TICK_CONF, where the C6 reads PCR 0x60096130 |
| the block at 0x600AF400 | 0xF400–0xF40C, 0xF480 (`coex_force_bt`), 0xF4B4 | 0xF4B4 | the same unnamed block |
| i2c_mst | 0x600AF800–0xF834, its command memory 0xFC00–0xFCF4 | the same | `phy_i2c_master_mem_txcap` writes 0xFC08, the C6's 0xFC04 |
| PMU | 0x600B0000–0x01A8 | 0x600B0000–0x01A0 | the RF power-up word 0x600B0154 becomes 0x600B0158 (`phy_xpd_rf`) |
| LP_I2C_ANA_MST, LP_ANA, LP_AON, LP_APM, I2C0 | 0x600B2400–0x2414, 0x600B2C00–0x2C2C, 0x600B1000–0x107C, 0x600B3800–0x38EC, 0x60004000–0x4080 | none | dumped on the C5 alone, under `i2c_ana` |

**MODEM_SYSCON** (ESP-IDF's headers, **known**):
on the C5, CLK_CONF1_FORCE_ON is gone, WIFI_BB_CFG moves from +0x1C to +0x18, FE_CFG is new at +0x1C,
MEM_RF1_CONF and MEM_RF2_CONF replace MEM_CONF, and DATE is at +0x28;
so the C6's 0x600A981C, which `phy_wifi_enable_set` writes, is the C5's 0x600A9C18.
**MODEM_LPCON** grows to DATE at +0x4C, MODEM_SRC_CLK_CONF where the C6 has I2C_MST_CLK_CONF at +0x10.

The front end's words that moved, each a pair a function of both chips' libraries touches (**known**, both pseudo-C and disassembly):

| Word | Function | C6 | C5 |
|---|---|---|---|
| the BT channel | `bt_set_chn`, `phy_dis_hw_set_freq_new` | 0x600A00C0 | 0x600A001C |
| coex PTI | `coex_pti_v2` | 0x600A0018 | 0x600A0010 |
| the digital TX gain | `force_dig_gain`, `set_tx_pwr_index` | 0x600A0410 | 0x600A0408 |
| the tone generator | `start_tx_tone` | 0x600A0420 | 0x600A041C |
| the IQ estimator | `phy_corr_get_pwr`, `ram_get_corr_power` | 0x600A0478–0x0490 | 0x600A0454–0x046C |
| the RX DC estimate | `dc_iq_est_test` | 0x600A0488–0x0498 | 0x600A0464–0x0474 |
| the TX rate | `phy_get_tx_rate` | 0x600A0814 | 0x600A080C |
| BLE TX's check | `bt_bb_v2_init_cmplx`, `ble_tx_config_check` | 0x600A08FC | 0x600A0870 |
| TX on | `txon_set`, `force_txon` | 0x600A0910 | 0x600A0890 |
| the dump mode | `set_dump_mode` | 0x600A0958 | 0x600A08CC |
| the memory test word | `mem_read_func`, `mem_write_func` | 0x600A0BFC | 0x600A08DC |
| WIFI_BB_CFG | `phy_wifi_enable_set`, `phy_xpd_rf` | 0x600A981C | 0x600A9C18 |
| the RF power-up | `phy_xpd_rf` | 0x600B0154 | 0x600B0158 |

**The Wi-Fi MAC's TX queues** (**known**, `mac_common.o`'s tables, which `Plcp1AddrGet` and its siblings index by queue):
the C5 adds two words, VHT-SIG1 and VHT-SIG2, after HT-SIG,
so a queue's group of PLCP1, threshold, HT-SIG, VHT-SIG1/2, HE-SIG1/2, packet count and HT40 length is 0x78 bytes, the C6's 0x74.
Queues 4 to 10 share one group, at 0x600A54B0 on the C5 and 0x600A5488 on the C6:

| Word | C6 | C5 |
|---|---|---|
| PLCP1 | 0x600A5488 | 0x600A54B0 |
| threshold | 0x600A548C | 0x600A54B4 |
| HT-SIG | 0x600A5494 | 0x600A54C0 |
| VHT-SIG1, VHT-SIG2 | none | 0x600A54C4, 0x600A54C8 |
| HE-SIG1, HE-SIG2 | 0x600A54A0, 0x600A54A4 | 0x600A54CC, 0x600A54D0 |
| packet count | 0x600A54B0 | 0x600A54DC |
| HT40 length | 0x600A54B8 | 0x600A54E4 |
| duration, queue 0 | 0x600A544C | 0x600A5474 |

The other queue words, PLCP0 and the CONF words at 0x600A4CB0–0x600A4D6C, are where the C6 has them.
So are the Wi-Fi MAC's other words, the BT AGC's, the 802.15.4 baseband's, and the PHY's CCA, noise-floor and RSSI words:
70 functions present in both chips' libraries touch the same addresses.

## The modem's frame, measured

The kernel gives a program the modem as one frame, 0x600A0000 to 0x600B0000, as on the ESP32-C6.
Each block ends, or repeats, with a word that reads as a date, 0x0YYMMDDn, as the ESP32-C6's do;
on the ESP32-C5 most are of 2024, where the ESP32-C6's are of 2020 to 2022.
Measured on one board, ESP32-C5HF4 v1.0, by a probe in machine mode with the modem's clocks forced on,
and again by `prog/frame.c` from user mode, which found the same words in each block the boot leaves clocked:

| From | Block | Date word, at | Repeats every | As on the ESP32-C6 |
|---|---|---|---|---|
| 0x600A0000 | FECOEX | 0x02411050, 0x54 | 0x80 | the C6's date is at 0x3FC |
| 0x600A0400 | FEDATA | 0x02411280, 0x88 | 0x100 | moved |
| 0x600A0800 | FECTRL | 0x02412070, 0xE0 | 0x100, to 0x600A0C00 | moved |
| 0x600A0C00 | not known, new | 0x02411280, 0x54 | 0x80 | none there |
| 0x600A1000 | btmac | 0x02408090, 0x1FFC | | the same place |
| 0x600A2000 | the BT baseband | 0x02412050, 0x1FC | 0x200, to 0x600A2800 | the same place |
| 0x600A2800 | btagc | 0x02401220, 0x108 | 0x200 | the same place |
| 0x600A2C00 | zbbb, the 802.15.4 receiver | 0x02411070, 0xF8 | 0x100 | the same place |
| 0x600A3000 | the 802.15.4 MAC | 0x00240425, 0x18C | | the C6's at 0x184 |
| 0x600A4000 | the Wi-Fi MAC, four blocks of 1 KiB | 0x024112xx and 0x02412040, 0x3F8 of each | | the same place |
| 0x600A5000 | 5 KiB of words, all other than zero, read only with the clocks on | | | the C6's key table is at 0x5800 |
| 0x600A7000 | agc, bbtx, nrx and bb, of 1 KiB each | 0x024112x0 and 0x12410261, 0x3FC of each | | bb has a date now |
| 0x600A8000 | brx | 0x02305101, 0x384 | | the same place |
| 0x600A8400 | not known, new | 0x02210310, 0x154 | 0x200 | none there |
| 0x600A8800 | the modem's ETM | 0x20231228, 0xA0 | 0x100 | the same place |
| 0x600A9000 | the dump engine | 0x02202221, 0x1C | 0x20, to 0x600A9800 | the C6's repeats every 0x40 |
| 0x600A9800 | not known | 0x02401090, 0xCC | 0x100 | the C6's is at 0x600A9400 |
| 0x600A9C00 | MODEM_SYSCON | 0x02410160, 0x28 | 0x40 | the C6's is at 0x600A9800; ESP-IDF's `modem/reg_base.h` |
| 0x600AD000 | the MAC's power and timers | 0x02208080, 0x6C | 0x80, to 0x600AD800 | |
| 0x600AD800 | the Wi-Fi MAC's timer, two blocks of 1 KiB | 0x02411280, 0x3F8 of each | | the C6's at 0x600AD000 |
| 0x600AF000 | MODEM_LPCON | 0x02410170, 0x4C | 0x80 | the C6's date at 0x2C; ESP-IDF's `modem/reg_base.h` |
| 0x600AF400 | not known | 0x02203141, 0xE4 | 0x100 | the same place and date |
| 0x600AF800 | i2c_mst, the analog I2C's master | 0x02411290, 0x3C | 0x40 | the C6's date at 0x34 |
| 0x600AFC00 | i2c_mst's command memory | | | the same place |

- **Before the clocks.** Read cold, the blocks of 0x600A2000 to 0x600A3000, 0x600A5000 to 0x600A6400,
  0x600A8000, 0x600AD000 to 0x600AD800 and 0x600AFC00 each took 33.6 million cycles a KiB, about 0.68 ms a word,
  the bus giving up on them, and read zero.
  A reset of the chip leaves the modem's clocks as the image before left them, so a run after the probe found them on.
- **The empty.** 0x600AA000 to 0x600AD000 and 0x600AE000 to 0x600AF000 read zero, answered at once.

## The Wi-Fi MAC beside the ESP32-C6's

Inferred from the two chips' `libpp.a` and ROMs, the functions both have, each one's accesses to the MAC taken in order
and paired with the other chip's:

- Most of the MAC's registers the ESP32-C6's driver names lie where they do on the ESP32-C6: 98 of the 152 its image reaches.
- A run of them lies one word further on, from the C6's 0x600A40F8 to 0x600A416C, at 0x600A40FC to 0x600A4170,
  and another three words further, the C6's 0x600A4440 to 0x600A4458 at 0x600A444C to 0x600A4464.
- The MAC's timer block, the C6's 0x600AD000, lies at 0x600AD800, most words at the same offsets.
- The rest are fewer than twenty words, each to be found by the same pairing before the MAC's start uses it.
- The timer block's words the driver uses: the TSF's control at 0x600AD858, timer 30 at 0x600AD830,
  the slow clock's cycle at 0x600AD878, and the power status and its clear at 0x600AD8B8 and 0x600AD8BC.
- A TX slot's busy bits are 0xE000, and the sniffer's filter of other frames sits eight bits higher than the ESP32-C6's.
- The interrupt's words are the enable at 0x600A4C40, the MAC's enable word as on the ESP32-C6, the raw at 0x4C44 and the status at 0x4C48.
- The control header the MAC writes before a received frame is 64 bytes, the MAC's version 3, ESP-IDF's `esp_wifi_rxctrl_t`:
  the signal's length at byte 56 and the state at 60, where the ESP32-C6's header is shorter;
  measured too, as `prog/listen.c` reads beacons whole by it.
- `chm_set_current_band` writes the low byte of 0x600A4C94, 0x20 for 2.4 GHz and 0x17 for 5 GHz, as `mac_channel` does.
- A TX slot's PPDU block is 0x78 bytes, slot 0's at 0x600A54B0, PLCP1 first and the protection word next,
  as on the ESP32-C6; then a word more, so the power, the length, the response and the end lie 4 bytes further on:
  slot 0's at 0x600A54D8, 0x54E4, 0x54E8 and 0x5514, read from the ROM's `mac_tx_set_len` and `hal_mac_get_txq_complete`,
  and measured, as 1 Mb/s frames are acknowledged.
- The modem's clock gating code while the PMU is active, `PMU_HP_ACTIVE_ICG_MODEM` 0x600B000C bits 31:30,
  powers on as 0, which no clock domain's map holds, where ESP-IDF's bootloader sets 2 at every boot (**measured**):
  with 0 the power detector's SAR reads zero, at 0x600A081C to 0x600A0828, so `register_chipv7_phy` never returns;
  with 2, set and updated through the PMU's `IMM_MODEM_ICG` and `IMM_SLEEP_SYSCLK`,
  the calibration ends and OFDM's frames go out and are acknowledged on both bands.
  The PMU keeps the code through a reset, so a chip an ESP-IDF image ran on since its power came on calibrates without it,
  as the board did after MicroPython's runs; those runs sent no OFDM frame, under a code that was not read.
  The kernel sets 2 at boot.
- The baseband's and the front end's words outlive a reset of the chip too (**measured**, `prog/listen.c` dumping
  the modem's frame at its start: some 2500 words not zero). Calibrated on what a run before left,
  `libphy.a` left the receiver deaf on a band, a channel's beacons unheard for the whole run, in most runs after
  one that heard, the AGC's 0x600A708C, low bits 3 or 0, and 0x600A7138, bits 13:12, telling the two apart at the start;
  the pbus's readback, 0x600A0894 and 0x600A08A0, then missed bits a run that heard had.
  With WIFIBB and the front end's five resets, MODEM_RST_CONF bits 8 and 10 to 14, pulsed before the calibration,
  every run heard, `phy.c`.
- The responses' power words, 0x600A4408 on, which `hal_init_imrsp_power` fills from `phy_get_max_pwr`'s table,
  are the band's the table was asked on: `chm_set_current_band` asks it again through `hal_init_tx_pwr` as the band changes.
  Asked on 2.4 GHz at the start, bits 8 to 13 held 0x14 on channel 40, where `libphy.a` gives every OFDM rate 0x12
  or less there (**measured**, the words and the table read on the board); `mac_channel` asks again as the band changes.
- The access point's RTS frames are answered: an ESP32-C6 listening beside it heard the C5's CTS a SIFS after each,
  and the access point's frame after it acknowledged (**measured**, on 2.4 GHz).
- On 5 GHz the board misses the first try of many of the MikroTik access point's frames at 36 Mb/s and above,
  the more the longer the frame, and hardly any at 24 Mb/s (**measured**).
  It hears nothing of a missed try, no frame with a bad checksum, and the retry, behind RTS and CTS, comes whole.
  ESP-IDF 5.5.4's own stack on the board, joined as an 802.11a station as the driver is, misses them alike,
  so the driver does not cause it.
  Neither the channel nor the level moves it: channels 40 and 44, -66 and -82 dBm, alike.
  A phone's hotspot on channel 149 was heard whole at 48 and 54 Mb/s, at -52 and at -79 dBm,
  so the loss lies between this access point's higher rates and the board's receiver;
  the access point sending from both its antennas is a hypothesis.
  Bits 15 to 29 of the reception header's word 4, a 15-bit signed value, read about -900 from the access point
  and about +700 from the phone, which lost nothing,
  so the frequency offset libphy's `phy_get_rx_freq` reads in such a value does not explain it.

## The PHY on libphy.a

Measured with `prog/phyup.c`, `libphy.a` of version `108,cdfe9643` (Dec 2 2025),
esp-phy-lib's commit 3d57415a, which `tools/esp-fetch.py` fetches for the code;
`phy.md` reads another, of the commit 20f1db05, whose band byte turns at 4000 MHz where this one's turns at 3001:

- Its full calibration, `register_chipv7_phy`, returns 0 after 1.17 s, most of it the kernel's report of each feigned access.
- It tunes 2412 MHz, channel 1, in 2.2 ms, and 5180 MHz, channel 36, in 2.7 ms, `phy_change_channel` taking the centre in MHz;
  `phy_set_chanfreq` takes 3001 MHz and up for the 5 GHz band, a flag in `phy_param`.
- The writes it makes outside the frames, feigned, in order: `PMU_RF_PWC` to 0xF3000000, 0xFB000000, 0xFF000000 and 0xFF800000,
  the last `XPD_CKGEN5G`; `IMM_HP_CK_POWER` to 0x10000000; PCR's `SARADC_CONF` to 4 then 5, `TSENS_CLK_CONF` to 0x00500000,
  and the reserved word at 0x6009614C to 0x0A; and `LP_AON_SAR_CCT`'s top three bits between 4 and 2, ending at 2,
  where the kernel sets 4, as the ESP32-C6's ends; which the power detector's calibration measures with is open.
- It reads the eFuse's words from 0x600B4844 to 0x600B4854, the MAC address and the PA's DC calibration, `phy_pa_dc_code_set`.

## The ROM

The board's ROM is the v1.0 revision's, `esp32c5_rev100_rom.elf` of Espressif's ROM ELFs,
whose code matched the board's byte for byte where it was read back; ESP-IDF's ROM linker scripts of v5.5.4 match it,
but for `esp32c5.rom.eco3.ld`, a later revision's.
Its `ets_delay_us` counts in `mcycle`, which user mode cannot read, so a program has its own, `delay.c`.
With `mcounteren` set, user mode reads `cycle` and `time`, measured by a probe in machine mode; rvuos sets neither.

## The SPLL

Measured with a probe in machine mode and the kernel's boot, which says the clock it found and the one it set:
the SPLL's dividers, in the analog block 0x66 that the modem's I2C master reaches by its second command word,
keep what was set before through a reset and the ROM's boot from flash.
The board's boots found them as for a 40 MHz crystal, the reference divided by 12, so the SPLL ran at 576 MHz
on the board's 48 MHz crystal and PLL_F160M at 192; ESP-IDF's settings for 48 MHz, which the ROM's download mode leaves, give 480.
The master's words read zero and take no write until its clock is forced on, MODEM_LPCON's 0x01C bit 2,
whatever its enable and its gating maps say; the kernel forces it for the change, see `kernel/board/esp32c5/timer.c`.

## The modem's power block behind a TEE word of its own

The TEE controller's HP permission words run past the last its register header names, the bit scrambler's at 0x60098140,
to the SLC host's at 0x60098158, as ESP-IDF's `soc/apm_defs.h` numbers them.
The modem's power, `APM_TEE_HP_PERIPH_MODEM_PWR` at 0x60098148, covers MODEM_PWR0 and MODEM_PWR1:
the MAC's power and timer block at 0x600AD000, and MODEM_LPCON with the analog I2C master at 0x600AF000.
Measured: left at its reset value, TEE mode alone, the word let machine mode read LPCON's date, 0x02410170,
and user mode read zero there, with no fault, its writes lost.
So `libphy.a` calibrated without reaching an analog block, and the MAC heard nothing;
with the word granted, as the kernel now does, `prog/listen.c` hears what a computer beside the board hears, on both bands.

The words 0x600A0894 to 0x600A08A8 are the pbus's readback, three nine-bit fields a word,
whose address and shift `phy_pbus_rd_addr` gives for each of its eleven selectors.
