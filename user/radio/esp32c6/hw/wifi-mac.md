# The Wi-Fi MAC

The Wi-Fi MAC's words lie from 0x600A4000 to 0x600A7000, and its timer's in the page at 0x600AD000;
`macregs.h` names those the radio's code reaches, `mac.c`'s and `macstart.c`'s.
A MAC register is named for the library function that reaches it;
one that only the start's own group reaches, or several with no one role, carries the offset it was reached at,
and one address has one name, `macregs.h`'s when two files share it.
How the MAC came to be the driver's own, its receiving, its sending and its start, is in the station's `NOTES.md`,
which also tells what the receive header's time and the sniffer's masks showed, in "Frames to the group and beacons lost".

## What the register map read

`prog/regmap.c`'s passes, see `README.md`:

- The MAC is blocks of 1 KiB, each with its date word, 0x02207040, at its offset 0x3F8,
  from 0x600A4000 to 0x600A5000, and a block from 0x600A5000 whose date word, 0x02006110, lies at 0x600A55B0.
- The timer's page holds one block of 1 KiB four times over, its date word the MAC's;
  its first word, 0x600AD000, is the microsecond counter the radio's code reads, and the block reads zero
  until the Wi-Fi clock is on.
- The key table lies at 0x600A5800, `mac.c`'s `KEY_BASE`.
- From 0x600A5C00 to 0x600A6400 the words read the same in every pass and every run, halfwords below 0x600A6000
  and whole words above, and no bring-up writes them: a memory left as it powered up, as a hypothesis.
- The receive base, 0x600A4084, read 0x00800000 after the libraries' `wifi_reset_mac` in three runs,
  before the driver's own list is written there;
  how the MAC makes an address of it is not known, and read as their 0x4086e138 is, without bit 30,
  it would be 0x40800000, `__ram_start`, the kernel's `_start`.

## The words named only by their offset

Some of the words the own start writes are named by nothing but the address they were reached at.
Their writers and readers were found by scanning `libpp.a`, `libnet80211.a` and the ROM's ELF for the constant address, each a load or store whose address a register's constant gives.
The scan cannot follow a read through a base and an offset it does not know, so "no reader" below means none found, not that none exists.
A writer's bits are its `andi`, `ori`, `and` and `lui` constants, computed, and a field is named only where a writer's name covers it.
No word below is reached by a ROM function in the scan; every access is in `libpp.a`.

Their `mac_txrx_init`, which `macstart.c`'s `mac_queues_init` writes out:

| word | address | their writer, beyond it | their reader | what is known |
|---|---|---|---|---|
| `TXRX_MAC_110` | 0x600a4110 | -- | -- | nothing |
| `TXRX_MAC_114` | 0x600a4114 | -- | -- | nothing |
| `TXRX_HAL_C20` | 0x600a4c20 | -- | -- | nothing |
| `TXRX_HAL_C24` | 0x600a4c24 | -- | -- | nothing |
| `TXRX_HAL_C60` | 0x600a4c60 | -- | -- | nothing |
| `TXRX_HAL_C98` | 0x600a4c98 | `hal_he_init` | -- | nothing |
| `TXRX_HAL_C9C` | 0x600a4c9c | -- | -- | nothing |
| `TXRX_HAL_CA4` | 0x600a4ca4 | -- | -- | nothing |
| `HAL_CTRL` | 0x600a4308 | `hal_deinit`, `esp_test_clr_hw_statistics` | -- | bit 0 the hardware statistics, which `esp_test_clr_hw_statistics` clears and sets and `hal_deinit` sets; bit 1 `mac_txrx_init`'s |
| `HAL_WORD` | 0x600a4c8c | `hal_init` | `dbg_read_bsr_info` | nothing; a debug dump reads it |

