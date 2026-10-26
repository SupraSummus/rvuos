# Espressif's RF test tool and its firmware, as a source

Espressif's EspRFTestTool runs on a Windows host.
It drives a test firmware it loads into the chip's RAM, and that firmware programs the radio's registers directly:
no ESP-IDF, no Wi-Fi or BLE stack, only Espressif's RF test library on top of `libphy`.
What each of its commands touches is a list of register writes with a name beside them,
and so a source for the blocks' files.
This file says what the package is, how the host loads and talks to the firmware,
and which command reaches which block.
The blocks' own facts are in `phy.md`, `btbb.md`, `wifi-mac.md`, `i154.md` and `dump.md`.
A fact taken from here says it is **known** where a named Espressif function writes the value,
and **inferred** where a print string or the code's role gives the meaning, as `README.md` defines the two.
Nothing here was **measured**: no command of this firmware has run on the board.

## The package

`EspRFTestTool_v5.2_Manual.zip` holds four things:

- The host tool, a PyInstaller archive of Python 3.7 and PyQt5.
- Its test firmwares, one RAM image a chip, under `Bin/RF_TEST_BIN/`.
  The ESP32-C6's is `ESP32-C6_RFTest_V106_4cc3bb5_20250909.bin`.
- Separate firmwares for the BLE DTM over HCI (`Bin/BQB_BIN/`) and for the adaptivity tests (`Bin/Adaptive_BIN/`).
  These two are not read here.
- The PHY init data's spreadsheets and bins for the certification power limits.

The C6 image has three segments and is entered at 0x40801658:

| segment | bytes |
|---|---|
| IRAM 0x40800000 | 0x171AC |
| DRAM 0x40820000 | 0x30C |
| DRAM 0x40820310 | 0x378C |

Its version strings name four builds:

- the PHY `4cc3bb5`, of Sep 9 2025;
- `libbtbb` `a945109`;
- a BLE and 802.15.4 test, "RW V9 LE autotest", at `17a37456`;
- `rtc_lib` `b31401a9`.

The image is stripped.
Its main command dispatcher is `librftest.a`'s `run_rftest_case`, the library of the same `esp-phy-lib` commit `tools/esp-fetch.py` pins,
whose PHY is `552b044`, newer than the image's.
The two command tables match.
The image has 199 commands, and the library 203.
They differ in these:

- The library alone has `ate_rxframe`, `set_23dbm`, `step_01k` and `tx_cca_num`.
- In the image, `ate_txframe` calls `ate_txframe_dut_fb_c5`.
- In the image, `phy_cal_track` calls a function of its own.

Every handler-level register write this file and the blocks' files cite is the same in both.

## Loading the image

The host's `Download_Bin/Esp_Download_Bin.py` loads the image with its bundled esptool:

1. It opens the ROM loader with `ESP32C6ROM` at 115200 baud.
2. It resets the chip into download mode by DTR and RTS, esptool's `default_reset`.
3. It reads the MAC.
4. If the user chose a higher baud, it changes to it.
5. It runs esptool's `load_ram`, the default "RAM" mode:
   - `flash_spi_attach(0)`;
   - for each segment, `mem_begin` at the segment's address and `mem_block` in esptool's RAM blocks;
   - `mem_finish(0x40801658)`, which jumps to the entry.

Nothing is written to flash.
A "flash" mode instead writes the image at 0x0 with esptool's `write_flash` and resets the chip.
Then the host opens the same port as a console, with DTR and RTS held low, so the console does not reset the chip.

## The console

A command is one ASCII line ended by `\r`, its name and up to twenty numbers, decimal or `0x` hex,
with no checksum and no binary framing.
The image parses the line into an array of 32-bit numbers, the handler's `argN` below, and their count.
Then it runs two dispatchers in turn, each a chain of `strcmp`:

1. the main one, `run_rftest_case`, image 0x4080a15e, with 199 names;
2. the BLE and 802.15.4 one, "RW V9 LE autotest", image 0x40801840, with 15 names, see below.

The image's strings hold `cmd not exist!` and `cmd syntax error!` for a name neither knows and a line the parser refuses.

Replies are text lines; most handlers print their name and the values they set.
The host waits on a few markers:

| command | the marker or reply the host waits on |
|---|---|
| `init_print` | ends with `print end` |
| `read_flash` | ends with `FLASH READ END.` |
| `init_para_chg` | answers `name[i]=0x..`, and the host takes what follows the `=` |
| `temp_read` | answers `temp=%d, init_temp=%d` |

The Wi-Fi receive prints one line a report:
`Correct: %d Desired: %d RSSI: %d noise: %d gain: %d para1: %d para2: %d freq: %d`.
The RSSI, the noise and the gain are in tenths of a dB, and `freq` is the frequency offset in kHz.

A command that runs in a loop polls the console between its rounds, through `esp_phy_getstopcmd`:
the packet sending, the receive's count, the tones and the burn-in.
The line it reads decides what happens:

| the line | the command |
|---|---|
| `CmdStop` or `cmdstop` | stops |
| `CmdStatus` or `cmdstatus` | goes on, the poll returning 3 |
| anything else | answers `Plz run CmdStop to exit current cmd!` |

The host stops a test with `cmdstop`.

## What the host sends

The host tool's Python says what its windows send, the same for the C6 and the C5 (known, the host's code).
On its Wi-Fi tab, Start sends:

1. `cbw40m_en 1` or `0`, as the bandwidth says;
2. by the mode:
   - continuous TX: `tx_contin_en 1`, `phy_11ax_tx_set fmt pe giltf ru`, `esp_tx chan rate atten`;
   - packets: `phy_11ax_tx_set ...`, `tx_contin_en 0`, `esp_tx chan rate atten`, or with `len delay 0` for a duty of 10, 50 or 90 %;
   - receive: `phy_11ax_rx_set id` for HE-MU alone, then `esp_rx chan rate`;
   - a tone: `wifiscwout 1 chan atten`.

Stop sends `cmdstop`, then `tx_contin_en 0` after continuous TX, or `wifiscwout 0 ...` for a tone.
A rate that is not HE goes with `phy_11ax_tx_set 0 16 1 61`.

- **`chan`** is the channel's number at 2.4 GHz, 3 to 11 the primary's at 40 MHz;
  above 14 the firmware takes it as a frequency in MHz, which the C5's 5 GHz uses, see `../../esp32c5/hw/rftest.md`.
- **`atten`** is the backoff from the target power, 0 to 255 in quarter dB.
- **`esp_tx`'s six arguments** are `chan rate backoff length delay count`; a 0 takes the firmware's default, and a count of 0 sends until `cmdstop`.
- **`phy_11ax_tx_set fmt pe giltf ru`**:
  - `fmt` 0 leaves 11ax, 1 is HE-SU, 2 HE-ER, 3 HE-TB;
  - `pe` is the nominal packet extension, 0, 8 or 16 us;
  - `giltf` 0 is 1×LTF with a 0.8 us GI, 1 2×LTF and 0.8 us, 2 2×LTF and 1.6 us, 3 4×LTF and 3.2 us;
  - `ru` is HE-TB's resource unit, 0 to 68: 0 to 36 RU26, 37 to 52 RU52, 53 to 60 RU106, 61 to 64 RU242, 61 for 20 MHz.
  The host's 11ax window works out HE-SIG-A's two words but never sends them;
  the firmware builds HE-SIG-A from the rate and these four, see `wifi-mac.md`.

The rate codes, the host's `ratedic`:

| code | rate |
|---|---|
| 0 to 3 | 11b 1, 2, 5.5 and 11 Mbit/s, long preamble |
| 5 to 7 | 11b 2, 5.5 and 11 Mbit/s, short preamble |
| 8 to 15 | OFDM 48, 24, 12, 6, 54, 36, 18 and 9 Mbit/s, L-SIG's RATE field read with its first bit lowest |
| 16 to 31 | HT MCS 0 to 15, at 20 or 40 MHz as `cbw40m_en` says |
| 32 to 43 | HE MCS 0 to 11, the format as `phy_11ax_tx_set` says |
| 48 to 57 | VHT MCS 0 to 9, which the host's `RFSupportRateConfig.txt` offers the C5 at 5 GHz, MCS 0 to 7, and never the C6 |

The host's lists offer the C6 11b, 11g, HT, HE-SU MCS 0 to 9, HE-ER MCS 0 to 2 and HE-TB MCS 0 to 9 for sending, and HE-MU for receiving.
Its BLE and 802.15.4 tabs send the second dispatcher's commands, below.

