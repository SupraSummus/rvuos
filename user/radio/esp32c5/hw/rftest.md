# The ESP32-C5's side of the RF test firmware

Espressif's RF test tool, `EspRFTestTool`, drives a chip through a firmware it loads, a console on the UART;
the tool, its protocol and the C6's commands are in `../../esp32c6/hw/rftest.md`, which this file does not repeat.
This file is the C5's side.
Sources and labels are in `README.md` beside this file; nothing here ran on a C5.

## No C5 image

The tool's package, `EspRFTestTool_v5.2_Manual.zip`, has no C5 test firmware in `Bin/RF_TEST_BIN/`;
for the C5 the user picks a `.bin` with a file dialog.
Asked for one, Espressif answered "write to sales" (https://github.com/espressif/esp-idf/issues/17489).
The firmware's command handlers are in esp-phy-lib's `esp32c5/librftest.a`, which ESP-IDF links for its `cert_test` example,
so this file is that library's:
the dispatcher and the handlers are there, the main loop that reads a line and splits it, `UartGetCmdLn` and `cmd_parse`, is not.
That Espressif's binary has the same dispatcher is **inferred**:
the host tool's C5 commands, `esp_tx 5180 ...`, `init_para_chg 1 idx val`, `test_5g`, `get_5g_en`, all match it.

## The dispatcher

- `run_rftest_case(name, args, argc)`, in `wifi.o`, is one chain of 246 `strcmp`s;
  a match runs its handler and returns 1 (**known**).
- `run_rftest_case2`, which the firmware tries second, is `return 0` in this build.
- The arguments are integers, up to 16, `a0`, `a1`, ... below.
- A command that runs long polls the UART through `esp_phy_getstopcmd`,
  which ends it on `CmdStop` or `cmdstop`, answers `CmdStatus` or `cmdstatus`,
  and to anything else prints " Plz run CmdStop to exit current cmd!".
- One name begins with a space, `" phy_tx_paon_set"`, so a console that trims its input never reaches it.

### Stubs

This build's handlers are a lone `ret`, or fall through to the common return, for
`phy_band_sel`, so `test_5g`;
`pa_setting_by_chan`, `open_pwr_domain`, `init_param_chg` (the command `init_para_chg` does not call it), `rfpll_cal_time`, `gen_ate_data`, `cpu_reset`,
`fedump_wr_txmem`, `fedump_rd_txmem`, `fedump_rd_rxmem`, `rf_spi_flash_test`, `rf_spi_psram_test`,
`phy_2480m_opt`, `rfrx_sat_check`;
and the commands `set_pbus_mem`, `close_peri`, `clk10m_enable`, `uart1_sel` and `rx_gain_comp`.
`rtc_sar_read` is a tail call of four bytes.
Espressif's own binary may fill some of them (**unknown**).
There is no `set_5g_en`, though the tool's manual command list has it.

### The commands that matter for the C5

- `esp_tx a0 a1 [a2 [a3 [a4 [a5]]]]`, `esp_tx_func`:
  - a0 a channel up to 14, or a frequency in MHz, which also chooses the band (`phy.md`);
  - a1 the rate; one above 0x1F becomes (a1 & 0xF) | 0x10, one above 0x2F sets `phy_11ac_enable`;
  - a2 the backoff, a signed byte, 0 when absent;
  - a3 the length, else (600 or 200, less 200, 100 or 40 by rate) × the rate's Mbit/s (`change_data_rate`) / 8, or 500 for HE-TB;
  - a4 the delay, else 1200 or 400, in µs as the host tool names it;
  - a5 the count, else 0, without end;
  - it sets the channel (`rftest_set_chan`, 40 MHz when `tx_cbw40m_en`), the backoff, the descriptor (`FillTxPacket`), sends (`WifiTxStart_org`),
    and prints the MAC's state, 0x600A4C74, as "Tx Over";
  - at 5620 and 5690 MHz it changes three analog I2C fields for the burst (`phy.md`);
  - with `fcc_mode_sel` = 1 it sends 4095-byte frames back to back instead, with the power tracking and correction on.
- `esp_rx a0 a1`: a0 channel or MHz, a1 the rate; `WifiRxStart(a1)`;
  its result line is `Correct: Desired: RSSI: noise: gain: para1: para2: freq: rxevm:`, the C6's with `rxevm` added, as the host tool says too.
- `wifiscwout a0 a1 a2`: a0 1 to start, 0 to stop; a1 channel or MHz; a2 the attenuation in 0.25 dB, about 20 − a2/4 dBm;
  it puts the TX calibration in its debug mode, 0x6E register 0xD bit 2, and plays a tone at the channel's centre.
- `init_para_chg 1 a1 a2`: byte a1 of the 256-byte init table = a2, then `register_chipv7_phy_init_param`, which applies it; it prints `name[a1]=0x..`.
- `RF_init_sel a0 a1 a2`: a0 = 0 takes the init table built in; a0 = 1 reads 256 bytes from flash a1 + 8,
  checks their CRC-8 against the last byte, and prints "esp init data is from flash"; then `register_chipv7_phy`.
- `get_5g_en`: prints the band byte, 1 on 5 GHz.
- `rd`, `rd_dport`, `wr`, `wr_dport`, `RdMem addr len`, `sm addr mask`, `cm addr mask`, `gb addr msb lsb`: any address, no check.
- `ri`, `wi`, `rim`, `wim`: the analog I2C, `block host reg [msb lsb] [val]`; the host is ignored (`phy.md`).
- `ate_*`, `phy_rf_param_flash_burn`, `phy_rf_param_flash_get`, `ref_curr_set_io`: the PA's current trim (`phy.md`).
- `flash_test_init`, `psram_test_init` and their `_enable`s set up flash or PSRAM traffic, through functions outside the library, to measure the RF's spurs under it (**inferred**).

## Every command

In the dispatcher's order.
"Arguments" is what the dispatcher, or the handler when it takes the whole array, does with them, read from the pseudo-C.
"Words / I2C blocks" counts the MMIO words of the handler's whole call tree and lists the analog I2C blocks it reaches;
`regs.md` has the words by block.

| Command | Arguments | Calls | Words / I2C blocks |
|---|---|---|---|
| `WifiTxStart` | `WifiTxStart(a0..a7)`; prints the MAC state 0x600A4C74 | `WifiTxStart` | 109 / 61 63 65 66 67 68 69 6A 6B 6E |
| `tx_cbw40m_en` | `tx_cbw40m_en = a0` | inline |  |
| `cbw40m_en` | `cbw40m_en(a0)` | `cbw40m_en` |  |
| `short_gi_enable` | `short_gi_enable = a0` | inline |  |
| `tx_contin_en` | `tx_contin_en(a0)` | `tx_contin_en` |  |
| `wifitxout` | the same arguments as `esp_tx` | `wifitxout` | 120 / 61 63 65 66 67 68 69 6A 6B 6E |
| `wifiscwout` | a0 enable, a1 channel or MHz, a2 attenuation in 0.25 dB (about 20 − a2/4 dBm) | `wifiscwout` | 97 / 61 63 65 66 67 68 69 6A 6B 6E |
| `esp_tx` | a0 channel or MHz, a1 rate, a2 backoff, a3 length, a4 delay (µs), a5 count; see below | `esp_tx_func` | 120 / 61 63 65 66 67 68 69 6A 6B 6E |
| `esp_rx` | a0 channel or MHz, a1 rate; `WifiRxStart(a1)` | `esp_rx_func` | 111 / 61 63 65 66 67 68 69 6A 6B 6E |
| `WifiRxStart` | `do_rx_poll(a0)` | `do_rx_poll` | 19 |
| `FillTxPacket` | `FillTxPacket(a0, a1, a2, a3, a4, a5, a6, a7)` | `FillTxPacket` | 8 |
| `beacon_print` | `beacon_print(a0)` | `beacon_print` | 19 |
| `sw_chan_test` | `sw_chan_test(a0, a1, a2, a3)` | `sw_chan_test` | 112 / 61 63 65 66 67 68 69 6A 6B 6E |
| `rd` | `rd addr`: prints the word at a0 | inline |  |
| `rd_dport` | as `rd` | inline |  |
| `wr_dport` | as `wr` | inline |  |
| `wr` | `wr addr val`: writes a1 to a0, prints it back | inline |  |
| `rftest_init` | `rftest_init()` | `rftest_init` | 275 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `phy_close_rf` | `phy_close_rf()` | `phy_close_rf` | 13 / 63 69 |
| `RdMem` | `RdMem addr len`: prints the words from a0 to a0 + a1 | inline |  |
| `sm` | `sm addr mask`: `*a0 |= a1` | inline |  |
| `cm` | `cm addr mask`: `*a0 &= ~a1` | inline |  |
| `gb` | `gb addr msb lsb`: prints bits a1..a2 of `*a0` | inline |  |
| `ri` | `phy_i2c_readReg(block, host, reg)` | `phy_i2c_readReg` | 2 |
| `wi` | `phy_i2c_writeReg(block, host, reg, val)` | `phy_i2c_writeReg` | 1 |
| `wim` | `phy_i2c_writeReg_Mask(block, host, reg, msb, lsb, val)` | `phy_i2c_writeReg_Mask` | 2 |
| `rim` | `phy_i2c_readReg_Mask(block, host, reg, msb, lsb)` | `phy_i2c_readReg_Mask` | 2 |
| `pbus_debugmode` | `phy_pbus_debugmode()` | `phy_pbus_debugmode` | 4 |
| `pbus_workmode` | `phy_pbus_workmode()` | `phy_pbus_workmode` | 4 |
| `pbus` | `phy_pbus_force_test(a0, a1, a2)`: 0x600A0884 bits 16:1 = a1 << 15 \| a2 << 6 \| a0 << 2 | `phy_pbus_force_test` | 2 |
| `pbus_rd` | `phy_pbus_rd(a0, a1)` | `phy_pbus_rd` |  |
| `txtone` | tone on if a0 or a3; `start_tx_tone(a1, a2, a4, a5)` | `start_tx_tone` | 7 |
| `txtone_step` | tone on if a0 or a3; `phy_start_tx_tone_step(a0, a1, a2, a3, a4, a5)`: two tones, enable, offset, attenuation each | `phy_start_tx_tone_step` | 7 |
| `stoptone` | `phy_stop_tx_tone(a0)` | `phy_stop_tx_tone` | 4 |
| `adctrig` | `adctrig(a0, a1, a2, a3, a4, a5, a6, a7)` | `adctrig` | 28 / 6B |
| `init_para_chg` | `init_para_chg 1 idx val`: byte a1 of the 256-byte init table = a2, then `register_chipv7_phy_init_param` | `register_chipv7_phy_init_param` |  |
| `bb_init` | `phy_bb_init()` | `phy_bb_init` | 152 / 61 63 65 66 67 68 69 6A 6B 6E |
| `mac_init` | `mac_init()` | `mac_init` | 16 |
| `rxdc_cal` | `phy_pbus_rx_dco_cal(4000, ...)` with the RX and TX clocks on | `phy_set_rxclk_en`, `phy_set_txclk_en`, `phy_pbus_rx_dco_cal` | 9 |
| `phy_set_freq` | `phy_set_freq(a0, a1)` | `phy_set_freq` | 74 / 61 63 65 66 67 68 69 6A 6B |
| `RFChannelSel` | `rftest_set_chan(a0, a1)` | `rftest_set_chan` | 94 / 61 63 65 66 67 68 69 6A 6B 6E |
| `set_chan` | `phy_chip_set_chan(a0, a1)` | `phy_chip_set_chan` | 74 / 61 63 65 66 67 68 69 6A 6B |
| `target_power_backoff` | `target_power_backoff(a0)` | `target_power_backoff` | 6 |
| `set_tx_gain` | `force_tx_gain(a0, a1, a2)`, a0 and a1 kept as the forced PA and BB gain, a3 kept | `force_tx_gain` | 5 |
| `set_ble_tx_gain` | a0 and a1 kept as the forced PA and BB gain; `force_ble_tx_gain(a2)` | `force_ble_tx_gain` | 5 |
| `check_noise_floor` | `check_noise_floor()` | `check_noise_floor` | 3 |
| `phy_set_most_tpw` | `phy_set_most_tpw(a0)` | `phy_set_most_tpw` | 6 |
| `phy_get_most_tpw` | `phy_get_most_tpw()` | `phy_get_most_tpw` |  |
| `set_tx_dig_gain` | `force_dig_gain(a0, a1)` | `force_dig_gain` | 1 |
| `accumiq` | `accumiq(a0, a1)` | `accumiq` |  |
| `phy_init` | `phy_init()` | `phy_init` | 243 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `set_chan_offset` | `phy_chip_set_chan_offset(a0)` | `phy_chip_set_chan_offset` | 29 / 63 65 68 6A |
| `rf_init` | `phy_rf_init()` | `phy_rf_init` | 72 / 61 63 65 66 67 68 69 6A 6B |
| `esp_origin_mac` | `esp_origin_mac()` | `esp_origin_mac` | 2 |
| `print_ver` | prints "phy_version: 1.9, Mar 10 2026, 10:22:19" | inline |  |
| `init_print` | `module_test_cal_print()` | `module_test_cal_print` | 15 / 69 6B |
| `get_rx_buffer` | `get_rx_buffer(a0, a1)` | `get_rx_buffer` | 10 |
| `dig_11b_filter_sel` | bbtx 0x600A7400 bits 14:13 = a0 | inline |  |
| `test_txtone_pwr` | `test_txtone_pwr(a0, a1, a2, a3)` | `test_txtone_pwr` | 20 / 6E |
| `txpwr_track_en` | `phy_tx_pwr_track_en = a0; phy_tx_pwr_correct_en = a1; phy_tx_pwr_print_en = a2; phy_pwdet_always_en(1)` | `phy_pwdet_always_en` |  |
| `adc_rand` | `phy_get_adc_rand(a0)` | `phy_get_adc_rand` |  |
| `set_pbus_mem` | none; falls through to the common return, does nothing | inline |  |
| `rssi_min_max_print` | `rssi_min_max_print = a0` | inline |  |
| `pll_cap_track_en` | a `phy_param` flag = a0 | inline |  |
| `burn_in_test` | `burn_in_test()` | `burn_in_test` | 278 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `get_pll_vol` | none; prints nothing of its own | inline |  |
| `phy_get_vdd33` | `phy_get_vdd33()` | `phy_get_vdd33` | 11 / 69 6B |
| `pll_cap_print_en` | a `phy_param` flag = a0 | inline |  |
| `set_freq` | `phy_set_rf_freq_offset(chan, a0, 0)`: a frequency offset on the current channel | `phy_set_rf_freq_offset` | 5 / 63 65 68 6A |
| `txdc_cal` | `phy_txdc_cal_init(...)` | `phy_txdc_cal_init` | 34 / 63 65 68 6A |
| `tx_cap_init` | `phy_tx_cap_init()` | `phy_tx_cap_init` | 38 / 63 65 68 6A 6B 6E |
| `tx_pwctrl_init` | `phy_tx_pwctrl_init_new(1)` | `phy_tx_pwctrl_init_new` | 38 / 63 65 68 6A 6B 6E |
| `txiq_cal_init` | `phy_txiq_cal_init(...)` | `phy_txiq_cal_init` | 39 / 63 65 68 6A 6B 6E |
| `test_rxgain_table` | `phy_set_rx_gain_table(a0, 1)` | `phy_set_rx_gain_table` | 10 |
| `get_sar2_vol` | `phy_get_sar2_vol(a0)` | `phy_get_sar2_vol` | 6 / 69 |
| `read_sar2_code` | `phy_read_sar2_code()` | `phy_read_sar2_code` | 3 |
| `meas_tone_pwr_db` | `phy_meas_tone_pwr_db(a0, a1, a2)` | `phy_meas_tone_pwr_db` | 11 |
| `tx_ack_test` | `tx_ack_test(a0, a1, a2, a3, a4, a5, a6, a7)` | `tx_ack_test` | 38 |
| `tx_ack_init` | `tx_ack_init(a0, a1, a2, a3, a4, a5)` | `tx_ack_init` | 18 |
| `remove_11b_4p8G_spur` | `remove_11b_4p8G_spur(a0, a1, a2)` | `remove_11b_4p8G_spur` | 2 |
| `phy_wakeup_init` | `phy_wakeup_init()` | `phy_wakeup_init` | 124 / 63 66 67 6A 6B |
| `phy_test_init` | `phy_test_init()` | `phy_test_init` | 243 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `rfpll_cal_time` | `rfpll_cal_time(a0, a1)` | `rfpll_cal_time` |  |
| `freq_init` | none; `phy_get_freq_init(0x53, 0)` | `phy_get_freq_init` | 5 / 63 |
| `force_txtone` | `force_txtone(a0, a1)` | `force_txtone` | 9 |
| `test_mem_time` | `test_mem_time(a0, a1)` | `test_mem_time` | 2 |
| `get_tx_rate` | `get_tx_rate()` | `get_tx_rate` | 1 |
| `set_rx_sense` | `phy_set_rx_sense(a0)` | `phy_set_rx_sense` | 7 |
| `close_peri` | none; does nothing | inline |  |
| `clk10m_enable` | none; does nothing | inline |  |
| `get_tone_sar_dout` | `phy_get_tone_sar_dout(a0)` | `phy_get_tone_sar_dout` | 5 |
| `get_power_db` | `phy_get_power_db` on the current channel | `phy_get_pwdet_offset_new`, `phy_get_power_db` | 5 |
| `pwdet_code_cal` | `phy_pwdet_code_cal()` | `phy_pwdet_code_cal` | 20 / 6E |
| `temp_read` | `phy_tsens_temp_read()` | `phy_tsens_temp_read` | 3 / 69 |
| `uart1_sel` | none; does nothing | inline |  |
| `btpwr_track_en` | `phy_bt_power_track(a0)` | `phy_bt_power_track` |  |
| `rx_gain_comp` | none; does nothing | inline |  |
| `tx_chan_atten` | `tx_chan_atten()` | `tx_chan_atten` | 85 / 61 63 65 66 67 68 69 6A 6B 6E |
| `rx_chan_noise` | `rx_chan_noise(a0)` | `rx_chan_noise` | 76 / 61 63 65 66 67 68 69 6A 6B |
| `rx_chan_noise_all` | `rx_chan_noise_all()` | `rx_chan_noise_all` | 76 / 61 63 65 66 67 68 69 6A 6B |
| `set_noise_floor` | `set_noise_floor(-a0)` | `set_noise_floor` | 1 |
| `get_iq_est_pwr` | `get_iq_est_pwr()` | `get_iq_est_pwr` | 5 |
| `dc_iq_est` | `dc_iq_est_test(1, a0, ...)`, prints six values | `dc_iq_est_test` | 9 |
| `bt_pll_track_en` | prints a0, keeps nothing | inline |  |
| `freq_mem_backup` | `phy_freq_mem_backup(a0, 0x40830000)` | `phy_freq_mem_backup` |  |
| `set_tx_power` | `set_tx_power(a0)` | `set_tx_power` | 5 |
| `bt_rx_force` | `bt_rx_force(a0, 1)` | `bt_rx_force` | 2 |
| `tx_gain_print` | recomputes and prints the Wi-Fi and BT TX gains | `phy_wifi_set_tx_gain_new`, `phy_bt_set_tx_gain_new` | 6 |
| `phy_ftm_comp` | `phy_ftm_comp()` | `phy_ftm_comp` |  |
| `esp_txrx_test` | `esp_txrx_test(a0, a1, a2, a3, a4, a5, a6, a7)` | `esp_txrx_test` | 43 |
| `le_master_test` | `le_master_test(a0, a1 + 1, a2, a3, a4, ...)` | `le_master_test` |  |
| `le_slave_test` | `le_slave_test(a0, a1 + 1, a2, a3)` | `le_slave_test` |  |
| `write_flash` | `write_flash addr len data...` | `write_flash` |  |
| `read_flash` | `read_flash addr len`, len ≤ 0x400 | `read_flash` |  |
| `phy_i2c_check` | `phy_i2c_check()` | `phy_i2c_check` | 2 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `ate_rxframe_fb` | `ate_rxframe_fb(a0, a1)` | `ate_rxframe_fb` | 100 / 61 63 65 66 67 68 69 6A 6B |
| `ate_txframe_dut` | `ate_txframe_dut(a0, 0, a1, 1)` | `ate_txframe_dut` | 105 / 61 63 65 66 67 68 69 6A 6B |
| `ESP_TEST_GPIO` | up to three GPIO settings from the arguments | `ESP_TEST_GPIO`, `CMDSTOP_GPIO` | 2 |
| `cmdstop_gpio` | `CMDSTOP_GPIO()` | `CMDSTOP_GPIO` | 1 |
| `trig_tx_frame` | a0 enable, a1 delay (µs); `trig_tx_frame()` | `trig_tx_frame` | 4 |
| `phy_disable_cca` | `phy_disable_cca()` | `phy_disable_cca` | 1 |
| `phy_enable_cca` | `phy_enable_cca()` | `phy_enable_cca` | 1 |
| `set_cca` | `phy_set_cca(a0, a1)` | `phy_set_cca` | 2 |
| `rxdc_remain` | `ate_rxdc_remain_check(a0, 1)` | `ate_rxdc_remain_check` |  |
| `tx_force_pwr_en` | a flag = a0 | inline |  |
| `rx_init_gain` | `rx_init_gain(a0)` | `rx_init_gain` | 2 |
| `dump_pbus` | a0 enable, a1..a5 the rfrx, bb1, bb2, rftx1, rftx2 words to force | `get_dump_pbus` |  |
| `read_hw_noisefloor` | `phy_read_hw_noisefloor()` | `phy_read_hw_noisefloor` | 1 |
| `bt_txiq_cal` | `phy_bt_txiq_cal()` | `phy_bt_txiq_cal` |  |
| `bt_txdc_cal` | `phy_bt_txdc_cal()` | `phy_bt_txdc_cal` | 14 |
| `rf_freq_debug` | `rf_freq_debug(a0, a1, a2, a3, a4, a5, a6)` | `rf_freq_debug` | 11 |
| `phy_11ax_tx_set` | a0 HE format, a1, a2, and a3 when a0 = 3 (HE-TB) | `phy_11ax_tx_set` |  |
| `phy_11ax_rx_set` | `phy_11ax_rx_set(a0)` | `phy_11ax_rx_set` | 1 |
| `force_rx_gain` | `force_rx_gain(a0, a1, a2); force_rx_gain(a0, a1, a2)` | `force_rx_gain` | 9 |
| `force_txon_mode` | `force_txon_mode(a0, a1, a2)` | `force_txon_mode` | 8 |
| `force_iq_set` | `force_iq_set(a0, a1, a2, a3)` | `force_iq_set` | 2 |
| `test_5g` | `phy_band_sel(a0)`, a stub | `phy_band_sel` |  |
| `rxiq_opt_flag` | a `phy_param` flag = a0 | inline |  |
| `get_5g_en` | prints the band byte, 1 on 5 GHz | inline |  |
| `rx_spur_remove` | `rx_spur_remove(a0, a1, a2)` | `rx_spur_remove` | 3 |
| `usb_clk_en` | PCR 0x60096094 bit 0 = a0 | inline |  |
| `bt_set_rx_comp` | `bt_set_rx_comp(a0, a1, a2, a3, a4)` | `bt_set_rx_comp` | 1 |
| `rxiq_get_pwr` | `rxiq_get_pwr(a0, ...)` | `rxiq_get_pwr` | 9 |
| `get_rx_sig_pwr` | `phy_get_rx_sig_pwr(12)` | `phy_get_rx_sig_pwr` | 9 |
| `fedump_wr_txmem` | `fedump_wr_txmem(a0, a1, &a2)`, a stub | `fedump_wr_txmem` |  |
| `fedump_rd_txmem` | `fedump_rd_txmem(a0, a1)` | `fedump_rd_txmem` |  |
| `fedump_rd_rxmem` | `fedump_rd_rxmem(a0, a1)` | `fedump_rd_rxmem` |  |
| `set_cpu_freq` | `rtc_set_cpu_freq(a0)` | `rtc_set_cpu_freq` |  |
| `loop_mode` | `loop_mode_set(a0, a1, a2)` | `loop_mode_set` | 15 / 6B |
| `rxiq_cal_test` | `rxiq_cal_test(a0)` | `rxiq_cal_test` | 10 |
| `get_cal_iq` | `get_cal_iq(a0)` | `get_cal_iq` |  |
| `phy_reg_check` | `phy_reg_check()` | `phy_reg_check` | 24 |
| `set_aa` | the BLE access address = a0 | inline |  |
| `rx_max_gain` | `rx_max_gain(a0)` | `rx_max_gain` | 2 |
| `force_rfrx_param` | `force_rfrx_param(a0, a1)` | `force_rfrx_param` |  |
| `cpu_reset` | `cpu_reset()` | `cpu_reset` |  |
| `rfagc_disable` | `phy_rfagc_disable()` | `phy_rfagc_disable` | 2 |
| `phy_pkdadc_set` | `phy_pkdadc_set(a0, a1)` | `phy_pkdadc_set` | 3 |
| `rxiq_cal_init` | `phy_set_rx_gain_cal_iq(a1, ...)`, a flag = a0 | `phy_set_rx_gain_cal_iq` | 46 / 63 65 68 6A 6B |
| `rxiq_get_mis` | `phy_rxiq_get_mis(12, ...)` | `phy_rxiq_get_mis` | 9 |
| `rxiq_cal_loop` | `phy_set_rx_gain_cal_iq` every millisecond until `CmdStop` | `phy_set_rx_gain_cal_iq`, `ets_delay_us` | 46 / 63 65 68 6A 6B |
| `freq_stable_test` | `freq_stable_test(a0, a1, a2)` | `freq_stable_test` | 24 / 63 65 68 6A |
| `phy_init_loop` | `phy_init()` every millisecond until `CmdStop` | `phy_init`, `ets_delay_us` | 243 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `txdc_cal_pwdet_init` | `phy_txdc_cal_pwdet_init(1, a0, ...)`, prints the 2.4 and 5 GHz TX DC codes | `phy_txdc_cal_pwdet_init` | 37 / 63 65 68 6A |
| `pbus_xpd_iq_path` | `phy_pbus_xpd_iq_path(a0, a1, a2)` | `phy_pbus_xpd_iq_path` | 13 / 6B |
| `dreg_pvt_opt` | `phy_rfpll_dreg_pvt_opt(MHz)` | `phy_rfpll_dreg_pvt_opt` | 2 / 63 6A |
| `get_chan_cap` | `phy_get_chan_cap(a0, ...)`, the band from a0 > 4000 | `phy_get_chan_cap` |  |
| `get_pkdet_data` | `phy_get_pkdet_data()` | `phy_get_pkdet_data` | 1 |
| `xtal_spur_cal` | `xtal_spur_cal(a0, 1)` | `xtal_spur_cal` | 89 / 61 63 65 66 67 68 69 6A 6B |
| `ble_rx_opt_en` | `ble_rx_opt_en = a0` | inline |  |
| `bandedge_opt` | a0..a4: TX output filter and the 20/40 MHz scales (`phy_bbtx_outfilter`) | `phy_bbtx_outfilter` | 1 |
| `read_pkdet_original` | `read_pkdet_original()` | `read_pkdet_original` | 1 |
| `get_freq_idx` | prints `phy_freq_to_index` of the current channel | `phy_freq_to_index` |  |
| `get_txiq_dc` | prints the stored TX IQ and DC of gain a0 | inline |  |
| `get_rxiq_dc` | prints the stored RX IQ and DC of gain a0 | inline |  |
| `reg_opt_on` | a flag = a0 | inline |  |
| `bt_tx_tone` | `bt_zb_tx_tone(a0, a1, a2, 0)` | `bt_zb_tx_tone` | 82 / 61 63 65 66 67 68 69 6A 6B |
| `zb_tx_tone` | `bt_zb_tx_tone(a0, a1, a2, 1)` | `bt_zb_tx_tone` | 82 / 61 63 65 66 67 68 69 6A 6B |
| `pwdet_debug` | `pwdet_debug_test(a0, a1)` | `pwdet_debug_test` | 18 / 6E |
| `mem_read_test` | `mem_read_test(a0, a1, a2, a3)` | `mem_read_test` | 1 |
| `pbus_xpd_txon` | `pbus_xpd_txon_test(a0)` | `pbus_xpd_txon_test` | 26 / 63 65 68 6A |
| `pbus_xpd_rx_on` | `phy_pbus_xpd_rx_on(a0)`, and RX gain a0 << 4 \| a1 << 12 if a1 | `phy_pbus_debugmode`, `phy_pbus_xpd_rx_on`, `phy_pbus_set_rxgain` | 5 |
| `pbus_xpd_tx_on` | `phy_pbus_xpd_tx_on(a0, a1)`, then the DC and capacitor of TX gain a1 | `phy_pbus_debugmode`, `phy_pbus_xpd_tx_on`, `phy_txbbgain_to_index`, `phy_pbus_set_dco`, `phy_set_txcap_reg` | 7 / 6B |
| `pbus_loop_gain` | `phy_set_loopback_gain(a0, a1, a2, 0)` | `phy_pbus_debugmode`, `phy_set_loopback_gain`, `phy_set_rx_pbus_freq` | 6 / 6B |
| `rfcal_rxiq` | `phy_get_rfcal_rxiq_data(a0, a1, ...)` | `phy_get_rfcal_rxiq_data` | 17 |
| `txiq_ckgen_dcode2` | prints a `phy_param` byte | inline |  |
| `sdm_cal_check` | `phy_sdm_cal_check(a0, a1)` | `phy_sdm_cal_check` | 3 / 63 68 |
| `set_freq_sw_start` | `phy_set_freq_sw_start(a0, 0, 0)` | `phy_set_freq_sw_start` | 2 |
| `i2c_clk_sel` | `phy_i2c_clk_sel(a0)` | `phy_i2c_clk_sel` | 3 |
| `sdm_read_code` | `phy_sdm_read_code()` | `phy_sdm_read_code` | 2 / 63 |
| `sdm_start_cal_1p1` | `phy_sdm_start_cal_1p1(a0, a1)` | `phy_sdm_start_cal_1p1` | 2 / 63 6A |
| `sdm_restart` | `phy_restart_cal()` | `phy_restart_cal` | 2 / 63 |
| `get_loop_snr` | `phy_get_loop_snr(a0, a1, ...)` | `phy_get_loop_snr` | 24 / 6B |
| `get_freq_param` | prints six stored frequency-calibration values | inline |  |
| `rx_corr_pwr` | `phy_corr_pwr_sum(a0)` | `phy_corr_pwr_sum` | 15 |
| `rf_open_clk` | `phy_get_romfunc_addr(); rftest_open_clk()` | `phy_get_romfunc_addr`, `rftest_open_clk` | 6 |
| `get_max_pwr` | `phy_get_target_pwr(a0, ...)` | `phy_get_target_pwr` |  |
| `sar_vol_1p1` | `phy_sar_vol_1p1(a0, a1)` | `phy_sar_vol_1p1` | 6 / 69 6A |
| `xtal_duty_cal` | `phy_xtal_duty_cal(a0, 1)` | `phy_xtal_duty_cal` | 89 / 61 63 65 66 67 68 69 6A 6B |
| `pa_dc_test` | `pa_dc_test(a0)` | `pa_dc_test` | 74 / 61 63 65 66 67 68 69 6A 6B |
| `phy_11p_set` | `phy_11p_set(a0, a1)` | `phy_11p_set` | 5 / 67 |
| `txon_tone_test` | `phy_force_txon_test(a0, a1, a2, a3)` | `phy_force_txon_test` | 17 / 63 |
| `esp_set_tx_power_offset` | a0 enable; if set, a1..a7 seven signed offsets into `phy_param` 0x2EF.. | inline |  |
| `esp_get_tx_power_offset` | none; prints the enable and the offsets | inline |  |
| `RF_init_sel` | `RF_init_sel(a0, a1, a2)` | `RF_init_sel` | 244 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `adc_rate_set` | `phy_adc_rate_set(a0)` | `phy_adc_rate_set` | 3 / 66 |
| `erase_flash` | `erase_flash addr`, a 4 KiB sector | `esp_rom_spiflash_erase_sector` |  |
| `phy_freq_cal_test` | `phy_freq_cal_test(a0)` | `phy_freq_cal_test` | 2 / 63 |
| `freq_i2c_data_write` | `phy_freq_i2c_data_write(1)` | `phy_freq_i2c_data_write` | 9 / 63 6A |
| `xtal_32k_enable` | `x32k_enable(a0)`, then `x32k_set` of DAC a1, DRES a2, DGM a3, DBUF a4 | `x32k_enable`, `x32k_set` |  |
| `rxiq_cal` | `phy_rxiq_cover_mg_mp(14, ...)` | `phy_rxiq_cover_mg_mp` | 10 |
| `loop_dump_test` | `loop_dump_test(a0, a1)` | `loop_dump_test` | 120 / 61 63 65 66 67 68 69 6A 6B 6E |
| `print_dump_data` | `print_dump_data(a0, a1, 1)` | `print_dump_data` |  |
| `pbus_xpd_dpd_path` | `phy_pbus_xpd_dpd_path(a0, a1, a2)` | `phy_pbus_xpd_dpd_path` | 7 / 6B |
| `dpd_rxdc_cal` | `phy_dpd_rxdc_cal` with the RX and TX clocks on | `phy_set_rxclk_en`, `phy_set_txclk_en`, `phy_dpd_rxdc_cal` | 9 |
| `apb_get_mem` | `apb_get_mem(a0, a1)` | `apb_get_mem` | 4 |
| `test_rf_cal_level` | `test_rf_cal_level(a0, a1)` | `test_rf_cal_level` | 244 / 61 63 65 66 67 68 69 6A 6B 6D 6E |
| `txcap_comp` | three bytes a0, a1, a2 kept for the TX capacitor compensation | inline |  |
| `pbus_print` | `phy_pbus_print()` | `phy_pbus_print` |  |
| `ref_curr_set_io` | `pull_internal_voltage(a0)` | `pull_internal_voltage` |  |
| `phy_rf_param_flash_burn` | `phy_rf_param_flash_burn (a0, a1, a2, a3, a4, a5, a6)` | `phy_rf_param_flash_burn` |  |
| `phy_rf_param_flash_get` | prints the 8-byte record at flash 0x30000 | `phy_rf_param_flash_get` |  |
| `rfrx1_en2` | a `phy_param` half-word = a0 | inline |  |
| ` phy_tx_paon_set` | a0..a5: bb 0x600A7C00[20:11] = a0, FECTRL 0x600A086C[15:8] = a1, bb 0x600A7C6C = a4 << 20 \| a3 << 10 \| a2, bb 0x600A7C30[9:0] = a5 | inline |  |
| `tx_signal_test` | `tx_signal_test(a0, a1, a2, a3)` | `tx_signal_test` | 83 / 61 63 65 66 67 68 69 6A 6B |
| `dumptone_test` | `phy_dumptone_test(a0, a1, a2, a3)` | `phy_dumptone_test` | 14 / 6B |
| `check_rssi` | `phy_check_rssi(a0, a1, a2)` | `phy_check_rssi` | 101 / 61 63 65 66 67 68 69 6A 6B |
| `check_rssi_band` | `phy_check_rssi_band_set(a0)` | `phy_check_rssi_band_set` | 5 / 67 |
| `check_rssi_1m` | `phy_check_rssi_1m(a0, a1)` | `phy_check_rssi_1m` | 18 |
| `fft_dump_init` | `fft_dump_init(a0)` | `fft_dump_init` | 17 |
| `phy_magtrk_cfg` | `phy_cfg_tx_magtrk(chan, a0, a1)` | `phy_cfg_tx_magtrk` | 5 |
| `phy_rxevm_calc` | `phy_rxevm_calcu(1)` | `phy_rxevm_calcu` | 5 |
| `phy_rxevm_test` | `phy_rxevm_init_cfg(1, a0, a1); ets_delay_us(100); phy_rxevm_calcu(a2)` | `phy_rxevm_init_cfg`, `ets_delay_us`, `phy_rxevm_calcu` | 7 |
| `module_test_sel` | a0 selects the module test, argc ≥ 3 | `module_test_sel` | 111 / 61 63 65 66 67 68 69 6A 6B |
| `ble_freq_offset` | a0 TX, a1 RX offset: `phy_chan_freq_hw_init(a0 + 2, a1 + 2)`, bt_v3_2 0x600A2170 | `phy_chan_freq_hw_init` | 11 / 63 6A |
| `ble_freq_offset_test` | `ble_freq_offset_test(a0)` | `ble_freq_offset_test` | 4 |
| `flash_test_init` | `flash_test_init(a0, a1, a2, a3)` | `flash_test_init` |  |
| `psram_test_init` | `psram_test_init(a0, a1, a2, a3)` | `psram_test_init` |  |
| `flash_test_enable` | `flash_test_enable = a0` | inline |  |
| `psram_test_enable` | `psram_test_enable = a0` | inline |  |
| `rf_spi_flash_test` | `rf_spi_flash_test(a0)` | `rf_spi_flash_test` |  |
| `rf_spi_psram_test` | `rf_spi_psram_test(a0)` | `rf_spi_psram_test` |  |
| `phy_dis_hw_set_freq` | `phy_dis_hw_set_freq_new()` | `phy_dis_hw_set_freq_new` | 3 |
| `phy_track_temp_debug` | `phy_track_temp_debug(a0, a1)` | `phy_track_temp_debug` |  |
| `force_tx_seed` | `force_tx_seed(a0, a1)` | `force_tx_seed` | 1 |
| `check_freq_mem` | `phy_check_freq_mem(a0)` | `phy_check_freq_mem`, `phy_freq_to_index` | 5 |

## Against the C6's library

The C6's `librftest.a` of the same esp-phy-lib commit has 203 commands to the C5's 246.

Only on the C5:
`adc_rate_set`, `apb_get_mem`, `ate_rxframe_fb`, `ate_txframe_dut`, `bandedge_opt`, `ble_freq_offset`, `ble_freq_offset_test`, `check_freq_mem`,
`check_rssi_1m`, `check_rssi_band`, `dpd_rxdc_cal`, `dreg_pvt_opt`, `dumptone_test`, `esp_get_tx_power_offset`, `esp_set_tx_power_offset`, `fft_dump_init`,
`flash_test_enable`, `flash_test_init`, `force_rfrx_param`, `force_tx_seed`, `freq_i2c_data_write`, `get_5g_en`, `get_chan_cap`, `get_freq_idx`, `get_freq_param`,
`get_loop_snr`, `get_max_pwr`, `get_pkdet_data`, `get_rx_sig_pwr`, `get_rxiq_dc`, `get_txiq_dc`, `mem_read_test`, `module_test_sel`, `pa_dc_test`, `pbus_loop_gain`,
`pbus_print`, `pbus_xpd_dpd_path`, `pbus_xpd_iq_path`, `pbus_xpd_rx_on`, `pbus_xpd_tx_on`, `pbus_xpd_txon`, `phy_dis_hw_set_freq`, `phy_freq_cal_test`,
`phy_init_loop`, `phy_magtrk_cfg`, `phy_pkdadc_set`, `phy_rf_param_flash_burn`, `phy_rf_param_flash_get`, `phy_rxevm_calc`, `phy_rxevm_test`,
`phy_track_temp_debug`, `" phy_tx_paon_set"`, `psram_test_enable`, `psram_test_init`, `pwdet_debug`, `read_pkdet_original`, `ref_curr_set_io`, `reg_opt_on`,
`rf_open_clk`, `rf_spi_flash_test`, `rf_spi_psram_test`, `rfagc_disable`, `rfcal_rxiq`, `rfrx1_en2`, `rx_corr_pwr`, `rxdc_remain`, `rxiq_cal`, `rxiq_cal_loop`,
`rxiq_get_mis`, `rxiq_opt_flag`, `sar_vol_1p1`, `sdm_cal_check`, `sdm_read_code`, `sdm_restart`, `sdm_start_cal_1p1`, `set_aa`, `set_cca`, `set_freq_sw_start`,
`test_5g`, `tx_signal_test`, `txcap_comp`, `txiq_ckgen_dcode2`, `txon_tone_test`, `xtal_32k_enable`, `xtal_duty_cal`, `xtal_spur_cal`.

Only on the C6:
`ate_bt`, `ate_rfpll_cap_sign`, `ate_rxframe`, `ate_txframe`, `ble_rx_start`, `consum_auto_test`, `disable_lr`, `enable_lr`, `esp_en_retest`, `esp_hetb_enable`,
`force_txdc`, `freq_rx_cal_en`, `get_rfcal_num`, `get_rfrx_sat`, `gpio_dis_output`, `gpio_input_get`, `gpio_output_set`, `ht20_rx_opt`, `i2c_xpd_test`,
`phy_ble_set_chan_base`, `phy_cal_track`, `phy_corr_pwr`, `phy_force_freq_set`, `phy_init_param_set`, `phy_set_clk_conf`, `phy_tx_tone`, `pll_cap_mem_update`,
`rc_cal`, `read_sar_dout`, `rftest_open_clk`, `rx_11b_opt`, `scan_rssi`, `set_23dbm`, `set_txdc_dig`, `step_01k`, `tx_cca_num`, `txdc_cal_pwdet`, `txdc_cal_to_flash`,
`wifi_rx_opt`, `wifi_rx_opt_en`, `xtal_freq_rx_cal`, `zb_master_test`, `zb_slave_test`.

The C5's additions are the analog chain's debug paths, the DPD, the SDM's readback, the 1.1 V level, the PA's current trim, EVM and FFT measures,
and the flash and PSRAM spur tests;
the 802.15.4 master and slave tests and the calibrations saved to flash are gone from its dispatcher.
The C6's RF test binary in the tool's package, V106 of 2025-09, is older than this library, and its list may differ from either.

## The init parameters

The init data the PHY starts from, `init_param_default` in `wifi.o`, is a table of 256 bytes on the C5, 128 on the C6.
The tool's file for it (`config/` and `Power_limit_table/system/ESP32C5/` of the package, **known** from the host's code and the spreadsheet):

