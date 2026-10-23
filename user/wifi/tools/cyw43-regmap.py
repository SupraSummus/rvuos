#!/usr/bin/env python3
"""The CYW43439's registers read off the chip over the bridge, every word of a page twice in a row, in states.

Usage: user/wifi/tools/cyw43-regmap.py read OUT [--states parked,set,sending] [--setup RECORD]
       user/wifi/tools/cyw43-regmap.py vendor RECORD OUT [--stop-at TEXT]
       user/wifi/tools/cyw43-regmap.py show FILE... [--page ADDR]
       user/wifi/tools/cyw43-regmap.py erom [OUT]
       user/wifi/tools/cyw43-regmap.py wlan OUT
       user/wifi/tools/cyw43-regmap.py trace RECORD [--page ADDR] [--json OUT]
       user/wifi/tools/cyw43-regmap.py phy OUT [--count N] [--conf FILE]
       user/wifi/tools/cyw43-regmap.py radio OUT [--count N] [--flags F,...] [--conf FILE]
       user/wifi/tools/cyw43-regmap.py d11mem OUT [--shm N] [--ucm N] [--scr N] [--tpl N] [--conf FILE]
       user/wifi/tools/cyw43-regmap.py dump ADDR BYTES OUT
       user/wifi/tools/cyw43-regmap.py phytbl OUT [--ids A-B,C,...] [--entries N] [--conf FILE]

read parks the Bluetooth core with the executor's tail, as user/wifi/tools/cyw43-exec.py does, and reads every page in each state:
  parked   as the park leaves the core
  set      once the setup's writes are in, user/wifi/btcore/bt-tx.rec by default
  sending  while the executor sends a packet each turn of a loop, as BT.md's "Our executor" does
vendor parks the core as user/wifi/tools/cyw43-replay.py does, plays the record up to the line of HCI holding TEXT,
the transmitter test's answer by default, and reads every page then, in the state vendor: the vendor's own setup.
Each state's words go into OUT, JSON, by the backplane's address: the two reads and how many asks each took.
show puts the files' states side by side: for each page, how many words read nonzero, moved between the two reads,
or differ between the states, and with --page every such word of that page.

wlan reads the backplane's own pages with the WLAN firmware up and nothing parked, in the state wlan:
ChipCommon, the 802.11 core (D11), the SDIO device, the WLAN's Cortex-M3, the memory's controller, their wrappers,
and the first page of ARM's component 0x301; the PHY's and the radio's registers, reached through D11's address and
data words, which the firmware uses as well, are not read.
trace lists what a record of user/wifi/tools/cyw43-rehost.py's, --record, did to each register of the core's from 0x300000:
its reads and writes, the instructions that made them, and the values written, first and last,
a page at a time, or with --page every register of that page; --json keeps it for the map.
phy reads the 802.11 core's PHY registers, 0 to --count less one, through D11's phyregaddr and phyregdata,
each twice in a row, once the WLAN's processor is held in reset as the driver disables a core,
so that no access of the firmware's comes between an address and its data; the firmware does not run again,
and the chip's power goes off at the next start of the Wi-Fi system. The state is phy.
radio does the same through radioregaddr and radioregdata, the radio's registers 0 to --count less one,
once with each of --flags or'ed into the address, as brcmsmac marks a read for the radios it knows, which this one is not;
it needs none, see user/wifi/hw/phy.md, and the flags show the address's bits.
Only the address word is written, never the data word, so no register of the radio's is.
It first reads the radio's id as brcmsmac does on a core of revision 24 or more: registers 0, 1 and 2, unflagged.
d11mem reads D11's memories, the WLAN's processor held in reset as for phy:
through objaddr and objdata, brcmsmac's way, the shared memory (--shm words), the microcode's (--ucm) and the scratch registers (--scr),
objaddr's read increment set so that each word is one read of objdata;
and through tplatewrptr and tplatewrdata the template memory (--tpl words), whose pointer a read moves on by a word.
phytbl reads the PHY's tables as the ROM's LCN20 code does, the WLAN's processor held in reset:
0x9a5's bit 1 set, the table's id written to 0x0d and an entry's offset to 0x0e, then 0x456, the low half, and 0x457, the high,
each entry at its own offset, --entries of each id in --ids.
A read of a table that is not there hangs the PHY's access, until the chip's next power-up:
every D11 read gives the halfword read last from then on; phyversion and maccontrol's upper half, which differ,
are read every CANARY entries, and what came after the last time both read as they should is dropped,
and the next id is read over a power-up of its own.
dump writes BYTES of the backplane from ADDR to OUT as they read, a word at a time, with the WLAN firmware up.
--conf names a configuration of the Wi-Fi system, such as local/wifi.conf, whose mode's work comes first:
mode=sta's join or mode=ap's access point, so that the PHY or the radio is read as that left it; its run= is the bridge's.
erom reads the chip's enumeration ROM, which ChipCommon's word at 0xfc points to, and lists each core it names:
its id, with the name Linux's bcma gives it, its revision, and its slave ports' and wrappers' address ranges;
the format is bcma's, drivers/bcma/scan.c, and the names its include/linux/bcma/bcma.h.

A read is the bridge's R, after a word of the core's RAM, so a slow register's answer of the read before is read again;
a register no read answers is marked so.
The pages are the Bluetooth core's that the vendor firmware's boot and transmitter test touched,
user/wifi/tools/cyw43-rehost.py's list of register windows; a read only, though a register a read changes is changed.
"""

