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
  for it.
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
- Analog I2C: `i2c_rfpll` 0x62, `i2c_rfpll_sdm` 0x63 -- `phy_i2c_check`'s names (**known**).
- `phy_param`: `PHY_PARAM_CAP_TEMP` 0x130, `PHY_PARAM_CAP_BUSY` 0x19C, the frequency-select byte
  0x19B (**inferred** from the RFPLL-capacitance tracking loop).

### BBPLL

- Analog I2C: `i2c_bbpll` 0x66, host 0 -- `phy_i2c_check`'s name; IDF's `regi2c_bbpll.h` `I2C_BBPLL`
  0x66 with its fields (**known**). The driver's `i2c_bbpll_set` and `phy_bbpll_cal` write it.
- `phy_param`: `PHY_PARAM_BBPLL_BYTE` 0x4A (**inferred**: the byte read and written back around reg 4).

### Transmit

- MMIO `FEDATA` 0x600A0400: `FEDATA_GAIN_CHAN` 0x410 (the transmit gain channel), `FEDATA_REG_SET1/2/3`
  0x44C/0x46C/0x450, and the transmit DC calibration word 0x418 (below, under corrections).
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
  (**known** labels).
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

- `phy_param`: `PARAM_FLAG` 0xA4's bits 7 (receive DC done), 9 (receive rows built), 14 (TXIQ done);
  the transmit DC calibration's settled pairs `TXDC_CAL_PWDET` +0/+2/+4/+6 at 0x18E..0x194, its printed
  sources `TXDC` 0xA8/0xAA, and 0xB0/0xB2 beside them (an open question).
- MMIO: the transmit DC calibration word `FEDATA_TXDC_CTRL` 0x600A0418 (`FEDATA`) -- meaning **unknown**,
  its bits set out in the table below.
- The receive DC calibration's words: `FECTRL` `FECTRL_RX_GAIN_TRIG_EN` 0x904, `FEDATA`
  `FEDATA_RX_IQ_EST0/1` 0x470/0x474 and `FEDATA_IQ_EST_I/Q/MAG` 0x488/0x48C/0x490, `FEDATA_RX_GAIN_SAR`
  0x4A0 (**inferred** from `set_rx_gain_cal_dc_new`'s loop).

### Sensors

- MMIO `apb` (the SAR ADC) 0x6000E000: `APB_SARADC_CTRL`, `APB_SARADC_TSENS_CTRL` 0x58 (bit 22 its
  power-up), `APB_SARADC_TSENS_CTRL2` 0x5C (bit 15, or bit 19 -- [open], see below). IDF's
  `apb_saradc_reg.h` names them (**known**).
- MMIO `FECTRL` 0x600A0800: the power detector's words `FECTRL_PWDET_CONF0..4`
  0x810/0x814/0x818/0x81C/0x820 (**unknown**: no header names them; the driver's own names) and
  `FECTRL_BT_FILTER` 0x900.
- Analog I2C: `i2c_perif` 0x69, host 0 -- `phy_i2c_check`'s name; IDF's `regi2c_saradc.h` names
  reg 0x6 `I2C_SARADC_TSENS_DAC` (**known**). The driver's temperature read through `tsens_code_read`
  uses it.
- `phy_param`: `temp_code` 0x00, `temp_code_init` 0x12E, `sar_dc_code` 0x1A, `sar_ref_code` 0x1C,
  the DAC index 0x16 (**inferred**: the temperature read writes it and `rf_init` reads it).

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
- `set_tx_gain_mem` takes `&phy_param[0xD0]`, and 0xD0/0xD2 is `TXIQ` in `phy_cal_print`: the transmit
  IQ correction goes into the transmit gain memory beside the gain entries. Whether the library reads
  past 0xD2 is not known here -- the label is a halfword pair, the gain memory's own length is a
  question.

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

### What the driver has named

| where | bits | name | meaning | source | certainty |
|---|---|---|---|---|---|
| MODEM+0x418 | 0, 1 | `FEDATA_TXDC_CTRL` start bits | set before each step's wait | `txdc_cal_new`'s disassembly | unknown / inferred |
| MODEM+0x418 | 22 | `FEDATA_TXDC_CTRL_READY` | the wait ends when it is set | the same | inferred |
| MODEM+0x418 | 29 | `FEDATA_TXDC_CTRL_STEP2` | set: the window-2 offset steps down, else up | the same | inferred |
| MODEM+0x418 | 16 | `FEDATA_TXDC_CTRL_STEP3` | set: the window-3 offset steps down, else up | the same | inferred |

## Open questions

- `FECTRL_PWDET_CONF0` 0x600A0810's bits and `FECTRL_PWDET_CONF3` 0x600A081C's low byte: set around the
  power detector's calibration, meaning not known.
- `PHY_PARAM_TXDC` 0xB0/0xB2: written by `txdc_cal_pwdet_init` beside the printed `TXDC` pair at
  0xA8/0xAA, but not part of `phy_cal_print`'s `TXDC`; what they are is not known.
- Analog I2C 0x6A: `libphy` uses host 1, IDF's `regi2c_bias.h` says host 0. Recorded, not resolved.
- `I2C_ANA_MST_TXCAP` 0x600AFC04: in none of `phy_reg_check`'s windows.
