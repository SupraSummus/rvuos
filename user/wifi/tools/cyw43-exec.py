#!/usr/bin/env python3
"""Our executor on the CYW43439's Bluetooth core, parked: user/wifi/btcore/bt-exec.c, over the bridge.

Usage: user/wifi/tools/cyw43-exec.py [--setup RECORD] [--times N] PROGRAM

The core is parked as user/wifi/tools/cyw43park.py parks it, but its slide leads to the executor, which runs no vendor code
and waits for a list of operations in its RAM; see user/wifi/BT.md.
--setup writes a record's writes over the bridge first, a list of lines "w ADDR 4 VALUE" as user/wifi/tools/cyw43-replay.py takes,
the transmitter's nine words of user/wifi/btcore/bt-tx.rec for one, or the receiver's of bt-rx.rec.
PROGRAM is the list, an operation a line, numbers in hex but for microseconds and counts:
  write ADDR VALUE    a word written
  wait US             microseconds from now
  mark                the time noted, for wait-mark
  wait-mark US        microseconds from the last mark
  read ADDR           the word read, with the time, into the results, the first 512 of a run
  until ADDR MASK     until a bit of MASK is set in the word at ADDR, or a second has gone by
  label NAME          where a loop goes back to
  loop NAME N         back to NAME, above it, until this line has run N times; no loop in another
The list runs --times times, and each run's results print as the microseconds since its first read and the word.
Each address is a word's.
Every write of the carrier, in the setup or the list, is held to the band with bit 7 set, as the transmitter writes it,
since the executor writes whatever it is handed; the bridge holds it to the band alone.
No write goes to the patch's code, the executor's or its words, which would change what runs.
"""

import argparse
import os
import struct
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "../../.."))
sys.path.insert(0, TOOLS)
from cyw43bridge import Bridge  # noqa: E402
from cyw43park import BOARD_WINDOW, PATCH_CODE, SNIFFER_AT, park as park_core, read  # noqa: E402

EXEC_BIN = os.path.join(ROOT, "build/cyw43/bt-exec.bin")
EXEC_AT = 0xD0D00  # BT_STUB of user/wifi/btcore/bt-map.h
EXEC_ROOM = 0x200  # the bytes bt-exec.ld keeps it within
BRANCH = bytes.fromhex("40f60150c0f20d000047")  # movw r0, #0x0d01; movt r0, #0x0d; bx r0: to EXEC_AT, in Thumb
BASE = 0x21C000  # bt-exec.c's words
GO, DONE, COUNT, ALIVE, GAVE_UP = BASE, BASE + 4, BASE + 8, BASE + 12, BASE + 20
LIST, OUT, OPS, RESULTS = BASE + 0x100, BASE + 0xD00, 256, 512
STACK_ROOM = 0x400  # below BASE, where bt-exec.c's stack grows
END, WRITE, WAIT, READ, LOOP, UNTIL, MARK, WAIT_MARK = range(8)
MHZ = 48  # the parked core's clock, cycles a microsecond
CARRIER = 0x3186A4
# What no write touches: the patch's code with the slide and the branch, the executor, its stack, words, list and results.
OWN = ((PATCH_CODE, EXEC_AT + EXEC_ROOM), (BASE - STACK_ROOM, OUT + 8 * RESULTS))


def writable(addr, value):
    """Why a word's write is refused, or None: unaligned, outside 32 bits, the executor's own, or the carrier out of the band."""
    if addr & 3 or not 0 <= addr < 1 << 32 or not 0 <= value < 1 << 32:
        return "not a word"
    if any(lo <= addr < hi for lo, hi in OWN):
        return "the executor's own"
    if addr == CARRIER and ((value & ~0xFF) != 0 or (value & 0x80) == 0 or (value & 0x7F) > 78):
        return "leaves the band"
    return None


def park(board):
    """Park the core, its slide leading to the executor; raise if the executor does not run."""
    code = open(EXEC_BIN, "rb").read()
    code += bytes(-len(code) % 4)
    assert PATCH_CODE < SNIFFER_AT and len(code) <= EXEC_ROOM

    def before():  # once the core's power is up, before the patch, whose installing sends the core down the slide
        board.write_block(BOARD_WINDOW + BASE, bytes(32))
        board.write_block(BOARD_WINDOW + EXEC_AT, code)

    park_core(board, BRANCH, before)
    before_turns = read(board, ALIVE)
    time.sleep(0.1)
    if read(board, ALIVE) == before_turns:
        raise RuntimeError("the executor does not run")


def setup(board, path):
    """A record's writes, over the bridge, each held as the list's are; every line is checked before any is written."""
    writes = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            w = line.split()
            if not w or w[0] != "w":
                continue
            addr, size, value = int(w[1], 16), int(w[2]), int(w[3], 16)
            why = "not a word" if size != 4 else writable(addr, value)
            if why:
                raise ValueError(f"{path}:{n}: {addr:#x} = {value:#x}: {why}")
            writes.append((addr, value))
    for addr, value in writes:
        board.write(BOARD_WINDOW + addr, value)