import argparse
import importlib.util
import json
import os
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "../../.."))
sys.path.insert(0, TOOLS)
from cyw43bridge import Bridge  # noqa: E402
import cyw43park  # noqa: E402

PAGE = 0x1000
W = cyw43park.BOARD_WINDOW
# The Bluetooth core's register pages the vendor firmware touched, from its reset through the transmitter test.
BT_PAGES = [W + p for p in (
    0x300000, 0x310000, 0x314000, 0x318000, 0x320000, 0x321000, 0x326000, 0x327000, 0x329000, 0x32A000, 0x32C000,
    0x351000, 0x352000, 0x360000, 0x364000, 0x370000, 0x371000, 0x410000, 0x600000, 0x640000, 0x650000, 0x651000,
    0x6E8000, 0x6E9000, 0x6EA000, 0x702000)]
STATES = ("parked", "set", "sending")
# The executor's loop for sending: a packet a turn, the next once 0x314004's bit 2 says the one before is done.
SENDING = "label top\nwrite 314004 7f\nwrite 318014 80000005\nuntil 314004 4\nloop top {turns}\n"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS, path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_pages(board, pages, say):
    """Every word of the pages, twice in a row: {addr: [first, second, asks of the first, asks of the second]}."""
    words = {}
    for page in pages:
        start = time.time()
        for addr in range(page, page + PAGE, 4):
            a, n = board.read_after(addr, W + cyw43park.SPARE)
            b, m = board.read_after(addr, W + cyw43park.SPARE)
            words[f"{addr:08x}"] = [a, b, n, m]
        say(f"page {page:08x} in {time.time() - start:.1f} s")
    return words


def cmd_read(args):
    ex = load("ex", "cyw43-exec.py")
    states = args.states.split(",")
    for s in states:
        if s not in STATES:
            raise SystemExit(f"cyw43-regmap: no state {s!r}; the states are {', '.join(STATES)}")
    subprocess_make()
    out = {}
    board = Bridge(log=lambda line: None)
    try:
        ex.park(board)
        say("the executor runs")
        if "parked" in states:
            out["parked"] = read_pages(board, BT_PAGES, say)
        ex.setup(board, args.setup)
        if "set" in states:
            out["set"] = read_pages(board, BT_PAGES, say)
        if "sending" in states:
            # Long enough to outlast the pages' reads; the list is started and left to run, not waited for.
            ex.load(board, ex.parse(SENDING.format(turns=args.turns)))
            go = (cyw43park.read(board, ex.GO) + 1) & 0xFFFFFFFF
            board.write(W + ex.GO, go)
            out["sending"] = read_pages(board, BT_PAGES, say)
            if cyw43park.read(board, ex.DONE) == go:
                say("the sending loop ended before the reads did; raise --turns")
            # The loop is let end before the board is let go, so the chip sends nothing once the run is over.
            end = time.time() + args.turns * 0.002
            while cyw43park.read(board, ex.DONE) != go and time.time() < end:
                time.sleep(0.1)
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"{', '.join(out)} in {args.out}")


