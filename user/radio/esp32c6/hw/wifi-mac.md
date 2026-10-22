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