Their `hal_init` and `hal_deinit`, which `mac_config_start` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HAL_CFG` | 0x600a4ddc | `hal_init`, `hal_deinit` | -- | the handshake `mac_config_start` polls for bit 0 |
| `MAC_ENABLE` | 0x600a4c40 | `hal_init`, `hal_enable_mac`, `hal_disable_mac`, `hal_deinit`, `hal_enable/disable_sta_beacon_filter`, `hal_sniffer_rx_set_promis` | `print_isr_regs` | the MAC's enable mask: 0 is it disabled and a set bit lets that function run, so `hal_enable_mac` writes its argument and `hal_disable_mac` 0, `hal_init` leaves 0x19a879e0, and the driver's start and stop write 0; bit 2 the sniffer's promiscuous mode, bit 15 the station's beacon filter |

Their `mac_rxbuf_init`, which `mac_rx_base_init` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `RXBUF_MAC_7C` | 0x600a407c | `mac_rxbuf_init` | -- | nothing |

Their `hal_he_init`, which `mac_he_init` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HE_CTRL` | 0x600a4c80 | `hal_he_init`, `hal_he_bsr_init`, `hal_he_set_default_pe`, `hal_he_set_ul_mu` | `dbg_read_ax_diag`, `dbg_read_bsr_info`, `dbg_read_rx_misc` | bits 0-2 the default packet extension (`hal_he_set_default_pe`, from its argument), bits 3-11 `hal_he_init`'s, bit 27 the buffer status report (`hal_he_bsr_init`), bits 30-31 uplink multi-user (`hal_he_set_ul_mu`) |
| `HE_MAC_10C` | 0x600a410c | `hal_he_init` | `dbg_read_rx_misc` | nothing; a debug dump reads it |
| `HE_HAL_C88` | 0x600a4c88 | `hal_he_init` | -- | nothing |
| `HE_HAL_CBC` | 0x600a4cbc | `hal_he_init` | -- | nothing |

Their `hal_init_bf`, which `mac_init_bf` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `HE_HAL_C78` | 0x600a4c78 | `hal_init_bf`, `esp_test_set_csi_dump_cfg`, `dbg_disable_report_cbf` | `esp_test_get_csi_dump_cfg`, `dbg_read_rx_misc` | `esp_test_set_csi_dump_cfg` clears bits 0-7 and sets bits 1-8 from its argument, the CSI dump configuration; `hal_init_bf` writes 0x20 into bits 0-7 and 0x71 into bits 8-15, which overlap it, 5 into bits 16-18, sets bit 19, and clears bits 21 and 23; `dbg_disable_report_cbf` clears bit 20 |
| `HE_HAL_DE0` | 0x600a4de0 | `hal_init_bf` | -- | nothing |
| `HE_HAL_7128` | 0x600a7128 | `hal_init_bf` | -- | nothing |
| `HE_MAC_470` | 0x600a4470 | `hal_init_bf` | -- | nothing |
| `HE_MAC_474` | 0x600a4474 | `hal_init_bf` | -- | nothing |

Their `hal_timer_update_by_rtc`, which `mac_timer_update_by_rtc` writes out:

| word | address | their writer | their reader | what is known |
|---|---|---|---|---|
| `TIMER_30` | 0x600ad030 | `hal_timer_update_by_rtc`, `pwr_hal_set_lpclk_sync_enable`, `pwr_hal_set_lpclk_sync_disable`, `pwr_hal_set_modem_state_interface`, `pwr_hal_set_modem_state_rxend_exit_code`, `tsf_hal_map_tbtt_target_to_rx_frame`, `tsf_hal_unmap_tbtt_target_to_rx_frame` | -- | bit 16-17 the modem state's rxend exit code, bits 18-19 the modem state interface, bit 27 the low-power clock's sync, bit 28 the TBTT target to RX frame |
| `LPCLK_CYCLE` | 0x600ad070 | `hal_timer_update_by_rtc`, `pwr_hal_set_lpclk_cycle_time` | -- | bits 0-17 the low-power clock's cycle, which both write as a Q13.19 period |

Their names come from a library function, but their fields are not known; the same scan gives their writers and
readers, and `macstart.c`'s own transcription gives the fields:

| word | address | their writers (fields) | their readers | what is known |
|---|---|---|---|---|
| `BBRXHUNG_TIME` | 0x600a4c1c | `hal_he_set_bbrxhung_time` (bits 0-11), `mac_txrx_init` (bits 30-31) | -- | their name says a receive-hung timeout, bits 0-11; the field's scale is not known |
| `HE_MAC_DELAY0` | 0x600a4c54 | `hal_he_set_mac_delay` (bits 10-19, 21-29) | `dbg_read_wdevdelay` | one of the delay pair their mac-delay body writes; its two `env_is_chip` paths write different values, and the driver's the chip's |
| `HE_MAC_DELAY1` | 0x600a4c58 | `hal_he_set_mac_delay` (bits 0-9, 10-19, 21-29) | `dbg_read_wdevdelay` | as `HE_MAC_DELAY0` |
| `HE_TB_TX` | 0x600a4df8 | `hal_init_tb_tx` (bits 14, 15) | `esp_test_get_hw_tb_statistics` | the trigger-based TX word; the reader names it a TB statistics source |
| `HE_TB_POWER0` | 0x600a4430 | `hal_init_tb_power` (bits 0-5, 8-13, 16-21, 24-29) | -- | four 6-bit power indices, from their power table |
| `HE_TB_POWER1` | 0x600a4434 | `hal_init_tb_power` (the same four) | -- | as `HE_TB_POWER0` |
| `HE_TB_POWER2` | 0x600a4438 | `hal_init_tb_power` (the same four, the last two from libphy's `phy_get_max_pwr`) | -- | as `HE_TB_POWER0` |
| `OFDMA_PTI_W0` | 0x600a4dd0 | `hal_set_tb_pti`, `hal_set_beamf_pti` | `hal_set_ofdma_sequence_pti` | a coex PTI word, which their sequence function reads; the fields' layout is not known |
| `OFDMA_PTI_W1` | 0x600a4dd4 | `hal_set_beamf_pti`, `hal_set_beamf_mt_pti` | `hal_set_ofdma_sequence_pti` | as `OFDMA_PTI_W0` |
| `ANTENNA_MAC_2CC` | 0x600a42cc | `hal_attenna_init` (bits 0-2, bit 5) | -- | their antenna init's other word; what it holds is not known |
| `HE_ERSU` | 0x600a4c7c | `hal_he_set_ersu` (bit 10), `hal_he_bsr_init` (bits 0, 4-8, 15), `hal_he_bsr_ctrl` (bit 15), `hal_he_set_bsr_acihigh_comp_method`, `dbg_tb_ignore_cca_enable` (bit 12) | `dbg_read_bsr_info`, `dbg_read_rx_misc`, `hal_he_get_bsr_acihigh_comp_method` | bit 10 their extended-range single-user, bits 0 and 4-8 the buffer status report's, bit 12 their test's "ignore CCA"; the rest is not known |
| `HE_UORA` | 0x600a4c84 | `hal_he_set_uora_parameter` (bits 18-24, 25-31) | -- | uplink-OFDMA random access: 2^OCWmin - 1 and 2^OCWmax - 1, from their start's byte; the word's other bits are not known |
| `HE_BF_REPORT_RATE` | 0x600a4458 | `hal_he_set_bf_report_rate` (bits 0-6, 7-13, 14-20, 21-27) | `dbg_read_bfr_rate` | four 7-bit report-rate fields, their name and the reader's |
| `RX_POLICY` | 0x600a40d8 | `hal_init` (bits 0, 2, 7, 9 set; bits 6, 10, 13 cleared), the driver's own `mac_receive` (bits 4, 6, 10 cleared; bits 1, 8 set) | -- | their `hal_mac_rx_set_policy(0, 1, 1, 1)` clears bits 4, 6 and 10 and their `hal_mac_set_rxq_policy(0, 1)` sets bits 1 and 8; the scan cannot follow those two, which reach the word through its interface's number |

`mac.c` names every word of the first group's kind that it writes, so none of its own is above.

## What Espressif's RF test writes

Espressif's RF test firmware sends and receives Wi-Fi frames with no pp or net80211, see `rftest.md`;
its library, `librftest.a`, writes the MAC's words below (known, the library's disassembly and Ghidra's pseudo-C of the functions named).
The roles are inferred from the functions' names and prints, and nothing here was run on the board.

### The slots, by the test's queue numbers

The library reaches a slot's words through getters that map a queue number from 1 to 27 to an address, through tables in its data.
Queues 4 to 10 and 18 to 20 give slot 0, and queues 11 to 17 and 21 to 27 slots 1 to 7, in `macregs.h`'s layout, which agrees.
The getters name words `macregs.h` does not:

| getter | the word | `macregs.h` |
|---|---|---|
| `Conf2AddrGet` | `TX_QUEUE(s)` + 0x00 | `TX_CONF0` |
| `Conf1AddrGet` | + 0x04 | `TX_MPLEN` |
| `ConfAddrGet` | + 0x08; `BackOffCountGet` reads its low 12 bits as the backoff count | `TX_EDCA` |
| `Plcp0AddrGet` | + 0x0C | `TX_PLCP0` |
| `Plcp1AddrGet` | `TX_PPDU(s)` + 0x00 | `TX_PLCP1` |
| `ThresholdAddrGet` | + 0x04 | `TX_PROT` |
| `HTsigAddrGet` | + 0x0C, the HT-SIG | -- |
| `Hesig1AddrGet` | + 0x18, the HE-SIG-A1 | -- |
| `Hesig2AddrGet` | + 0x1C, the HE-SIG-A2 | -- |
| `PakagenumAddrGet` | + 0x28, a packet count | -- |
| `HT40LENAddrGet` | + 0x30 | `TX_TXLEN` |
| `DurAddrGet` | + 0x38, a duration | -- |

Queues 1, 2 and 3 give addresses where slots 9, 10 and 11 would lie by the same steps:
for queue 1, PLCP0 at 0x600A4CDC and PLCP1 at 0x600A5074.
But slot 11's would be words the MAC uses otherwise:
- its `TX_CONF0`, 0x600A4CB0, is `mac.c`'s `TXQ_STATE01`, which the test's sender clears and prints as `col`;
- its `TX_MPLEN`, 0x600A4CB4, is `TXQ_CLR2`;
- its `TX_PLCP0`, 0x600A4CBC, is `HE_HAL_CBC`.
So queues 1 to 3 are not slots in that layout; what they are is an open question.
The test sends on queue 10, slot 0.

### The test sender

`WifiTxStart` and the functions under it, for each frame:

- `fill_tx_frame` writes slot 0:
  - `TX_EDCA` 0 and `TX_PLCP0` the low 20 bits of the first descriptor's address;
  - `TX_PLCP1` the length in bits 0 to 11 and the rate in bits 12 and up, with 0x200B000 more for an HT rate;
  - the HT-SIG word the MCS, the length shifted 8 and two flags;
  - for an HT rate, `TX_TXLEN`'s bits 0 to 18 the length, and `HAL_WORD` 0x600A4C8C's bit 4 cleared;
  - the HE-SIG-A2 word's bits 11 to 27 the length plus 4.
  The frame lies in up to four descriptors, one an aggregate's subframe, each with its length shifted 14,
  `mac.c`'s `DESC_LEN_SHIFT`, and its bits 30 and 31 as `mac.c`'s `DESC_EOF` and `DESC_OWNER`, which agrees.
- `tx_a_frame` starts it:
  - `MAC_ENABLE` 0x600A4C40 gets bit 1 and `INT_CLEAR` 0x600A4C4C bits 0 and 1;
  - the slot's `TX_EDCA` takes 1 in bits 12 to 21;
  - `TX_PLCP0` takes bits 30 and 31, `mac.c`'s `TX_PLCP0_ARM`, which agrees.
  It then waits for `INT_STATUS` 0x600A4C48's bit 1.
  After 50 ms it prints `tx is blocked!` with five words:
  - `col`, `TXQ_STATE01` 0x600A4CB0, `mac.c`'s collision group, which agrees;
  - `block`, `TX_BLOCK` 0x600A4CA8, which agrees;
  - `diag3`, 0x600A4E54, the word `README.md` saw move between two reads;
  - `bb`, `bb` 0x600A7C08;
  - and the slot's `TX_PLCP0` and `TX_EDCA`.
- `WifiTxStart` writes `TXQ_STATE01` 0 before the run.
  It takes the CCA off where asked, and prints `tx_state` from 0x600A4C74 at the end.
- libphy's `phy_disable_cca_new` writes 0x600A4C5C's bits 28 to 31 as 0xA, and its `phy_enable_cca_new` clears the four.
  The word is the CCA's, as a hypothesis from the names; the ROM's own `phy_disable_cca` writes the AGC instead, see `phy.md`, "Receive".
- For an HE rate, `phy_11ax_tx_mode` writes:
  - `TX_PLCP1`'s bits 25 and 26 as 2;
  - the HE-SIG-A1 word, HE-SIG-A1's 26 bits with HE-SIG-A2's low 6 bits above them in bits 26 to 31;
  - the HE-SIG-A2 word's bits 0 to 9, the rest of HE-SIG-A2, and its bit 10 set for HE-ER and cleared for HE-SU and HE-TB;
  - the HT-SIG word's bit 7 cleared.
  Where the console gives no HE-SIG-A1, the library makes one: the GI and LTF code shifted 21, plus 0xE007, plus eight times the MCS.
- For HE-TB it also writes these words:
  - 0x600A4480's bit 15, set for HE-TB and cleared otherwise;
  - 0x600A44B0, with bit 0, bits 21 and 23, and the RU allocation's index in bits 14 to 20;
  - 0x600A44B4's bits 0 to 11 and 23 to 27;
  - 0x600A44B8, a table's 16 bits with HE-SIG-A2 above;
  - 0x600A44BC's bits 0 to 25, HE-SIG-A1 less 1;
  - 0x600A44C0's bits 0 to 11.
  The values come from `get_hetb_reg`'s table by the RU.
- `tx_nominal_pe_set(rate, pe)` writes the HE nominal packet extension's code into 0x600A4C90, a five-bit field for each group of MCSs:

  | rate codes | MCS | bits |
  |---|---|---|
  | 0x10 | MCS0 | 0 to 4 |
  | 0x11 and 0x12 | MCS1 and 2 | 5 to 9 |
  | 0x13 and 0x14 | MCS3 and 4 | 10 to 14 |
  | 0x15 to 0x17 | MCS5 to 7 | 15 to 19 |
  | 0x18 and 0x19 | MCS8 and 9 | 20 to 24 |

- `esp_hetb_enable(1)` writes fixed values, and `esp_hetb_enable(0)` its others:

  | word | `esp_hetb_enable(1)` | `esp_hetb_enable(0)` |
  |---|---|---|
  | `HE_MAC_DELAY0` | 0x1409D800 | 0x2EA9D800 |
  | `HE_MAC_DELAY1` | 0x0BD234A0 | 0x0BD2356D |
  | `HE_AID` 0x600A4038 | 0x3FE801 | 0x3FE800 |
  | `BSSID_LO` | 0x66666666 | 0 |
  | `BSSID_HI` | 0x80016666, the BSSID 66:66:66:66:66:66 with `BSSID_FLAG` | 0x80000000 |
  | `HE_CTRL` 0x600A4C80 | 0x28010C80 | -- |

- `phy_11ax_rx_set(id)` sets `HE_AID`'s bit 22 and writes `id` into its bits 0 to 10 and 11 to 21.

### The test receiver

`do_rx_poll`, under the console's `esp_rx`, counts frames:

- It writes `KEY_CFG0` 0 and 0x600AD004 0, and clears `INT_CLEAR`'s bits 2 and 3.
- It then polls `INT_STATUS` 0x600A4C48:
  - bit 2 a frame received;
  - bit 3 an event at which it reads `agc` 0x600A706C's low byte, a level as a hypothesis.
  It clears each through `INT_CLEAR`.
  The test's status bits, 1 for a frame sent and 2 and 3 for the receive, are not `mac.c`'s, `INT_RX` 0x4000 and `INT_TX_END`.
  `mac_init` sets `MAC_ENABLE` to 0x1F, not the libraries' 0x19A879E0, so the mask chooses which causes the word reports, as a hypothesis.
- It finds the frame at `*(0x600A4090 + 4 + (RX_LAST_HIGH & 0xFFF00000)) + (RXBUF_MAC_7C & 0xFF)`.
- It reads the last frame's rate from three words:
  - 0x600A40AC's bits 16 to 20, a legacy rate's code, and bits 22 and 23, a format;
  - 0x600A40B0's bit 7, set for an HT frame, and bits 0 to 5, its MCS;
  - 0x600A40A8's low byte.
- The receive filters: `set_mac_filter` and `setmacaddr` write `STA_ADDR_LO/HI` with `STA_ADDR_FLAG` and the BSSID with `BSSID_FLAG`,
  and `rx_per_init` sets `RX_POLICY` and the three interfaces' words after it to 0xD and 0xF in their low bits.

The test's `mac_init` writes these words:
- `HAL_CFG`'s bit 1;
- `RX_POLICY`'s low three bits cleared;
- `HAL_WORD`'s bits 1, 4 and 31 cleared and bit 23 set;
- `RXBUF_MAC_7C`'s bit 28 and its low byte 0xC;
- `MAC_ENABLE` 0x1F;
- `STA_ADDR` 04:05:06:07:08:09, with no flag;
- slot 0's `TX_EDCA` 0x04013000;
- 0x600A4C24's bit 31 cleared;
- the receive's `RX_BASE` and `RX_CTRL`.