## The raw access commands

These make the shipped image a register probe on the board: loaded into RAM, it reads and writes any word and any analog I2C register,
with no build of ours.
None of them was run here.

| command | what it does | source |
|---|---|---|
| `rd a`, `rd_dport a` | prints the word at `a` | inline in the dispatcher |
| `wr a v`, `wr_dport a v` | writes `v` at `a`, then prints the word | the same |
| `sm a m`, `cm a m` | sets, or clears, the bits `m` of the word at `a` | the same |
| `gb a msb lsb` | prints the bits `msb` to `lsb` of the word at `a` | the same |
| `RdMem a n` | prints the words from `a` to `a + n` | the same |
| `ri blk host reg` | `rom_i2c_readReg`, through `g_phyFuns` slot 0x50 | the slot table, see `phy.md` |
| `wi blk host reg val` | `rom_i2c_writeReg`, slot 0x58 | the same |
| `rim blk host reg msb lsb` | `rom_i2c_readReg_Mask`, slot 0x5C | the same |
| `wim blk host reg msb lsb val` | `rom_i2c_writeReg_Mask`, slot 0x60 | the same |
| `pbus row col val` | `rom_pbus_force_test`, slot 0x74 | the same |
| `pbus_rd row col` | `rom_pbus_rd`, slot 0x78 | the same |
| `pbus_debugmode`, `pbus_workmode` | the ROM's own | |
| `set_pbus_mem a b c` | librftest's `set_pbus_mem_debug` | |
| `phy_reg_check` | prints every word of `phy.md`'s windows, as `<window> 0x<offset>: 0x<value>` | the image's dump, the same windows as libphy's |
| `phy_i2c_check` | prints the analog I2C blocks of `phy.md`'s table, as `i2c_<name> %02d: 0x%x` | the same |

The tool's own command history, `manual_cmd.txt`, holds `wi 0x6d 0x0 0x7 0x9c`, then `0x9a` and `0x98`:
writes of `i2c_dig_reg` 0x6D, host 0, register 7, stepped down.
They look like a regulator's trim, as a hypothesis.

## The commands, by what they touch

`argN` is the Nth number after the name.
A "slot" is the ROM table's, `g_phyFuns`.
The registers' detail is in the file named.