def cycles(us, n):
    """Microseconds as the executor's cycles, a word's worth at most."""
    c = round(float(us) * MHZ)
    if not 0 <= c < 1 << 32:
        raise ValueError(f"line {n}: {us} us is more than a word of cycles")
    return c


def parse(text):
    """A program's text as operations (kind, addr, value)."""
    ops, labels, loops = [], {}, []
    for n, line in enumerate(text.splitlines(), 1):
        f = line.split("#", 1)[0].split()
        if not f:
            continue
        try:
            parse_line(n, f, ops, labels, loops)
        except ValueError as e:
            raise ValueError(e if str(e).startswith("line ") else f"line {n}: {e}") from None
    return ops


def parse_line(n, f, ops, labels, loops):
    """A program's line n, its words f, as an operation added to ops."""
    word, args = f[0], f[1:]
    need = {"write": 2, "wait": 1, "mark": 0, "wait-mark": 1, "read": 1, "until": 2, "label": 1, "loop": 2}
    if word not in need:
        raise ValueError(f"line {n}: no operation {word!r}")
    if len(args) != need[word]:
        raise ValueError(f"line {n}: {word} takes {need[word]} arguments")
    if word == "write":
        ops.append((WRITE, int(args[0], 16), int(args[1], 16)))
    elif word == "wait":
        ops.append((WAIT, 0, cycles(args[0], n)))
    elif word == "mark":
        ops.append((MARK, 0, 0))
    elif word == "wait-mark":
        ops.append((WAIT_MARK, 0, cycles(args[0], n)))
    elif word == "read":
        ops.append((READ, int(args[0], 16), 0))
    elif word == "until":
        ops.append((UNTIL, int(args[0], 16), int(args[1], 16)))
    elif word == "label":
        if args[0] in labels:
            raise ValueError(f"line {n}: label {args[0]!r} again")
        labels[args[0]] = len(ops)
    elif word == "loop":
        if args[0] not in labels:
            raise ValueError(f"line {n}: no label {args[0]!r} above")
        times = int(args[1])
        if not 1 <= times < 1 << 32:
            raise ValueError(f"line {n}: a loop of {times} times")
        # The executor keeps one count of turns, so no loop goes back past another.
        if loops and loops[-1] >= labels[args[0]]:
            raise ValueError(f"line {n}: a loop around another")
        loops.append(len(ops))
        ops.append((LOOP, labels[args[0]], times))
    if ops and ops[-1][0] in (WRITE, READ, UNTIL) and ops[-1][1] & 3:
        raise ValueError(f"line {n}: {ops[-1][1]:#x} is not a word's address")


def load(board, ops):
    """The operations into the core's RAM, once every write in them is held to the band."""
    if len(ops) >= OPS:
        raise ValueError(f"{len(ops)} operations, where the executor takes {OPS - 1}")
    for kind, addr, value in ops:
        why = writable(addr, value) if kind == WRITE else None
        if why:
            raise ValueError(f"{addr:#x} = {value:#x}: {why}")
    board.write_block(BOARD_WINDOW + LIST, b"".join(struct.pack("<3I", *op) for op in ops + [(END, 0, 0)]))


def run(board, timeout=60.0):
    """Run the list loaded once; its results as (cycle count, word), and how many of its UNTILs gave up."""
    go = (read(board, GO) + 1) & 0xFFFFFFFF
    board.write(BOARD_WINDOW + GO, go)
    end = time.time() + timeout
    while read(board, DONE) != go:
        if time.time() > end:
            raise RuntimeError("the list did not end")
        time.sleep(0.005)
    results = [(read(board, OUT + 8 * i), read(board, OUT + 8 * i + 4)) for i in range(read(board, COUNT))]
    return results, read(board, GAVE_UP)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("program")
    p.add_argument("--setup", help="RECORD: its writes, over the bridge, before the program")
    p.add_argument("--times", type=int, default=1)
    args = p.parse_args()
    ops = parse(open(args.program).read())
    subprocess.run(["make", "-s", "-C", ROOT, "build/cyw43/bt-exec.bin"], check=True)
    board = Bridge(log=lambda line: None)
    try:
        park(board)
        print("cyw43-exec: the executor runs")
        if args.setup:
            setup(board, args.setup)
        load(board, ops)
        for k in range(args.times):
            results, gave_up = run(board)
            print(f"cyw43-exec: run {k}, {len(results)} results" + (f", {gave_up} untils gave up" if gave_up else ""))
            for at, word in results:  # the cycle counter wraps every 89 s
                print(f"  {((at - results[0][0]) & 0xFFFFFFFF) / MHZ:10.2f} {word:#010x}")
    finally:
        board.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
