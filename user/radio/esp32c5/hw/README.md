# The ESP32-C5's radio hardware

What is known of the ESP32-C5's modem, a file a subject.
The repository has no C5 radio code yet;
these files gather what Espressif's libraries say of the chip, ahead of it.
Every fact here is static: nothing ran on a C5.

| File | Subject |
|---|---|
| `phy.md` | the PHY: the band switch, the analog I2C and its blocks, the PA's current trim, per-channel quirks |
| `rftest.md` | the C5's side of Espressif's RF test firmware: its commands, its stubs, its init parameters |
| `regs.md` | every MMIO word the libraries touch with a constant address, by block, with the functions that touch it |

The ESP32-C6's files, `../../esp32c6/hw/`, hold most of what the two chips share;
these say where the C5 differs, and what only the C5 has.

## How sure a fact is

A fact says where it comes from and how sure it is, with the labels of the C6's `README.md`:
**known**, a source names it, an ESP-IDF header, a `libphy` debug string or a library function's name,
or a named function writes the value;
**inferred**, the role of a library's loop, a print string or a name says it;
**measured**, a run on the board answered it, of which there is none here;
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