# The backplane's pages of the enumeration ROM's cores, and of their wrappers.
WLAN_PAGES = [0x18000000, 0x18001000, 0x18002000, 0x18003000, 0x18004000,
              0x18100000, 0x18101000, 0x18102000, 0x18103000, 0x18104000, 0x18105000, 0x18106000, 0x18200000]


REGS_FROM = 0x300000  # the core's registers start here; below lie its ROM and RAM


def trace_registers(path):
    """{word: {"r": reads, "w": writes, "rpc": {pc: n}, "wpc": {pc: n}, "values": [written, in order, distinct]}}."""
    regs = {}
    with open(path) as lines:
        for line in lines:
            f = line.split()
            if len(f) < 5 or f[0] not in ("r", "w"):
                continue
            addr, value, pc = int(f[1], 16), int(f[3], 16), int(f[4], 16)
            if addr < REGS_FROM:
                continue
            word = addr & ~3
            r = regs.setdefault(word, {"r": 0, "w": 0, "rpc": {}, "wpc": {}, "values": []})
            r[f[0]] += 1
            pcs = r["rpc" if f[0] == "r" else "wpc"]
            pcs[pc] = pcs.get(pc, 0) + 1
            if f[0] == "w" and (not r["values"] or r["values"][-1] != value):
                if value not in r["values"] or len(r["values"]) < 8:
                    r["values"].append(value)
    return regs


def cmd_trace(args):
    regs = trace_registers(args.record)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({f"{k:06x}": v for k, v in regs.items()}, f)
    if args.page is None:
        pages = {}
        for word, r in regs.items():
            p = pages.setdefault(word & ~(PAGE - 1), [0, 0, 0, set()])
            p[0] += 1
            p[1] += r["r"]
            p[2] += r["w"]
            p[3].update(r["rpc"], r["wpc"])
        print(f"{'page':>8} {'regs':>5} {'reads':>7} {'writes':>7} {'places':>6}")
        for page, (n, reads, writes, places) in sorted(pages.items()):
            print(f"{page:08x} {n:>5} {reads:>7} {writes:>7} {len(places):>6}")
        return
    page = int(args.page, 16) & ~(PAGE - 1)
    for word in sorted(w for w in regs if w & ~(PAGE - 1) == page):
        r = regs[word]
        values = " ".join(f"{v:x}" for v in r["values"][:8]) + (" ..." if len(r["values"]) > 8 else "")
        places = sorted(set(r["rpc"]) | set(r["wpc"]))
        where = " ".join(f"{pc:x}" for pc in places[:6]) + (f" +{len(places) - 6}" if len(places) > 6 else "")
        print(f"  {word:06x}  r {r['r']:>6}  w {r['w']:>6}  at {where:40}  wrote {values}")


D11 = 0x18001000
PHYREGADDR, PHYREGDATA, PHYVERSION = D11 + 0x3FC, D11 + 0x3FE, D11 + 0x3E0
CM3_WRAPPER = 0x18103000
AI_IOCTRL, AI_RESETCTRL = 0x408, 0x800


def cmd_phy(args):
    board = Bridge(log=lambda line: say(line) if "root:" in line or "drv:" in line else None, conf=conf_of(args.conf),
                   timeout=90.0)
    try:
        version = board.read16(PHYVERSION)
        say(f"phyversion {version:#06x}: analog {version >> 12}, type {version >> 8 & 0xF}, revision {version & 0xFF}")
        stop_wlan_cpu(board)
        regs, start = {}, time.time()
        for addr in range(args.count):
            board.write16(PHYREGADDR, addr)
            a = board.read16(PHYREGDATA)
            board.write16(PHYREGADDR, addr)
            b = board.read16(PHYREGDATA)
            regs[f"{addr:04x}"] = [a, b]
        say(f"{args.count} PHY registers in {time.time() - start:.1f} s")
        out = {"phy": regs, "phyversion": version}
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"phy in {args.out}")


