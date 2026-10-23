#!/usr/bin/env python3
"""The CYW43439's receiver sampled: its I/Q estimator, one sample at a time, at about 310 thousand samples a second.

Usage: user/wifi/tools/cyw43-iq.py OUT --conf FILE [--count N] [--wait W] [--tones MHZ,...]

--conf names a configuration of the Wi-Fi system, such as an access point's, whose mode's work comes first,
so that the vendor's firmware sets the radio up and tunes it to the mode's channel; its run= is the bridge's.
Then the WLAN's processor is held in reset, the 802.11 core's receive DMA stopped, so that no frame lands in the RAM,
and the processor started again in our own code, user/wifi/wlancore/iq-cap.c, built by make as build/cyw43/iq-cap.bin,
which lies in RAM the firmware had cleared; the host checks it still reads 0 first.
It takes --count estimates of the PHY's estimator, each over one sample, after --wait samples at 20 MHz, see user/wifi/hw/phy.md;
a wait of 2 takes a sample every 262 cycles of the processor's 81.6 MHz, which it measures against D11's TSF.

--tones is the control: the firmware's own receive IQ calibration's loopback set up, by its ROM's functions,
and one run of --count samples before any tone and one during each tone of the list, in MHz, played by the ROM
at the calibration's amplitude, as the firmware plays its own, ±2 MHz among them;
the samples then fit in the cleared RAM, about 3000 in all, as the ROM's functions use the rest.
Each run's summary gives the frequency the samples' square is strongest at, which a tone's is twice of,
folded by the rate, and where the ROM's 40 Msps play puts it; see phy.md for how far they agree.

Once done, D11, and with it the PHY, is put in reset, so that no tone plays on; the firmware does not run again,
and the chip's power goes off at the next start of the Wi-Fi system.

Each estimate gives I times Q, I squared and Q squared, which give I and Q but for one sign common to both:
OUT, JSON, holds the processor's clock in Hz, the wait, and each run's tone, start and samples:
per sample its cycle count and the three, I and Q with I's sign taken as positive,
and the square of the sample, (I + jQ)^2, which is the sample's own, sign and all, at twice its phase.
The summary checks that each estimate is of one sample, I^2 Q^2 = (IQ)^2, and that none was late.
"""

import argparse
import json
import math
import os
import statistics
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "../../.."))
sys.path.insert(0, TOOLS)
from cyw43bridge import Bridge  # noqa: E402

PROGRAM = os.path.join(ROOT, "build/cyw43/iq-cap.bin")
D11, CM3_WRAPPER, D11_WRAPPER = 0x18001000, 0x18103000, 0x18101000
AI_IOCTRL, AI_RESETCTRL = 0x408, 0x800
RCV_CONTROL, FIFOS = D11 + 0x220, 6  # brcmsmac's fifo64regs: each 0x40, its receive control at 0x20, bit 0 enables
PI_TYPE, LCN20 = 0, 0xC  # the PHY's state begins with its type, and holds D11's address at 0x44
PI_SEARCH = range(0x60000, 0x70000, 4)
PI_KNOWN = 0x63788  # where the firmware of 43439A0.bin keeps it, an access point up
# iq-cap.c's words and RAM
CODE, BASE, OUT_AT = 0x20000, 0x1F000, 0x21000
MAGIC, COUNT, WAIT, GO, FAULT, CYCLES, STEP, PI, TONES = (BASE + 4 * i for i in range(9))
FREQ, FIRST = BASE + 0x40, BASE + 0x80
CLEARED = range(0x1E000, 0x2D000)  # RAM the firmware had cleared, an access point up: the code's, and the samples' with tones
OUT_END, OUT_END_TONES, MOST_TONES = 0x7E000, 0x2D000, 16
MAGIC_VALUE, GO_VALUE, ENDED, LATE, TSF_SPAN = 0x49514341, 2, 3, 1, 100000


def say(text):
    print(f"cyw43-iq: {text}", flush=True)


def conf_of(path):
    with open(path) as f:
        return "".join(line for line in f if not line.startswith("run="))


def signed(x, bits=32):
    return x - (1 << bits) if x >> (bits - 1) else x


def sample(product, i_power, q_power):
    """I and Q, I's sign taken as positive, from an estimate of one sample."""
    i, q = math.isqrt(i_power), math.isqrt(q_power)
    return (i, q if product >= 0 else -q) if i else (0, q)


def fold(f, rate):
    return ((f / rate + 0.5) % 1 - 0.5) * rate


def strongest(square, times, rate, bins=1024):
    """The frequency the samples' square is strongest at, over a fold of the rate, and its share of the square's power."""
    z = [complex(*v) for v in square]
    total = sum(abs(v) ** 2 for v in z) * len(z)
    if not total:
        return 0.0, 0.0
    best = (0.0, 0.0)
    for b in range(bins):
        f = (b / bins - 0.5) * rate
        w = [complex(math.cos(2 * math.pi * f * t), -math.sin(2 * math.pi * f * t)) for t in times]
        power = abs(sum(v * x for v, x in zip(z, w))) ** 2
        best = max(best, (power, f))
    return best[1], best[0] / total


