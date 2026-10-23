#!/usr/bin/env python3
"""The ESP32-C6's modem frame as the register map told it, from a run's log: user/radio/esp32c6/prog/regmap.c.

Usage: user/radio/esp32c6/tools/regmap.py LOG [blocks | words KIND | i2c | dump FROM TO]

The program reads every word of 0x600A0000 to 0x600B0000 in four passes, cold, up, again and tuned,
and each word twice in a row; this puts the passes back together, refusing a log whose lines do not follow on, and tells:

blocks (the default)  each 1 KiB block that reads as anything, its window's name, and its words that
                      read nonzero, change as the radio comes up, drift between up and again,
                      move between two reads in a row, and change with the channel but drift not;
                      a block that repeats itself within; and each 4 KiB page's reads' time, which a page
                      that answers only once the bus gives up on it shows, and the stretch it repeats in
words KIND            the words of one kind, each with its values: up, drift, moving or tuned
i2c                   the analog I2C blocks' registers in the passes after the bring-up, those that differ marked
dump FROM TO          the words between two addresses in every pass
"""

import re
import sys

FRAME, SIZE, BLOCK, PAGE = 0x600A0000, 0x10000, 0x400, 0x1000
PASSES = ("cold", "up", "again", "tuned")

# The modem's windows: libphy's phy_reg_check names most, see the station's PHYREGS.md; the rest say whose they are.
WINDOWS = [
    (0x600A0000, 0x600A0400, "FECOEX"),
    (0x600A0400, 0x600A0800, "FEDATA"),
    (0x600A0800, 0x600A0C00, "FECTRL"),
    (0x600A1000, 0x600A2000, "btmac"),
    (0x600A2000, 0x600A2800, "bt_v3_2 (BT baseband)"),
    (0x600A2800, 0x600A2C00, "btagc"),
    (0x600A2C00, 0x600A3000, "zbbb"),
    (0x600A3000, 0x600A4000, "802154_reg"),
    (0x600A4000, 0x600A7000, "Wi-Fi MAC (macregs.h)"),
    (0x600A7000, 0x600A7400, "agc"),
    (0x600A7400, 0x600A7800, "bbtx"),
    (0x600A7800, 0x600A7C00, "nrx"),
    (0x600A7C00, 0x600A8000, "bb"),
    (0x600A8000, 0x600A8800, "brx"),
    (0x600A8800, 0x600A9000, "ETM (btbb_retention_reg.h)"),
    (0x600A9000, 0x600A9800, "dump engine (dump.c)"),
    (0x600A9800, 0x600AA000, "MODEM_SYSCON"),
    (0x600AD000, 0x600AE000, "Wi-Fi MAC timer (mac.c)"),
    (0x600AF000, 0x600AF800, "MODEM_LPCON"),
    (0x600AF800, 0x600B0000, "i2c_mst"),
]

LINE = re.compile(r"^\s*[\d.]+ regmap: (\w+) (.*)$")


def window(a):
    for lo, hi, name in WINDOWS:
        if lo <= a < hi:
            return name
    return "-"