RADIOREGADDR, RADIOREGDATA = D11 + 0x3D8, D11 + 0x3DA
TPLATEWRPTR, TPLATEWRDATA, OBJADDR, OBJDATA = D11 + 0x130, D11 + 0x134, D11 + 0x160, D11 + 0x164
OBJADDR_RINC = 1 << 25
OBJECTS = (("shm", 1), ("ucm", 0), ("scr", 2))  # brcmsmac's OBJADDR_*_SEL


def cmd_dump(args):
    board = Bridge(log=lambda line: say(line) if "drv:" in line else None, timeout=90.0)
    try:
        start = time.time()
        data = b"".join(board.read(args.addr + i).to_bytes(4, "little") for i in range(0, args.bytes, 4))
    finally:
        board.close()
    with open(args.out, "wb") as f:
        f.write(data)
    say(f"{len(data)} bytes from {args.addr:#x} in {time.time() - start:.1f} s, in {args.out}")


PHY_TABLE_GATE, PHY_TABLE_ID, PHY_TABLE_OFFSET, PHY_TABLE_LO, PHY_TABLE_HI = 0x9A5, 0x0D, 0x0E, 0x456, 0x457
PHY_VERSION = 0xDC00
MACCONTROL_HIGH = D11 + 0x122
CANARY = 16


def phy_read(board, reg):
    board.write16(PHYREGADDR, reg)
    return board.read16(PHYREGDATA)


def phy_write(board, reg, value):
    board.write(PHYREGADDR, (value & 0xFFFF) << 16 | reg)  # the firmware's way: address and data in one word


def table_key(out, tid):
    """The key of a table's read in out: its id, and a count after it when the id was read before."""
    key, n = f"{tid:02x}", 1
    while key in out:
        n += 1
        key = f"{tid:02x}#{n}"
    return key


def read_tables(board, ids, entries, out):
    """Each id of ids into out until one hangs the PHY's access; the ids left."""
    control = board.read16(MACCONTROL_HIGH)
    assert control != PHY_VERSION, "phytbl: the canary's two halfwords read alike"
    phy_write(board, PHY_TABLE_GATE, phy_read(board, PHY_TABLE_GATE) | 2)
    for n, tid in enumerate(ids):
        got = []
        for off in range(entries):
            phy_write(board, PHY_TABLE_ID, tid)
            phy_write(board, PHY_TABLE_OFFSET, off)
            got.append(phy_read(board, PHY_TABLE_LO) | phy_read(board, PHY_TABLE_HI) << 16)
            if (off + 1) % CANARY == 0 or off + 1 == entries:
                if board.read16(PHYVERSION) != PHY_VERSION or board.read16(MACCONTROL_HIGH) != control:
                    good = (off // CANARY) * CANARY
                    out[table_key(out, tid)] = {"entries": got[:good], "hung": True}
                    say(f"table {tid:#04x}: {good} entries, then the PHY's access hung")
                    return ids[n + 1:]
        out[table_key(out, tid)] = {"entries": got, "hung": False}
        say(f"table {tid:#04x}: {entries} entries, {sum(1 for v in got if v)} nonzero")
    return []


def cmd_phytbl(args):
    ids, out = [], {}
    for part in args.ids.split(","):
        first, _, last = part.partition("-")
        ids += range(int(first, 0), int(last or first, 0) + 1)
    while ids:
        board = Bridge(log=lambda line: say(line) if "root:" in line else None, conf=conf_of(args.conf), timeout=90.0)
        try:
            stop_wlan_cpu(board)
            ids = read_tables(board, ids, args.entries, out)
        finally:
            board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"phytbl in {args.out}")


