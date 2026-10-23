"""The CYW43439's Bluetooth core parked over the bridge, its processor stopped in the vendor patch; see user/wifi/BT.md.

user/radio/cyw43439/tools/cyw43-rehost.py, user/radio/cyw43439/tools/cyw43-replay.py and user/radio/cyw43439/tools/cyw43-exec.py park the core so, each with its own tail:
the vendor patch goes in as the host writes it, see user/wifi/bt.c, but its code a slide of `cpsid i` into the tail,
so the ROM checks the patch's signature, installs it and, at its first call into the patch, runs the tail for good,
its interrupts masked.
The rehost's tail is SNIFFER, the executor's a branch to its own code.
Each read of the core comes after one of SPARE, see read().
"""

import collections
import time

# The core's addresses the backplane's window reaches, at BOARD_WINDOW and up; and the patch's code, which park() fills.
BOARD_WINDOW = 0x19000000
PATCH = "build/cyw43/43439A0_btfw.bin"
PATCH_CODE, PATCH_CODE_END = 0xD0200, 0xD0CD4
CPSID_I = b"\x72\xb6"  # cpsid i, in Thumb, at an even address
# What the rehost's parked processor runs, at the end of the patch's code, where the slide of cpsid i leads:
#       r4 = 0xe000e100 (NVIC_ISER0), r5 = SNIFFED
#       ICER0 = ICER1 = ~0                      no line wakes the wfi, so the processor sleeps between ticks
#       SYST_RVR = 40000; SYST_CVR = 0; SYST_CSR = 7     its own SysTick, about every millisecond
#       r6 = ICSR; r7 = PENDSTCLR; SHPR3 = 0    the tick the most urgent, so it wakes the wfi wherever this runs
#   1:  if [r5 + 16]: wfi                       asleep while the emulated processor sleeps,
#       else: until ICSR's PENDSTSET: b 1b      awake while it runs, as the chip's power manager sees its processor
#       ICSR = PENDSTCLR                        the tick taken back
#       [r5] |= ISPR0; [r5 + 4] |= ISPR1        the lines seen pending, a disabled line pending too
#       ICPR0 = ISPR0; ICPR1 = ISPR1            so a line still asserted pends again by the next tick
#       [r5 + 8] += 1; b 1b                     the ticks, counted
SNIFFER = bytes.fromhex("4ef20014cef200044ff60075c0f221054ff0ff30c4f88000c4f8840049f6404044f8ec0c002044f8e80c07"
                        "2044f8f00c4ef60456cef200064ff000770020f0612a690ab130bf02e032685201f8d53760d4f80001d4f8"
                        "04112a6802432a606a680a436a60c4f88001c4f88411aa680132aa60e5e7")
SNIFFER_AT = PATCH_CODE_END - 0x80  # where the tail goes, even
SNIFFED = 0x21FF00
# A slow register's read gives the word of the read before, see BT.md: each read of the core comes after one of SPARE,
# so that a read that gives its word is known for one and made again.
SPARE, SPARE_WORD = SNIFFED + 12, 0x5CA1AB1E
ASLEEP = SNIFFED + 16  # nonzero while the emulated processor waits in a wfi
BT_PWRUP = 0x640894
BT_CTRL, BT_FW_RDY = 0x18000C7C, 1 << 24
READS = 4  # the bridge's most reads of one word, BRIDGE_READS
SILENT = collections.Counter()  # registers that never answered a read
REREAD = collections.Counter()  # registers a read gave the word of the read before, read again


def patch_records(path):
    """The patch's records, Intel HEX without the text after a length-prefixed name: (address, bytes)."""
    blob = open(path, "rb").read()
    off, high = blob[0] + 2, 0
    while off + 4 <= len(blob):
        n, addr, kind = blob[off], blob[off + 1] << 8 | blob[off + 2], blob[off + 3]
        if n == 0:
            return
        data = blob[off + 4 : off + 4 + n]
        off += 4 + n
        if kind == 4:
            high = data[0] << 8 | data[1]
        elif kind == 0:
            yield (high << 16) + addr, bytes(data)


def read(bridge, addr):
    """The word at addr in the core, read again where a slow register gives the word of the read before."""
    word, reads = bridge.read_after(BOARD_WINDOW + addr, BOARD_WINDOW + SPARE)
    if reads > 1:
        REREAD[addr] += 1
    if reads >= READS and word == SPARE_WORD:  # every read gave the read before: the register does not answer
        SILENT[addr] += 1
    return word


def host_read(bridge, addr):
    """A word of the host's backplane, read after a word of the core's, so that a stale answer is known for one."""
    return bridge.read_after(addr, BOARD_WINDOW + SPARE)[0]


def parked(addr, data, tail):
    """A record of the patch with its code the slide, and the tail at SNIFFER_AT.

    Each byte of the slide is chosen by its own address, so a record that starts at an odd one keeps the slide
    in step: a halfword at an even address is cpsid i whichever record wrote its two bytes.
    """
    data = bytearray(data)
    for at in range(max(addr, PATCH_CODE), min(addr + len(data), SNIFFER_AT)):
        data[at - addr] = CPSID_I[at & 1]
    for at in range(max(addr, SNIFFER_AT), min(addr + len(data), SNIFFER_AT + len(tail))):
        data[at - addr] = tail[at - SNIFFER_AT]
    return bytes(data)


def park(bridge, tail=SNIFFER, before=None):
    """Have the core's processor stop for good where the vendor patch is first called, running tail.

    before() runs once the core's power is up and before the patch, whose installing sends the core down the slide:
    the place for code the tail leads to.
    """
    if len(tail) > PATCH_CODE_END - SNIFFER_AT:
        raise ValueError(f"a tail of {len(tail)} bytes, where the patch's code leaves {PATCH_CODE_END - SNIFFER_AT}")
    records = [(addr, parked(addr, data, tail)) for addr, data in patch_records(PATCH)]
    for at in range(SNIFFED, ASLEEP + 4, 4):
        bridge.write(BOARD_WINDOW + at, SPARE_WORD if at == SPARE else 0)
    bridge.write(BOARD_WINDOW + BT_PWRUP, 3)
    time.sleep(0.002)
    if before:
        before()
    # In the file's order, each record a block of its own as user/wifi/bt.c writes them: the last two set the
    # pointers the ROM looks for, so they go after everything they point at.
    for addr, data in records:
        start, end = addr & ~3, (addr + len(data) + 3) & ~3
        block = bytearray(end - start)
        block[:4] = read(bridge, start).to_bytes(4, "little")
        block[-4:] = read(bridge, end - 4).to_bytes(4, "little")
        block[addr - start : addr - start + len(data)] = data
        bridge.write_block(BOARD_WINDOW + start, bytes(block))
    # Parked, the core never says it is ready; its stack's words stay as they are.
    time.sleep(0.5)
    if bridge.read(BT_CTRL) & BT_FW_RDY:
        raise RuntimeError("the core said it is ready: the patch ran, it did not park")
    before_words = [read(bridge, 0x200300 + 4 * i) for i in range(64)]
    time.sleep(0.2)
    if before_words != [read(bridge, 0x200300 + 4 * i) for i in range(64)]:
        raise RuntimeError("the core's stack still moves: it did not park")
