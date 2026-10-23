# The CYW43439's hardware

What is known of the CYW43439's hardware, a file a part:

| File | Part |
|---|---|
| `bt.md` | the Bluetooth core's registers, as its own processor sees them, through the backplane's window at `0x19000000` |
| `d11.md` | the 802.11 core, D11, at `0x18001000`: its registers, its memories, its microcode |
| `phy.md` | the 802.11 PHY and its radio, behind D11's words: their registers, the PHY's tables, the receiver's samples, what the firmware's code does to them |
| `radio.md` | the WLAN's radio, the 20692, by what the ROM's code does to each of its registers: its overrides, its blocks, the receiver's baseband filter |

`user/wifi/BT.md` tells how the Bluetooth core was taken over and what experiments showed;
this map is where each register's facts are kept, the experiments' among them.

A fact says where it comes from and how sure it is:
**known**, a source names it: Linux's `bcma` and `brcmfmac`, which are open, or the chip's own enumeration ROM;
**traced**, the vendor firmware's own accesses, recorded by `user/wifi/tools/cyw43-rehost.py --record` as it ran over the chip;
**read**, `user/wifi/tools/cyw43-regmap.py` read it off the chip in a state named;
**measured**, a run on the board answered a question, the experiment named;
and **unknown**, or a hypothesis, said as one.

## The backplane

The chip is its cores on one backplane, which the host reaches through gSPI.
The enumeration ROM lists them, **known**: `user/wifi/tools/cyw43-regmap.py erom` reads it, at `0x18108000`,
as ChipCommon's word `0xfc` says, and parses it as `bcma` does, `drivers/bcma/scan.c`.
ChipCommon's first word, `0x1545a9af`, says chip `0xa9af`, 43439, revision 5, package 4.

| From | To | Core, `bcma`'s name | Id | Revision | Wrapper |
|---|---|---|---|---|---|
| `0x18000000` | `0x18001000` | ChipCommon | `0x800` | 49 | `0x18100000` |
| `0x18001000` | `0x18002000` | 802.11, D11 | `0x812` | 39 | `0x18101000` |
| `0x18002000` | `0x18003000` | SDIO device, the host's way in | `0x829` | 21 | `0x18102000` |
| `0x18003000` | `0x18004000` | ARM Cortex-M3, the WLAN's processor | `0x82a` | 9 | `0x18103000` |
| `0x18004000` | `0x18005000` | internal memory, its controller | `0x80e` | 22 | `0x18104000` |
| `0x00000000` | `0x00080000` | internal memory, 512 KiB, the WLAN's RAM | | | |
| `0x10000000` | `0x10080000` | internal memory again | | | |
| `0x1e000000` | `0x1e0a0000` | internal memory, 640 KiB: the WLAN's ROM, the `wl` driver's code and its 3497 strings, **read**; its processor sees it at `0x800000`, where its pointers to its own strings point | | | |
| `0x18000000` | `0x18005000` | ARM's component `0x135`, a bridge over the five cores' pages | | 0 | `0x18105000` |
| `0x19000000` | `0x1a000000` | ARM's component `0x240`: the Bluetooth core's window | | 0 | `0x18106000` |
| `0x18107000` | `0x18108000` | the OOB router, ARM's `0x367` | | 0 | |
| `0x18108000` | `0x18109000` | ARM's `0x366`: the enumeration ROM itself, by its address | | 0 | |
| `0x18200000` | `0x18300000` | ARM's `0x301`, unknown; its first page reads all zeros | | 0 | |

No PMU or GCI is a core of its own: ChipCommon holds the PMU,
and the Bluetooth core's window holds a GCI block, `0x650000` in it, see `bt.md`.
The rest of the space is ARM's default slave, which answers what no core takes.

With the WLAN firmware up and nothing parked, `user/wifi/tools/cyw43-regmap.py wlan` read, **read**,
the words that read nonzero in each core's page and those that moved between two reads in a row:

| Page | Nonzero | Moving |
|---|---|---|
| ChipCommon | 93 | 4 |
| D11 | 198 | 10 |
| SDIO device | 22 | 0 |
| Cortex-M3 | 1012 | 0 |
| memory's controller | 16 | 1 |
| the wrappers, each | 15 to 44 | 0 |

The Cortex-M3's page reads nonzero almost throughout, unlike any other core's: what it shows is not yet known.
D11's PHY and radio registers lie behind its address and data words, which the WLAN firmware uses too;
both are read with the WLAN's processor held in reset, see `phy.md`.

## The tools

They are in `user/wifi/tools/`, beside the code they serve, the observer's beside it in `user/observer/`:

    cyw43-regmap.py erom                         # the cores
    cyw43-regmap.py wlan OUT                     # the cores' pages, the WLAN firmware up
    cyw43-regmap.py read OUT                     # the Bluetooth core's pages: parked, set, sending
    cyw43-regmap.py vendor RECORD OUT            # the same, after the vendor's setup played back
    cyw43-regmap.py trace RECORD [--page ADDR]   # what the vendor firmware did to each register
    cyw43-regmap.py phy OUT                      # the 802.11 PHY's registers, the WLAN's processor stopped
    cyw43-regmap.py radio OUT                    # the 802.11 radio's registers, the same way
    cyw43-regmap.py phytbl OUT                   # the PHY's tables, the same way
    cyw43-regmap.py d11mem OUT                   # D11's memories: shared, microcode, scratch, template
    cyw43-regmap.py dump ADDR BYTES OUT          # any of the backplane, as it reads; the WLAN's ROM so
    cyw43-regmap.py show FILE... [--page ADDR]   # the states side by side
    cyw43-code.py import                         # the WLAN's RAM firmware and ROM in Ghidra, as one program
    cyw43-code.py regs OUT                       # what that code does to the PHY and the radio
    cyw43-code.py decompile ADDR...              # a function's C
    cyw43-iq.py OUT --conf FILE [--gain P6,P7,P8 | --tones MHZ,...]  # the receiver's samples, our code on the WLAN's processor
    cyw43-iq.py OUT --conf FILE --tune MHZ [...]  # the same, the radio tuned to MHZ, 2400 to 2500, by the ROM's own tuning
    cyw43-iq.py OUT --conf FILE --mode psm[,no-q] [...]  # the same, by our microcode on D11's processor, to 1.3 MS/s
    cyw43-iq.py OUT --conf FILE --mode stream[,no-q] [...]  # and through D11's receive DMA, to 1.46 MS/s and 15270 a run
    cyw43-iq.py OUT --conf FILE --mode bits [--tones MHZ,...]  # a converter's bits through the PHY's 0x7e7, 4.47 MS/s, the calibration's, not the air's
    le-observer.py --channel 19 --seconds 10     # the packets heard on a channel, and how strong, on an nRF52840

The observer, `user/observer/` on an nRF52840, stands where BLE's adapter stood in BLE's receiver test:
it counts the packets of the test's access address on one channel, whole or not, in hardware, and measures each one's strength,
and `user/observer/le-observer.py` lets an experiment ask what it heard between two of its own times.

Their files stay in `build/cyw43/regmap/`, and Ghidra's project in `build/cyw43/ghidra/`, out of the tree; the record is the vendor firmware's run.
