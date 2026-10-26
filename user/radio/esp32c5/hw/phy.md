# The ESP32-C5's PHY: what differs from the C6's

The C6's `../../esp32c6/hw/phy.md` lays the PHY out block by block;
this file says what the C5's `libphy.a` and `librftest.a` do otherwise, and what only the C5 has.
Sources and labels are in `README.md` beside this file; every fact is static, nothing ran on a C5.

## The band

The C5 has no band command of its own: `test_5g` calls `phy_band_sel`, a lone `ret` in this build (`rftest.md`).
The band follows the frequency (**known**, `phy_set_channel_rfpll_freq_new` and `phy_chan_to_freq`):

- The channel argument is a channel number up to 14, 2407 + 5n MHz and 2484 for 14, and a frequency in MHz above 14.
  So `esp_tx 5180 ...` sends on 5180 MHz, as the host tool does at 5 GHz.
- The band byte, `phy_param[0x2a]`, is 1 when the frequency is above 4000 MHz, and `get_5g_en` prints it.
- `phy_band_change(band)` then runs `phy_band_i2c_set`, `phy_band_reg`, `phy_iq_swap_set`,
  and copies the band's 20 target-power bytes into `phy_param[0x32..0x45]`,
  from a table in `.rodata` for 2.4 GHz and from constants (0x48, 0x48, 0x48, 0x44, ...) for 5 GHz.

What the switch writes, in order (**known**, the functions named):

| Word | 2.4 GHz | 5 GHz | Function |
|---|---|---|---|
| analog I2C 0x6A, register 1 | 0x7F | 0x7F | `phy_band_i2c_set`, which takes no band |
| FEDATA 0x600A0434 | bits 23:22 set | bit 22 cleared | `phy_band_reg` |
| FECTRL 0x600A0888 | bit 29 cleared | bit 29 set | `phy_band_reg` |
| nrx 0x600A7848 | 0x170433AF | 0x180433AF | `phy_band_reg` |
| FEDATA_WIFI 0x600A0C08 | bits 26:25 set | bit 25 cleared | `phy_iq_swap_set` |
| FEDATA 0x600A0434 | bits 23:22 cleared | bits 23:22 cleared | `phy_iq_swap_set`, after `phy_band_reg` |
| FECOEX 0x600A001C, bits 6:0 | MHz − 2400 below 2483 MHz, else 0x53 | 0x53 | `phy_set_channel_rfpll_freq_new` |

So 0x600A0434's bits 23:22 end cleared on both bands, `phy_band_reg`'s setting undone at once; why is not known.
`phy_iq_swap_set`'s name says 0x600A0C08's bits swap I and Q per band (**inferred**).
FECOEX 0x600A001C's field is the BT channel's on the C6 at 0x600A00C0 (`README.md`), so channel 14, 2484 MHz, takes 0x53 as 5 GHz does.

On 5 GHz the channel's path also runs `phy_ckgen_5g_cal` (**known**), which writes the clock generator 0x65:

- register 2 = {0x10, 0x0E, 0x08, 0x06, 0x04, 0x02, 0x00}[`phy_freq_to_index`(MHz)];
- register 1 = 8;
- register 4 = 0x0D;
- then waits 20 µs.

`phy_set_pbus_mem_5g` loads the pbus memory, FECTRL 0x600A0844 and 0x600A0848, with the 5 GHz receive gain and capacitor entries
(`phy_get_chan_cap`, `phy_freq_to_mbgain`), where `phy_set_pbus_mem_2g` copies fixed tables.

`phy_freq_band_reg_set` (**known**) writes agc 0x600A7030 bit 5 = 1 when its argument's bit 0 is 0, then calls `phy_vht_support`.

## The analog I2C

The master is the C6's, at 0x600AF800, with the same words (ESP-IDF's `i2c_ana_mst_reg.h`, **known**).
The C5's ROM does not hold the access code, so `libphy.a` shows it (**known**, `phy_chip_i2c_writeReg`, `phy_chip_i2c_readReg_org`,
`phy_get_i2c_hostid_`, `phy_get_i2c_read_mask_`):