def cmd_d11mem(args):
    board = Bridge(log=lambda line: say(line) if "root:" in line or "drv:" in line else None, conf=conf_of(args.conf),
                   timeout=90.0)
    out = {}
    try:
        stop_wlan_cpu(board)
        for name, sel in OBJECTS:
            count, start = getattr(args, name), time.time()
            board.write(OBJADDR, sel << 16 | OBJADDR_RINC)
            board.read(OBJADDR)
            out[name] = [board.read(OBJDATA) for _ in range(count)]
            say(f"{name}: {count} words in {time.time() - start:.1f} s, {sum(1 for v in out[name] if v)} nonzero")
        start = time.time()
        board.write(TPLATEWRPTR, 0)
        out["tpl"] = [board.read(TPLATEWRDATA) for _ in range(args.tpl)]
        say(f"tpl: {args.tpl} words in {time.time() - start:.1f} s, {sum(1 for v in out['tpl'] if v)} nonzero, "
            f"the pointer at {board.read(TPLATEWRPTR):#x}")
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"d11mem in {args.out}")


def stop_wlan_cpu(board):
    board.write(CM3_WRAPPER + AI_IOCTRL, 0)
    time.sleep(0.01)
    board.write(CM3_WRAPPER + AI_RESETCTRL, 1)
    time.sleep(0.01)
    say(f"the WLAN's processor in reset: RESETCTRL {board.read(CM3_WRAPPER + AI_RESETCTRL):#x}")


def conf_of(path):
    if not path:
        return ""
    with open(path) as f:
        return "".join(line for line in f if not line.startswith("run="))


def cmd_radio(args):
    board = Bridge(log=lambda line: say(line) if "root:" in line or "drv:" in line else None, conf=conf_of(args.conf),
                   timeout=90.0)
    try:
        stop_wlan_cpu(board)
        b = []
        for addr in range(3):
            board.write16(RADIOREGADDR, addr)
            b.append(board.read16(RADIOREGDATA))
        rid = (b[0] & 0xF) << 28 | (b[2] << 8 | b[1]) << 12 | (b[0] >> 4 & 0xF)
        say(f"the radio's id {rid:#010x}: registers 0, 1, 2 read {b[0]:#06x} {b[1]:#06x} {b[2]:#06x}")
        out = {"radio_id": rid, "id_regs": b}
        for flag in (int(x, 0) for x in args.flags.split(",")):
            regs = {}
            for addr in range(args.count):
                board.write16(RADIOREGADDR, addr | flag)
                a = board.read16(RADIOREGDATA)
                board.write16(RADIOREGADDR, addr | flag)
                regs[f"{addr:04x}"] = [a, board.read16(RADIOREGDATA)]
            nz = sum(1 for v in regs.values() if v[0])
            say(f"flag {flag:#06x}: {nz} of {args.count} read nonzero, {len(set(v[0] for v in regs.values()))} values")
            out[f"radio {flag:#x}"] = regs
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"radio in {args.out}")


def cmd_wlan(args):
    board = Bridge(log=lambda line: None)
    try:
        out = {"wlan": read_pages(board, WLAN_PAGES, say)}
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"wlan in {args.out}")


def cmd_vendor(args):
    replay = load("replay", "cyw43-replay.py")
    ops = list(replay.accesses(args.record, args.stop_at))
    board = Bridge(log=lambda line: None)
    try:
        cyw43park.park(board)
        say(f"parked; playing {len(ops)} accesses")
        for kind, addr, size, value in ops:
            word = addr & ~3
            if kind == "r":
                cyw43park.read(board, word)
                continue
            if size < 4:
                shift, mask = 8 * (addr & 3), ((1 << 8 * size) - 1) << 8 * (addr & 3)
                value = cyw43park.read(board, word) & ~mask | (value << shift) & mask
            board.post(W + word, value & ~(0x20000 if word == 0x321520 else 0))  # as the replay's default --clear
        cyw43park.read(board, cyw43park.SPARE)
        out = {"vendor": read_pages(board, BT_PAGES, say)}
    finally:
        board.close()
    with open(args.out, "w") as f:
        json.dump(out, f)
    say(f"vendor in {args.out}")


