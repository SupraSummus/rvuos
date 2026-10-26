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

## The test's BLE engine, `btmac`

Espressif's RF test firmware, see `rftest.md`, sends and receives BLE test packets by writing the BT MAC's block, `btmac`, directly,
with no link layer and no HCI.
The code is the image's alone, its "RW V9 LE autotest", whose functions are unnamed and given by their address in the image.
The values are known, from the image's disassembly and Ghidra's pseudo-C; the roles are inferred from its prints and the code's order.
None of it was run here: a BLE packet sent this way, without a controller, is untried on the board.

The packet's set-up, the image's 0x40801c04 `(chan, phy)`, for `ble_tx`, `fcc_le_tx`, `ble_tx_continue` and the receive:

| word | written | as a hypothesis |
|---|---|---|
| 0x600a1508 | a byte from the image's table at 0x40820C04 by the BLE channel: 2, 4, ... 22 for channels 0 to 10, 26 to 76 for 11 to 36, 0, 24 and 78 for 37 to 39 | the RF channel, MHz above 2402 |
| 0x600a150c | the power index, held at 15 | the TX power |
| 0x600a1510 | 0, 3, 4, 5, 6 by `phy`, from the image's table at 0x40820014 | the PHY's code |
| 0x600a1514 | 0, 0x00100108, 0x01100108, 0x63900108, 0x63900108 by `phy`, from 0x40820000 | the PHY's timing |
| 0x600a1518 | 0x000300FF | |
| 0x600a151c | 0x76412900 | the access address 0x71764129, its upper three bytes |
| 0x600a1524 bits 0..7 | 0x71 | its low byte |
| 0x600a152c, 0x600a1530, 0x600a1534, 0x600a1538 | 0, 1, 0x103, 0x65B | |
| 0x600a153c | 0x555555 | the CRC's initial value, BLE's for test packets |
| 0x600a1554 | the channel index with bit 6 set | the whitening's seed, as BLE's is |
| 0x600a1830 | the transmit buffer's address, 0x40824578 | the TX data pointer |
| 0x600a1834 | the receive buffer's address, 0x4082443B, the buffer filled with 0x55 first | the RX data pointer |

The transmit buffer holds a header byte, the payload type, then the length, then the payload:
type 0 bytes of 0x55, 1 of 0x0F, 2 a sequence from the image's 0x40801c94 (PRBS9, as a hypothesis), 4 of 0xFF,
and any other type a count, under the header 0x11.
Before it writes, the set-up asserts that 0x600a1550 reads 0, the word `bbmac_idle_for_swrst` reads, above, whose low 4 bits are its idle.

The run, each packet, the image's 0x40802060:
- 0x40801e2c writes 0x600a1110 0 and 0x600a1100 0, then 0x600a1200 0x21, or 3 where its argument and a flag of the image's are both 0,
  then 0x600a1000 1, the start;
- 0x40801f1a waits for 0x600a1110 to read 1, the end; a stop typed on the console writes 0x600a1010 1, the abort;
- 0x40801f48 then takes the packet as sent where 0x600a110c reads 1, 0x600a113c's bit 0 is clear and 0x600a1100 reads 1,
  and clears 0x600a110c, 0x600a1110 and 0x600a1100.
With hopping on, the channel is the Wi-Fi MAC's microsecond counter, 0x600AD000, modulo 40, written into 0x600a1508 and 0x600a1554 again.
Between packets 0x40801660 reads the transmit CCA's fifo, the words of the table above:
it clears 0x600a20d4's bits 21 to 24, returns unless 0x600a2088's bit 19 is set, sets 0x600a2090's bit 19,
and reads an entry from 0x600a20d8, a signed level in bits 15 to 22, a kind from 0 to 2 in bits 30 and 31, counted, and bits 23 to 29 printed.

The receive, `ble_rx`, the image's 0x4080235a:
- clock mode 2, see `rftest.md`, and the set-up above;
- 0x408020cc writes 0x600a1010 1 and reads 0x600a1110, 0x600a1548 and `cur_rx_info` 0x600a20b0;
- 0x40802024 counts a good packet where 0x600a1110 reads 1, 0x600a1134 reads 0 and 0x600a1130 does not,
  then clears 0x600a110c, 0x600a1110, 0x600a1100 and 0x600a1130; where 0x600a1134 is set, it counts a bad one.
  So 0x600a1130 is "received" and 0x600a1134 "CRC error", as a hypothesis;
- with no packet heard, it toggles GPIO 0x60091004's bit 8, a debug pin of the image's.

Continuous transmission, `ble_tx_continue`, the image's 0x40801e58:
- it calls `g_phyFuns` slot 0x14, a slot libphy fills, with the channel's MHz above 2400;
- it writes 0x60049058 1 and 0x6004905c 0, a page no C6 source names;
- it runs the set-up, writes 0x600a150c with the power and 0x600a1110, 0x600a1100 and 0x600a1200 0, and starts with 0x600a1000 1;
- it sets 0x600a2004's bits 10 and 11, continuous.
`ble_tx_continue_stop`, 0x40801f00, writes 0x600a1010 1 and clears the two bits.
0x600a2004's bits 8 and 9, `channel_pwr`'s enable above, lie beside them.

Other words the test writes:
- `le_tx_on_delay d`, 0x40801cea and 0x40816632, writes `lc_tx_on_delay`'s 0x600a1650 bits 0 to 7 and the same `d` into bits 16 to 23,
  and `bb_tx_on_delay`'s 0x600a2010 bits 5 to 12 as `d` - 5, as the table above has the getter add 5 back, which agrees.
- librftest's `phy_check_rssi_1m`, under the console's `check_rssi`, writes 0x600a20b8's bit 31, 0x600a2010's bits 5 to 12 as 0x46, and back to 0x23,
  and 0x600a1650's bytes 0 and 2 as 0x46, and back to 0x32; it starts and stops through 0x600a1000, 0x600a1010, 0x600a1100, 0x600a1110 and 0x600a110c as above.
- librftest's `bt_rx_force` and `force_txon_mode` set `btagc` 0x600a28a0's bits 30 and 31 to force the receiver or transmitter on, see `phy.md`, "Transmit".
- librftest's `force_rx_gain` writes `btagc` 0x600a2840's bits 24 to 31 and 23, the forced gain, see `phy.md`.
