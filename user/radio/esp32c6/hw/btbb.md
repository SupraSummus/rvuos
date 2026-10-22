# The BLE and IEEE 802.15.4 baseband

The baseband's words lie from 0x600A2000, its blocks named by `libphy`'s `phy_reg_check`, each repeating in its window,
as the register map read them, see `README.md`:
`bt_v3_2`, 0x15C bytes, its date word 0x02206080 last, four times in 0x600A2000 to 0x600A2800;
`btagc`, 0x100 bytes, its date word 0x02201240 last, four times to 0x600A2C00;
and `zbbb`, the 802.15.4 receiver's, 0xFC bytes, its date word 0x02206170 last, four times to 0x600A3000.
`btbb.c` brings it up, as `libbtbb.a`'s `bt_bb_v2_init_cmplx` did, and the BT MAC's block, `btmac`, lies below it,
from 0x600A1000, its date word, 0x02207081, at 0x600A1FFC.

## The words, what is known

The 0x600a2000 block the IDF register header does not name, only dividing it in btbb_retention_reg.h.
Its fields come from weaker sources, strongest first: the field names ble_tx_config_check prints, which its own
disassembly pairs with each read, checked there; then a getter's own cut, or a setter's write whose bits its
disassembly gives and whose name the function's; then the archive's function names alone, which name a group and not a
field; constants are leads, called hypotheses.
The archive is libbtbb e4897a5.

Its disassembly wants llvm-objdump with --mattr=+m, or every divide by 80 vanishes as unknown, as ble_tx_config_check's two did.

| address | field | what the driver knows | source |
|---|---|---|---|
| 0x600a08fc bits 0..7 | rf_on_bt_delay | a delay; the print divides the byte by 80, hypothesis: 80 MHz ticks, the print in us | ble_tx_config_check .LC3, its disassembly (divu by 80) |
| 0x600a08fc bits 8..15 | pa_on_bt_delay | a delay, divided by 80 in the print, the same hypothesis; btbb_init sets it 0xc8, 2.5 us by it | ble_tx_config_check .LC4 |
| 0x600a1650 bits 0..7 | lc_tx_on_delay | a delay; constant_DC_time is this less bb_tx_on_delay, tx_ramp_delay and tx_rampup_delay, and a check fails when it is 9 or less | ble_tx_config_check .LC7 and .LC8 |
| 0x600a2010 bits 5..12 | bb_tx_on_delay | the BB transmit-on delay; bt_bb_set_le_tx_on_delay writes it, bt_bb_get_le_tx_on_delay reads it and adds 5 | ble_tx_config_check .LC2, and the setter's and getter's names |
| 0x600a2010 bits 24..31 | zb_tx_on_delay | the ZB transmit-on delay, a field of its own beside bb_tx_on_delay's; bt_bb_set_zb_tx_on_delay writes it, bt_bb_get_zb_tx_on_delay reads it and adds 5 | the setter's and getter's names |
| 0x600a2018 bits 3..7 | tx_ramp_delay | a delay | ble_tx_config_check .LC5 |
| 0x600a2018 bits 19..23 | tx_rampup_delay | a delay | ble_tx_config_check .LC6 |
| 0x600a20b8 bits 12..19 | tx_cca_end_ts | the CCA window's end, as the print reads it; bt_bb_tx_cca_period writes bits 9 to 19 there, eleven, so the setter's field and the print's do not agree yet: it writes (first - 5) * 8 - 1 in bit 9's units, hypothesis a finer field the print shows divided by 8 | the setter's disassembly and ble_tx_config_check .LC1, kept apart |
| 0x600a20b8 bits 23..30 | tx_cca_start_ts | the CCA window's start; bt_bb_tx_cca_period writes bits 23 to 30 there too, so the two agree | ble_tx_config_check .LC0, and the setter |

The archive's other functions, the getters and the setters outside the bring-up,
each field the bits its own disassembly cuts, its name from the function's:

| address | field | what the driver knows | source |
|---|---|---|---|
| 0x600a20b0 whole | cur_rx_info | the receive's info word, cut by nothing | bt_bb_get_cur_rx_info's name |
| 0x600a20b0 bits 0..7, signed | channel_pwr | the same word's low byte, sign-extended; hypothesis RSSI in dBm, the only signed level here | bt_get_channel_pwr's name and its sign extension |
| 0x600a20d4 bit 20 | cca fifo reset | written 1 to reset the fifo | bt_bb_tx_cca_fifo_reset's disassembly |
| 0x600a20d4 bits 25..28 | cca fifo count | how many entries wait | bt_bb_tx_cca_fifo_count's disassembly |
| 0x600a20d4 bit 29 | cca fifo empty | | bt_bb_tx_cca_fifo_empty's disassembly |
| 0x600a20d4 bit 30 | cca fifo full | | bt_bb_tx_cca_fifo_full's disassembly |
| 0x600a20d8 whole | cca fifo read | the front entry, one word a read | bt_bb_tx_cca_fifo_read's disassembly |
| 0x600a2064 bits 20..25 | corr thresh LE, argument 0 | bt_bb_set_corr_thresh_le writes its first argument there; bt_bb_rx_correlator_set writes 3 there | the two disassemblies |
| 0x600a2064 bits 14..19 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 4 there | the same |
| 0x600a2060 bits 26..31 | corr thresh LE, argument 2 | the same setter writes its third there; bt_bb_rx_correlator_set writes 4 there, bit 28 | the same |
| 0x600a20f8 bits 5..10 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 3 there | the same |
| 0x600a20f8 bits 19..24 | corr thresh LE, argument 1 | the same setter writes its second there; bt_bb_rx_correlator_set writes 2 there, and bit 25 beside it | the same |
| 0x600a28a0 bits 16..23 | rx_sense | the receive sense; bt_bb_set_rx_sense writes its third argument there when its first is nonzero and its second is 1 or less; bt_agc_rssi_thresh writes 0x9c there | bt_bb_set_rx_sense and bt_agc_rssi_thresh |
| 0x600a2004 bits 8..9 | channel_pwr's enable | cleared by bt_get_channel_pwr_unset | its name |
| 0x600a00c0 bit 25, bits 4..10 | channel_pwr's own word | bt_get_channel_pwr_set sets bit 25, clears bit 15 and writes its argument, 7 bits, in bits 4 to 10; unset clears bit 25; a MODEM word, not the baseband's | the two names |
| 0x600a207c bits 28..31 and 22..23 | bbmac_idle_for_swrst's state | a predicate: the function returns 1 when these, 0x600a1550's low 4, 0x600a3088's low 4 and 0x600a2c38's low 3 are all idle | the function's name and its read of each |
| 0x600a20dc bit 31, bits 15..30, bits 13..14 | wdt | bt_bb_wdt_init(a0, a1) sets bit 31, a0 in bits 15 to 30 and a1 in bits 13 to 14; hypothesis the timeout and a prescaler, a0 16 bits and a1 2 | the function's name and its two arguments' widths |

The blocks of words each group writes, computed from the archive's disassembly and, for `bt_bb_rx_set`, whose call to
`bt_agc_v2_set` hides the words after it, from `btbb.c`, which the replay holds to the archive:

| group | the words it writes |
|---|---|
| bt_bb_v2_tx_set | 0x600a2008, 0x600a200c, 0x600a2018 |
| set_gauss_coeff_1m | 0x600a20e0, 0x600a20e4, 0x600a20e8, 0x600a20ec |
| set_gauss_coeff_2m | 0x600a20f0, 0x600a20f4 |
| bt_bb_set_le_tx_on_delay, bt_bb_set_zb_tx_on_delay | 0x600a2010 |
| bt_bb_tx_cca_period | 0x600a20b8 |
| ieee802154_txon_delay_set | 0x600a3104, 0x600a310c, 0x600a3110, 0x600a3114 |
| bt_bb_v2_rx_set | none of its own; it calls the groups below |
| bt_bb_rx_set | 0x600a2000, 0x600a2008, 0x600a200c, 0x600a204c, 0x600a2050, 0x600a2054, 0x600a20bc, 0x600a20c0, 0x600a2800, 0x600a284c, 0x600a2888, 0x600a288c, 0x600a2c00, 0x600a2c04, 0x600a2c0c, 0x600a4c24, and bt_agc_v2_set |
| bt_agc_v2_set | 0x600a2058, 0x600a2824, 0x600a2828, 0x600a282c, 0x600a2854, 0x600a28a0, 0x600a28c8, 0x600a28d0, 0x600a28d4 |
| bt_bb_rx_correlator_set | 0x600a2060, 0x600a2064, 0x600a2068, 0x600a20f8, 0x600a20fc, 0x600a2100 |
| bt_bb_rx_dpo_set | 0x600a2020, 0x600a2024, 0x600a2028, 0x600a202c, 0x600a2030, 0x600a2034, 0x600a2038, 0x600a28f4 |
| bt_agc_restart_set | 0x600a2884, 0x600a2888, 0x600a28bc, 0x600a28dc, 0x600a28e0, 0x600a28e4, 0x600a28e8, 0x600a28ec, 0x600a28f0 |
| bt_agc_target_set | 0x600a2840, 0x600a2894, 0x600a2898, 0x600a289c, 0x600a28b0, 0x600a28b4 |
| bt_agc_detect_set | 0x600a28a0, 0x600a28a8, 0x600a28ac, 0x600a28b8, 0x600a28c0, 0x600a28c4 |
| bt_agc_recorrect_set | 0x600a2868, 0x600a286c, 0x600a28b0, 0x600a28b4, 0x600a28bc |
| bt_agc_rssi_thresh | 0x600a28a0, 0x600a28ac, 0x600a28b8 |
| bt_agc_gain_offset | 0x600a2848, 0x600a2868 |
| bt_agc_gain_max | 0x600a2840, 0x600a2850 |
| bt_agc_gain_set | 0x600a2854, 0x600a7cd0, and bt_agc_gain_max |
| bt_bb_rx_filter_sel | 0x600a20fc |
| bt_set_rx_comp | 0x600a2844 |
| zb_rx_core_set | 0x600a2c04, 0x600a2c08, 0x600a2c20, 0x600a2c24 |
| zb_rx_v0_0_set | 0x600a2c0c, 0x600a2c10, 0x600a2c14, 0x600a2c18, 0x600a2c2c, 0x600a2c30, 0x600a2c34 |
| zb_rx_auto_sel | 0x600a2c48 |

The constants a read gives a number that says something, each a lead and a hypothesis, computed from the disassembly:

| constant | where | hypothesis |
|---|---|---|
| 0x7a120, 500000 | zb_rx_core_set's 0x600a2c20 bits 8 to 26 and zb_rx_v0_0_set's 0x600a2c2c bits 8 to 26 | 500000, a count or a duration |
| 0xf85edf, -500001 | zb_rx_core_set's 0x600a2c24 bits 8 to 31 and zb_rx_v0_0_set's 0x600a2c30 bits 8 to 31 | the negative beside the 500000, a range about ±500000; -500001 as a signed 24-bit, one from -500000, so the magnitude is the lead |
| 0xbb8, 3000 | zb_rx_core_set's 0x600a2c04 bits 16 to 31 and 0x600a2c08 bits 0 to 15 | 3000, a duration or a count |
| 0x1770, 6000 | zb_rx_core_set's 0x600a2c08 bits 16 to 31 | 6000, twice the 3000 or a second count |
| 0x199a, 6554 | bt_bb_rx_set's 0x600a200c bits 12 to 27 and 0x600a2c00 bits 12 to 27 | 0.1 in Q16, 6554 of 65536 |