- 272 bytes: the magic `PHYINIT\0`, the 256 bytes, the magic again; byte i of the table is at file offset 8 + i.
- Byte 255 is a CRC-8, the reflected polynomial 0x8C (Dallas/Maxim), initial value 0, over bytes 0 to 254;
  `RF_init_sel` checks it with `esp_crc8`.
- Byte 253 is `multiple_bins_enable` and byte 254 `certification_code`, as on the C6.
- A multi-country file appends a 32-byte control block and 256-byte entries; the tool reads its count at file offset 8 + 274.
- The tool puts the file in flash at 0x1FC000 and sends `RF_init_sel 1 0x1fc000`.

The table, from `phy_C5_init_param_v00_20250320.xlsx`; powers and limits are in 0.25 dB:

| Index | What |
|---|---|
| 0 | `param_ver_id` |
| 1–2 | 2.4 GHz 11b target power: 1/2, 5.5/11 Mbit/s |
| 3–6 | 2.4 GHz 11g: 6/9, 12/18, 24/36, 48/54 |
| 7–10 | 2.4 GHz HT20, MCS 0/1, 2/3, 4/5, 6/7 |
| 11–16 | 2.4 GHz HE20, MCS 0/1, 2/3, 4/5, 6/7, 8, 9 |
| 17–20 | 2.4 GHz HT40, MCS 0/1, 2/3, 4/5, 6/7 |
| 21–24 | 5 GHz 11a: 6/9, 12/18, 24/36, 48/54 |
| 25–29 | 5 GHz HT20 and VHT20, MCS 0/1, 2/3, 4/5, 6/7, VHT MCS 8 |
| 30–35 | 5 GHz HE20, MCS 0/1, 2/3, 4/5, 6/7, 8, 9 |
| 36–39 | 5 GHz HT40, MCS 0/1, 2/3, 4/5, 6/7 |
| 40 | `fcc_enable`: 1 applies the per-channel limits |
| 41–54 | 2.4 GHz 11b limit, channels 1–14 |
| 55–68 | 2.4 GHz 11g limit, channels 1–14 |
| 69–82 | 2.4 GHz HT20 limit, channels 1–14 |
| 83–96 | 2.4 GHz HE20 limit, channels 1–14 |
| 97–105 | 2.4 GHz HT40 limit, channels 3–11 |
| 106–133 | 5 GHz 11a limit, 28 channels: 36–64, 100–144, 149–177, by 4 |
| 134–161 | 5 GHz HT20 and VHT20 limit, the same channels |
| 162–189 | 5 GHz HE20 limit, the same channels |
| 190–203 | 5 GHz HT40 limit, 14 channels: 38–62, 102–142, 151–175, by 8 |
| 204–252 | reserved |
| 253 | `multiple_bins_enable` |
| 254 | `certification_code` |
| 255 | CRC-8 |

The C6's has a crystal byte at index 1; the C5's has none.
The tool sets a certification's limits on the C5 by `init_para_chg 1 i v` for each i from 1 to 203, the C6's from 2 to 75.

## The host tool's C5 specifics

From the tool's decompiled Python (`../../esp32c6/hw/rftest.md` has the rest):

- It loads the firmware through the ROM with esptool's `ESP32C5ROM`;
  in its flash mode it writes the image at 0x2000, the C5 ROM's bootloader offset, where the C6's goes to 0x0.
- Its `read_flash_c5` takes the bytes from the line after a header that ends in `:`, and waits for the line `type_len: 1`,
  where on other chips it waits for `FLASH READ END.`; this library's `read_flash` prints "type_len: %d" after the bytes.
- At 5 GHz it sends the channel as its centre frequency in MHz, 5000 + 5 × channel, 5190 for a 40 MHz pair, and never sends `test_5g`.
- `get_5g_en`'s reply is parsed as its second integer.
- When it stops a tone at 5 GHz it sends `wifiscwout 0` with the list's index, not the MHz, which the stop ignores (**inferred**).
