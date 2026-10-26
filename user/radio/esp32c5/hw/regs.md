# The MMIO words the C5's libraries touch

Every word of the ESP32-C5 that `librftest.a`, `libphy.a` and `libbtbb.a` read or write at a constant address, about 520,
by block, one line a word, or a run of words the same functions touch alike, with the functions.
A word reached through an address computed at run time is not here: the analog I2C master's host words among them, which `phy.md` explains.
Blocks and their names are `README.md`'s; sources and labels too.
Nothing here ran on a C5.

Every line is **known**: the disassembly's `lui`/`addi` pair, held to Ghidra's pseudo-C of the same function.
A function without a mark is in both.
**(asm)** marks a function only the disassembly shows touching the word:
Ghidra drops a read whose value is unused (`mem_read_func`, `phy_wait_freq_set_busy_new`),
a word written back unchanged (`bt_bb_set_max_gain`'s 0x600A2888),
and what follows a call it takes for one that does not return (`phy_check_rssi_1m`).
**(G)** marks one only the pseudo-C shows, a callee Ghidra inlined or a queue table it resolved; its R/W is `?` when no other function gives it.
R/W says whether the functions read, write, or both.


## I2C0

| Address | R/W | Functions |
|---|---|---|
| 0x60004000 | R | `phy_reg_check` |

## APB_SARADC

| Address | R/W | Functions |
|---|---|---|
| 0x6000E000 | RW | `phy_get_pll_vol` (G), `phy_get_sar2_vol`, `phy_init_clk_set`, `phy_pwdet_reg_init`, `phy_pwdet_tone_start`, `phy_reg_check` |
| 0x6000E058 | RW | `phy_set_tsens_power`, `phy_set_tsens_power_`, `phy_tsens_code_read`, `phy_tsens_temp_read_local` |
| 0x6000E05C | RW | `phy_set_tsens_pwr`, `phy_tsens_read_init` |

## USB_SERIAL_JTAG

| Address | R/W | Functions |
|---|---|---|
| 0x6000F018 | RW | `ESP_TEST_GPIO` |

## IO_MUX

| Address | R/W | Functions |
|---|---|---|
| 0x60090004 | RW | `CMDSTOP_GPIO`, `ESP_TEST_GPIO` |

## HP_SYSTEM

| Address | R/W | Functions |
|---|---|---|
| 0x60095004 | RW | `adctrig` |

## PCR

| Address | R/W | Functions |
|---|---|---|
| 0x60096000 | R | `phy_reg_check` |
| 0x60096018 | RW | `read_flash_init_para`, `test_rf_cal_level`, `tx_signal_test` |
| 0x60096088 | RW | `phy_rf_init`, `phy_tsens_read_init`, `phy_wakeup_init` |
| 0x60096090 | RW | `phy_tsens_read_init` |
| 0x60096094 | RW | `run_rftest_case` |
| 0x6009614C | RW | `phy_rf_init`, `phy_wakeup_init` |

## HP_APM

| Address | R/W | Functions |
|---|---|---|
| 0x600990C4 | RW | `rftest_init` |

## FECOEX

| Address | R/W | Functions |
|---|---|---|
| 0x600A0000 | R | `phy_reg_buff`, `phy_reg_check` |
| 0x600A0008 | RW | `bt_rx_force`, `force_txon_mode` |
| 0x600A0010 | RW | `coex_pti_v2` |
| 0x600A001C | RW | `phy_chip_set_chan_ana` (G), `phy_write_chan_freq` (G), `ble_freq_offset_test`, `bt_get_channel_pwr_set`, `bt_get_channel_pwr_unset`, `bt_set_chn`, `force_txon_mode`, `phy_dis_hw_set_freq_new` and 10 more |
| 0x600A0020 | RW | `phy_freq_reg_init_new`, `phy_read_rf_freq_mem` |
| 0x600A0024 | RW | `phy_freq_reg_init_new`, `phy_get_freq_mem_param` |
| 0x600A0028 | RW | `phy_get_freq_mem_param` (G), `phy_set_freq` (G), `phy_chip_set_chan`, `phy_freq_reg_init_new`, `phy_set_freq_sw_start`, `phy_wakeup_init` |
| 0x600A002C | W | `phy_freq_i2c_mem_write` |
| 0x600A0030 | RW | `phy_wait_freq_set_busy_new` (asm), `ble_freq_offset_test`, `phy_freq_i2c_num_addr`, `phy_read_rf_freq_mem` |
| 0x600A0034–0x600A003C | W | `phy_freq_i2c_num_addr` |
| 0x600A0040 | R | `phy_read_rf_freq_mem` |
| 0x600A0044 | RW | `ble_freq_offset_test` |

## FEDATA

| Address | R/W | Functions |
|---|---|---|
| 0x600A0400 | RW | `phy_close_fe_bb_clk`, `phy_open_fe_bb_clk`, `phy_reg_check` |
| 0x600A0408 | RW | `esp_tx_func` (G), `wifitxout` (G), `WifiTxStart`, `esp_tx_func_org`, `force_dig_gain`, `phy_fe_reg_init`, `phy_force_pwr_index`, `phy_set_tx_gain_mem` and 2 more |
| 0x600A040C | RW | `phy_fe_reg_init`, `phy_start_tx_tone_step`, `phy_stop_tx_tone` |
| 0x600A0410–0x600A0414 | RW | `phy_i2c_txrate_init_new` (G), `phy_txgain_comp_pacfg_new_part_2` (G), `phy_txgain_comp_pacfg_part_1` (G), `phy_start_tx_tone_step`, `phy_txgain_comp_pacfg`, `phy_txgain_comp_pacfg_new` |
| 0x600A0418 | RW | `phy_txdc_cal` |
| 0x600A041C | RW | `phy_get_tone_sar_dout`, `phy_pwdet_tone_start`, `phy_rfcal_txiq`, `phy_start_tx_tone_step`, `phy_stop_tx_tone`, `phy_txiq_get_mis_pwr`, `pwdet_debug_test`, `start_tx_tone` |
| 0x600A0420 | RW | `phy_start_tx_tone_step`, `phy_stop_tx_tone` |
| 0x600A0428 | RW | `phy_start_tx_tone_step`, `phy_txiq_get_mis_pwr`, `pwdet_debug_test` |
| 0x600A0430 | RW | `phy_rx_filter_mode` |
| 0x600A0434 | RW | `phy_band_reg`, `phy_iq_swap_set` |
| 0x600A0438 | RW | `force_iq_set`, `phy_fe_reg_init`, `phy_iq_corr_enable`, `phy_rfcal_rxiq`, `phy_rxiq_set_reg`, `phy_set_rx_gain_cal_iq` |
| 0x600A043C | RW | `phy_fe_reg_init`, `phy_rxiq_scale_set` |
| 0x600A0440 | RW | `phy_fe_txrx_reset` |
| 0x600A0444 | RW | `phy_agc_max_gain_set`, `phy_fe_reg_init` |
| 0x600A0448 | RW | `phy_i2c_bbpll_set` (G), `phy_set_freq` (G), `phy_adc_rate_set`, `phy_chip_set_chan`, `phy_dac_rate_set`, `phy_wr_rx_gain_mem` |
| 0x600A044C | RW | `phy_iq_est_enable_new` |
| 0x600A0450 | RW | `phy_dc_iq_est_new` (G), `phy_rxiq_get_mis` (G), `phy_iq_est_disable`, `phy_iq_est_enable_new` |
| 0x600A0454–0x600A0460 | R | `phy_get_rx_sig_pwr` (G), `phy_corr_get_pwr`, `phy_get_iq_est_snr`, `phy_rx_sig_pwr_sub`, `phy_rxiq_get_mis`, `ram_get_corr_power`, `rxiq_get_pwr` |
| 0x600A0464–0x600A0468 | R | `dc_iq_est_test`, `phy_dc_iq_est_new`, `phy_get_iq_est_snr`, `ram_get_corr_power` |
| 0x600A046C | R | `dc_iq_est_test`, `get_iq_est_pwr`, `phy_dc_iq_est_new`, `phy_get_iq_est_snr`, `phy_rfrx_gain_cal`, `phy_rxiq_get_mis`, `ram_get_corr_power` |
| 0x600A0470–0x600A0474 | R | `dc_iq_est_test` |
| 0x600A047C | R | `phy_iq_est_enable_new` |

## FECTRL

| Address | R/W | Functions |
|---|---|---|
| 0x600A0800 | RW | `adctrig`, `phy_close_fe_bb_clk`, `phy_init_clk_set`, `phy_open_fe_bb_clk`, `phy_reg_buff`, `phy_reg_check` |
| 0x600A0808 | RW | `phy_en_pwdet`, `phy_pkdet_vol_start`, `phy_pwdet_reg_init`, `phy_pwdet_sar2_init`, `phy_pwdet_tone_start`, `phy_txdc_cal_pwdet_init` |
| 0x600A080C | RW | `phy_get_pll_vol` (G), `get_tx_rate`, `phy_get_sar2_vol`, `phy_get_tx_rate`, `phy_pwdet_sar2_init`, `phy_pwdet_tone_start`, `phy_pwdet_wait_idle` |
| 0x600A0810 | W | `phy_pwdet_reg_init` |
| 0x600A0814 | RW | `phy_pwdet_reg_init`, `phy_txdc_cal_pwdet_init` |
| 0x600A0818 | W | `phy_pwdet_ref_code`, `phy_pwdet_reg_init`, `phy_pwdet_sar2_init` |
| 0x600A081C | R | `phy_read_sar_dout` |
| 0x600A0834 | RW | `phy_ant_wifirx_cfg`, `phy_ant_wifitx_cfg`, `phy_dig_reg_backup`, `phy_tx_state_set` |
| 0x600A0838 | RW | `phy_ant_bttx_cfg`, `phy_ant_wifirx_cfg`, `phy_dig_reg_backup`, `phy_tx_state_set` |
| 0x600A083C | RW | `phy_ant_btrx_cfg`, `phy_ant_bttx_cfg`, `phy_dig_reg_backup`, `phy_tx_state_set` |
| 0x600A0840 | RW | `phy_ant_btrx_cfg`, `phy_dig_reg_backup`, `phy_tx_state_set` |
| 0x600A0844 | RW | `phy_write_gain_mem`, `phy_write_pbus_mem` |
| 0x600A0848 | W | `phy_write_gain_mem`, `phy_write_pbus_mem` |
| 0x600A084C–0x600A0850 | W | `phy_write_gain_mem` |
| 0x600A0854–0x600A0868 | RW | `phy_set_pbus_mem` (G), `phy_write_pbus_mem` (G), `phy_pbus_reg_store`, `phy_set_pbus_reg` |
| 0x600A086C | RW | `phy_tx_paon_set`, `run_rftest_case` |
| 0x600A0870 | RW | `ble_tx_config_check`, `bt_bb_v2_init_cmplx`, `phy_fe_reg_init`, `phy_tx_paon_set` |
| 0x600A0874 | RW | `phy_bt_filter_reg`, `phy_wifi_fbw_sel` |
| 0x600A0880 | R | `phy_set_pbus_mem` (G), `phy_write_pbus_mem` (G), `phy_pbus_reg_store` |
| 0x600A0884 | RW | `phy_pbus_clear_reg` (G), `phy_pbus_debugmode` (G), `phy_pbus_set_dco` (G), `phy_pbus_set_rftx1_5g` (G), `phy_pbus_set_rxgain` (G), `phy_pbus_workmode` (G), `phy_pbus_xpd_dpd_path` (G), `phy_pbus_xpd_rx_off` (G) and 4 more |
| 0x600A0888 | RW | `phy_band_reg`, `phy_fe_reg_init` |
| 0x600A088C | RW | `phy_pbus_clear_reg` (G), `phy_pbus_debugmode` (G), `phy_pbus_set_rftx1_5g` (G), `phy_pbus_workmode` (G), `phy_set_rx_pbus_freq` (G), `phy_close_pa`, `phy_pbus_force_mode` |
| 0x600A0890 | RW | `phy_pbus_set_dco` (G), `phy_pbus_set_rxgain` (G), `phy_pbus_xpd_dpd_path` (G), `phy_pbus_xpd_rx_off` (G), `phy_pbus_xpd_tx_off` (G), `rf_freq_debug` (G), `txon_set_part_2` (G), `bt_rx_force` and 11 more |
| 0x600A0894 | RW | `phy_fe_reg_init` |
| 0x600A08BC | RW | `phy_agc_max_gain_set` |
| 0x600A08CC | RW | `set_dump_mode` |
| 0x600A08D0 | ? | `phy_iq_est_enable_new` (G) |
| 0x600A08DC | RW | `mem_read_func` (asm), `mem_write_func`, `test_mem_time` |

## FEDATA_WIFI and FEDATA_BLE

| Address | R/W | Functions |
|---|---|---|
| 0x600A0C00 | R | `phy_reg_check` |
| 0x600A0C04 | RW | `phy_start_tx_tone_step` (G), `phy_stop_tx_tone` (G), `phy_dac_scale_set` |
| 0x600A0C08 | RW | `phy_iq_swap_set` |
| 0x600A0C0C | RW | `force_iq_set`, `phy_fe_reg_init`, `phy_iq_corr_enable`, `phy_rfcal_txiq`, `phy_set_rx_gain_cal_iq`, `phy_txiq_set_reg` |
| 0x600A0C14 | RW | `phy_bb_cfo_cfg` |
| 0x600A0C18–0x600A0C1C | RW | `phy_bb_cfo_cfg`, `phy_freq_correct` |
| 0x600A0C20 | RW | `phy_fe_reg_init`, `phy_freq_correct` |
| 0x600A0C38 | RW | `phy_pkdadc_set`, `phy_rx_pkdet_num_set` |
| 0x600A0C3C | RW | `phy_pkdadc_set`, `phy_rx_pkdet_dc_cal` |
| 0x600A0C50 | R | `phy_get_pkdet_data`, `read_pkdet_original` |

## btmac

| Address | R/W | Functions |
|---|---|---|
| 0x600A1000 | RW | `phy_check_rssi_1m` (asm), `phy_reg_check` |
| 0x600A1010 | RW | `phy_check_rssi_1m` (asm) |
| 0x600A1100 | RW | `phy_check_rssi_1m` (asm) |
| 0x600A110C–0x600A1110 | RW | `phy_check_rssi_1m` (asm) |
| 0x600A1508 | RW | `phy_check_rssi` (G), `phy_check_rssi_1m` |
| 0x600A1650 | RW | `phy_check_rssi` (G), `ble_tx_config_check`, `phy_check_rssi_1m` |
| 0x600A1FF0 | R | `phy_reg_check` |

## bt_v3_2, the BT baseband

| Address | R/W | Functions |
|---|---|---|
| 0x600A2000 | RW | `bt_bb_rx_set`, `phy_reg_buff`, `phy_reg_check` |
| 0x600A2004 | RW | `bt_get_channel_pwr_set`, `bt_get_channel_pwr_unset` |
| 0x600A200C | RW | `bt_bb_coex_config`, `bt_bb_rx_set`, `bt_bb_v2_tx_set` |
| 0x600A2010 | RW | `bt_bb_tx_cca_en` (G), `phy_check_rssi` (G), `ble_tx_config_check`, `bt_bb_get_le_tx_on_delay`, `bt_bb_get_zb_tx_on_delay`, `bt_bb_rx_set`, `bt_bb_set_le_tx_on_delay`, `bt_bb_set_zb_tx_on_delay` and 2 more |
| 0x600A2014 | RW | `bt_bb_v2_init_cmplx` |
| 0x600A2018 | RW | `ble_tx_config_check`, `bt_bb_v2_tx_set` |
| 0x600A201C | R | `zb_tx_config_check` |
| 0x600A202C–0x600A2030 | RW | `bt_bb_rx_dpo_set` |
| 0x600A203C–0x600A2040 | RW | `bt_bb_rx_dpo_set` |
| 0x600A204C–0x600A2054 | RW | `bt_bb_rx_set` |
| 0x600A2058 | RW | `bt_agc_v2_set` |
| 0x600A2060 | RW | `bt_bb_corr_thresh_aa_lc_en`, `bt_bb_rx_correlator_set`, `bt_bb_set_corr_thresh_le` |
| 0x600A2064 | RW | `bt_bb_rx_correlator_set`, `bt_bb_set_corr_thresh_le` |
| 0x600A2068 | RW | `bt_agc_restart_set`, `bt_bb_rx_correlator_set` |
| 0x600A2088 | R | `phy_check_rssi_1m` (asm) |
| 0x600A2090 | RW | `phy_check_rssi_1m` (asm) |
| 0x600A20AC | RW | `adctrig`, `bt_bb_ble_diag_all` |
| 0x600A20B0 | R | `bt_bb_get_cur_rx_info`, `bt_get_channel_pwr` |
| 0x600A20B4 | RW | `adctrig` |
| 0x600A20B8 | RW | `phy_check_rssi` (G), `ble_tx_config_check`, `bt_bb_tx_cca_period`, `bt_bb_tx_cca_set`, `phy_check_rssi_1m` |
| 0x600A20BC–0x600A20C0 | RW | `bt_bb_rx_set`, `bt_bb_tx_cca_set` |
| 0x600A20D4 | RW | `phy_check_rssi_1m` (asm), `bt_bb_tx_cca_fifo_count`, `bt_bb_tx_cca_fifo_empty`, `bt_bb_tx_cca_fifo_full`, `bt_bb_tx_cca_fifo_reset` |
| 0x600A20D8 | R | `phy_check_rssi_1m` (asm), `bt_bb_tx_cca_fifo_read` |
| 0x600A20DC | RW | `bt_bb_wdt_init` |
| 0x600A20E0–0x600A20EC | RW | `set_gauss_coeff_1m` |
| 0x600A20F0–0x600A20F4 | RW | `set_gauss_coeff_2m` |
| 0x600A20F8 | RW | `bt_bb_rx_correlator_set`, `bt_bb_set_corr_thresh_le` |
| 0x600A20FC | RW | `bt_bb_rx_correlator_set`, `bt_bb_rx_filter_sel` |
| 0x600A2100 | RW | `bt_agc_restart_set`, `bt_bb_rx_correlator_set` |
| 0x600A210C | RW | `bt_bb_ble_diag`, `bt_bb_ble_diag_all` |
| 0x600A2124 | RW | `ble_bb_cte_get_cte_ant_force_on` (asm), `ble_bb_cte_get_cte_en` (asm), `ble_bb_cte_get_cte_ant_force`, `ble_bb_cte_get_cte_delay_rx_aoa`, `ble_bb_cte_get_cte_delay_rx_aod`, `ble_bb_cte_get_cte_iq_sel_1us`, `ble_bb_cte_get_cte_iq_sel_2us`, `ble_bb_cte_set_cte_ant_force` and 6 more |
| 0x600A2128 | RW | `ble_bb_cte_get_cte_ant_delay_rx_aoa_1us`, `ble_bb_cte_get_cte_ant_delay_rx_aoa_2us`, `ble_bb_cte_get_cte_ant_delay_tx_aod_1us`, `ble_bb_cte_get_cte_ant_delay_tx_aod_2us`, `ble_bb_cte_set_cte_ant_delay_rx_aoa_1us`, `ble_bb_cte_set_cte_ant_delay_rx_aoa_2us`, `ble_bb_cte_set_cte_ant_delay_tx_aod_1us`, `ble_bb_cte_set_cte_ant_delay_tx_aod_2us` |
| 0x600A212C | RW | `ble_bb_cte_get_cte_ant_switch_delay_rx_aoa_1us`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aoa_2us`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aod_1us`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aod_2us`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aoa_1us`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aoa_2us`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aod_1us`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aod_2us` |
| 0x600A2150 | RW | `ble_bb_cte_get_cte_samp_limit_ref`, `ble_bb_cte_get_cte_samp_limit_samp`, `ble_bb_cte_get_cte_samp_limit_switch`, `ble_bb_cte_set_cte_samp_limit_ref`, `ble_bb_cte_set_cte_samp_limit_samp`, `ble_bb_cte_set_cte_samp_limit_switch` |
| 0x600A2154 | RW | `ble_bb_cte_get_cte_delay_rx_aoa_tail_1us`, `ble_bb_cte_get_cte_delay_rx_aoa_tail_1us_2m`, `ble_bb_cte_get_cte_delay_rx_aod_tail_1us`, `ble_bb_cte_get_cte_delay_rx_aod_tail_1us_2m`, `ble_bb_cte_set_cte_delay_rx_aoa_tail_1us`, `ble_bb_cte_set_cte_delay_rx_aoa_tail_1us_2m`, `ble_bb_cte_set_cte_delay_rx_aod_tail_1us`, `ble_bb_cte_set_cte_delay_rx_aod_tail_1us_2m` |
| 0x600A2158 | RW | `ble_bb_cte_get_cte_ant_switch_delay_rx_aoa_1us_2m`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aoa_2us_2m`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aod_1us_2m`, `ble_bb_cte_get_cte_ant_switch_delay_rx_aod_2us_2m`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aoa_1us_2m`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aoa_2us_2m`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aod_1us_2m`, `ble_bb_cte_set_cte_ant_switch_delay_rx_aod_2us_2m` |
| 0x600A215C | RW | `ble_bb_cte_get_fstep_cte`, `ble_bb_cte_get_fstep_cte_en`, `ble_bb_cte_get_nfstep_cte`, `ble_bb_cte_set_fstep_cte`, `ble_bb_cte_set_fstep_cte_en`, `ble_bb_cte_set_nfstep_cte`, `ble_cte_freq_adjust` |
| 0x600A2160 | RW | `ble_bb_cte_get_cte_delay_rx_aoa_tail_2us`, `ble_bb_cte_get_cte_delay_rx_aoa_tail_2us_2m`, `ble_bb_cte_get_cte_delay_rx_aod_tail_2us`, `ble_bb_cte_get_cte_delay_rx_aod_tail_2us_2m`, `ble_bb_cte_set_cte_delay_rx_aoa_tail_2us`, `ble_bb_cte_set_cte_delay_rx_aoa_tail_2us_2m`, `ble_bb_cte_set_cte_delay_rx_aod_tail_2us`, `ble_bb_cte_set_cte_delay_rx_aod_tail_2us_2m` |
| 0x600A2164 | RW | `ble_bb_cte_get_cte_sqi_delay`, `ble_bb_cte_get_cte_sqi_delay_2m`, `ble_bb_cte_get_cte_sqi_en`, `ble_bb_cte_get_cte_sqi_sel`, `ble_bb_cte_set_cte_sqi_delay`, `ble_bb_cte_set_cte_sqi_delay_2m`, `ble_bb_cte_set_cte_sqi_en`, `ble_bb_cte_set_cte_sqi_sel` |
| 0x600A2168 | RW | `ble_bb_cte_get_cte_sqi_dphi_db`, `ble_bb_cte_get_cte_sqi_dphi_max`, `ble_bb_cte_get_cte_sqi_dphi_min`, `ble_bb_cte_get_cte_sqi_ready`, `ble_bb_cte_set_cte_sqi_dphi_db`, `ble_bb_cte_set_cte_sqi_dphi_max`, `ble_bb_cte_set_cte_sqi_dphi_min`, `ble_bb_cte_set_cte_sqi_ready` |
| 0x600A216C | RW | `ble_bb_cte_get_cte_sqi_inband_max`, `ble_bb_cte_get_cte_sqi_inband_min`, `ble_bb_cte_get_cte_sqi_rssi_max`, `ble_bb_cte_get_cte_sqi_rssi_min`, `ble_bb_cte_set_cte_sqi_inband_max`, `ble_bb_cte_set_cte_sqi_inband_min`, `ble_bb_cte_set_cte_sqi_rssi_max`, `ble_bb_cte_set_cte_sqi_rssi_min` |
| 0x600A2170 | RW | `ble_freq_offset_test`, `bt_bb_rx_set`, `bt_bb_v2_tx_set`, `run_rftest_case` |
| 0x600A2174 | RW | `bt_bb_tx_cca_free_timer_set`, `bt_bb_tx_cca_free_timer_sw_start` |
| 0x600A2178 | RW | `bt_bb_tx_cca_free_timer_sw_start_value` |
| 0x600A217C | R | `bt_bb_tx_cca_free_timer_cnt`, `bt_bb_tx_cca_free_timer_is_sw_start`, `bt_bb_tx_cca_free_timer_last_chan` |
| 0x600A2184 | RW | `bt_bb_coex_config`, `force_rx_gain` |
| 0x600A2188 | RW | `ble_rx_start_rst_en` |
| 0x600A21C0 | RW | `ble_cte_freq_adjust` |
| 0x600A21C4 | RW | `bt_bb_rx_dpo_set` |
| 0x600A21FC | RW | `ble_bb_cte_get_clk_en` (asm), `ble_bb_cte_get_clk_force_on` (asm), `ble_bb_cte_get_bt_bb_date`, `ble_bb_cte_set_clk_en`, `ble_bb_cte_set_clk_force_on`, `bt_bb_rx_dpo_set` |

## btagc

| Address | R/W | Functions |
|---|---|---|
| 0x600A2800 | RW | `ble_bb_cte_get_dc_shift_max`, `ble_bb_cte_set_dc_shift_max`, `bt_bb_rx_set`, `phy_reg_buff`, `phy_reg_check` |
| 0x600A2820 | RW | `ble_bb_cte_get_re_gain_enable`, `ble_bb_cte_set_re_gain_enable` |
| 0x600A2828–0x600A282C | RW | `bt_agc_v2_set` |
| 0x600A2840 | RW | `bt_bb_set_max_gain` (G), `ble_bb_get_agc_target_value`, `ble_bb_set_agc_target_value`, `bt_agc_gain_max`, `bt_agc_target_set`, `force_rx_gain`, `phy_rx_gain_force` |
| 0x600A2844 | RW | `bt_set_rx_comp` |
| 0x600A2848 | RW | `bt_agc_gain_offset`, `phy_bt_gain_offset` |
| 0x600A284C | RW | `bt_bb_rx_set`, `phy_rfagc_disable` |
| 0x600A2850 | RW | `bt_bb_set_max_gain` (G), `bt_agc_gain_max`, `rx_max_gain` |
| 0x600A2854 | RW | `bt_agc_gain_set`, `bt_agc_v2_set` |
| 0x600A2868 | RW | `bt_agc_gain_offset`, `bt_agc_recorrect_set`, `phy_bt_gain_offset` |
| 0x600A286C | RW | `bt_agc_recorrect_set` |
| 0x600A2884 | RW | `bt_agc_restart_set` |
| 0x600A2888 | RW | `bt_bb_set_max_gain` (asm), `bt_agc_restart_set`, `bt_bb_rx_set` |
| 0x600A288C | RW | `bt_bb_rx_set` |
| 0x600A2894 | RW | `bt_agc_target_set` |
| 0x600A2898 | RW | `ble_bb_cte_get_agc_target_value`, `ble_bb_cte_set_agc_target_value`, `bt_agc_target_set` |
| 0x600A28A0 | RW | `ieee802154_set_rx_sense` (G), `bt_agc_detect_set`, `bt_agc_rssi_thresh`, `bt_bb_set_rx_sense` |
| 0x600A28A8 | RW | `bt_agc_detect_set` |
| 0x600A28AC | RW | `ieee802154_set_rx_sense` (G), `bt_agc_detect_set`, `bt_agc_rssi_thresh`, `bt_bb_set_rx_sense` |
| 0x600A28B0–0x600A28B4 | RW | `bt_agc_recorrect_set`, `bt_agc_target_set` |
| 0x600A28B8 | RW | `ieee802154_set_rx_sense` (G), `bt_agc_detect_set`, `bt_agc_rssi_thresh`, `bt_bb_set_rx_sense` |
| 0x600A28BC | RW | `bt_agc_recorrect_set`, `bt_agc_restart_set` |
| 0x600A28C0–0x600A28C4 | RW | `bt_agc_detect_set` |
| 0x600A28C8 | RW | `bt_agc_v2_set` |
| 0x600A28D0–0x600A28D4 | RW | `bt_agc_v2_set` |
| 0x600A28DC–0x600A28F0 | RW | `bt_agc_restart_set` |
| 0x600A28F8 | RW | `bt_agc_restart_set` |
| 0x600A2900 | RW | `bt_agc_restart_set` |

## zbbb

| Address | R/W | Functions |
|---|---|---|
| 0x600A2C04 | RW | `bt_bb_rx_set`, `zb_rx_core_set` |
| 0x600A2C08 | RW | `zb_rx_core_set` |
| 0x600A2C0C | RW | `bt_bb_rx_set`, `zb_rx_v0_0_set` |
| 0x600A2C10–0x600A2C18 | RW | `zb_rx_v0_0_set` |
| 0x600A2C20–0x600A2C24 | RW | `zb_rx_core_set` |
| 0x600A2C28–0x600A2C34 | RW | `zb_rx_v0_0_set` |
| 0x600A2C48 | RW | `bt_bb_v2_rx_set` (G), `zb_rx_auto_sel` |

## 802.15.4 MAC

| Address | R/W | Functions |
|---|---|---|
| 0x600A3104 | RW | `ieee802154_txon_delay_set`, `zb_tx_config_check` |
| 0x600A310C–0x600A3114 | RW | `ieee802154_txon_delay_set`, `zb_tx_config_check` |

## Wi-Fi MAC

| Address | R/W | Functions |
|---|---|---|
| 0x600A4000 | W | `set_mac_filter` |
| 0x600A4004 | RW | `set_mac_filter` |
| 0x600A4038 | RW | `phy_11ax_rx_set` |
| 0x600A405C | RW | `ate_fill_frame`, `burnin_fill_beacon`, `fill_txaddr`, `fill_txdataframe`, `mac_init`, `set_mac_filter`, `setmacaddr` |
| 0x600A4060 | RW | `ate_fill_frame`, `burnin_fill_beacon`, `fill_txaddr`, `fill_txdataframe`, `get_rx_buffer`, `mac_init`, `set_mac_filter`, `setmacaddr` |
| 0x600A407C | RW | `auto_ack_test`, `do_rx_poll`, `get_rx_buffer`, `get_rxctrl_addr`, `mac_init`, `tx_ack_init` |
| 0x600A4080 | RW | `ate_rxframe_fb`, `auto_ack_test`, `clear_macrxfilter`, `get_rx_buffer`, `rx_buffer_ena`, `rx_buffer_init`, `rx_link_des`, `tx_ack_init` |
| 0x600A4084 | W | `rx_ampdu_buffer_init`, `rx_buffer_init` |
| 0x600A4090 | R | `do_rx_poll`, `get_rxctrl_addr` |
| 0x600A4098 | RW | `fft_dump_init` |
| 0x600A40A4 | W | `ate_rxframe_fb` |
| 0x600A40A8 | R | `beacon_print`, `do_rx_poll` |
| 0x600A40AC | R | `tx_ack_start` (G), `beacon_print`, `do_rx_poll` |
| 0x600A40B0 | R | `beacon_print` (asm), `do_rx_poll` |
| 0x600A40D8 | RW | `clear_macrxfilter`, `get_rx_buffer`, `mac_init`, `rx_per_init`, `set_macrxfilter` |
| 0x600A40DC–0x600A40E4 | RW | `clear_macrxfilter`, `get_rx_buffer`, `mac_init`, `rx_per_init`, `set_mac_filter`, `set_macrxfilter` |
| 0x600A4308 | R | `phy_mac_debug_array` |
| 0x600A4380–0x600A4384 | R | `phy_mac_debug_array` |
| 0x600A4400 | RW | `phy_bb_cbw_chan_cfg` |
| 0x600A444C–0x600A4454 | W | `ack_rate_tab` |
| 0x600A448C–0x600A4490 | RW | `phy_i2c_txrate_init_new` |
| 0x600A4494 | RW | `phy_11ax_tx_mode` |
| 0x600A44C4–0x600A44D4 | RW | `phy_11ax_tx_mode` |
| 0x600A4800 | W | `beacon_print`, `crypto_disable`, `do_rx_poll` |
| 0x600A4814 | W | `crypto_disable` |
| 0x600A4C24 | RW | `bt_bb_rx_set`, `mac_init` |
| 0x600A4C40 | RW | `burnin_test_func`, `mac_init`, `trig_tx_frame`, `tx_a_frame`, `tx_data_frame` |
| 0x600A4C48 | R | `phy_check_rssi` (G), `phy_check_rssi_20m` (G), `beacon_print`, `burnin_test_func`, `do_rx_poll`, `fft_dump_init`, `phy_sigrssi_rx_frame`, `trig_tx_frame` and 2 more |
| 0x600A4C4C | RW | `phy_check_rssi` (G), `phy_check_rssi_20m` (G), `beacon_print`, `burnin_test_func`, `do_rx_poll`, `fft_dump_init`, `phy_sigrssi_rx_frame`, `trig_tx_frame` and 2 more |
| 0x600A4C50 | RW | `tx_ack_init` |
| 0x600A4C54–0x600A4C58 | RW | `phy_sifs_reg_init` |
| 0x600A4C5C | RW | `mac_init`, `phy_disable_cca`, `phy_enable_cca` |
| 0x600A4C70 | R | `do_rx_poll`, `get_rxctrl_addr` |
| 0x600A4C74 | R | `esp_tx_func` (G), `wifitxout` (G), `burnin_test_func`, `esp_tx_func_org`, `run_rftest_case`, `wifitxout_func` |
| 0x600A4C8C | RW | `fill_tx_frame`, `mac_init` |
| 0x600A4C90 | RW | `tx_nominal_pe_set` |
| 0x600A4C9C | RW | `ate_rxframe_fb`, `auto_ack_test`, `tx_ack_init` |
| 0x600A4CA0 | RW | `tx_cuntom_type_set` |
| 0x600A4CA4 | RW | `ate_rxframe_fb` |
| 0x600A4CA8 | RW | `tx_a_frame` (G), `phy_check_rssi` |
| 0x600A4CB0 | W | `tx_a_frame` (G), `WifiTxStart`, `burnin_test_func` |
| 0x600A4CF8 | R | `BackOffCountGet` |
| 0x600A4D08 | R | `BackOffCountGet` |
| 0x600A4D18 | R | `BackOffCountGet` |
| 0x600A4D28 | R | `BackOffCountGet` |
| 0x600A4D38 | R | `BackOffCountGet` |
| 0x600A4D48 | R | `BackOffCountGet` |
| 0x600A4D58 | R | `BackOffCountGet` |
| 0x600A4D68 | RW | `ConfAddrGet` (G), `BackOffCountGet`, `burnin_rtc_init`, `fill_tx_frame`, `mac_init` |
| 0x600A4D6C | W | `Plcp0AddrGet` (G), `fill_tx_frame` |
| 0x600A4DD8 | RW | `mac_init`, `phy_check_rssi` |
| 0x600A4DDC | RW | `mac_init` |
| 0x600A4E38 | RW | `adctrig` |
| 0x600A4E5C | ? | `tx_a_frame` (G) |
| 0x600A54B0 | W | `Plcp1AddrGet` (G), `fill_tx_frame` |
| 0x600A54C0 | W | `HTsigAddrGet` (G), `fill_tx_frame` |
| 0x600A54C8 | RW | `Vhtsig2AddrGet` (G), `fill_tx_frame` |
| 0x600A54D0 | RW | `Hesig2AddrGet` (G), `fill_tx_frame` |
| 0x600A54D8 | RW | `set_rate_power` (G), `max_power_api_test`, `set_mactx_pwrdb` |
| 0x600A54E4 | RW | `HT40LENAddrGet` (G), `HT40LenAddrGet` (G), `fill_tx_frame` |
| 0x600A5514 | R | `tx_data_frame` |
| 0x600A5C00 | R | `fft_dump_init`, `phy_rxevm_calcu` |

## agc

| Address | R/W | Functions |
|---|---|---|
| 0x600A7000 | R | `phy_reg_check` |
| 0x600A7008–0x600A700C | RW | `fft_dump_init`, `phy_check_sigrssi_en` |
| 0x600A7010 | RW | `fft_dump_init`, `phy_check_sigrssi_en`, `phy_dig_reg_backup`, `phy_rx_sense_set`, `phy_set_rx_sense` |
| 0x600A7014 | RW | `phy_dig_reg_backup`, `phy_rx_11b_opt`, `phy_rx_sense_set`, `phy_set_rx_sense` |
| 0x600A7018 | RW | `beacon_print`, `check_noise_floor`, `phy_check_sigrssi_en`, `phy_dig_reg_backup`, `phy_noise_floor_auto_set`, `phy_set_cca`, `set_noise_floor` |
| 0x600A701C | RW | `phy_dig_reg_backup`, `phy_get_cca`, `phy_set_cca`, `phy_set_rx_sense` |
| 0x600A702C | RW | `phy_pbus_clear_reg` (G), `phy_pbus_set_rftx1_5g` (G), `phy_pbus_workmode` (G), `phy_set_rx_pbus_freq` (G), `adctrig`, `force_rx_gain`, `phy_agc_max_gain_set`, `phy_agc_reg_init_new` and 7 more |
| 0x600A7030 | RW | `phy_bb_cfo_cfg` (G), `fft_dump_init`, `phy_11p_set`, `phy_11p_set_1`, `phy_ant_init`, `phy_check_sigrssi_en`, `phy_disable_agc`, `phy_enable_agc` and 1 more |
| 0x600A7034 | RW | `phy_agc_reg_init_new` |
| 0x600A703C | RW | `phy_bb_dcmem_clr` |
| 0x600A7044 | RW | `bb_agc_reg_update`, `phy_dig_reg_backup`, `phy_rx_11b_opt`, `phy_rx_sense_set`, `phy_set_rx_sense` |
| 0x600A7048 | RW | `bb_agc_reg_update`, `phy_11p_set`, `phy_check_sigrssi_en` |
| 0x600A7050 | R | `check_noise_floor`, `phy_get_noise_floor`, `run_rftest_case` |
| 0x600A7058 | RW | `bb_agc_reg_update` |
| 0x600A705C | RW | `phy_rfagc_disable`, `rfrx_sat_rst` |
| 0x600A7064 | W | `phy_set_rx_comp_new`, `phy_wifi_agc_sat_gain` |
| 0x600A7068 | RW | `phy_pkdadc_set`, `phy_rx_pkdet_num_set` |
| 0x600A706C | R | `beacon_print` (G), `do_rx_poll`, `phy_get_rssi`, `phy_get_sigrssi` |
| 0x600A7074 | RW | `phy_chan_filt_set`, `phy_dig_reg_backup` |
| 0x600A708C | R | `get_rfrx_sat`, `phy_read_hw_noisefloor` |
| 0x600A7090 | RW | `phy_check_sigrssi_en` |
| 0x600A7094 | RW | `fft_dump_init`, `phy_agc_max_gain_set`, `rx_init_gain` |
| 0x600A70A0 | RW | `phy_set_rx_comp_new` |
| 0x600A70A4 | RW | `phy_csidump_force_lltf_cfg` |
| 0x600A70B0 | RW | `phy_check_sigrssi_en` |
| 0x600A70B8 | RW | `set_dump_mode` |
| 0x600A70C4 | RW | `bb_agc_reg_update`, `fft_dump_init`, `phy_check_sigrssi_en` |
| 0x600A70CC | RW | `phy_dig_reg_backup`, `phy_set_rx_sense` |
| 0x600A70DC | RW | `force_txon` |
| 0x600A70EC | RW | `fft_dump_init`, `phy_check_sigrssi_en` |
| 0x600A70F4 | RW | `phy_wifi_rifs_mode_en` |
| 0x600A7104 | RW | `bb_agc_reg_update`, `phy_rx_11b_opt` |
| 0x600A7108 | RW | `phy_dig_reg_backup`, `phy_rx_sense_set` |
| 0x600A7114 | W | `phy_set_rx_comp_new`, `phy_wifi_agc_sat_gain` |
| 0x600A711C | RW | `phy_ant_dft_cfg`, `phy_ant_init`, `phy_ant_wifirx_cfg`, `phy_dig_reg_backup` |
| 0x600A7120 | RW | `phy_ant_init` |
| 0x600A7124 | RW | `bb_agc_reg_update`, `phy_dig_reg_backup`, `phy_rx_11b_opt`, `phy_set_rx_sense` |
| 0x600A7128 | RW | `phy_agc_reg_init_new` |
| 0x600A713C | RW | `phy_agc_max_gain_set`, `rx_init_gain` |
| 0x600A7150 | RW | `fft_dump_init`, `phy_check_sigrssi_en` |
| 0x600A7158 | RW | `phy_agc_reg_init_new` |
| 0x600A71B0 | RW | `phy_agc_reg_init_new` |
| 0x600A71C4 | RW | `phy_11p_set` |
| 0x600A71D4 | RW | `rx_spur_remove` |
| 0x600A71D8 | RW | `phy_rx_11b_opt` |

## bbtx

| Address | R/W | Functions |
|---|---|---|
| 0x600A7400 | RW | `esp_tx_func` (G), `phy_chan14_mic_enable_new` (G), `wifitxout` (G), `WifiTxStart`, `force_tx_seed`, `phy_bb_reg_init_new`, `phy_chan14_mic_cfg_new`, `phy_reg_check` and 4 more |
| 0x600A7414 | RW | `remove_11b_4p8G_spur` |
| 0x600A7418 | RW | `WifiTxStart`, `remove_11b_4p8G_spur` |
| 0x600A7428 | RW | `set_rate_power` (G), `phy_bb_reg_init_new`, `phy_bb_txpwr_init`, `set_mactx_pwrdb` |
| 0x600A7430–0x600A7434 | RW | `phy_bb_cfo_cfg`, `phy_freq_correct` |
| 0x600A7438 | RW | `phy_bb_cfo_cfg` |
| 0x600A743C | RW | `phy_bb_reg_init_new` |
| 0x600A7440 | RW | `phy_bbtx_outfilter` |
| 0x600A7454 | RW | `phy_cfg_tx_magtrk`, `phy_tx_magtrk_init` |
| 0x600A7458 | RW | `phy_cfg_tx_magtrk`, `phy_magtrk_scale_set` |
| 0x600A745C–0x600A7464 | RW | `phy_magtrk_scale_set` |
| 0x600A7474 | W | `phy_tx_magtrk_init` |

## nrx

| Address | R/W | Functions |
|---|---|---|
| 0x600A7800 | RW | `phy_fft_scale_force`, `phy_reg_check` |
| 0x600A7808 | RW | `phy_bb_reg_init_new` |
| 0x600A780C | RW | `phy_dig_reg_backup` |
| 0x600A7830 | RW | `phy_spur_coef_cfg` (G), `phy_spur_reg_write` |
| 0x600A7848 | RW | `phy_band_reg`, `phy_bb_reg_init_new`, `phy_nrx_freq_set` |
| 0x600A7880–0x600A7884 | RW | `fft_dump_init` |
| 0x600A7890 | RW | `phy_bb_reg_init_new`, `phy_hemu_ru26_good_res` |
| 0x600A7898 | RW | `phy_freq_correct` |
| 0x600A78A4 | RW | `bb_agc_reg_update`, `phy_bb_reg_init_new` |
| 0x600A78B8 | RW | `phy_bb_cfo_cfg` |
| 0x600A78C8 | RW | `bb_agc_reg_update` |
| 0x600A78DC | RW | `phy_bb_reg_init_new` |
| 0x600A78E4 | RW | `phy_bb_reg_init_new` |
| 0x600A7904 | RW | `phy_chan_filt_set`, `rftest_optimize` |
| 0x600A790C | RW | `fft_dump_init`, `phy_bb_reg_init_new`, `phy_chan_dump_cfg`, `phy_chan_dump_cfg_752`, `phy_dig_reg_backup`, `phy_lltf_mask_en` |
| 0x600A7920 | RW | `phy_rxevm_init_cfg` |
| 0x600A7980 | RW | `phy_bb_reg_init_new` |
| 0x600A7A28 | RW | `bb_agc_reg_update`, `phy_bb_reg_init_new` |
| 0x600A7A2C | RW | `phy_bb_reg_init_new` |
| 0x600A7A34 | RW | `phy_rxevm_en`, `phy_rxevm_init_cfg`, `phy_rxevm_reset_mem` |
| 0x600A7A38 | RW | `phy_rxevm_en`, `phy_rxevm_init_cfg` |
| 0x600A7A3C | R | `phy_rxevm_calcu`, `phy_rxevm_get_coef` |

## bb

| Address | R/W | Functions |
|---|---|---|
| 0x600A7C00 | RW | `phy_bb_reg_init_new`, `phy_chan_dump_cfg_752`, `phy_reg_check`, `phy_tx_paon_set`, `run_rftest_case` |
| 0x600A7C08 | R | `phy_bb_wdt_get_status`, `tx_a_frame` |
| 0x600A7C14 | RW | `phy_spur_coef_cfg`, `rx_spur_remove` |
| 0x600A7C28 | RW | `phy_bb_fsm_rst` |
| 0x600A7C2C | RW | `rx_spur_remove` |
| 0x600A7C30 | RW | `phy_freq_correct`, `phy_tx_paon_set`, `run_rftest_case` |
| 0x600A7C3C | RW | `phy_bb_wdg_cfg`, `phy_bb_wdg_test_en`, `phy_set_bb_wdg` |
| 0x600A7C40 | RW | `phy_bb_wdg_cfg`, `phy_bb_wdg_test_en`, `phy_bb_wdt_int_enable`, `phy_bb_wdt_rst_enable`, `phy_bb_wdt_timeout_clear`, `phy_set_bb_wdg` |
| 0x600A7C44 | RW | `phy_noise_floor_auto_set` |
| 0x600A7C48 | R | `phy_rxevm_calcu` |
| 0x600A7C50 | RW | `phy_rxevm_en` (G), `phy_rxevm_init_cfg` (G), `phy_noise_floor_auto_set`, `phy_rxevm_reset_mem` |
| 0x600A7C58 | RW | `phy_set_cca_cnt` |
| 0x600A7C5C–0x600A7C60 | R | `phy_get_cca_cnt` |
| 0x600A7C6C | W | `phy_tx_paon_set`, `run_rftest_case` |
| 0x600A7C80 | W | `phy_close_fe_bb_clk`, `phy_open_fe_bb_clk` |
| 0x600A7CA8 | RW | `fft_dump_init`, `phy_bb_reg_init_new` |
| 0x600A7CD0 | RW | `bt_agc_gain_set`, `phy_bb_reg_init_new` |
| 0x600A7CE0 | RW | `phy_bb_cbw_chan_cfg` |
| 0x600A7CE4 | RW | `phy_bb_cfo_cfg` (G), `phy_freq_band_reg_set` (G), `phy_11p_set`, `phy_bb_cbw_chan_cfg`, `phy_vht_support` |
| 0x600A7CE8–0x600A7CEC | RW | `phy_bb_cfo_cfg` |

## brx

| Address | R/W | Functions |
|---|---|---|
| 0x600A8000 | R | `phy_reg_check` |
| 0x600A8004 | RW | `bb_agc_reg_update`, `phy_rx_11b_opt` |
| 0x600A8010 | W | `bb_agc_reg_update` |
| 0x600A8018–0x600A8020 | W | `bb_agc_reg_update` |
| 0x600A8024 | RW | `bb_agc_reg_update` |
| 0x600A8028–0x600A802C | W | `bb_agc_reg_update` |
| 0x600A8050 | RW | `phy_freq_correct` |
| 0x600A8060 | RW | `phy_dig_reg_backup`, `phy_disable_low_rate`, `phy_enable_low_rate`, `phy_is_low_rate_enabled`, `phy_rx11blr_cfg` |
| 0x600A8070 | RW | `bb_agc_reg_update` |
| 0x600A8078 | RW | `bb_agc_reg_update` |
| 0x600A807C | RW | `phy_dig_reg_backup`, `phy_disable_low_rate`, `phy_enable_low_rate`, `phy_rx11blr_cfg` |

## The dump engine

| Address | R/W | Functions |
|---|---|---|
| 0x600A9004–0x600A9008 | RW | `adctrig` |
| 0x600A9018 | RW | `adctrig` |

## The block at 0x600A9400

| Address | R/W | Functions |
|---|---|---|
| 0x600A9404 | RW | `bt_bb_ble_diag_all` |

## MODEM_SYSCON

| Address | R/W | Functions |
|---|---|---|
| 0x600A9C00 | RW | `apb_get_mem`, `phy_reg_check` |
| 0x600A9C04 | RW | `adctrig`, `rftest_open_clk` |
| 0x600A9C0C | W | `rftest_open_clk` |
| 0x600A9C14 | RW | `apb_get_mem`, `register_chipv7_phy`, `rftest_open_clk` |
| 0x600A9C18 | RW | `phy_pbus_clear_reg` (G), `phy_pbus_set_rftx1_5g` (G), `phy_pbus_workmode` (G), `phy_set_rx_pbus_freq` (G), `bb_agc_reg_update`, `phy_11p_set_1`, `phy_bb_bss_cbw40_dig`, `phy_bb_reg_init_new` and 6 more |

## MODEM_PWR0 and the Wi-Fi MAC's timer

| Address | R/W | Functions |
|---|---|---|
| 0x600AD800 | R | `check_noise_floor` (asm), `fft_dump_init` (asm), `phy_sigrssi_rx_frame` (asm), `phy_wait_freq_set_busy_new` (asm), `sw_chan_test` (G), `test_mem_time` (asm), `tx_data_frame` (asm), `adctrig` and 11 more |
| 0x600AD804 | W | `beacon_print`, `do_rx_poll` |

## MODEM_LPCON

| Address | R/W | Functions |
|---|---|---|
| 0x600AF000 | R | `phy_reg_check` |
| 0x600AF008 | RW | `coex_force_bt` |
| 0x600AF018–0x600AF020 | W | `rftest_open_clk` |
| 0x600AF028 | RW | `phy_get_xtal_freq` |
| 0x600AF02C | RW | `apb_get_mem` |
| 0x600AF038 | W | `apb_get_mem`, `mem_read_test` |

## The block at 0x600AF400

| Address | R/W | Functions |
|---|---|---|
| 0x600AF400–0x600AF408 | RW | `coex_force_bt` |
| 0x600AF40C | RW | `coex_unforce_bt` |
| 0x600AF480 | R | `coex_force_bt` |
| 0x600AF4B4 | RW | `force_rx_gain`, `force_txon_mode` |

## i2c_mst

| Address | R/W | Functions |
|---|---|---|
| 0x600AF800 | RW | `phy_i2c_master_reset`, `phy_i2c_paral_read`, `phy_i2c_paral_write`, `phy_reg_check` |
| 0x600AF804 | RW | `phy_i2c_master_reset`, `phy_i2c_paral_read`, `phy_i2c_paral_write` |
| 0x600AF818 | RW | `phy_bbpll_cal`, `phy_bbpll_recal`, `phy_i2cmst_reg_init` |
| 0x600AF81C | W | `phy_chip_i2c_readReg_org`, `phy_i2c_paral_set_read` |
| 0x600AF820 | RW | `phy_get_i2c_hostid_`, `phy_i2c_init1`, `phy_i2c_paral_set_mst0` |
| 0x600AF824–0x600AF82C | RW | `phy_i2c_clk_sel` |

## i2c_mst's command memory

| Address | R/W | Functions |
|---|---|---|
| 0x600AFC00 | W | `phy_i2c_master_fill` (G), `phy_i2c_master_cmd_mem_init` |
| 0x600AFC04 | W | `phy_i2c_master_cmd_mem_init` |
| 0x600AFC08 | W | `phy_i2c_master_cmd_mem_init`, `phy_i2c_master_mem_txcap` |
| 0x600AFC0C–0x600AFCF4 | W | `phy_i2c_master_cmd_mem_init` |

## PMU

| Address | R/W | Functions |
|---|---|---|
| 0x600B0000 | R | `phy_reg_check` |
| 0x600B0028 | RW | `rftest_init` |
| 0x600B00CC | RW | `phy_open_i2c_xpd` |
| 0x600B0158 | RW | `phy_open_i2c_xpd`, `phy_rf_init`, `phy_wakeup_init`, `phy_xpd_rf` |

## LP_AON

| Address | R/W | Functions |
|---|---|---|
| 0x600B1000 | RW | `burnin_rtc_init` (G), `burnin_test_func`, `burnin_test_func_5g`, `phy_reg_check`, `wdg_rst_init` |
| 0x600B1054 | RW | `phy_pwdet_reg_init`, `phy_pwdet_sar2_init` |

## LP_I2C_ANA_MST

| Address | R/W | Functions |
|---|---|---|
| 0x600B2400 | R | `phy_reg_check` |

## LP_ANA

| Address | R/W | Functions |
|---|---|---|
| 0x600B2C00 | R | `phy_reg_check` |
| 0x600B2C04 | RW | `brown_out_rst_dis` |
| 0x600B2C0C | RW | `brown_out_rst_dis` |

## LP_APM

| Address | R/W | Functions |
|---|---|---|
| 0x600B3800 | R | `phy_reg_check` |

## EFUSE

| Address | R/W | Functions |
|---|---|---|
| 0x600B4844–0x600B4848 | R | `phy_get_mac_addr` |
| 0x600B484C | R | `curr_cal_param_set_from_flash`, `phy_pa_dc_code_set` |
| 0x600B4850–0x600B4854 | R | `phy_pa_dc_code_set` |