| command | handler | what it touches | detail |
|---|---|---|---|
| `esp_tx chan rate backoff [len delay num]` | `esp_tx_func_org` | tunes (`rftest_set_chan`), the PA option of I2C 0x61, the power index, then `WifiTxStart`; `backoff` in quarter dB | `phy.md` "Transmit", `wifi-mac.md` |
| `WifiTxStart rate num delay cbw40 htdup discca format` | `WifiTxStart` | `bbtx` 0x7400/0x7418, the CCA, TX slot 0, `MAC_ENABLE`, `INT_CLEAR`; prints `tx_state` | `wifi-mac.md` "The test sender" |
| `FillTxPacket`, `wifitxout`, `tx_contin_en n`, `cbw40m_en n`, `tx_cbw40m_en n`, `short_gi_enable n` | `FillTxPacket`, `wifitxout_func`, or a global | `tx_cont_cfg`'s I2C 0x67 writes for continuous sending; the rest only sets the sender's globals | `phy.md` "The analog I2C's writes of the RF test library" |
| `phy_11ax_tx_set fmt pe giltf ru` | a table | only sets the HE parameters `WifiTxStart` takes | `wifi-mac.md` |
| `phy_11ax_rx_set id`, `esp_hetb_enable n` | their own | `HE_AID`, `BSSID_LO/HI`, `HE_CTRL`, `HE_MAC_DELAY0/1` | `wifi-mac.md` |
| `esp_rx chan rate`, `WifiRxStart` | `esp_rx_func`, `do_rx_poll` | tunes; clock mode 1; the MAC's receive status and the last frame's rate | `wifi-mac.md` "The test receiver" |
| `wifiscwout en chan atten` | `wifiscwout` | tunes, then slot 0x98's tone at the carrier | `phy.md` "The loopback" |
| `txtone`, `txtone_step`, `stoptone`, `force_txtone` | `start_tx_tone`, slots 0x98 and 0x9C | the tone words `FEDATA` 0x420 to 0x42C | `phy.md` "The loopback" |
| `bt_tx_tone`, `zb_tx_tone`, `phy_tx_tone` | `bt_zb_tx_tone`, `force_txon_mode` | `phy_set_freq`, the forced TX-on, a tone | `phy.md` "Transmit" |
| `force_txon_mode on bt idx` | `force_txon_mode` | `FECTRL` 0x910, `btagc` 0x28A0, 0x600AF4B4, the power index | `phy.md` "Transmit" |
| `force_rx_gain force gain bt`, `bt_rx_force` | `force_rx_gain`, `bt_rx_force` | `AGC_2` 0x702C or `btagc` 0x2840, 0x600AF4B4, `FECTRL` 0x910 | `phy.md` "The receiver's gain and filter" |
| `rx_init_gain n`, `rx_max_gain` | their own | `AGC_0` 0x713C, `AGC_1` 0x7094 | `phy.md` "Receive" |
| `set_tx_dig_gain en g` | `force_dig_gain` | `FEDATA` 0x410 | `phy.md` "Transmit" |
| `set_tx_gain`, `set_ble_tx_gain`, `target_power_backoff`, `set_tx_power`, `tx_gain_print` | `force_tx_gain` and the gain chains | the TX gain memory, `FECTRL` 0x8C8 to 0x8D4 | `phy.md` "Transmit" |
| `force_iq_set en tx gain phase` | `force_iq_set` | `FEDATA` 0x450 | `phy.md` "Corrections" |
| `set_txdc_dig i q`, `force_txdc` | `set_txdc_dig` | `FEDATA` 0x444 | `phy.md` "The DC word" |
| `rx_spur_remove f a b`, `remove_11b_4p8G_spur a b c`, `dig_11b_filter_sel n` | their own | `bb` 0x7C14, `agc` 0x71D4; `bbtx` 0x7414, 0x7418 and 0x7400 | `phy.md` "Receive" and "Transmit" |
| `check_noise_floor`, `set_noise_floor`, `read_hw_noisefloor` | their own, and the ROM's | `agc` 0x7018, 0x7050 | `phy.md` "Receive" |
| `phy_disable_cca`, `phy_enable_cca` | the ROM's | `agc` | |
| `RFChannelSel chan bw`, `set_chan`, `phy_set_freq`, `set_freq`, `set_chan_offset`, `freq_init`, `phy_force_freq_set`, `pll_cap_mem_update`, `rfpll_cal_time` | libphy's | the RF PLL, as `phy.md` has it | `phy.md` "RF PLL" |
| `txdc_cal`, `txiq_cal_init`, `rxiq_cal_init`, `bt_txiq_cal`, `bt_txdc_cal`, `tx_cap_init`, `tx_pwctrl_init`, `rc_cal`, `rxdc_cal`, `pwdet_code_cal`, `txdc_cal_pwdet*` | libphy's calibrations | as `phy.md` has them | `phy.md` "Corrections" |
| `rxiq_get_pwr`, `get_iq_est_pwr`, `dc_iq_est`, `phy_corr_pwr` | their own, between `rom_iq_est_enable` and `_disable` | the estimate's words `FEDATA` 0x478 to 0x498 | `phy.md` "Corrections" |
| `get_rfrx_sat` | `force_rx_gain`, `rfrx_sat_rst` | then reads `AGC_GAIN_SAR` 0x708C | `phy.md` "Corrections" |
| `read_sar_dout`, `get_tx_rate`, `get_sar2_vol`, `phy_get_vdd33`, `temp_read` | `pwdet_tone_test` and libphy's | `FEDATA` 0x420 bit 18, `FECTRL` 0x810, 0x814; the SAR ADC; I2C 0x69 | `phy.md` "Sensors" |
| `i2c_xpd_test n m` | its own | `PMU_RF_PWC` bits 28 to 31, then reads I2C 0x62, 0x63, 0x6B | `phy.md` "Clocks and power" |
| `xtal_freq_rx_cal a b c`, `wifi_rx_opt n` | their own, `phy_rx_opt_cfg` | I2C 0x6A, 0x6D, 0x61 | `phy.md` "The analog I2C's writes of the RF test library" |
| `loop_mode a b c` | `loop_mode_set` | slot 0x0C's loopback, the IQ correction zeroed, slot 0x90's loopback gain, `pbus_rx_dco_cal` | `phy.md` "The loopback" |
| `adctrig`, `loop_dump_test`, `print_dump_data`, `dump_pbus` | `adctrig` and its callers | the dump engine | `dump.md` |
| `fedump_wr_txmem`, `fedump_rd_txmem`, `fedump_rd_rxmem` | return at once | nothing | `dump.md` "A player" |
| `phy_set_clk_conf n`, `rftest_open_clk` | their own | `MODEM_SYSCON`, `MODEM_LPCON` | `phy.md` "Clocks and power" |
| `rftest_init`, `phy_test_init`, `phy_init`, `rf_init`, `bb_init`, `mac_init`, `phy_wakeup_init` | the bring-up | `rftest_init` below; the rest as `phy.md` and `wifi-mac.md` have them | |
| `init_para_chg wr i v`, `RF_init_sel sel addr`, `test_rf_cal_level`, `read_flash`, `write_flash`, `erase_flash` | below | the init data, the flash | below |
| `le_master_test`, `le_slave_test`, `zb_master_test`, `zb_slave_test`, `ble_rx_start` | the image's | `btmac`, `bt_v3_2`, the 802.15.4 MAC | `btbb.md`, `i154.md` |
| `usb_clk_en n` | inline | `PCR_USB_DEVICE_CONF` 0x6009608C bit 0 | |
| `cpu_reset` | its own | `LP_AON_CPUCORE0_CFG` 0x600B1038 bit 28 | |
| `gpio_output_set`, `gpio_dis_output`, `gpio_input_get`, `ESP_TEST_GPIO`, `cmdstop_gpio` | their own | `GPIO`, `IO_MUX` 0x60090004; `ESP_TEST_GPIO` clears `USB_SERIAL_JTAG` 0x6000F018's bits 9 and 14, giving the USB pins up | |
| `test_mem_time read func` | its own | reads `FE`'s last word 0x600A0BFC, or writes 0 there, in a timed loop | |
| `set_cpu_freq n` | `rtc_lib`'s | the CPU's clock | |
| `print_ver`, `init_print`, `esp_origin_mac`, `phy_i2c_check`, `phy_reg_check` | prints | | |

