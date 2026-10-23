#!/usr/bin/env python3
"""A record of user/radio/cyw43439/tools/cyw43-rehost.py's, --record, played back on the chip over the bridge, with no firmware running.

Usage: user/radio/cyw43439/tools/cyw43-replay.py RECORD [--stop-at TEXT] [--clear ADDR=BITS] [--again ADDR=VALUE --again-for S [--every S]]

The CYW43439's Bluetooth core is parked as user/radio/cyw43439/tools/cyw43park.py parks it, its processor stopped in the vendor patch,
and every access of the record is made again in its order, reads as well as writes, the reads' answers dropped:
what the emulated firmware did to the chip, without the firmware.
A record's line is read as far as its fourth field, so a list of lines "w ADDR SIZE VALUE" is one too.
--stop-at ends the play at the record's first line of HCI that holds TEXT, as an event's bytes;
--clear keeps bits of a register clear as the record's run did, 0x321520's bit 17 by default, see user/wifi/BT.md;
the ring in the WLAN core's RAM, the host's and not the core's, is left out.
Then --again writes its words, in order, every --every seconds for --again-for seconds:
a register that starts something, played on its own.
"""

import argparse
import os
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "../../../.."))
sys.path.insert(0, TOOLS)
sys.path.insert(1, os.path.join(ROOT, "user/wifi/tools"))  # cyw43bridge, beside the bridge it is the host side of
from cyw43bridge import Bridge  # noqa: E402
import cyw43park  # noqa: E402

RING_FROM = 0x680000  # the WLAN core's RAM as the core reaches it, where the HCI ring lies


def pairs(texts):
    return [tuple(int(x, 16) for x in t.split("=")) for t in texts]


def accesses(path, stop_at):
    """The record's accesses up to the line of HCI holding stop_at: (kind, addr, size, value)."""
    with open(path) as lines:
        for line in lines:
            f = line.split()
            if not f:
                continue
            if f[0] == "#":
                if stop_at and stop_at in line:
                    return
                continue
            addr = int(f[1], 16)
            if addr < RING_FROM:
                yield f[0], addr, int(f[2]), int(f[3], 16)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("record")
    p.add_argument("--stop-at", help="TEXT: end at the record's first line of HCI holding it")
    p.add_argument("--clear", action="append", help="ADDR=BITS: bits a write leaves clear, 0x321520=0x20000 by default")
    p.add_argument("--again", action="append", default=[], help="ADDR=VALUE: written every turn once the play ends")
    p.add_argument("--every", type=float, default=0.001, help="S: between two turns of --again")
    p.add_argument("--again-for", type=float, default=0, help="S: how long --again goes on")
    args = p.parse_args()
    clear = dict(pairs(args.clear or ["321520=20000"]))
    ops = list(accesses(args.record, args.stop_at))
    print(f"cyw43-replay: {len(ops)} accesses, {sum(op[0] == 'w' for op in ops)} of them writes")
    board = Bridge(log=lambda line: None)
    try:
        cyw43park.park(board)
        start = time.time()
        print(f"cyw43-replay: parked at {start:.3f}")
        for kind, addr, size, value in ops:
            word = addr & ~3
            if kind == "r":
                cyw43park.read(board, word)
                continue
            if size < 4:  # a part of a word: the rest as it reads, as the emulator writes it
                shift, mask = 8 * (addr & 3), ((1 << 8 * size) - 1) << 8 * (addr & 3)
                value = cyw43park.read(board, word) & ~mask | (value << shift) & mask
            board.post(cyw43park.BOARD_WINDOW + word, value & ~clear.get(word, 0))
        cyw43park.read(board, cyw43park.SPARE)  # the queued writes go
        print(f"cyw43-replay: played at {time.time():.3f}, in {time.time() - start:.1f} s")
        if args.again_for:
            again, end, turns = pairs(args.again), time.time() + args.again_for, 0
            while time.time() < end:
                for addr, value in again:
                    board.write(cyw43park.BOARD_WINDOW + addr, value)
                turns += 1
                time.sleep(args.every)
            print(f"cyw43-replay: {turns} turns of {', '.join(args.again)}, to {time.time():.3f}")
    finally:
        board.close()
    print(f"cyw43-replay: {board.asks} asks of the board")
    return 0


if __name__ == "__main__":
    sys.exit(main())