def find_pi(board):
    """The firmware's PHY state: its type first, and D11's address at 0x44."""
    for at in [PI_KNOWN, *PI_SEARCH]:
        if board.read(at + 0x44) == D11 and board.read(at + PI_TYPE) == LCN20:
            return at
    sys.exit("cyw43-iq: the PHY's state not found")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("out")
    p.add_argument("--conf", required=True)
    p.add_argument("--count", type=int, default=4000)
    p.add_argument("--wait", type=lambda x: int(x, 0), default=2)
    p.add_argument("--tones", type=lambda x: [round(float(f) * 1e6) for f in x.split(",")], default=[])
    args = p.parse_args()
    runs = 1 + len(args.tones)
    most = ((OUT_END_TONES if args.tones else OUT_END) - OUT_AT) // 16 // runs
    if not 0 < args.count <= most or not 2 <= args.wait <= 0xFF or len(args.tones) > MOST_TONES:
        sys.exit(f"cyw43-iq: --count 1 to {most} here, --wait 2 to 255, at most {MOST_TONES} tones")
    subprocess.run(["make", "-s", "-C", ROOT, "build/cyw43/iq-cap.bin"], check=True)
    with open(PROGRAM, "rb") as f:
        code = f.read()
    code += bytes(-len(code) % 4)

    board = Bridge(log=lambda line: say(line) if "root:" in line else None, conf=conf_of(args.conf), timeout=180.0)
    try:
        board.write(CM3_WRAPPER + AI_IOCTRL, 0)
        time.sleep(0.01)
        board.write(CM3_WRAPPER + AI_RESETCTRL, 1)
        time.sleep(0.01)
        stopped = []
        for k in range(FIFOS):
            control = board.read(RCV_CONTROL + 0x40 * k)
            if control & 1:
                board.write(RCV_CONTROL + 0x40 * k, control & ~1)
                stopped.append(k)
        used = [a for a in range(CLEARED.start, CLEARED.stop, 0x40) if board.read(a)]
        if used:
            sys.exit(f"cyw43-iq: RAM taken to be cleared is not, at {used[0]:#x}")
        pi = find_pi(board) if args.tones else 0
        board.write_block(CODE, code)
        if any(board.read(CODE + a) != int.from_bytes(code[a:a + 4], "little") for a in range(0, len(code), 4)):
            sys.exit("cyw43-iq: the program did not read back")
        board.write_block(0, code[:64])
        board.write_block(BASE, bytes(0x100))
        board.write(COUNT, args.count)
        board.write(WAIT, args.wait)
        board.write(PI, pi)
        board.write(TONES, len(args.tones))
        for k, f in enumerate(args.tones):
            board.write(FREQ + 4 * k, f & 0xFFFFFFFF)
        # As the driver resets a core: clock and forced gated clocks, out of reset, then the clock alone.
        board.write(CM3_WRAPPER + AI_IOCTRL, 3)
        board.read(CM3_WRAPPER + AI_IOCTRL)
        board.write(CM3_WRAPPER + AI_RESETCTRL, 0)
        time.sleep(0.01)
        board.write(CM3_WRAPPER + AI_IOCTRL, 1)
        time.sleep(0.05)
        if board.read(MAGIC) != MAGIC_VALUE:
            sys.exit(f"cyw43-iq: the program did not start; fault word {board.read(FAULT):#x}")
        board.write(GO, GO_VALUE)
        start = time.time()
        while board.read(GO) != ENDED:
            if time.time() - start > 10:
                sys.exit(f"cyw43-iq: no end; step {board.read(STEP):#x}, fault word {board.read(FAULT):#x}")
            time.sleep(0.05)
        hz = board.read(CYCLES) / TSF_SPAN * 1e6
        firsts = [board.read(FIRST + 4 * r) for r in range(runs)]
        words = [board.read(OUT_AT + 4 * k) for k in range(4 * args.count * runs)]
    finally:
        board.write(D11_WRAPPER + AI_IOCTRL, 0)
        time.sleep(0.01)
        board.write(D11_WRAPPER + AI_RESETCTRL, 1)
        board.close()

    say(f"receive DMA stopped on fifos {stopped}; the processor at {hz / 1e6:.4f} MHz by D11's TSF"
        + (f"; the PHY's state at {pi:#x}" if pi else ""))
    out, failed = {"hz": hz, "wait": args.wait, "runs": []}, False
    for r in range(runs):
        w = words[4 * args.count * r:4 * args.count * (r + 1)]
        rows = [(w[4 * k] & ~LATE, signed(w[4 * k + 1]), w[4 * k + 2], w[4 * k + 3], w[4 * k] & LATE)
                for k in range(args.count)]
        late = sum(row[4] for row in rows)
        odd = sum(1 for _, pr, ip, qp, _ in rows if pr * pr != ip * qp)
        steps = [(rows[k + 1][0] - rows[k][0]) & 0xFFFFFFFF for k in range(args.count - 1)] or [0]
        rate = hz * len(steps) / max(1, sum(steps))
        power = [ip + qp for _, _, ip, qp, _ in rows]
        square = [(ip - qp, 2 * pr) for _, pr, ip, qp, _ in rows]
        tone = args.tones[r - 1] if r else None
        peak, share = strongest(square, [((row[0] - rows[0][0]) & 0xFFFFFFFF) / hz for row in rows], rate) \
            if tone is not None else (0.0, 0.0)
        line = (f"{'no tone' if tone is None else f'tone {tone / 1e6:+g} MHz'}: {args.count} samples at {rate / 1e3:.1f} kS/s,"
                f" steps {min(steps)} to {max(steps)} cycles; {odd} not of one sample, {late} late;"
                f" I^2 + Q^2 median {statistics.median(power)}, most {max(power)}")
        if share:
            line += f"; the square's strongest at {peak / 1e3:+.1f} kHz, {share:.0%} of its power"
        if tone is not None:
            line += f", the play's {fold(2 * tone, rate) / 1e3:+.2f} kHz"
        say(line)
        failed |= bool(odd or late)
        out["runs"].append({"tone": tone, "first": firsts[r], "estimates": [row[:4] for row in rows],
                            "late": [k for k, row in enumerate(rows) if row[4]],
                            "iq": [sample(pr, ip, qp) for _, pr, ip, qp, _ in rows], "square": square})
    with open(args.out, "w") as f:
        json.dump(out, f)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