These take or set a global and touch no register in this build:

- `txpwr_track_en track correct print`;
- `tx_force_pwr_en`, `ht20_rx_opt`, `ble_rx_opt_en`, `wifi_rx_opt_en`, `freq_rx_cal_en`;
- `pll_cap_track_en`, `pll_cap_print_en`, `bt_pll_track_en`, `rssi_min_max_print`, `get_rfcal_num`.

These answer and do nothing more:

- `close_peri`, `clk10m_enable`, `uart1_sel`, `rx_gain_comp`.

The library's `rftest_init` brings the radio up and writes these words:

- `PCR_MODEM_APB_CONF` 0x60096108 bit 0 set;
- `MODEM_SYSCON` `CLK_CONF` 0x600A9804 all ones and `CLK_CONF1` 0x600A9814 0x7FFFF;
- `MODEM_LPCON_I2C_MST_CLK_CONF` 0x600AF010 bit 0 set;
- `HP_APM_FUNC_CTRL` 0x600990C4 bits 0 to 3 cleared.

It then runs `phy_init`, `mac_init` and `crypto_disable`, which writes the Wi-Fi MAC's `KEY_CFG0` and `KEY_VALID` 0.
The flash's three commands set `PCR_MSPI_CONF` 0x60096018 to 5 around the ROM's SPI flash calls, and then restore it.

## The init data

The PHY's init data is the 128 bytes `phy.md` spreads into `phy_param`, "The init data's spread into `phy_param`".
The host and the firmware share their layout.
On flash, a bin is 144 bytes, the 128 between two copies of the magic `PHYINIT\0`, so byte `i` lies at the bin's offset 8 + `i`.
The tool's spreadsheet, `phy_C6_init_param_v10.xls`, names the bytes:

| byte | what | unit |
|---|---|---|
| 0 | `param_ver_id`, 10 | |
| 1 | `crystal_select`, 0 for 40 MHz | |
| 2 to 19 | the target power of each rate group, in this order: 1/2M, 5.5/11M, 6/9M, 12/18M, 24/36M, 48/54M; HT20 and HE20 MCS0/1, 2/3, 4/5, 6/7; HE20 MCS8, 9, 10, 11; HT40 MCS0/1, 2/3, 4/5, 6/7 | quarter dBm |
| 20 to 23 | reserved | |
| 24 | `fcc_enable`; the tool's generator writes 1, or 3 for HE | |
| 25 to 38 | 11b's limit, channels 1 to 14 | quarter dB |
| 39 to 52 | 11g's limit, channels 1 to 14 | quarter dB |
| 53 to 66 | HT20's limit, channels 1 to 14 | quarter dB |
| 67 to 75 | HT40's limit, channels 3 to 11 | quarter dB |
| 76 to 80 | a receive spur's notch: `spur_freq_cfg_msb`, `spur_freq_cfg`, `spur_freq_cfg_div`, `spur_freq_en_h`, `spur_freq_en_l`; the frequency cfg over div, a bit a channel | |
| 81 | `force_freq_offset_enable` | |
| 82 | `force_freq_offset_num`, signed | 8 kHz |
| 83 to 124 | reserved | |
| 125 | `multiple_bins_enable` | |
| 126 | `certification_code`: 0 default, 1 SRRC, 2 FCC, 3 CE, 4 NCC, 5 KCC, 6 MIC, 7 IC, 8 ACMA, 9 ANATEL, 10 ISED, 11 WPC, 12 OFCA, 13 IFETEL, 14 RCM | |
| 127 | a CRC-8 over bytes 0 to 126 | |

These names agree with `phy.md`'s spread.
Bytes 2 to 19 go to `phy_param` 0x52 to 0x63, the byte 24 to 0x66, bytes 0x19 to 0x42 to 0x70 to 0x99, and bytes 0x43 to 0x4B, HT40's nine, to 0x67 to 0x6F.
So `phy_param` 0x67 to 0x6F hold HT40's limits and 0x70 to 0x99 the other modes' (known for the layout, the tool's spreadsheet; the offsets, libphy's `register_chipv7_phy_init_param`).

The CRC-8 is librftest's `esp_crc8` (known, its pseudo-C): the reflected polynomial 0x8C, initial value 0, over the bytes in order.
A bin of several countries follows the first with a block of 32 bytes and then entries of 128 bytes with no magic.
The block holds two CRC-32s and the entries' count.

The commands:

- **`RF_init_sel sel addr`.**
  - `sel` 0 takes the library's built-in default and prints `esp init data is from factory bin`.
    The default holds version 10 in byte 0 and, in bytes 2 to 19, 0x54, 0x54, 0x50, 0x50, 0x50, 0x4C, 0x4C, 0x4C, 0x4C, 0x48, 0x44, 0x3C, 0x3C, 0x3C, 0x4C, 0x4C, 0x4C, 0x48; the rest is 0.
  - `sel` 1 reads the 128 bytes from flash at `addr + 8` and checks the CRC-8 against byte 127.
    It prints `init bin crc error! err_code=1` on a mismatch, and otherwise `esp init data is from flash`.
  - The tool's address, and its spreadsheets', is 0x1FC000.
- **`init_para_chg wr i v`.**
  It writes `v` into byte `i` of the firmware's copy where `wr` is 1, and reads it where `wr` is 0.
  Either way it hands the whole copy to `register_chipv7_phy_init_param` again, and prints `init_para_chg[..]=0x..`.
  The host sends a negative `v` as 65536 + `v`.
  To apply a country's limits it sends one command a byte for bytes 2 to 75, rather than asking the firmware to read the flash.
- **`read_flash`, `write_flash`, `erase_flash`.**
  Raw flash; by the image's error strings, 120 bytes a write at most, at four-byte-aligned addresses.
  `read_flash` ends its dump with `FLASH READ END.`
- **`test_rf_cal_level en addr`.**
  By the host's docstring, it takes the RF calibration from flash at `addr` where `en` is 1, and otherwise calibrates afresh.

## The BLE and 802.15.4 dispatcher

The image's second dispatcher is not in the library.
It holds `ble_rx`, `ble_tx`, `fcc_le_tx`, `fcc_le_tx_syncw`, `ble_tx_continue`, `ble_tx_continue_stop`, `rw_le_rx_per`, `rw_le_rx_per_chan`,
`zb_tx`, `zb_rx`, `le_tx_on_delay`, `zb_rx_check`, `zb_ed_cca`, `ble_tx_config_check` and `bt_tx_cca_set`.
It sends and receives BLE test packets through `btmac`'s registers, with no link layer, and 802.15.4 frames through its MAC.
The registers are in `btbb.md`, "The test's BLE engine, `btmac`", and in `i154.md`, "The RF test's 802.15.4".
The host's arguments are these:

| command | arguments |
|---|---|
| `fcc_le_tx` | `pwr chan len type phy num contin [delay hop]` |
| `fcc_le_tx_syncw` | the same, with `0x71764129` after `type` |
| `zb_tx` | `len chan pwr num contin` |
| `zb_rx` | `chan` |

In these:

- `chan` is BLE's RF channel index;
- `phy` is 0 for 1M, 1 for 2M, 2 for coded S8 and 3 for coded S2;
- `num` 0 sends forever.

## The modem's clocks by protocol

librftest's `phy_set_clk_conf(n)` writes `MODEM_SYSCON`'s `CLK_CONF` 0x600A9804 and `CLK_CONF1` 0x600A9814 for each protocol.
It is known, from its disassembly and the image's pseudo-C.
The bits are in ESP-IDF's `modem_syscon_reg.h`.

| `n` | `CLK_CONF` | `CLK_CONF1` | who calls it |
|---|---|---|---|
| 1 | 0 | 0x000167FF | Wi-Fi: `WifiRxStart`, around the receive |
| 2 | 0 | 0x00076000 | BLE: the image's `ble_tx` and `ble_rx` |
| 3 | 0x01800000 | 0x00076000 | 802.15.4: the image's `zb_tx` and `zb_rx` |
| any other | 0xFFFFFFFF | 0x0007FFFF | every clock on: each test's end, and `rftest_init` |

`rftest_open_clk` also sets every bit of these three words:

- `MODEM_SYSCON`'s `CLK_CONF_POWER_ST` 0x600A980C;
- `MODEM_LPCON`'s `CLK_CONF` 0x600AF018;
- `MODEM_LPCON`'s `CLK_CONF_FORCE_ON` 0x600AF01C and `CLK_CONF_POWER_ST` 0x600AF020.

## Sources

| what | where | SHA-256 |
|---|---|---|
| the package | https://dl.espressif.com/RF/EspRFTestTool_v5.2_Manual.zip | 0b836ca247076ff98d12a00c432398c2b179300535f4897963e4384d01d53142 |
| its C6 image | `Bin/RF_TEST_BIN/ESP32-C6_RFTest_V106_4cc3bb5_20250909.bin` | e19b591fbb85760e7f45643c453ee68e1ae0635f05cdec429a59fa629ae2d8f0 |
| the library | `esp32c6/librftest.a` of `esp-phy-lib` commit 20f1db053a0e6cb9f1c09d255c43bf42483041d0, which `tools/esp-fetch.py` pins | 7e426894d18c765097d4c98e3c2bfc4edfcdf91e583817bd4890799e0f723096 |

How they were read:

- **The host.**
  The host tool's Python was decompiled, and the functions that would not decompile were read from their bytecode.
- **The library and the image, disassembled.**
  `librftest.a` was linked with that commit's `libphy.a` and `libbtbb.a` and ESP-IDF's ROM linker scripts, at the image's own address.
  It was then disassembled with `llvm-objdump`.
  A copy of `tools/phymap.py` followed every command's calls, through the ROM's jump table and `g_phyFuns`'s slots.
  It noted every load and store at a constant address and every analog I2C or pbus call with constant arguments.
- **The image's names.**
  The image's functions were named by matching them to the library's:
  - by the strings each prints;
  - by aligning each pair's calls, starting from the two dispatchers, whose command lists name their handlers.
  That named 290 of its 468 functions; a function this file calls by its address, image 0x..., is one left unnamed.
- **Ghidra.**
  Ghidra 11.4.2's pseudo-C of the library's objects and of the image confirmed the values constant for constant where it was read.
  This covers the BLE and 802.15.4 functions, `phy_set_clk_conf`, `force_iq_set`, `force_dig_gain`, `rx_init_gain`, `tx_a_frame`, `phy_11ax_tx_mode`, `esp_crc8` and `RF_init_sel`.
- **What was not followed.**
  An address computed from a table or in a loop is not seen by the copy of `phymap.py`.
  The MAC's per-queue address getters were read by hand, see `wifi-mac.md`.