- **The host is the block's, not the caller's.**
  `phy_get_i2c_hostid_` returns 1 when block − 0x63 is below 12 and bit (block − 0x63) of 0x9AD is set,
  so for 0x63, 0x65, 0x66, 0x68, 0x6A, 0x6B and 0x6E, and 0 for every other block.
  `phy_i2c_writeReg(block, host, reg, data)` drops its host argument, as do the reads,
  so the console's `wi`, `ri`, `wim` and `rim` and `phy_i2c_check` pass a host that changes nothing.
  `phy_get_i2c_hostid_` also writes ANA_CONF2, 0x600AF820, bits 16:4 = 0x19C1, at every access.
- **A write** waits until bit 25 of I2C0_CTRL 0x600AF800 (host 0) or I2C1_CTRL 0x600AF804 (host 1) is clear,
  writes `block | reg << 8 | data << 16 | 0x05000000`, and waits again.
- **A read** writes ANA_CONF1 0x600AF81C = ~(sel << 2), `sel` the block's bit in the table below,
  writes `block | reg << 8 | 0x04000000`, waits for bit 25, and takes bits 23:16.
- **Block 0x62 is rewritten as 0x65** in both, after the host is chosen from 0x62, so on host 0.
  0x62 is the C6's RF PLL, `i2c_rfpll`; on the C5 the number reaches the 5 GHz clock generator.

The blocks, as `phy_i2c_check` names and dumps them (**known**):

| Block | Name | Registers dumped | Host | Read select (`sel`) | On the C6 |
|---|---|---|---|---|---|
| 0x61 | `i2c_ulp` | 0–10 | 0 | 0x040 | the same |
| 0x62 | none; rewritten as 0x65 | | 0 | 0 | `i2c_rfpll`, host 1 |
| 0x63 | `i2c_sdm` | 0–27 | 1 | 0x004 | `i2c_rfpll_sdm` |
| 0x64 | none | | 0 | 0 | |
| 0x65 | **`i2c_ckgen6g`** | 0–6 | 1 | 0x400 | none |
| 0x66 | `i2c_bbpll` | 0–10 | 1, where the dump passes 0 | 0x020 | host 0 |
| 0x67 | `i2c_bbtop` | 0–31 | 0, where the dump passes 1 | 0x001 | host 1 |
| 0x68 | **`i2c_ckgen`** | 0–7 | 1 | 0x008 | none |
| 0x69 | `i2c_perif` | 0–12 | 0 | 0x080 | the same |
| 0x6A | `i2c_bias` | 0–3 | 1 | 0x010 | the same |
| 0x6B | `i2c_txrf` | 1–21 | 1 | 0x002 | the same |
| 0x6C | none | | 0 | 0x800 | |
| 0x6D | `i2c_dig_reg` | 0–14 | 0 | 0x100 | the same |
| 0x6E | **`i2c_txtop`** | 0–23 | 1 | 0x200 | none |
| 0x6F | none | | | 0x1000 | |

`i2c_ckgen6g`'s name and `phy_ckgen_5g_cal` say 0x65 is the 5 GHz LO's clock generator (**inferred**);
`i2c_txtop` holds the PA's current trim (below), so it is the transmitter's top, the PA's block (**inferred**).
`phy_i2c_init1` writes most blocks in bulk through `phy_i2c_paral_write_num` from tables, which were not decoded.

The registers the libraries write or read with constant arguments, beyond the band's and the trim's:

| Block, register | Bits | What | Function |
|---|---|---|---|
| 0x6A, 0 | 3:0 | 0xF, or 7 at 5620 and 5690 MHz (below) | `phy_bias_reg_set`, `esp_tx_func` |
| 0x6A, 0 | 7:4 | a level the 1.1 V calibrations step: 9 at bring-up, 0xC on a frequency set | `phy_bias_reg_set`, `phy_set_freq_i2c_new`, `phy_sdm_start_cal_1p1`, `phy_sar_vol_1p1`, `rx_spur_cal`, `phy_freq_start_cal_5g_code` |
| 0x6A, 1 | 3:0, 7:4 | 0xF and 7 at bring-up; the whole register 0x7F on a band change | `phy_bias_reg_set`, `phy_band_i2c_set` |
| 0x63, 0 | 0, 3, 6 | the calibration's restart, the SDM's write strobe, bit 6 cleared at 5620/5690 MHz | `phy_restart_cal`, `phy_write_rfpll_sdm`, `esp_tx_func` |
| 0x63, 2–5 | 7:0 | the synthesizer's word | `phy_write_rfpll_sdm` |
| 0x63, 9–0xD | | the SDM calibration's codes, read back | `phy_sdm_cal_check`, `phy_sdm_read_code` |
| 0x63, 0x13 | 0, 1, 4:2 | the SDM's enables; 0 to close the RF | `phy_i2c_sdm_en`, `phy_i2c_correct`, `phy_close_rf` |
| 0x63, 0x19 | 6:4 | 4, or 6 at 5620/5690 MHz | `esp_tx_func` |
| 0x68, 0 | 5, 6 | 2.4 GHz clock generator calibration and reset | `phy_ckgen_2g_cal`, `phy_reset_ckgen` |
| 0x68, 2, 3 | 5:0 | read back by the TX IQ calibration | `phy_rfcal_txiq`, `phy_get_txiq_set` |
| 0x68, 5, 6 | 5:0 | the IQ calibration's clock-generator code | `phy_set_iqcal_ckgen_code` |
| 0x67, 6–0xD | 7:0 | 0x3C each for 802.11p, and the filter's codes | `phy_11p_set`, `tx_cont_cfg` |
| 0x67, 0x1D | 6:4, 7 | the peak detector, 4 and 1 | `phy_i2c_pkdet_set` |
| 0x6E, 0xD | 2 | 1 for the TX calibration's debug mode, 0 for work | `phy_txcal_debuge_mode_new`, `phy_txcal_work_mode` |
| 0x6E, 4 and 0xF | 6:4, 2:0 | read and printed as `pavd_0`, `pavd_1` | `burnin_test_func_5g` |
| 0x6B, 0x13 | 7 | 1 while the VDD33 is read | `phy_get_vdd33` |
| 0x6D, 4 and 6 | 4:0 | 0x1C each | `register_chipv7_phy`, `rftest_init` |
| 0x66, 4 | 2, 4 | the ADC's and DAC's rate | `phy_adc_rate_set`, `phy_dac_rate_set` |
| 0x69, 6 | 3:0 | 0xB, the temperature sensor's DAC | `phy_tsens_dac_cal` |
| 0x61, 7 and 8 | | the crystal's duty calibration | `phy_xtal_duty_cal_init`, `phy_xtal_reg_set`, `phy_xtal_duty_cal` |

The values are **known**; what each field does is **inferred** from its function's name.

## The PA's current trim

The C5 trims its PA's bias currents from four eFuse fields,
which ESP-IDF's `esp_efuse_table.csv` names, BLK1 bit for bit (**known**, `phy_pa_dc_code_set`, `curr_cal_param_set_from_flash`):

| eFuse field, BLK1 bits | Read at | `phy_param` | Written to |
|---|---|---|---|
| `PA_TRIM_VERSION`, 93–95 | 0x600B484C bits 31:29 | | when 0, the trims are read back from the I2C instead |
| `TRIM_N_BIAS`, 96–100 | 0x600B4850 bits 4:0 | 0x410 | 0x6E register 0xA, bits 4:0 |
| `TRIM_P_BIAS`, 101–105 | 0x600B4850 bits 9:5 | 0x412 | 0x6E register 0xB, bits 4:0 |
| `REF_CURR_CODE`, 134–137 | 0x600B4854 bits 9:6 | 0x414 | 0x6A register 3, bits 3:0 |
| `RES_TUNE_CODE`, 138–142 | 0x600B4854 bits 14:10 | 0x416 | 0x6E register 5 bits 2:0 = code >> 2, register 0xC bits 6:4 = code & 3 |

When `PA_TRIM_VERSION` is 0, `curr_cal_param_set_from_flash` takes them from an 8-byte record at flash 0x30000 (`phy_rf_param_flash_get`):
byte 0 a flag and byte 1 `REF_CURR_CODE`, byte 2 a flag and byte 3 `RES_TUNE_CODE`, byte 4 a flag and bytes 5 and 6 `TRIM_N` and `TRIM_P`,
each written only when its flag is 1.

