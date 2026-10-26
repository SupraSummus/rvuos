# The dump engine, and the C5's other dumps

The ESP32-C5 keeps the C6's dump engine at 0x600A9000,
which copies what the receiver hears into a bank of SRAM, a word a sample;
`../../esp32c6/hw/dump.md` tells what the C6's engine does, measured.
On the C5 nothing was run: what follows is the C5's `librftest.a`'s `adctrig` and its callers,
read from their disassembly and Ghidra's pseudo-C, see `README.md`,
with ESP-IDF's C5 headers for the names they give.
A value a function writes is **known**; what a field does, unless a header names it, is **inferred** or a hypothesis.

The C5's library also dumps two things the C6's does not reach,
the receiver's FFT and its EVM, into a memory of the Wi-Fi MAC's frame, "The FFT and EVM dump" below.
For sending, it has no more than the C6's: see "No player".

## The engine's words, as `adctrig` writes them

`adctrig(n, source, mode, div, gain_mode, gain, gain2)` takes `n` samples from `source` (known, its code; the arguments' names are this file's).
Beside the C6's, the fields move:

| Word | Bits | C5 | C6 |
|---|---|---|---|
| `0x600A9004` | the count | bits 0 to 16, `n + 1` | bits 0 to 14 |
| | 17 | cleared, and set for source 6 | cleared |
| | 18 | the samples written: `adctrig` waits on it | the same |
| | 19 | cleared for source 0 alone, the capture's start | the same |
| | 20, 21 | cleared | (bits 15 and 16 held the clock's divider) |
| | 31 | set while the engine runs, cleared at the end | the same |
| `0x600A9008` | the source | bits 17 to 20 | bits 15 to 18 |
| | 21 to 23 | `div >> 1`, 0 where `div` is below 2 | none |
| the lanes | four fields of six bits | **`0x600A9018`**, bit 24 set | `0x600A9014`, bit 24 cleared |
| HP_SYSTEM `SRAM_USAGE_CONF` `0x60095004` | 8 to 11, 16 | bit 9 and `MAC_DUMP_ALLOC` bit 16 set, cleared at the end | bit 10 |
| MODEM_SYSCON `CLK_CONF` | | **`0x600A9C04`** written all ones, `CLK_DATA_DUMP_EN` bit 31 among them; bit 21, `CLK_DATA_DUMP_MUX`, cleared for BLE | `0x600A9804`, not written by `adctrig` |
| FECTRL `0x600A0800` | 2 | set | not written |

- **The bank.**
  ESP-IDF's C5 `hp_system_reg.h` names bits 8 to 11 `HP_SYSTEM_SRAM_USAGE`, a bank of SRAM each given to the MAC's dump,
  and bit 16 `HP_SYSTEM_MAC_DUMP_ALLOC`, which adds 64 KiB to the dump's address (known).
  Bit 9 is the second bank of 128 KiB, from 0x40820000, and the offset makes it **0x40830000**,
  the address `adctrig` prints at its end with the length 0x10000, and that `loop_dump_test` reads back:
  the engine writes the upper half of the second bank, 64 KiB (the bank's arithmetic inferred, the address known).
  The C6's capture lands at 0x40840000.
- **The divider.**
  The C6's bits 15 and 16 divide the engine's clock, measured; the C5's count takes them,
  and bits 20 and 21 of `0x600A9004` and the new field of `0x600A9008` are where a divider would go, as a hypothesis.
- **The time-out.**
  `adctrig` gives up after a second, by the Wi-Fi MAC's timer, which on the C5 is at 0x600AD800, see `README.md`.
  At its end it prints the source word's low half, the done bit, 0x40830000 and 0x10000.

The sources `adctrig` selects, with the lanes it writes into `0x600A9018`, lane 0 lowest:

| `source` argument | the source field | the lanes | what else |
|---|---|---|---|
| 0 | 15 | 24, 25, 26, 27 | the capture started by bit 19 |
| 1, 3 | 1 | the same | |
| 2 | 0 | the same | |
| 4 | 2 | the same | |
| 5 | 3 | the same | a frame sent as it waits, between `phy_disable_cca` and `phy_enable_cca` |
| 6 | 4 | the same | `0x600A9004` bit 17 set; a frame sent as for 5 |
| 7 | 5 | the same | the Wi-Fi MAC's `0x600A4E38` gets bit 8 and a byte, as the C6's `0x600A4E30` does for its source 5 |
| 11 | 11 | 20, 21, 22, 23 | |
| 12 | 9 | 20, 21, 8, 11 | BLE's side: `0x600A20B4` bit 0 set, `0x600A20AC` bits 29 to 31 written 2, `CLK_DATA_DUMP_MUX` cleared, then `ble_rx_start` |
| any other | the argument's low four bits | the same as 0 | |

Every source but 12 clears `0x600A20B4`'s bit 0, Wi-Fi's side, as on the C6.
The lanes were read from the disassembly, field by field, as Ghidra's pseudo-C merges the writes;
the C6's Wi-Fi lanes are 0 to 3 and its BLE lanes 14 to 17, so the C5's signals are numbered anew.
What each lane carries is not known; BLE's 8 and 11 beside Wi-Fi's 20 and 21 is as the code writes it.
Whether `0x600A20AC` is the C6's `0x600A210C`, which the C6's BLE source writes 0x40000000 into the same way,
moved with the BT baseband's growth, is a hypothesis.

`gain_mode` holds the receiver's gain in `AGC_2` 0x600A702C's bits 24 to 31 with bit 23, as on the C6:
1 holds `gain`, 3 holds `gain2` and then `gain` while it waits, and 2 or 3 hold `gain` after the source is set.

## A loopback through the DPD path

`mode` 1 makes `adctrig` its own signal, which the C6's has no way to:
it calls libphy's `phy_pbus_xpd_dpd_path(gain, gain2, 3)`, forces the pbus's rows 2 and 3 from a table of the library's,
starts the tone generator with `phy_start_tx_tone_step(1, 0x19, ...)` and waits 10 ms before it captures,
and stops the tone and clears the pbus at its end (known, its code).
`mode` 2 sends a frame as it waits, as sources 5 and 6 do.
The DPD path is the transmitter's output fed back to the receiver for digital predistortion, by its name;
the C5 has it, `../../esp32c6/hw/phy.md` names no such path on the C6.

`loop_dump_test(mode, print)` uses the path on 5500 MHz, `phy_chip_set_chan(0x157C, k)` with `k` 0 and then 2:
each time it sends a frame with `WifiTxStart_org`, sets the DPD path with `phy_pbus_xpd_dpd_path(s, 1, 1)` for `s` 0x1C3 and then 0x1CB,
and for each captures 2048 samples of source 5, a frame sent as it waits, and reads them back with `print_dump_data(0x40830000, 2048, print)`.
`print_dump_data` takes each word's bits 0 to 9 and 10 to 19 as two signed 10-bit numbers, I and Q,
sums their squares and returns the first of samples 120 to 219 at three times the first hundred's peak or more, the frame's arrival as a hypothesis:
the sample format is the C6's (known, its code; I and Q by the C6's measurement).
`set_dump_mode(n)` clears FECTRL `0x600A08CC`'s bits 19 to 22 and writes `agc` `0x600A70B8`'s bits 0 to 2 as 1 for mode 0, else 0;
the C6's clears FECTRL `0x600A0958`'s bits 0 and 1 instead.

`freq_mem_backup` hands the bank, 0x40830000, to libphy's `phy_freq_mem_backup` as a scratch buffer.

## The FFT and EVM dump

A memory in the Wi-Fi MAC's frame, from `0x600A5C00`, holds what the receiver's baseband dumps.
Two of the C5's functions read it:

- `fft_dump_init(n)` sets up the receiver and the Wi-Fi MAC to dump a frame's FFT:
  - it clears `agc` 0x600A70C4's bit 30, 0x600A70EC's bit 23, 0x600A700C's bits 6 to 10, 0x600A7150's bits 0 to 4,
    0x600A7008's bits 0 to 15 and 0x600A7030's bits 5 to 7,
    and writes 0x600A7010's bits 23 to 31 as 0xF5 over a cleared bit 23 and 0x600A7094's bits 28 to 31 as 1;
  - `nrx` 0x600A7880 and 0x600A7884 get 0x40 in bits 0 to 13; `nrx` 0x600A790C bit 12 takes `n`'s bit 0; `bb` 0x600A7CA8 bit 15 is set;
  - the Wi-Fi MAC's 0x600A4098 bit 23 is set;
  - it waits for `INT_STATUS` 0x600A4C48's bit 2, clears `INT_CLEAR` 0x600A4C4C's bits 2 and 3, and waits again;
  - it then reads the 64 words 0x600A5C00 to 0x600A5CFC, each two signed bytes, bits 8 to 15 and 0 to 7,
    and prints each pair with its power in dB.
  So the 64 words are one OFDM symbol's 64 bins of a frame received, as a hypothesis from the function's name and the count.
- `phy_rxevm_calcu`, libphy's, waits up to 100 us for `bb` 0x600A7C48's bit 8, prints it as `dump_done`,
  takes `nrx` 0x600A7A3C's low byte as `dump_len`, and sums the low halves of that many words from 0x600A5C00:
  the receiver's EVM, by its print, which `phy_rxevm_init_cfg` sets up in `nrx` 0x600A7A38, 0x600A7A34, 0x600A7920 and `bb` 0x600A7C50.

`nrx` 0x600A790C also takes libphy's `phy_chan_dump_cfg(a, b, c)`: bit 3 `a`, bits 4 to 7 `b`, bit 2 `c`,
and `phy_chan_dump_cfg_752` writes its bits 4 to 7 and 11 and `bb` 0x600A7C00's bit 30: a channel dump's set-up, by the names.
The C6's ROM has `phy_chan_dump_cfg_rom`, at 0x40003154, which writes 0x117 into the same word, then the same three fields (known, its disassembly);
`phy.c` names the word `NRX_INIT_5`.
So the C6 may dump its channel into the memory its `wifi-mac.md` finds at 0x600A5C00 and calls a memory left as it powered up,
as a hypothesis no run has tried.

## No player

The C5's library has no more of a player than the C6's (known, its code):

- `dactrig` fills the bank's 64 KiB, from 0x40830000 here, with a ramp, by word stores a byte apart, so each byte ends as its offset divided by eight, its low byte;
  it waits 10 ms and prints its arguments, and writes no register;
- `fedump_wr_txmem`, `fedump_rd_txmem` and `fedump_rd_rxmem` return at once;
- no function of the three libraries writes 0x600A900C or 0x600A9010, the C6's player and its gain word.

So where the C5 plays a bank to its transmitter, if it does, these libraries do not say.
The C6's player sits eight bytes past its capture control, measured; the C5's block keeps its base and its capture control,
so 0x600A900C is the first word to try, as a hypothesis.

## Open questions

- What the new field of `0x600A9008`, bits 21 to 23, and bits 20 and 21 of `0x600A9004` do; the divider, as a hypothesis.
- What the lanes 8, 11 and 20 to 27 carry.
- Whether the BLE source's `0x600A20AC` is the C6's `0x600A210C`.
- Whether 0x600A5C00's words are the FFT's bins, and whether the C6 dumps there too.
- Whether the C5 has a player, at 0x600A900C or elsewhere.
- Every fact here waits for a run on a C5.
