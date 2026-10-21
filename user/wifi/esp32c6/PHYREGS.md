# The PHY's registers and `phy_param` fields, as the driver understands them

The driver takes the PHY from `libphy.a` a function at a time, and names what it touches so the meaning
is in the code, not in a number. This file is that naming, laid out by hardware block, since that is the
map the driver is working towards: a block's registers, the analog I2C blocks it is reached through, the
`phy_param` fields it keeps, and how a calibration loop shows the blocks connected. Each name says where
it comes from and how sure it is:

- **known** -- a source names it: an ESP-IDF header, a `libphy` debug string, or a `libphy` function
  whose name states it.
- **inferred** -- its role in the library's loop says it; the comment in the code says so too.
- **unknown** -- nothing names it and its role does not state it: the name is the driver's own and the
  meaning is not known.

An address a header names follows the header's name; an address no header names gets the driver's own
name and a comment saying the meaning is not known. Names do not change the built image, so a commit of
names alone leaves `wifi-drv.bin`'s SHA-256 alone. Sources: the tables at the end.

## The blocks

Each block: the MMIO windows it owns (Espressif's window names), the analog I2C blocks it is reached
through, and the `phy_param` fields it keeps. Membership is a source's, or a loop's role in the code,
and says which.

### Clocks and power

- `MODEM_SYSCON` 0x600A9800, `MODEM_LPCON` 0x600AF000, `pmu` 0x600B0000, `PCR` 0x60096000,
  `i2c_mst` 0x600AF800 -- Espressif's windows, and the IDF `modem_syscon_reg.h`, `modem_lpcon_reg.h`,
  `pmu_reg.h`, `pcr_reg.h` name the words. The driver brings the modem up through these; the words the
  driver leaves to the kernel are PCR 0x60096130, PMU 0x600B00CC and 0x600B0154, LP_AON 0x600B1054.
  The analog I2C master's words, `I2C_MST_MST0` 0x600AF820 and `I2C_MST_CLK0/1/2`
  0x600AF824/0x600AF828/0x600AF82C, lie in the `i2c_mst` window, not in `MODEM_LPCON`'s, and are named
  for it. The master's command memory, thirty words from `I2C_ANA_MST_CMD0` 0x600AFC00 to 0x600AFC74, lies
  in the `i2c_mst` window's end; `phy_i2c_master_cmd_mem_init` programs all thirty and
  `phy_i2c_master_mem_txcap` its word at 0x600AFC04 alone. What the command words say is not known.
- Membership: **known** (window names), **inferred** for which word gates which domain.

### Bias and LDO

- Analog I2C: `i2c_bias` 0x6A, `i2c_dig_reg` 0x6D, `i2c_ulp` 0x61 -- `libphy`'s `phy_i2c_check` names
  them; IDF's `regi2c_bias.h`, `regi2c_dig_reg.h`, `regi2c_lp_bias.h` name the blocks and their fields.
  The driver's `bias_reg_set`, `reg_set_new` and `open_i2c_xpd_new` write them.
- Certainty: **known** (block), **inferred** for a field's meaning where the loop is the only source.

### RF PLL

- MMIO `FECOEX` 0x600A0000: the frequency setting's words -- `FECOEX_FREQ_SET_CONF`, `_TRIG`, `_STATUS`,
  `_MEM`, `_DATA`, `_CONF2`, `_CONF4` -- the driver's own names from `phy_wait_freq_set_busy_new`,
  `read_rf_freq_mem_new`, `pll_cap_mem_update_new`; meaning from those loops (**inferred**). The
  frequency setting lies in the window Espressif names FECOEX, the coexistence block's; recorded, not
  explained.
- Analog I2C: `i2c_rfpll` 0x62, `i2c_rfpll_sdm` 0x63 -- `phy_i2c_check`'s names (**known**); `rfpll_chgp_cal`
  calibrates the block 0x62's charge pump through its registers 0x0e and 0x0f (the table below).
- `phy_param`: `PHY_PARAM_CAP_TEMP` 0x130, `PHY_PARAM_CAP_BUSY` 0x19C, the frequency-select byte
  0x19B (**inferred** from the RFPLL-capacitance tracking loop), and `PHY_PARAM_I2C_RFPLL_0F` 0x196, the
  block 0x62's register 0x0f read back (**inferred** from `rfpll_chgp_cal` and `rf_init`).

### BBPLL

- Analog I2C: `i2c_bbpll` 0x66, host 0 -- `phy_i2c_check`'s name; IDF's `regi2c_bbpll.h` `I2C_BBPLL`
  0x66 with its fields (**known**). The driver's `i2c_bbpll_set` and `phy_bbpll_cal` write it.
- `phy_param`: `PHY_PARAM_BBPLL_BYTE` 0x4A (**inferred**: the byte read and written back around reg 4).

### Transmit

- MMIO `FEDATA` 0x600A0400: `FEDATA_GAIN_CHAN` 0x410 (the transmit gain channel), `FEDATA_REG_SET1/2/3`
  0x44C/0x46C/0x450, `FEDATA_TX_TONE0` 0x420 (the transmit tone generator's first word), and the transmit DC
  calibration word 0x418 (below, under corrections). `FEDATA_TX_TONE0` is the ROM's `ram_start_tx_tone_step`'s
  and `ram_stop_tx_tone`'s own word, and `txiq_get_mis_pwr` writes its measurement into it (**known**, the ROM's
  function names; no header names it). `FEDATA_REG_SET3`'s bits 13 and 14 are the TXIQ calibration's
  `FEDATA_REG_SET3_TXCAL` (raised before it runs and left raised) and `FEDATA_REG_SET3_TXIQ_DONE` (lowered while
  it runs and raised at its end) -- `rfcal_txiq_new`'s loop is the only source (**inferred**).
- MMIO `FECTRL` 0x600A0800: the gain memory `FECTRL_GAIN_MEM_CONF`/`_MEM0`/`_MEM1`/`_MEM2`
  0x8C8/0x8CC/0x8D0/0x8D4, `FECTRL_REG_SET0`/`_SET4` 0x8F8/0x8FC, `FECTRL_TXRX_OFF` 0x910,
  `FECTRL_RX_GAIN_TRIG_EN` 0x904.
- MMIO `bbtx` 0x600A7400: `BBTX_CHAN14` 0x7400 (libphy's `chan14_mic_cfg` writes its bits 13-14, `mic`
  not known, and the same word `bb_init` sets -- recorded, not resolved), `BBTX_REG_SET` 0x7428,
  `BBTX_INIT_10` 0x7424, `BBTX_INIT_12` 0x7438.
- MMIO `bb` 0x600A7C00: `BB_PAON_0/1/2` 0x7C00/0x7C6C/0x7C30 (the power amplifier's on-set),
  `BB_INIT_6` 0x7CA8, `BB_INIT_11` 0x7CD0.
- Analog I2C: `i2c_txrf` 0x6B, host 1 -- `phy_i2c_check`'s name (**known**); the driver's
  `set_txclk_en`, `tx_paon_set_new` and `wifi_get_tx_tab` write it.
- `phy_param`: `TXIQ` 0xD0/0xD2, `LB_TXIQ` 0xE6, `TXDC` 0xA8..0xAE, `BT_TXIQ` 0x102,
  `BT_TXDC` 0x104..0x10A, `wifi_init_power` 0x52, `wifi_cal_power` 0xF7, `wifi_pwctrl_atten`
  0xF1..0xF3, `wifi_cal_atten` 0xF4..0xF6, `bt_cal_power` 0xFE, `bt_cal_atten` 0xF8..0xFA
  (**known** labels), and `PHY_PARAM_TXCAL_TEMP` 0x48, the temperature the transmit calibration block last ran
  at (**inferred** from `phy_cal_param_track`; no label names it).
- The transmit capacitor calibration: `tx_cap_init_loop` retunes to each of the channels 1, 6 and 11 through the
  ROM's `set_channel_rfpll_freq`, sets the analog I2C's `i2c_txrf` 0x6B register 2 bits 3..0 to 1 and its bits
  7..4 to 5, and runs a tone at 0x80; `rfcal_txcap_loop` then walks bits 3..0 from 7 down to 0 and bits 7..4 from
  13 down to 1 by twos, keeping the value whose estimate magnitude (through `dc_iq_est_high`, from
  `FEDATA_IQ_EST_*`) is largest and writing it back, and the two values go to `phy_param`'s `DCAP_STG1`/`DCAP_STG2`
  at 0xDC/0xDE/0xE0 and 0xDD/0xDF/0xE1, one pair a channel. The fields and what they correct have no label,
  meaning **unknown**; the library's print argument is `phy_param`'s halfword 0x10 bit 7, also unlabelled.
- pbus: `rftx` (4,1), (4,2), (5,1); `dco` (2,1), (3,1), (2,2), (3,2).

### Receive

- MMIO `agc` 0x600A7000: `AGC_0..4` 0x713C/0x7094/0x702C/0x7128/0x7068, `AGC_NOISE_FLOOR` 0x7018,
  `AGC_11P` 0x7030, `AGC_11B_0/1/2` 0x7044/0x7124/0x7104, `AGC_GAIN_SAR` 0x708C, `AGC_RX_COMP1`
  0x70A0. `AGC_2` 0x702C is one word with several fields (`RX_COMP0`, `RX_GAIN_TRIG`): one name, fields
  apart. The names are the driver's from `agc_reg_init_new`, `set_rx_comp_new`, `set_rx_gain_table`
  (**inferred**).
- MMIO `nrx` 0x600A7800: `NRX_INIT_1..5,7,8,9`; MMIO `brx` 0x600A8000: `BRX_LOW_RATE0/1`
  0x8060/0x807C and `BRX_11B_3` 0x8004; MMIO `bb` 0x600A7C00: `BB_INIT_6/11`. Windows **known**, names
  the driver's (**inferred** from `rx_11b_opt`, `phy_disable_low_rate`, `bb_reg_init_new`).
- pbus: `rfrx` (0,1), (0,2); `bb` (1,1), (1,2).
- `phy_param`: `RXIQ` 0xD4/0xD6; `RXDC_RFRX_BT` 0x132/0x134, `RXDC_RFRX_WIFI` 0x14E/0x150,
  `RXDC_CHAN_WIFI` 0x172/0x174 (**known** labels); 0x16E is unlabelled (**unknown**); the gain-table
  counts 0x120/0x121 and the temperature 0x198 (**inferred** from the gain table). The offsets
  0x14E/0x16E/0x172 are also the rows `gen_rx_gain_table_new` packs, a role, not a label.

### Corrections (TXDC, TXIQ, RXDC, RXIQ)

- `phy_param`: `PARAM_FLAG` 0xA4's bits 3 (the baseband bring-up's calibrations done, which `bb_init` sets),
  5 (the frequency memory built), 7 (receive DC done), 9 (receive rows built),
  10 (the receive IQ calibration done, which `rxiq_cal_init` sets and returns at once on),
  12 (the Bluetooth transmit DC calibration done, which `bt_txdc_cal_new` sets and returns at once on),
  14 (TXIQ done), 23 (the RC calibration run);
  the transmit DC calibration's settled pairs `TXDC_CAL_PWDET` +0/+2/+4/+6 at 0x18E..0x194, its printed
  sources `TXDC` 0xA8/0xAA, and 0xB0/0xB2 beside them (an open question).
- MMIO: the transmit DC calibration word `FEDATA_TXDC_CTRL` 0x600A0418 (`FEDATA`) -- meaning **unknown**,
  its bits set out in the table below.
- The transmit DC calibration's two bring-ups: `txdc_cal_init_new` enters the pbus's debug mode, powers the
  transmitter with the caller's second and third arguments, forces five gains (`index_to_txbbgain`) into the
  pbus's window 1 place 2 and settles one DC pair each into the caller's twenty halfwords at `phy_param` 0xA8
  to 0xCF; `bt_txdc_cal_new` does the same for Bluetooth's pair into `phy_param`'s `BT_TXDC` 0x104, its pbus
  window 4 place 2 forced from `phy_param`'s unlabelled byte 0x14 shifted three, gated and marked in
  `param_flag` bit 12. `set_lb_txiq_new` splits its argument through `get_iq_value` and hands the forward and
  loopback halves to the ROM's `txiq_set_reg`, the second byte less four.
- The transmit IQ calibration's own: the front end's `FEDATA_REG_SET3` bits 13 and 14 (above, under
  transmit), the tone generator's `FEDATA_TX_TONE0` 0x600A0420, and `phy_param`'s `TXIQ` 0xD0 (the forward
  pair) and `LB_TXIQ` 0xE6 (the loopback pair), beside the two start bytes `PHY_PARAM_TXIQ_TEMP` 0x18 and
  `PHY_PARAM_TXIQ_ATTEN` 0x2B (**inferred** from `txiq_cal_init_new` / `rfcal_txiq_new`; the two start bytes
  are **unknown**, no label naming them).
- The receive DC calibration's words: `FECTRL` `FECTRL_RX_GAIN_TRIG_EN` 0x904, `FEDATA`
  `FEDATA_RX_IQ_EST0/1` 0x470/0x474 and `FEDATA_IQ_EST_I/Q/MAG` 0x488/0x48C/0x490, `FEDATA_RX_GAIN_SAR`
  0x4A0 (**inferred** from `set_rx_gain_cal_dc_new`'s loop).
- The receive IQ mismatch and the saturation checks: `rfrx_sat_rst` writes the front end's `AGC_4` 0x7068 with
  0x404 and moves `AGC_RFRX_SAT` 0x705C's bits 31, 30, 28, 24 and 19 with its argument -- raised with it,
  cleared without -- raises its bit 11 or 10 and clears its bits 0..18; `phy_check_rx_sat` reads the receive
  gain's `AGC_GAIN_SAR` 0x708C, its field at bits 12..18, a hundred times against 0x45; and `rxiq_get_mis_new`
  reads the estimate's four words `FEDATA_RX_IQ_EST2..5` 0x478..0x484. `AGC_RFRX_SAT`, `AGC_4` and the four
  (like the rest of `rfrx_sat_rst`'s and `phy_check_rx_sat`'s work) have no label, meaning **unknown**.
- The receive IQ mismatch's correction: `rxiq_cover_mg_mp_new` hands the ROM's `rxiq_set_reg` the correction pair
  and folds the mismatch back, over two rounds; `rfcal_rxiq_new` raises `FEDATA_REG_SET3`'s bit 29 and lowers its
  bit 30 around a tone and the correction; and `get_rfcal_rxiq_data_new` repeats the reading up to four times and
  averages. The ROM's `rxiq_set_reg` [0x40005214] holds the high byte to `[-0x1f, 0x1f]` and the low to
  `[-0x3f, 0x3f]` (through the ROM's `get_data_sat`) and writes it into `FEDATA_REG_SET3`'s high field (mask
  0x3f0000, bits 12..15 and 17..23 cleared) or low field (mask 0x1fc00, bits 14..20 cleared). Bits 29 and 30
  (and what each field corrects) have no label, meaning **unknown**.
- The receive IQ calibration's block: `set_rx_gain_cal_iq_new` sets the loopback gain and the loopback transmit
  IQ pair, runs the ROM's `pbus_rx_dco_cal`, then steps the receive gain a sixteenth at a time while the
  estimate's `FEDATA_IQ_EST_MAG` decides -- below 0x1000 the gain falls, above 0x20000 it rises and the loopback
  side is dropped -- and hands the second pass's gain to `get_rfcal_rxiq_data_new`. `rxiq_cal_init` sets the
  channel's analog side to 6 through the ROM's `chip_v7_set_chan_ana`, writes the six-byte table at `phy_param`
  0xDC through the ROM's `rom_set_txcap_reg`, rises `FECTRL_TXRX_OFF` 0x600A0910's bits 14 and 15, runs the gain
  search into `phy_param` 0xD4, repacks the four halfwords 0xD4..0xDA through `(v >> 1 & 0x1F80) | (v & 0x7F)`,
  and, where its third argument is clear, clears `FECTRL_TXRX_OFF`'s bit 15, powers the transmitter down and
  restores the pbus's work mode. Around the search it rises `FEDATA_REG_SET3`'s bit 29 and bit 13 and lowers
  bit 30 and bit 14, and at the end lowers bit 29 and rises bit 30 and bit 14, so the block mirrors
  `rfcal_rxiq_new`'s bits 29 and 30. `FECTRL_TXRX_OFF`'s two bits and the four `FEDATA_REG_SET3` bits have no
  label, meaning **unknown**.

### Sensors

- MMIO `apb` (the SAR ADC) 0x6000E000: `APB_SARADC_CTRL`, `APB_SARADC_TSENS_CTRL` 0x58 (bit 22 its
  power-up), `APB_SARADC_TSENS_CTRL2` 0x5C (bit 15, or bit 19 -- [open], see below). IDF's
  `apb_saradc_reg.h` names them (**known**).
- MMIO `FECTRL` 0x600A0800: the power detector's words `FECTRL_PWDET_CONF0..4`
  0x810/0x814/0x818/0x81C/0x820 (**unknown**: no header names them; the driver's own names) and
  `FECTRL_BT_FILTER` 0x900.
- Analog I2C: `i2c_perif` 0x69, host 0 -- `phy_i2c_check`'s name; IDF's `regi2c_saradc.h` names
  reg 0x6 `I2C_SARADC_TSENS_DAC` (**known**). The driver's temperature read through `tsens_code_read`
  uses it, and its own `tsens_dac_cal_new` writes the DAC through the same register's bits 3..0.
- `phy_param`: `temp_code` 0x00, its `temp_code_init` 0x12E, `sar_dc_code` 0x1A, `sar_ref_code` 0x1C,
  the DAC index 0x16 (**inferred**: the temperature read writes it and `rf_init` reads it), and the tracked
  temperatures the read starts -- `PHY_PARAM_TRACK_TEMP_AT` 0x04, `PHY_PARAM_TXCAL_TEMP` 0x48 and
  `PHY_PARAM_CAP_TEMP` 0x130 (**inferred** from `get_temp_init_new`).
- The driver's own read and calibration: `tsens_dac_to_index`, `tsens_dac_cal_new`, `ram_tsens_temp_read_new`
  (the library's weak `phy_get_tsens_value` is the same read), `tsens_temp_read_new` and `get_temp_init_new`;
  the library's `phy_set_tsens_range` is left out, nothing calling it.

## Connections seen

What a calibration loop shows about the signal path, with the loop as the source. Added as the driver
walks each loop.

- `txdc_cal_pwdet` / `txdc_cal_pwdet_init`: a tone is started on the transmit side
  (`rom_start_tx_tone_step`), the front end's DCO words are forced (`pbus_set_dco` -> windows (2,1),
  (3,1), (2,2), (3,2)), and the result is read back through the power detector and the SAR ADC
  (`get_tone_sar_dout`); the settled pair is written to `phy_param`'s `TXDC`/0x18E fields. So the
  transmit DC path is DCO -> power detector -> SAR ADC.
- `set_rx_gain_cal_dc_new` / `dc_iq_est_new`: the receive path's DC offset is read through the modem's
  IQ estimate (`ram_iq_est_enable` -> `FEDATA_IQ_EST_I/Q/MAG`), so the receive DC path is baseband ->
  IQ estimate.
- `rfrx_sat_rst` / `phy_check_rx_sat`: the front end's receive-saturation word is reset, the receiver powered
  through the ROM's `ram_pbus_xpd_rx_on`, the pbus's DCO pair forced through the ROM's `pbus_set_dco`, and
  the receive gain's SAR `AGC_GAIN_SAR` read a hundred times; where its field at bits 12..18 sits at 0x45 or
  below, `phy_param`'s `rx_sat` 0x1A4 is set. So the saturation check is the receive gain's SAR against a level.
- `rxiq_get_mis_new` / `dc_iq_est_high`: the same estimate's words -- `rxiq_get_mis_new`'s four
  `FEDATA_RX_IQ_EST2..5` and `dc_iq_est_high`'s `FEDATA_IQ_EST_I/Q/MAG` -- taken with the estimate armed
  through the ROM's `rom_iq_est_enable` (the table's slot 40); the four are shifted by the gain less two and
  the two pairs' sum and difference crossed into the receive IQ mismatch's two bytes. So the receive IQ
  mismatch is the estimate's own words.
- `rxiq_cover_mg_mp_new` / `rfcal_rxiq_new` / `get_rfcal_rxiq_data_new`: a tone on the transmit side, the
  receive IQ mismatch read through `rxiq_get_mis_new`, each round's pair handed to the front end's
  `FEDATA_REG_SET3` through the ROM's `rxiq_set_reg` and the mismatch folded back, over two rounds; the reading
  is repeated up to four times and averaged, or two within one of each other averaged at once. So the receive IQ
  correction is the estimate's mismatch -> `rxiq_set_reg` -> `FEDATA_REG_SET3`, beside the tone.
- `txdc_cal_new`: a tone on the transmit side, the two DCO words forced into the pbus windows (2,1) and
  (3,1), and each step decided by the front end's calibration word `FEDATA_TXDC_CTRL` -- its bits 29 and
  28 move one offset each, its bit 22 is waited for; the settled pair goes to the caller's
  `pbus_set_dco` buffer. So the transmit DC path is DCO -> the front end's calibration word -> back.
- `txdc_cal_init_new` / `bt_txdc_cal_new`: the pbus's debug mode entered, the transmitter powered, then five (or
  Bluetooth's one) gains forced into the pbus's force words and a transmit DC pair settled through `txdc_cal_new`
  each; `set_lb_txiq_new` sets the loopback transmit IQ pair through the ROM's `txiq_set_reg`. So the transmit DC
  bring-up is the pbus's force words -> `txdc_cal_new` -> the DCO pair, and the loopback IQ set is `get_iq_value`
  -> `txiq_set_reg`.
- `set_tx_gain_mem` takes `&phy_param[0xD0]`, and 0xD0/0xD2 is `TXIQ` in `phy_cal_print`: the transmit
  IQ correction goes into the transmit gain memory beside the gain entries. Whether the library reads
  past 0xD2 is not known here -- the label is a halfword pair, the gain memory's own length is a
  question.
- `txiq_cover_new` / `rfcal_txiq_new` / `txiq_cal_init_new`: a tone on the transmit side, its power read
  back through the SAR ADC -- `txiq_get_mis_pwr`, and the attenuation through `get_power_atten` -- over
  seven rounds; each round's mismatch moves the correction pair, held between -0x1f and 0x1f (its second
  byte between -0x3f and 0x3f), the forward pair is packed into `TXIQ` 0xD0 and the loopback pair into
  `LB_TXIQ` 0xE6, and the correction is marked done in `param_flag` bit 14. So the transmit IQ path is the
  tone generator -> the SAR ADC -> the correction pair -> `phy_param`.
- `tx_cap_init_loop` / `rfcal_txcap_loop`: for each of the channels 1, 6 and 11, a tone at 0x80 is started and
  the estimate's magnitude read back, and the transmit capacitor's two analog I2C fields (`i2c_txrf` 0x6B
  register 2's bits 3..0 and 7..4) are walked until each field's largest-magnitude value is found; each
  channel's pair lands in `phy_param`'s `DCAP_STG1`/`DCAP_STG2`. So the transmit capacitor path is the tone ->
  the estimate -> the transmit block's DAC -> `phy_param`.
- `phy_cal_param_track`, which the library's `phy_param_track_tot` runs once a second: the temperature each
  calibration block last ran at is held against the current one, from `phy_param` 0x198 for the receive rows
  and 0x48 for the transmit calibration, and a block whose temperature has moved past its step (0x1e and
  0x28, or the byte at 0x1A6 where bit 1 of 0x1A5 is set) is rerun: the rows or the IQ and transmit-DC
  calibrations. So the two blocks are tied to the temperature beside them, and `phy_track_temp_debug` can
  widen the step.
- `phy_param_track_tot`, the whole tracking, run once a second: inside the ROM's critical section, and only
  while the RF-off and sensor-power flags are clear, the RFPLL capacitance runs where 0x0A is set, Bluetooth's
  transmit power where the caller asks, and Wi-Fi's -- the I2C track, the transmit power, and the parameter
  calibration where 0x19A is clear -- and the sensor's temperature is read last, so the corrections are tied to
  the temperature each is checked against.
- `i2c_rc_cal_set` / `rc_cal_init` / `rc_cal_new`: the analog RC oscillator is settled through `i2c_txrf`'s
  registers 0x08, 0x0B and 0x0D and its DAC read back from register 0x0E (the ROM's `get_rc_dout`, which does
  the settling, a 100 us wait and the read), and the read is turned into `phy_param`'s `rc_dout` bytes -- one
  raw, two for Wi-Fi and four for Bluetooth -- with the ROM's `get_data_sat` holding each; the run is marked in
  `PARAM_FLAG` bit 23. So the RC path is the analog RC oscillator -> its DAC through `i2c_txrf` -> `rc_dout`.
- `set_pbus_mem` / `write_pbus_mem`: twelve word arrays are programmed into the pbus, each entry through the
  front end's gain-memory word `FECTRL_GAIN_MEM0` 0x8CC under a countdown in `FECTRL_GAIN_MEM_CONF` 0x8C8, and
  each array's range of entries forced into one half of one of the six `FECTRL_PBUS_FORCE` words 0x8E0 to 0x8F4,
  its first and second array taking the low and high half. Four of the arrays are the library's own tables;
  `phy_param` 2 and 0x14 feed the first and the eighth. So the pbus is reached through the front end's
  gain-memory words, and what its entries mean is not known.
- `rfpll_chgp_cal`: the RF PLL's charge pump is calibrated through `i2c_rfpll` 0x62's registers 0x0f (its
  setting) and 0x0e (the measurement and its done bit): the block is armed through 0x0f's bits 6 and 5, the
  done bit 0x0e bit 7 is waited for, the measurement 0x0e bits 4..0 is turned into 0x0f bits 4..0, and the
  register is read back into `phy_param` 0x196, which `phy_i2c_init2` then reads. So the pump's setting is
  looped back to the analog I2C's parallel write.
- `get_rf_freq_init_new` / `set_chan_freq_hw_init` / `wr_rf_freq_mem_new`: the frequency memory is built once
  from the RF PLL's two capacitance ends, 0x960 MHz and 0x9a0 MHz read through the ROM's `read_pll_cap`, and
  their difference stepping a running value; each of the 0x55 indices is retuned through the library's
  `ram_rfpll_set_freq` (the table's slot 12), the running value turned into a baseband word by the ROM's
  `get_data_sat`, and the two packed words per index written into the frequency memory by
  `wr_rf_freq_mem_new`. The run is marked in `PARAM_FLAG` bit 5. So the frequency memory is a table of the
  PLL's own setting per channel, read back from the front end's `FECOEX` window.
- `tsens_dac_to_index` / `tsens_dac_cal_new` / `ram_tsens_temp_read_new` / `get_temp_init_new`: the temperature
  sensor's DAC code is read from `i2c_perif` 0x69, host 0, register 6, naming one of the library's five ranges;
  the sensor's own code (`tsens_code_read`) is turned into a temperature through the ROM's `code_to_temp` with
  that range's offset, a temperature outside the range corrects the DAC through the same register, and
  `get_temp_init_new` hands the temperature to the three tracking blocks it starts. So the path is the sensor's
  DAC -> its code -> a temperature -> the blocks' tracked temperatures.
- `rf_init` / `bb_init` / `phy_i2c_init1` / `phy_i2c_master_cmd_mem_init`: the library's bring-up, which its
  `register_chipv7_phy` calls in that order around the calibration's temperature init. `rf_init` brings the
  baseband PLL, the analog bias, the analog I2C, the I2C master, the power detector, the front end, the baseband
  registers and the temperature sensor up, runs the RC calibration, reads the RF PLL's charge pump word, opens
  the analog I2C's blocks through `phy_i2c_init1` (the transmit block 0x6b's registers 2 to 8, the RF PLL 0x62's
  registers 0, 4, 0x0f and 0x0b, the baseband top 0x67's register 2), calibrates the charge pump, programs the
  master's command memory and tunes the frequency module. `bb_init` raises the SAR ADC's power detector drive
  and the frequency setting's bit 28, runs the baseband calibrations once (`param_flag` bit 3), and brings the
  receive table, the gain table, the front end's registers and the channel up. So the analog path is the analog
  bias and PLL -> the analog I2C -> the RF PLL's charge pump -> the command memory, and the baseband path is
  the transmit DC and IQ, the power detector, the receive IQ and the gain tables, each around the frequency
  setting.

- `register_chipv7_phy`, the head the driver's own now: it hands the ROM the PHY table (`phy_get_romfunc_addr`),
  raises MODEM_SYSCON's Wi-Fi and modem clocks (`CLK_CONF1` 0x404), clears the baseband's bit 1, forces the front
  end's transmit and receive off, resets the frequency module and re-enables the frequency setting, and, where
  `phy_param` 0x4E is set, applies the library's `phy_set_23dbm` to its built-in default init data. On a fresh chip
  (`phy_param` 0x25 clear) it spreads the init data into `phy_param` (the caller's, or the built-in default where
  the caller brings none), then checks the saved calibration blob (`phy_rfcal_data_check`): a blob that stands is
  recovered in mode 1 and `param_flag`'s bits 5, 7, 9 and 16 cleared, one that will not do clears the whole flag
  and moves the mode to 2, and any other mode restores the blob into `phy_param` and clears the flag. `rf_init` and
  `bb_init` run, `get_temp_init_new`'s two gates come from `param_flag`'s bits 5 and 20, and, unless the blob was
  recovered in mode 1, the restored block is held against the chip (`phy_rfcal_data_check_value`), saved back
  (`rf_cal_data_backup`) and its negated sum rewritten. Where `phy_param` 0xA1 asks, `chip_v7_set_chan_offset`
  runs; the baseband PLL is taken down, the RF PLL's SDM register is read through the ROM's table, the chip is
  marked up (`phy_param` 0x25), the frequency setting is enabled and the front end's forcing released, and the Wi-Fi
  clock is put back as it was found. So the head is the frame around the bring-up and the calibration, and its one
  mark, `phy_param` 0x25, is what the first-on path and the wake-up path are told apart by.

- `phy_get_romfunc_addr` and the twenty-three slot fillers: the ROM's table at 0x4087f954, which the ROM's own calls reach through `phy_get_romfuncs()` and the library's through `g_phyFuns`. libphy fills twenty-three of its slots with its own functions; the driver's own fills the same twenty-three with the driver's, taken over from the library's with their disassembly. Three of them are the ROM's own byte for byte -- `ram_set_chan_cal_interp`, `ram_set_chan_freq_sw_start` and `ram_chip_i2c_readReg_org`, so the driver's calls the ROM's -- and the rest are written out, their two mask tables among them. The fillers the ROM reaches commonly are the analog I2C's, `ram_get_i2c_read_mask` and `ram_get_i2c_mst0_mask` and `ram_get_i2c_hostid` and `ram_chip_i2c_readReg_org`, and the pbus's and the power detector's. So the table is the seam between the ROM's calls and the driver's code.

- `chip_v7_set_chan_offset` / `mac_tx_chan_offset_new` / `phy_rx_table_init`: the channel's frequency offset is built from the channel itself -- the channel plus two, its low two bits dropped -- and, where `phy_param` 0xA1 is set, `phy_param` 0xA2's signed byte added in eighths, and the ROM's `phy_freq_correct` and `set_channel_rfpll_freq` take it; `mac_tx_chan_offset_new` sets the transmit channel's two offset bits in the transmit minimum power's word, `MAC_TX_MIN_PWR` 0x600a4400 (2 for channel 2, 1 for channel 3, 0 otherwise); and `phy_rx_table_init` writes `phy_param` 0x120/0x121's halfword 0x4f4f and the front end's gain memory 0x4f times, then the front end's registers and the AGC. So the receive table is written from the front end's gain-memory words before `set_rx_gain_table` builds its rows over it. That the PHY's channel setting and the MAC's own start write the same word -- the minimum power and the HE group's bit in `macstart.c`, the channel's offset here -- is an inference from the two functions, not a stated purpose: the PHY writes part of a MAC power word whose meaning is not known.

## Sources

### The modem's windows, Espressif's names

`libphy`'s `phy_reg_check` walks each window from its base to its end with the window's name.

| window | from | to (exclusive) | words |
|---|---|---|---|
| FECOEX | 0x600A0000 | 0x600A00E8 | 58 |
| FEDATA | 0x600A0400 | 0x600A04A4 | 41 |
| FECTRL | 0x600A0800 | 0x600A09CC | 115 |
| btmac | 0x600A1000 | 0x600A1B30 | 716 |
| bt_v3_2 | 0x600A2000 | 0x600A2158 | 86 |
| btagc | 0x600A2800 | 0x600A28FC | 63 |
| zbbb | 0x600A2C00 | 0x600A2C4C | 19 |
| 802154_reg | 0x600A3000 | 0x600A3184 | 97 |
| agc | 0x600A7000 | 0x600A71E4 | 121 |
| bbtx | 0x600A7400 | 0x600A743C | 15 |
| nrx | 0x600A7800 | 0x600A7A2C | 139 |
| bb | 0x600A7C00 | 0x600A7CF0 | 60 |
| brx | 0x600A8000 | 0x600A809C | 39 |
| MODEM_SYSCON | 0x600A9800 | 0x600A9828 | 10 |
| MODEM_LPCON | 0x600AF000 | 0x600AF030 | 12 |
| i2c_mst | 0x600AF800 | 0x600AF838 | 14 |
| pmu | 0x600B0000 | 0x600B01A4 | 105 |
| PCR | 0x60096000 | 0x60096128 | 74 |
| apb (SAR ADC) | 0x6000E000 | 0x6000E06C | 27 |

Source: `phy_reg_check`'s disassembly and strings, and the ROM's register interface
esp32c6.rom.phy.ld.

### The analog I2C's blocks

`libphy`'s `phy_i2c_check` names each block it walks; the IDF register headers under
`build/esp/idf-esp32c6-regi2c/` agree with most.

| block | host | name in `libphy` | IDF header |
|---|---|---|---|
| 0x6A | 1 | i2c_bias | `regi2c_bias.h` `I2C_BIAS` 0x6A, host **0**, where `libphy` reads host 1 -- recorded, not resolved |
| 0x66 | 0 | i2c_bbpll | `regi2c_bbpll.h` `I2C_BBPLL` 0x66, host 0, its fields |
| 0x6B | 1 | i2c_txrf | -- |
| 0x67 | 1 | i2c_bbtop | -- |
| 0x62 | 1 | i2c_rfpll | -- |
| 0x63 | 1 | i2c_rfpll_sdm | -- |
| 0x6D | 0 | i2c_dig_reg | `regi2c_dig_reg.h` `I2C_DIG_REG` 0x6D |
| 0x61 | 0 | i2c_ulp | `regi2c_lp_bias.h` / `regi2c_brownout.h` `I2C_ULP`/`I2C_BOD` 0x61 |
| 0x69 | 0 | i2c_perif | `regi2c_saradc.h` SAR ADC, `I2C_SARADC_TSENS_DAC` reg 0x6 among them |

Source: `phy_i2c_check`'s disassembly and strings, beside the IDF headers (Apache-2.0: the register
names are the facts).

### The pbus's addresses

`libphy`'s `pbus_print` reads each address with `rom_pbus_rd(address, word)` and names the groups.

| group | `rom_pbus_rd(address, word)` |
|---|---|
| rfrx | (0,1), (0,2) |
| rftx | (4,1), (4,2), (5,1) |
| bb | (1,1), (1,2) |
| dco | (2,1), (3,1), (2,2), (3,2) |

### `phy_param`'s labels, Espressif's names

`libphy`'s `phy_cal_print` and `phy_tx_gain_print` print `phy_param`'s fields under these labels; the
field's read stands before the string it belongs to.

| offset | type | label in `libphy` |
|---|---|---|
| 0x00 | s16 | `temp_code` |
| 0x12E | s16 | `temp_code_init` |
| 0xA4 | u32 | `param_flag` |
| 0xE8 | u8 | `rc_dout` |
| 0xE9..0xEC | u8 x4 | `rc_dout` (Wi-Fi) |
| 0xED..0xF0 | u8 x4 | `rc_dout` (BT) |
| 0xDC, 0xDE, 0xE0 | u8 | `DCAP_STG1` |
| 0xDD, 0xDF, 0xE1 | u8 | `DCAP_STG2` |
| 0xF7 | s8 | `wifi_cal_power` |
| 0xF4..0xF6 | s8 | `wifi_cal_atten` |
| 0xF1..0xF3 | s8 | `wifi_pwctrl_atten` |
| 0xD0, 0xD2 | u16 | `TXIQ` |
| 0xE6 | u16 | `LB_TXIQ` |
| 0xA8..0xAE | u16 x4 | `TXDC` |
| 0xFE | s8 | `bt_cal_power` |
| 0xF8..0xFA | s8 | `bt_cal_atten` |
| 0xFB..0xFD | s8 | `bt_pwctrl_atten` |
| 0x102 | u16 | `BT_TXIQ` |
| 0x104..0x10A | u16 x4 | `BT_TXDC` |
| 0xD4, 0xD6 | u16 | `RXIQ` |
| 0x132, 0x134 | s16 | `RXDC_RFRX_BT` |
| 0x14E, 0x150 | s16 | `RXDC_RFRX_WIFI` |
| 0x172, 0x174 | s16 | `RXDC_CHAN_WIFI` |
| 0x1A, 0x1C | u16 | `sar_dc_code`, `sar_ref_code` |
| 0x18E | u16 | `txdc_cal_pwdet` |
| 0x1A4 | u8 | `rx_sat` |
| 0x52 | s8 | `wifi_init_power` |
| 0x123 | s8 | `wifi_correct_power` |
| 0x11C | u16 | the channel |

Source: `phy_cal_print`'s and `phy_tx_gain_print`'s disassembly and strings.

### The init data's spread into `phy_param`

`libphy`'s `register_chipv7_phy_init_param` copies the caller's init data -- the 0x4c bytes it uses of the 0x80 --
into `phy_param` at these offsets, in this order; the bytes 0x51, 0x64 and 0x65 it leaves as it found them.

| `phy_param` | from the init data |
|---|---|
| 0x50 | its byte 0 |
| 0x52..0x63 | its bytes 2..0x13 (eighteen) |
| 0x66 | its byte 0x18 |
| 0x67..0x6f | its bytes 0x43..0x4b (nine) |
| 0x70..0x7d | its bytes 0x19..0x26 (fourteen) |
| 0x7e..0x8b | its bytes 0x27..0x34 (fourteen) |
| 0x8c..0x99 | its bytes 0x35..0x42 (fourteen) |

Source: `register_chipv7_phy_init_param`'s disassembly. `phy_param` 0x51, skipped here, is the crystal's byte,
set by `phy_get_xtal_freq`; the init data's byte 0 is the calibration version the saved blob's head carries.

### The calibration's saved blob

`libphy`'s `register_chipv7_phy` carries a calibration in and out as its `cal_data`, the blob the driver's own
`phy_get_rf_cal_version`, `phy_set_mac_data`/`phy_get_mac_addr`, `phy_rfcal_data_sub`/`rf_cal_data_backup`/
`rf_cal_data_recovery`, `phy_rfcal_data_check` and `phy_rfcal_data_check_value` keep now. Its head is the
calibration's version, `phy_get_rf_cal_version`'s 0x159, and the factory MAC; from 0x0C it holds
`phy_param`[0..0x1A8) word for word, so `phy_param`'s field at offset `o` lies at `o + 0x0C` -- save two that
`rf_cal_data_recovery` restores by hand: `phy_param` 0x1A (`sar_dc_code`) at blob 0x26 and `phy_param` 0x1C
(`sar_ref_code`) at 0x28. The last word, 0x1B4, is the negation of the sum of every word before it, which
`phy_rfcal_data_check` stores on a fresh calibration and checks against a carried-in one.
`phy_rfcal_data_check_value`'s caller's buffer is a second, flat layout of the same fields, 0x66 bytes, which it
fills from `phy_param` with mode zero and holds field by field against `phy_param` with mode set, counting each
that moved by more than four. The fields are `phy_cal_print`'s labels above, and the receive IQ pairs go through
`get_iq_value`'s unpacking first.

Source: these functions' disassembly, and `phy_cal_print`'s labels for the fields.

### What the driver has named

| where | bits | name | meaning | source | certainty |
|---|---|---|---|---|---|
| MODEM+0x418 | 0, 1 | `FEDATA_TXDC_CTRL` start bits | set before each step's wait | `txdc_cal_new`'s disassembly | unknown / inferred |
| MODEM+0x418 | 22 | `FEDATA_TXDC_CTRL_READY` | the wait ends when it is set | the same | inferred |
| MODEM+0x418 | 29 | `FEDATA_TXDC_CTRL_STEP2` | set: the window-2 offset steps down, else up | the same | inferred |
| MODEM+0x418 | 28 | `FEDATA_TXDC_CTRL_STEP3` | set: the window-3 offset steps down, else up; the loop reads it as `lui 0x10000`, the upper immediate, not bit 16 | the same | inferred |
| MODEM+0x420 | | `FEDATA_TX_TONE0` | the transmit tone generator's first word, written by `ram_start_tx_tone_step`/`ram_stop_tx_tone` and by `txiq_get_mis_pwr`'s measurement | the ROM's function names | known |
| MODEM+0x450 | 13 | `FEDATA_REG_SET3_TXCAL` | raised before the TXIQ calibration runs and left raised | `rfcal_txiq_new`'s disassembly | inferred |
| MODEM+0x450 | 14 | `FEDATA_REG_SET3_TXIQ_DONE` | lowered while the TXIQ calibration runs and raised at its end | the same | inferred |
| MODEM+0x450 | 29, 30 | the receive IQ calibration's bits | `rfcal_rxiq_new` raises bit 29 and lowers bit 30 around the receive IQ mismatch's correction, and `rxiq_cal_init` does the same around its gain search, rising bit 13 and lowering bit 14 beside them and reversing all four at its end; no label names them | `rfcal_rxiq_new`'s and `rxiq_cal_init`'s disassembly | unknown |
| FECTRL 0x910 | 14, 15 | `FECTRL_TXRX_OFF`'s receive IQ calibration bits | `rxiq_cal_init` rises both around its work and, where its third argument is clear, clears bit 15; no label names them | `rxiq_cal_init`'s disassembly | unknown |
| phy_param+0xA4 | 10 | `param_flag`'s receive IQ bit | where set, `rxiq_cal_init` has run and returns at once, and it sets it at the end | `rxiq_cal_init`'s disassembly | inferred |
| MODEM+0x450 | 16..21, 10..16 | the receive IQ correction's two fields | the ROM's `rxiq_set_reg` writes the correction's high byte into bits 16..21 (mask 0x3f0000, bits 12..15 and 17..23 cleared) and its low byte into bits 10..16 (mask 0x1fc00, bits 14..20 cleared); the field masks are the ROM's, meaning not known | the ROM's `rxiq_set_reg` [0x40005214] | unknown |
| MODEM+0x478..0x484 | | `FEDATA_RX_IQ_EST2..5` | the estimate's four words `rxiq_get_mis_new` reads and crosses into the receive IQ mismatch; no label names them | `rxiq_get_mis_new`'s disassembly | unknown |
| MODEM+0x705C | 31, 30, 28, 24, 19, 11, 10, 0..18 | `AGC_RFRX_SAT` | the receive saturation's word: `rfrx_sat_rst` raises bits 31, 30, 28, 24 and 19 with its argument and clears them where it is clear, raises bit 11 or bit 10, and clears bits 0..18; no label names it | `rfrx_sat_rst`'s disassembly | unknown |
| MODEM+0x7068 | | `AGC_4` | written 0x404 outright by `rfrx_sat_rst` before `AGC_RFRX_SAT`; no label names it | the same | unknown |
| the ROM's table 0x4087f954 slot 40 (0xA0) | | `rom_iq_est_enable` | the ROM's estimate arming, which `dc_iq_est_high` and `rxiq_get_mis_new` reach through it; the driver's table filling does not touch the slot | the ROM's names and the two readers' disassembly | known |
| phy_param+0x18 | | `PHY_PARAM_TXIQ_TEMP` | the byte `txiq_cal_init_new` starts the forward pair from, a signed temperature | no label names it | unknown |
| phy_param+0x2B | | `PHY_PARAM_TXIQ_ATTEN` | the byte the TXIQ and TX power calibrations take `0x40` (or `0x38`) minus; unsigned | no label names it | unknown |
| phy_param+0x48 | | `PHY_PARAM_TXCAL_TEMP` | the temperature the transmit calibration block last ran at, which the tracking reruns it by | `phy_cal_param_track`'s disassembly | inferred |
| phy_param+0x04 | | `PHY_PARAM_TRACK_TEMP_AT` | the temperature the transmit power tracking last ran at, which `get_temp_init_new` starts from `temp_code_init` | `txpwr_cal_track`'s and `get_temp_init_new`'s disassembly | inferred |
| phy_param+0x130 | | `PHY_PARAM_CAP_TEMP` | the temperature the RFPLL capacitance tracking last ran at, which `get_temp_init_new` starts from `temp_code` | `phy_rfpll_cap_track_new`'s and `get_temp_init_new`'s disassembly | inferred |
| phy_param+0x11F | | `PHY_PARAM_CHANNEL_SECOND` | the byte beside the channel, the retune's second argument | `chip_v7_set_chan`'s and `phy_cal_param_track`'s disassembly | inferred |
| phy_param+0x1A5 | 1 | `PHY_PARAM_TRACK_TEMP_DEBUG` | set: the byte below is the tracking's temperature step, else the block's own | `phy_track_temp_debug` writes it | known |
| phy_param+0x1A6 | | `PHY_PARAM_TRACK_TEMP_DEBUG_TH` | the step `phy_param` 0x1A5 bit 1 selects | the same | known |
| phy_param+0x0A | | `PHY_PARAM_CAP_TRACK` | where set, the whole tracking runs the RFPLL capacitance tracking | the tracking's gate; `phy_force_freq_set` sets it | inferred |
| phy_param+0x0B | | `PHY_PARAM_BT_TRACK` | passed to Bluetooth's transmit-power tracking, whose zero means it does nothing | the tracking's call | inferred |
| phy_param+0x19A | | `PHY_PARAM_TRACK_SKIP` | where set, the parameter calibration is left out | the same | inferred |
| phy_param+0xA4 | 23 | `param_flag`'s RC bit | where set, `rc_cal_new` has run and returns at once | `rc_cal_new`'s disassembly | inferred |
| phy_param+0xA4 | 12 | `param_flag`'s BT transmit DC bit | where set, `bt_txdc_cal_new` has run and returns at once, and it sets it at the end | `bt_txdc_cal_new`'s disassembly | inferred |
| phy_param+0x14 | | the Bluetooth transmit DC calibration's force byte | `bt_txdc_cal_new` shifts it three into the pbus's window 4 place 2; no label names it | `bt_txdc_cal_new`'s disassembly | unknown |
| phy_param+0x10 | 7 | the transmit capacitor scan's print bit | passed to `rfcal_txcap_loop`'s first argument, which prints where set, left out; bit 3 of the same halfword reaches `set_rx_gain_cal_iq_new`'s receive DC | `tx_cap_init_loop`'s and `set_rx_gain_cal_iq_new`'s disassembly | unknown |
| i2c_txrf 0x6B, host 1, reg 0x02 | 3..0 | `I2C_TXRF_DCAP_STG1` | the transmit capacitor's first field, set to 1 before the scan and its best value written after; `phy_param`'s `DCAP_STG1` at 0xDC/0xDE/0xE0 | `tx_cap_init_loop`'s and `rfcal_txcap_loop`'s disassembly | unknown |
| i2c_txrf 0x6B, host 1, reg 0x02 | 7..4 | `I2C_TXRF_DCAP_STG2` | the transmit capacitor's second field, set to 5 before the scan and its best value written after; `phy_param`'s `DCAP_STG2` at 0xDD/0xDF/0xE1 | the same | unknown |
| i2c_txrf 0x6B, host 1, reg 0x0B | 5..4 | `I2C_TXRF_RC_CAL_0B` | the RC calibration's first field, `i2c_rc_cal_set`'s 3 and `i2c_rc_set_reg`'s second argument | the same | unknown |
| i2c_txrf 0x6B, host 1, reg 0x08 | 7..3 | `I2C_TXRF_RC_CAL_08` | the second, 1 and the third argument | the same | unknown |
| i2c_txrf 0x6B, host 1, reg 0x0D | 5..2 | `I2C_TXRF_RC_CAL_0D` | the third, 9 and the first argument | the same | unknown |
| FECTRL 0x8E0..0x8F4 | | `FECTRL_PBUS_FORCE0..5` | the pbus's six force words; `save_pbus_reg` copies them and `write_pbus_mem` programs them, and nothing else names them | `save_pbus_reg`'s and `write_pbus_mem`'s disassembly | unknown |
| phy_param+0x30 | | `PHY_PARAM_PBUS_FORCE` | the six words `save_pbus_reg` copies the force words into | the same | unknown |
| FECTRL 0x8C8 | | `FECTRL_GAIN_MEM_CONF` | `write_pbus_mem`'s countdown word, its bits 11 to 20 taking `(first entry + 0x200)` and stepped by 0x800 an entry; the gain memory's config word too, so one name covers both roles | `write_pbus_mem`'s disassembly | unknown |
| FECTRL 0x8CC | | `FECTRL_GAIN_MEM0` | `write_pbus_mem`'s data word, one pbus entry a write; the gain memory's first word too | the same | unknown |
| libphy rodata | | `pbus_tab1..4` | the library's four tables, of 5, 8, 5 and 3 words, which `set_pbus_mem` copies into its twelve arrays; two of the copies have a word put back by hand and one array of the last pair differs | `set_pbus_mem`'s disassembly | unknown |
| phy_param+0x196 | | `PHY_PARAM_I2C_RFPLL_0F` | `i2c_rfpll`'s register 0x0f read back; `rf_init` reads it and `rfpll_chgp_cal` writes it, and `phy_i2c_init2` reads it | `rf_init`'s and `rfpll_chgp_cal`'s disassembly | unknown |
| i2c_rfpll 0x62, host 1, reg 0x0E | 7 | `I2C_RFPLL_0E_DONE` | `rfpll_chgp_cal` reads it after each 20 us wait, until it rises | `rfpll_chgp_cal`'s disassembly | inferred |
| i2c_rfpll 0x62, host 1, reg 0x0E | 4..0 | `I2C_RFPLL_0E_CHGP` | the measurement `rfpll_chgp_cal` turns into the pump's setting | the same | inferred |
| i2c_rfpll 0x62, host 1, reg 0x0F | 6, 5 | | cleared and then bit 5 set before the calibration; bit 6 set with the setting | the same | unknown |
| i2c_rfpll 0x62, host 1, reg 0x0F | 4..0 | `I2C_RFPLL_0F_CHGP` | the charge pump's setting, the measurement times 7 over 6 plus 9, held at 0x1f | the same | inferred |
| i2c_rfpll 0x62, host 1, reg 0x02 | 7 | | `get_rf_freq_init_new` clears it, retunes through the ROM, and sets it again | `get_rf_freq_init_new`'s disassembly | unknown |
| i2c_rfpll 0x62, host 1, reg 0x02 | 3..0 | | written back by `get_rf_freq_init_new` with register 6's low bits | the same | unknown |
| i2c_rfpll 0x62, host 1, reg 0x06 | 3..0 | | read by `get_rf_freq_init_new` and copied into register 2 | the same | unknown |
| I2C_ANA_MST 0x600AFC00..0x600AFC74 | | `I2C_ANA_MST_CMD0` | the analog I2C master's command memory, thirty words `phy_i2c_master_cmd_mem_init` programs and `phy_i2c_master_mem_txcap` one of; what each says is not known | `phy_i2c_master_cmd_mem_init`'s disassembly | unknown |
| APB_SARADC_CTRL 0x6000E000 | 29 | `APB_SARADC2_PWDET_DRV` | the power detector's drive, which `pwdet_reg_init_new` raises and `bb_init` raises and lowers around its baseband calibration | their disassembly | inferred |
| FECTRL 0x90C | 27 | `FECTRL_FORCE_MODE` | `ram_pbus_force_mode` clears it with the pbus on and sets it off; the ROM's own clears it only where MODEM_SYSCON's bit 1 is clear, which names the word by register alone | the library's `ram_pbus_force_mode`'s disassembly | unknown |
| FEDATA 0x414 | 0, 1 | `FEDATA_TX_TONE_CTRL` | the transmit tone's run bits: `ram_start_tx_tone_step` clears them where either caller word is set and runs them to 3 where both are zero, and `ram_stop_tx_tone` runs them to 3 | the tone functions' disassembly | unknown |
| FEDATA 0x424 | | `FEDATA_TX_TONE1` | the transmit tone generator's second word, packed as `FEDATA_TX_TONE0`'s first is | the same | unknown |
| FEDATA 0x42C | 0..3 | `FEDATA_TX_TONE_CONF` | the tone's step at bits 0..1 and its scale at bits 2..3, `ram_start_tx_tone_step`'s | the same | unknown |
| FECTRL 0x824..0x830 | | `FECTRL_SAR_DOUT0..3` | the SAR's four measurement words `ram_read_sar_dout` unpacks, each into halves at bits 1 and 17; the ROM's own reads eight words there, one halfword each | the library's `ram_read_sar_dout`'s disassembly | unknown |
| nrx 0x780C | 0 | `NRX_BSS_CBW40` | `ram_bb_bss_cbw40`'s bit 0, set where its channel is 3 and cleared otherwise; the ROM's own writes the same | the same | unknown |
| phy_param+0x1A1 | | `PHY_PARAM_PWDET_TONE` | where set, `ram_pwdet_tone_start` clears the tone generator's bit after its wait; the ROM's own always clears it, so the two gates differ | the library's `ram_pwdet_tone_start`'s disassembly | unknown |
| phy_param+0x1A7 | | `PHY_PARAM_STEP_01K` | `phy_set_step_01k` writes it; `ram_rfpll_set_freq` divides the parameter's offset by ten where it is set | the library's `phy_set_step_01k` and `ram_rfpll_set_freq` | inferred |
| libphy rodata 0x4200AC2C and 0x4200AC48 | | the i2c blocks' two mask tables | `ram_get_i2c_read_mask`'s and `ram_get_i2c_mst0_mask`'s thirteen halfwords, one a block from 0x61; the ROM's own hold the same values as words | the library's disassembly and `.rodata` | inferred |
| the ROM's table 0x4087f954 | | the twenty-three slots | `phy_get_romfunc_addr`'s 0x08, 0x10, 0x14, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x68, 0x6c, 0x70, 0x84, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa8, which the driver's package 5b fills with its own | `phy_get_romfunc_addr`'s disassembly and the table `talk/own-phy-phyfuns.txt` | known |
| WIFI_MAC 0x600a4400 | 1..0 | `MAC_TX_MIN_PWR` | the transmit channel's offset: 2 for channel 2, 1 for channel 3, 0 for every other, the word's other bits kept; the ROM's own `mac_tx_chan_offset` [0x40004dbe] also sets bit 0 for channel 2, which is how the library's own differs | `mac_tx_chan_offset_new`'s disassembly | inferred |
| WIFI_MAC 0x600a4400 | 9..4 | `MAC_TX_MIN_PWR` | the transmit minimum power, `macstart.c`'s `mac_set_tx_min_pwr` with -11 from their start | `macstart.c`'s disassembly | inferred |
| WIFI_MAC 0x600a4400 | 17 | `MAC_TX_MIN_PWR` | the HE group's bit, which `macstart.c`'s `mac_he_init` ors in | the same | inferred |
| phy_param+0x20 | | `PHY_PARAM_FREQ_OFFSET` | the frequency correction `chip_v7_set_chan_offset` builds and `chip_v7_set_chan`, `phy_freq_correct` and `set_channel_rfpll_freq` take; no label names it | `chip_v7_set_chan_offset`'s disassembly | unknown |
| phy_param+0xA1 | | `PHY_PARAM_CHAN_OFFSET_EN` | where set, `chip_v7_set_chan_offset` adds `phy_param` 0xA2's signed byte in eighths, and the library's `register_chipv7_phy` calls it at all; no label names it | `chip_v7_set_chan_offset`'s and `register_chipv7_phy`'s disassembly | unknown |
| phy_param+0xA2 | s8 | `PHY_PARAM_CHAN_OFFSET` | the offset `chip_v7_set_chan_offset` adds in eighths where 0xA1 is set; no label names it | the same | unknown |
| phy_param+0x120, 0x121 | u8 x2 | `PHY_PARAM_RX_N2`, `PHY_PARAM_RX_N1` | the receive gain table's two counts: `phy_rx_table_init` writes 0x4f to both through the halfword 0x4f4f, and `set_rx_gain_table` overwrites each with its own row count | `phy_rx_table_init`'s and `set_rx_gain_table`'s disassembly | inferred |
| phy_param+0x25 | | `PHY_PARAM_PHY_UP` | the library's mark that its bring-up has run: `register_chipv7_phy` reads it as the fresh-chip gate and sets it at its end, and `phy_close_rf`'s work waits on it | `register_chipv7_phy`'s and `phy_close_rf`'s disassembly | inferred |
| phy_param+0x4E | | `PHY_PARAM_23DBM_EN` | where set, `register_chipv7_phy` applies `phy_set_23dbm`; `phy_set_23dbm_en` writes it, and nothing the recorded run saw on the board sets it | `register_chipv7_phy`'s and `phy_set_23dbm_en`'s disassembly | unknown |
| phy_param+0xA4 (`param_flag`) | 5, 7, 9, 16 | the blob recovery's cleared bits | in mode 1, a blob that stands has `param_flag` cleared of these -- the mask 0xfffefd5f -- after the blob is restored | `register_chipv7_phy`'s disassembly | inferred |
| phy_param+0xA4 (`param_flag`) | 5, 20 | `get_temp_init_new`'s two gates, inverted | the head passes `~bit 5` as the cap argument and `~bit 20` as the txcal argument; bit 20 is set by no step of the walk | the same | inferred |

## Open questions

- `FECTRL_PWDET_CONF0` 0x600A0810's bits and `FECTRL_PWDET_CONF3` 0x600A081C's low byte: set around the
  power detector's calibration, meaning not known.
- `PHY_PARAM_TXDC` 0xB0/0xB2: written by `txdc_cal_pwdet_init` beside the printed `TXDC` pair at
  0xA8/0xAA, but not part of `phy_cal_print`'s `TXDC`; what they are is not known.
- `phy_param` 0x18 and 0x2B: read by the TXIQ calibration (`txiq_cal_init_new`; 0x2B by
  `tx_pwctrl_init_cal_new` too), no label naming either; what they hold is not known.
- `FEDATA_REG_SET3` 0x600A0450's bits 13 and 14: moved by `rfcal_txiq_new` around its work, no header
  naming them; what they gate is not known.
- Analog I2C 0x6A: `libphy` uses host 1, IDF's `regi2c_bias.h` says host 0. Recorded, not resolved.
- `I2C_ANA_MST_TXCAP` 0x600AFC04: in none of `phy_reg_check`'s windows.
- A DAC code the sensor's five ranges do not name: `tsens_dac_to_index` answers 5 for it, and the library's
  `tsens_dac_cal_new` then indexes a sixth entry past `phy_tsens_attribute`, which its rodata holds as
  `low` = `high` = 256 and `dac` = 0 -- so it answers 0 for a temperature of exactly 256 and corrects otherwise.
  Known and deliberate: the driver corrects for any index outside its five, so it differs from the library for
  that one input, `temp` == 256, which the sensor's code through the ROM's `code_to_temp` cannot reach; the
  off-board comparison leaves that input out. `tsens_dac_to_index` itself keeps the library's answer of 5.
- `phy_param` 0x20, 0xA1 and 0xA2: no label names them. 0x20 is the frequency correction `chip_v7_set_chan_offset`
  builds and `phy_freq_correct` and `set_channel_rfpll_freq` take; 0xA1 gates whether `chip_v7_set_chan_offset`
  runs and whether 0xA2, a signed byte, is added in eighths. What the gate and the offset mean, and where either
  is set -- nothing in the library writes 0xA1 or 0xA2, and in the recorded board run 0xA1 was 0, so
  `chip_v7_set_chan_offset` did not run there -- is not known.
- `MAC_TX_MIN_PWR` 0x600a4400's bits 4..9 and bit 17: the minimum power and the HE group's bit, both from
  `macstart.c` and not otherwise understood; why the PHY's channel setting writes the same transmit power's word
  is not known. Package 11 to 12, the transmit power's, must check whether any of its functions writes the word too.
- `AGC_RFRX_SAT` 0x705C, `AGC_4` 0x7068 and the estimate's four words `FEDATA_RX_IQ_EST2..5` 0x478..0x484: no
  label names any. `rfrx_sat_rst` moves the first's high bits and raises its bit 11 or 10 and writes the second
  with 0x404, and `phy_check_rx_sat` reads the receive gain's SAR field (bits 12..18 of 0x708C) against 0x45 and
  sets `rx_sat`; what the word, the `0x404` and the four estimate words mean -- and where `rx_sat` is read
  besides the estimate's loop -- is not known. The library's `rxiq_get_mis_new` prints the mismatch where its
  third argument is set; the whole driver leaves the PHY prints out, so the covered path is that argument clear.
- `FEDATA_REG_SET3` 0x600A0450's bits 29 and 30 and its two receive IQ correction fields: `rfcal_rxiq_new` and
  `rxiq_cal_init` move bits 29 and 30 around the receive IQ calibration, and the ROM's `rxiq_set_reg` writes the
  correction's high and low bytes into bits 16..21 and 10..16; no header names them, and what they gate or correct
  is not known. The library's `get_rfcal_rxiq_data_new` prints each reading where its third argument is set, left
  out as above.
- `FECTRL_TXRX_OFF` 0x600A0910's bits 14 and 15, and `FEDATA_REG_SET3`'s bits 13 and 14 beside the receive IQ
  calibration's 29 and 30: `rxiq_cal_init` rises the two `FECTRL_TXRX_OFF` bits around the block and clears bit 15
  where its third argument is clear, and moves the `FEDATA_REG_SET3` pair around the search, the same two bits
  `rfcal_txiq_new` names `FEDATA_REG_SET3_TXCAL` and `FEDATA_REG_SET3_TXIQ_DONE`; no header names any of them, and
  whether the two calibrations share the pair or it gates the front end's transmit-receive switch is not known.
- `i2c_txrf` 0x6B register 2's bits 3..0 and 7..4, `phy_param`'s `DCAP_STG1`/`DCAP_STG2` at 0xDC/0xDE/0xE0 and
  0xDD/0xDF/0xE1, and `phy_param`'s halfword 0x10 bit 7: `tx_cap_init_loop` and `rfcal_txcap_loop` scan the
  register for the value whose estimate magnitude is largest and keep each channel's pair in `phy_param`, and the
  halfword's bit chooses whether the library prints; no header names any, and what the capacitor's fields correct
  is not known.
- `phy_param`'s byte 0x14, which `bt_txdc_cal_new` shifts three into the pbus's window 4 place 2, and the twenty
  halfwords `txdc_cal_init_new` settles at 0xA8: no label names the byte, and how the five pairs there relate to
  `TXDC` 0xA8/0xAA (the only labelled pair) and to 0xB0/0xB2 is not known.
- `phy_param` 0x4E, the library's `phy_set_23dbm_en`'s byte: where set, `register_chipv7_phy` applies
  `phy_set_23dbm`, which puts 0x5c (92) into the init data's bytes 2 and 3 and into `phy_param`'s byte 6 -- a
  quarter of 23 dBm in the init data's own unit, hence the name. What sets the gate on the board is not known;
  package 11 to 12, the transmit power's, must read it and confirm the branch runs as the library's.
- `param_flag`'s bits 5, 16 and 20: the head clears bit 5 (beside 7, 9 and 16) when it recovers a blob in mode 1,
  and passes `~bit 5` and `~bit 20` to `get_temp_init_new`; bit 20 is set by no step of the walk, so that argument
  is always set there. What the three bits mean is not known.
- `register_chipv7_phy`'s last device access, the ROM's `rom_i2c_readReg` on the RF PLL's SDM block 0x63, host 1,
  register 0, whose answer it discards: why it is read there, after the baseband PLL is taken down, is not known.