The factory makes that record with the console's `ate_*` commands (**known**, the functions; **inferred**, that a tester measures):

- `ate_chip_ref_curr_cal` sets 0x69 register 7 bit 7, 0x6A register 2 bit 2 and bits 7:6, and scans 0x6A register 3, `REF_CURR_CODE`;
- `ate_chip_pa_curr_cal` first sets 5500 MHz, the 5 GHz pbus memory, TX gain 0x101C0 and TX on (`ate_pa_curr_cal_init`),
  then curve 7, 0x6E register 0xC bits 2:0 = 7, and scans `RES_TUNE_CODE`, `TRIM_P` and `TRIM_N` in turn;
- each step waits for a GPIO edge from the tester, `ate_curr_cal_wait_trig_gpio`, and prints the code it reached;
- `phy_rf_param_flash_burn` writes the record;
  `pa_dc_code_set_flash`, `ate_pa_trim_code_read` and `ate_pa_trim_code_write` handle `TRIM_N` and `TRIM_P` alone.

## A quirk at 5620 and 5690 MHz

`esp_tx_func` (**known**), at 5620 or 5690 MHz alone:

- before the channel is set, clears 0x63 register 0 bit 6;
- after it, sets 0x6A register 0 bits 3:0 = 7 and 0x63 register 0x19 bits 6:4 = 6;
- after the burst, restores 0xF, 4 and 1.

At every other frequency it still writes the three restoring values after the burst.
That two channels need the synthesizer and the bias changed is a workaround for something at those frequencies, a spur perhaps (**inferred**).

After every burst `esp_tx_func` also clears FEDATA 0x600A0408 bit 23 (mask 0xFF7FFFFF), the word that holds the digital TX gain.

## The 1.1 V level and the spur

0x6A register 0 bits 7:4 is a level that the calibrations named `_1p1` step (**inferred**, the names):
`sdm_start_cal_1p1` restarts the PLL's calibration at a level,
`sar_vol_1p1` reads the SAR ADC at a level,
and `xtal_spur_cal` runs `rx_spur_cal`, which for each level plays a tone into the receiver and measures the spur with `rxiq_get_pwr`.

## Other C5 words

- `dig_11b_filter_sel n`: bbtx 0x600A7400 bits 14:13 = n; `esp_tx` in its FCC mode holds them at 1 on channel 14.
- `" phy_tx_paon_set"`: bb 0x600A7C00 bits 20:11, FECTRL 0x600A086C bits 15:8, bb 0x600A7C6C, bb 0x600A7C30 bits 9:0, the PA-on timing (**inferred**).
- `pbus a0 a1 a2`, `phy_pbus_force_test`: waits for FECTRL 0x600A0890 bit 31 to clear,
  then writes 0x600A0884 bits 16:1 = a1 << 15 | a2 << 6 | a0 << 2.
- The tone generator: FEDATA 0x600A040C, 0x0410, 0x0414, 0x041C, 0x0420, 0x0428 and FEDATA_WIFI 0x600A0C04 (`phy_start_tx_tone_step`).
- The TX gain forced: FECTRL 0x600A0844–0x0850 and 0x0890 (`force_tx_gain`, `target_power_backoff`).
- `usb_clk_en n`: PCR 0x60096094 bit 0 = n.

## Open questions

- What 0x600A0434's bits 23:22 do, which `phy_band_reg` sets on 2.4 GHz and `phy_iq_swap_set` clears straight after.
- What `nrx` 0x600A7848's two values, 0x170433AF and 0x180433AF, hold beyond the band's bit.
- Whether `FEDATA_BLE`'s loop really dumps the same words as `FEDATA_WIFI`, or the source's base is wrong.
- What the blocks 0x64, 0x6C and 0x6F are, which have read-select bits but no name.
- What `phy_i2c_init1`'s bulk tables write.
- Whether Espressif's own C5 RF test binary fills the stubs of this build, `phy_band_sel` above all (`rftest.md`).
- What fails at 5620 and 5690 MHz without the quirk.
- Every fact here waits for a run on a C5.