def cmd_show(args):
    states = {}
    for path in args.files:
        with open(path) as f:
            states.update(json.load(f))
    names = list(states)
    print("states:", ", ".join(names))
    pages = sorted({int(a, 16) & ~(PAGE - 1) for s in states.values() for a in s})
    print(f"{'page':>8}  " + "  ".join(f"{n + ' nz':>10}" for n in names) + f"  {'moving':>6}  {'differ':>6}  {'silent':>6}")
    for page in pages:
        addrs = [f"{a:08x}" for a in range(page, page + PAGE, 4)]
        nz = [sum(1 for a in addrs if a in states[n] and states[n][a][0]) for n in names]
        moving = sum(1 for a in addrs if any(a in states[n] and states[n][a][0] != states[n][a][1] for n in names))
        differ = sum(1 for a in addrs if len({states[n][a][0] for n in names if a in states[n]}) > 1)
        silent = sum(1 for a in addrs if any(a in states[n] and states[n][a][2] >= cyw43park.READS
                                             and states[n][a][0] == cyw43park.SPARE_WORD for n in names))
        print(f"{page:08x}  " + "  ".join(f"{k:>10}" for k in nz) + f"  {moving:>6}  {differ:>6}  {silent:>6}")
    if args.page is not None:
        page = int(args.page, 16) & ~(PAGE - 1)
        print(f"\nthe words of {page:08x} that read nonzero, move or differ; a read's two words joined by /")
        for a in range(page, page + PAGE, 4):
            key = f"{a:08x}"
            vals = [states[n].get(key) for n in names]
            if all(v is None or (v[0] == 0 and v[1] == 0) for v in vals):
                continue
            cells = ["-" if v is None else f"{v[0]:08x}" if v[0] == v[1] else f"{v[0]:08x}/{v[1]:08x}" for v in vals]
            print(f"  {a & 0xFFFFFF:06x}  " + "  ".join(f"{c:>17}" for c in cells))


# Core ids as Linux's bcma names them, include/linux/bcma/bcma.h, by manufacturer: Broadcom's, and ARM's.
MANUF_BCM, MANUF_ARM = 0x4BF, 0x43B
CORE_NAMES = {
    (MANUF_BCM, 0x800): "ChipCommon", (MANUF_BCM, 0x80E): "internal memory", (MANUF_BCM, 0x812): "802.11 (D11)",
    (MANUF_BCM, 0x827): "PMU", (MANUF_BCM, 0x829): "SDIO device", (MANUF_BCM, 0x82A): "ARM Cortex-M3",
    (MANUF_BCM, 0x83E): "ARM Cortex-R4", (MANUF_BCM, 0x840): "GCI", (MANUF_ARM, 0x367): "OOB router",
    (MANUF_BCM, 0xFFF): "default", (MANUF_ARM, 0xFFF): "default",
}
EROM_PTR = 0x180000FC  # ChipCommon's word that says where the enumeration ROM lies
ADDR_TYPES = ("slave", "bridge", "slave wrapper", "master wrapper")


def erom_entries(board):
    """The enumeration ROM's words, up to its end."""
    at, words = board.read(EROM_PTR), [board.read(EROM_PTR)]
    for _ in range(1024):  # bounded: the ROM is a page at most
        w = board.read(at)
        words.append(w)
        at += 4
        if w & 0xF == 0xF:  # the end tag, valid
            break
    return words


def erom_cores(words):
    """The cores the words name, bcma's way: two component words, then master ports and address descriptors."""
    cores, i = [], 0
    while i < len(words):
        w = words[i]
        if w & 0xF == 0xF:
            break
        if w & 0x7 != 0x1:  # not a valid component's first word: skip it
            i += 1
            continue
        cia, cib = w, words[i + 1]
        i += 2
        core = {"manuf": cia >> 20 & 0xFFF, "id": cia >> 8 & 0xFFF, "class": cia >> 4 & 0xF,
                "rev": cib >> 24, "mports": cib >> 4 & 0x1F, "sports": cib >> 9 & 0x1F,
                "mwraps": cib >> 14 & 0x1F, "swraps": cib >> 19 & 0x1F, "ranges": [], "masters": []}
        while i < len(words) and words[i] & 0x1 and words[i] & 0xE in (0x2, 0x4):
            d = words[i]
            i += 1
            if d & 0xE == 0x2:  # a master port
                core["masters"].append(d)
                continue
            base = d & 0xFFFFF000
            if d & 0x8:  # 64-bit: the high word follows
                i += 1
            size = 0x1000 << (d >> 4 & 0x3)
            if d >> 4 & 0x3 == 3:  # the size in a descriptor of its own
                sz = words[i]
                i += 1
                size = sz & 0xFFFFF000
                if sz & 0x8:
                    i += 1
            core["ranges"].append((ADDR_TYPES[d >> 6 & 0x3], d >> 8 & 0xF, base, size))
        cores.append(core)
    return cores