def parse(path):
    words = {p: [0] * (SIZE // 4) for p in PASSES}
    moved = {p: [False] * (SIZE // 4) for p in PASSES}
    pages = {p: {} for p in PASSES}
    i2c = {p: {} for p in PASSES}
    at = {p: 0 for p in PASSES}  # the next word each pass's lines must start at
    last = {p: [0] * 8 for p in PASSES}
    done = set()
    for raw in open(path, errors="replace"):
        m = LINE.match(raw)
        if not m or m.group(1) not in PASSES:
            continue
        name, f = m.group(1), m.group(2).split()
        if f[0] == "done":
            if at[name] != SIZE // 4:
                sys.exit(f"{path}: the {name} pass ends at {FRAME + 4 * at[name]:#x}, short of the frame's end")
            done.add(name)
        elif f[0] == "page":
            pages[name][int(f[1], 16)] = int(f[3])
        elif f[0] == "i2c":
            block, part = int(f[1], 16), f[2]
            base = 16 if part.endswith("+") else 0
            regs = i2c[name].setdefault((block, int(part.rstrip("+"))), [None] * 32)
            for k, v in enumerate(f[3:]):
                regs[base + k] = int(v, 16)
        else:
            a = (int(f[0], 16) - FRAME) // 4
            if f[1] == "same":
                if a != at[name] + 8 * int(f[2]):
                    sys.exit(f"{path}: the {name} pass's run of the same line ends at {FRAME + 4 * a:#x}, "
                             f"not at {FRAME + 4 * (at[name] + 8 * int(f[2])):#x}: a line is missing")
                for k in range(at[name], a):
                    words[name][k] = last[name][k % 8]
                at[name] = a
                continue
            if a != at[name]:
                sys.exit(f"{path}: the {name} pass's line at {FRAME + 4 * a:#x} follows {FRAME + 4 * at[name]:#x}: "
                         "a line is missing")
            mask, vals = int(f[1], 16), [int(v, 16) for v in f[2:10]]
            for k in range(8):
                words[name][a + k] = vals[k]
                moved[name][a + k] = bool(mask >> k & 1)
            last[name] = vals
            at[name] = a + 8
    missing = [p for p in PASSES if p not in done]
    if missing:
        sys.exit(f"{path}: no end of the passes {', '.join(missing)}")
    return words, moved, pages, i2c


def period(ws):
    """The smallest stretch the block repeats in, if it repeats and is not one value throughout."""
    if len(set(ws)) <= 1:
        return None
    n = len(ws)
    p = 4
    while p < n:
        if all(ws[k] == ws[k + p] for k in range(n - p)):
            return p * 4
        p *= 2
    return None


def tuned_word(words, k):
    return words["again"][k] != words["tuned"][k] and words["up"][k] == words["again"][k]


def repeats(words, lo, hi):
    reps = {period(words[p][lo:hi]) for p in PASSES}
    return f"every {reps.pop():#x} bytes" if len(reps) == 1 and None not in reps else ""


def blocks(words, moved, pages):
    print(f"{'block':10} {'window':28} {'nonzero':>7} {'up':>4} {'drift':>5} {'moving':>6} {'tuned':>5}  repeats")
    for b in range(0, SIZE, BLOCK):
        lo, hi = b // 4, (b + BLOCK) // 4
        nz = sum(1 for k in range(lo, hi) if any(words[p][k] for p in PASSES))
        if nz == 0:
            continue
        up = sum(1 for k in range(lo, hi) if words["cold"][k] != words["up"][k])
        drift = sum(1 for k in range(lo, hi) if words["up"][k] != words["again"][k])
        mov = sum(1 for k in range(lo, hi) if any(moved[p][k] for p in PASSES))
        tuned = sum(1 for k in range(lo, hi) if tuned_word(words, k))
        rep = repeats(words, lo, hi)
        print(f"{FRAME + b:#010x} {window(FRAME + b):28} {nz:7} {up:4} {drift:5} {mov:6} {tuned:5}  {rep}")
    print("\npage       " + " ".join(f"{p:>8}" for p in PASSES) + "   us its 2048 reads took, and its repeats")
    for pg in range(FRAME, FRAME + SIZE, PAGE):
        lo = (pg - FRAME) // 4
        print(f"{pg:#010x} " + " ".join(f"{pages[p].get(pg, 0):8}" for p in PASSES) + "   "
              + repeats(words, lo, lo + PAGE // 4))


def words_of(kind, words, moved):
    test = {
        "up": lambda k: words["cold"][k] != words["up"][k],
        "drift": lambda k: words["up"][k] != words["again"][k],
        "moving": lambda k: any(moved[p][k] for p in PASSES),
        "tuned": lambda k: tuned_word(words, k),
    }.get(kind)
    if test is None:
        sys.exit(f"no kind {kind}: up, drift, moving or tuned")
    print(f"{'word':10} {'window':28} " + " ".join(f"{p:>8}" for p in PASSES))
    for k in range(SIZE // 4):
        if test(k):
            a = FRAME + 4 * k
            flags = "".join("*" if moved[p][k] else " " for p in PASSES)
            print(f"{a:#010x} {window(a):28} " + " ".join(f"{words[p][k]:08x}" for p in PASSES) + f"  {flags}")


def show_i2c(i2c):
    for key in sorted(i2c["up"]):
        block, host = key
        rows = [i2c[p].get(key) for p in PASSES[1:]]
        print(f"block {block:#x} host {host}")
        for k in range(32):
            vals = [r[k] if r else None for r in rows]
            mark = "  differs" if len(set(vals)) > 1 else ""
            print(f"  reg {k:2}: " + " ".join(f"{v:02x}" if v is not None else "--" for v in vals) + mark)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    words, moved, pages, i2c = parse(sys.argv[1])
    what = sys.argv[2] if len(sys.argv) > 2 else "blocks"
    if what == "blocks":
        blocks(words, moved, pages)
    elif what == "words" and len(sys.argv) == 4:
        words_of(sys.argv[3], words, moved)
    elif what == "i2c":
        show_i2c(i2c)
    elif what == "dump" and len(sys.argv) == 5:
        lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
        print(f"{'word':10} " + " ".join(f"{p:>8}" for p in PASSES))
        for a in range(lo & ~3, hi, 4):
            k = (a - FRAME) // 4
            print(f"{a:#010x} " + " ".join(f"{words[p][k]:08x}{'*' if moved[p][k] else ' '}" for p in PASSES))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