def cmd_erom(args):
    board = Bridge(log=lambda line: None)
    try:
        words = erom_entries(board)
        chip = board.read(0x18000000)
    finally:
        board.close()
    print(f"ChipCommon's chip id word {chip:#010x}: chip {chip & 0xFFFF:#x}, revision {chip >> 16 & 0xF},"
          f" package {chip >> 20 & 0xF}, {chip >> 24 & 0xF} cores, bus type {chip >> 28 & 0xF}")
    print(f"the enumeration ROM at {words[0]:#010x}: {len(words) - 1} words")
    for core in erom_cores(words[1:]):
        name = CORE_NAMES.get((core["manuf"], core["id"]), "?")
        print(f"core {core['id']:#05x} {name}, revision {core['rev']}, manufacturer {core['manuf']:#x},"
              f" ports: {core['mports']} master, {core['sports']} slave, wrappers {core['mwraps']} master,"
              f" {core['swraps']} slave")
        for kind, port, base, size in core["ranges"]:
            print(f"    {kind:15} port {port}  {base:#010x} .. {base + size:#010x}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump(words, f)


def subprocess_make():
    import subprocess
    subprocess.run(["make", "-s", "-C", ROOT, "build/cyw43/bt-exec.bin"], check=True)


def say(text):
    print(f"cyw43-regmap: {text}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("read")
    r.add_argument("out")
    r.add_argument("--states", default=",".join(STATES))
    r.add_argument("--setup", default=os.path.join(ROOT, "user/wifi/btcore/bt-tx.rec"))
    r.add_argument("--turns", type=int, default=40000, help="the sending loop's packets, about 1.25 ms each")
    v = sub.add_parser("vendor")
    v.add_argument("record")
    v.add_argument("out")
    v.add_argument("--stop-at", default="0e04011e2000", help="TEXT: the line of HCI the play ends at")
    s = sub.add_parser("show")
    s.add_argument("files", nargs="+")
    s.add_argument("--page")
    e = sub.add_parser("erom")
    e.add_argument("out", nargs="?")
    w = sub.add_parser("wlan")
    w.add_argument("out")
    ph = sub.add_parser("phy")
    ph.add_argument("out")
    ph.add_argument("--count", type=lambda x: int(x, 0), default=0x1000)
    ph.add_argument("--conf")
    pt = sub.add_parser("phytbl")
    pt.add_argument("out")
    pt.add_argument("--ids", default="0-0x3f")
    pt.add_argument("--entries", type=lambda x: int(x, 0), default=0x400)
    pt.add_argument("--conf")
    du = sub.add_parser("dump")
    du.add_argument("addr", type=lambda x: int(x, 0))
    du.add_argument("bytes", type=lambda x: int(x, 0))
    du.add_argument("out")
    dm = sub.add_parser("d11mem")
    dm.add_argument("out")
    for name, default in (("shm", 0x1000), ("ucm", 0x4000), ("scr", 0x40), ("tpl", 0x4000)):
        dm.add_argument(f"--{name}", type=lambda x: int(x, 0), default=default)
    dm.add_argument("--conf")
    ra = sub.add_parser("radio")
    ra.add_argument("out")
    ra.add_argument("--count", type=lambda x: int(x, 0), default=0x200)
    ra.add_argument("--flags", default="0")
    ra.add_argument("--conf")
    t = sub.add_parser("trace")
    t.add_argument("record")
    t.add_argument("--page")
    t.add_argument("--json")
    args = p.parse_args()
    {"read": cmd_read, "vendor": cmd_vendor, "show": cmd_show, "erom": cmd_erom, "wlan": cmd_wlan,
     "trace": cmd_trace, "phy": cmd_phy, "radio": cmd_radio, "d11mem": cmd_d11mem, "dump": cmd_dump, "phytbl": cmd_phytbl}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
