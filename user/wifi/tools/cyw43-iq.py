#!/usr/bin/env python3
"""The CYW43439's receiver sampled: its I/Q estimator, one sample at a time, at 311 thousand to 1.32 million samples a second.

Usage: user/wifi/tools/cyw43-iq.py OUT --conf FILE [--count N] [--wait W] [--mode fast[,check][,no-q] | psm[,no-q]]
                                  [--gain P6,P7,P8 | --tones MHZ,...]

--conf names a configuration of the Wi-Fi system, such as an access point's, whose mode's work comes first,
so that the vendor's firmware sets the radio up and tunes it to the mode's channel; its run= is the bridge's.
Then the WLAN's processor is held in reset, the 802.11 core's receive DMA stopped, so that no frame lands in the RAM,
and the processor started again in our own code, user/wifi/wlancore/iq-cap.c, built by make as build/cyw43/iq-cap.bin,
which lies in RAM the firmware had cleared; the host checks it still reads 0 first.
It takes --count estimates of the PHY's estimator, each over one sample, after --wait samples at 20 MHz, see user/wifi/hw/phy.md;
a wait of 2 takes a sample every 262 cycles of the processor's 81.6 MHz, which it measures against D11's TSF.

--gain holds the receiver's gain where the three numbers put it, out of its AGC's hands, see phy.md:
the overrides the firmware's calibration sets, 0x872534's, with 0x53c bits 9 to 12 at P6, 0 to 10, about 3 dB a step,
0x9ac bits 6 to 9 at P7, 0 to 12, about 3 dB a step, and 0x979 bits 1 to 8 at P8, in 3 dB steps at 4, 7, 10 and 13;
10,9,0 gives about the noise the AGC idles at. Without it the AGC sets the gain, and raises it for a weak frame.
The samples are I and Q of 8 bits, -128 to 127, so a strong signal clips.

--mode fast takes a sample without polling the estimator, whose estimate ends before the first read comes back,
and reads only the low halfwords, which hold an 8-bit sample's squares and product: 683 kS/s;
check reads the estimator's busy bit once before the results, as LATE, 527 kS/s;
no-q leaves Q squared unread, the host taking Q as the product over I, 0 where I is: 902 kS/s.
--mode psm has D11's own processor, the PSM, take the samples, by our microcode, see psm_loop,
into D11's shared memory, at most 1280 a run: 1074 kS/s, and with no-q 1920 a run at 1316 kS/s;
the vendor's microcode is then stopped, and the 802.11 core's MAC with it.

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
import cyw43psm as psm  # noqa: E402

PROGRAM = os.path.join(ROOT, "build/cyw43/iq-cap.bin")
D11, CM3_WRAPPER, D11_WRAPPER = 0x18001000, 0x18103000, 0x18101000
AI_IOCTRL, AI_RESETCTRL = 0x408, 0x800
PHYREGADDR, PHYREGDATA = D11 + 0x3FC, D11 + 0x3FE  # a halfword of address, then the data; or a word of both
RCV_CONTROL, FIFOS = D11 + 0x220, 6  # brcmsmac's fifo64regs: each 0x40, its receive control at 0x20, bit 0 enables
PI_TYPE, LCN20 = 0, 0xC  # the PHY's state begins with its type, and holds D11's address at 0x44
PI_SEARCH = range(0x60000, 0x70000, 4)
PI_KNOWN = 0x63788  # where the firmware of 43439A0.bin keeps it, an access point up
# iq-cap.c's words and RAM
CODE, BASE, OUT_AT = 0x20000, 0x1F000, 0x21000
MAGIC, COUNT, WAIT, GO, FAULT, CYCLES, STEP, PI, TONES = (BASE + 4 * i for i in range(9))
FREQ, FIRST, MODE = BASE + 0x40, BASE + 0x80, BASE + 0x24
MODES = {"fast": 1, "check": 2, "no-q": 4, "psm": 8}  # iq-cap.c's MODE_*
MODE_SETS = ("", "fast", "fast,check", "fast,no-q", "fast,check,no-q", "psm", "psm,no-q")
IQ_CONTROL, IQ_PRODUCT, IQ_I_POWER, IQ_Q_POWER = 0x481, 0x484, 0x486, 0x488
SHARED_FROM, SHARED_END = 0x100, 0x1000  # the shared memory's halfwords the microcode's samples go to, iq-cap.c's SHARED_FROM
PSM_DELAY = 14  # instructions between an estimate's start and its first read, see phy.md
CLEARED = range(0x1E000, 0x2D000)  # RAM the firmware had cleared, an access point up: the code's, and the samples' with tones
OUT_END, OUT_END_TONES, MOST_TONES = 0x7E000, 0x2D000, 16
MAGIC_VALUE, GO_VALUE, ENDED, LATE, TSF_SPAN = 0x49514341, 2, 3, 1, 100000
# The receiver's gain held, see phy.md: 0x872534's override enables, and 0x872484's fields of the three stages that act.
GAIN_ENABLES = ((0x4B0, 0x1000), (0x4F9, 0x80), (0x4B0, 0x400), (0x53D, 1), (0x53D, 8), (0x9AC, 1), (0x979, 1))
GAIN_FIELDS = ((0x53C, 9, 4, 10), (0x9AC, 6, 4, 12), (0x979, 1, 8, 0xFF))  # register, first bit, bits, most
CLIP = 128  # the estimator's samples are 8 bits; |I| or |Q| of 128 is a clipped one


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


def psm_loop(control, reads, end):
    """The microcode: estimates one after another, each started, PSM_DELAY instructions waited, and reads into the shared memory,
    a halfword each from SHARED_FROM, psm_base_0 stepping past each sample, until it reaches end; then it stays.
    Each PHY access through the external IHR waits for its busy bit, 14, to clear."""
    p = [psm.mov16(SHARED_FROM, psm.BASE_0), psm.mov16(control | 1, psm.r(2)), psm.mov16(0x4000 | IQ_CONTROL, psm.r(3)),
         psm.mov16(end, psm.r(7))]
    p += [psm.mov16(0x6000 | reg, psm.r(10 + k)) for k, reg in enumerate(reads)]
    loop = len(p)
    p += [psm.mov(psm.r(2), psm.EXT_DATA), psm.mov(psm.r(3), psm.EXT_ADDR)]
    p += [psm.jnzx(14, 1, psm.EXT_ADDR, len(p))]
    p += [psm.add(psm.r(20), psm.imm(0), psm.r(20)) for _ in range(PSM_DELAY)]
    for k in range(len(reads)):
        p += [psm.mov(psm.r(10 + k), psm.EXT_ADDR)]
        p += [psm.jnzx(14, 1, psm.EXT_ADDR, len(p)), psm.mov(psm.EXT_DATA, psm.indirect(0, k))]
    p += [psm.add(psm.BASE_0, psm.imm(len(reads)), psm.BASE_0), psm.jne(psm.BASE_0, psm.r(7), loop)]
    return p + [psm.jmp(len(p))]


def phy_modify(board, reg, mask, value):
    board.write16(PHYREGADDR, reg)
    old = board.read16(PHYREGDATA)
    board.write(PHYREGADDR, ((old & ~mask) | (value & mask)) << 16 | reg)


def hold_gain(board, codes):
    for (reg, shift, bits, _), code in zip(GAIN_FIELDS, codes):
        phy_modify(board, reg, ((1 << bits) - 1) << shift, code << shift)
    for reg, bit in GAIN_ENABLES:
        phy_modify(board, reg, bit, bit)
    held = []
    for reg, shift, bits, _ in GAIN_FIELDS:
        board.write16(PHYREGADDR, reg)
        held.append(board.read16(PHYREGDATA) >> shift & ((1 << bits) - 1))
    if held != codes:
        sys.exit(f"cyw43-iq: the gain reads {held}, not {codes}")


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
    p.add_argument("--gain", type=lambda x: [int(v, 0) for v in x.split(",")])
    p.add_argument("--mode", default="")
    args = p.parse_args()
    if args.gain is not None and (args.tones or len(args.gain) != 3
                                  or any(not 0 <= v <= most for v, (_, _, _, most) in zip(args.gain, GAIN_FIELDS))):
        sys.exit("cyw43-iq: --gain P6,P7,P8, each within its stage's codes, and without --tones")
    runs = 1 + len(args.tones)
    most = ((OUT_END_TONES if args.tones else OUT_END) - OUT_AT) // 16 // runs
    names = [m for m in args.mode.split(",") if m]
    if len(set(names)) != len(names) or set(names) not in [set(m.split(",")) - {""} for m in MODE_SETS]:
        sys.exit(f"cyw43-iq: --mode one of {', '.join(m for m in MODE_SETS if m)}")
    args.mode = sum(MODES[m] for m in names)
    reads = [IQ_PRODUCT, IQ_I_POWER] + ([] if args.mode & MODES["no-q"] else [IQ_Q_POWER])
    if args.mode & MODES["psm"]:
        most = min(most, (SHARED_END - SHARED_FROM) // len(reads))
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
        if args.gain is not None:
            hold_gain(board, args.gain)
        if args.mode & MODES["psm"]:
            board.write16(PHYREGADDR, IQ_CONTROL)
            loop = psm_loop(board.read16(PHYREGDATA) & ~3, reads, SHARED_FROM + args.count * len(reads))
            if not psm.load(board, D11, loop):
                sys.exit("cyw43-iq: the microcode did not read back")
        board.write_block(CODE, code)
        if any(board.read(CODE + a) != int.from_bytes(code[a:a + 4], "little") for a in range(0, len(code), 4)):
            sys.exit("cyw43-iq: the program did not read back")
        board.write_block(0, code[:64])
        board.write_block(BASE, bytes(0x100))
        board.write(COUNT, args.count)
        board.write(WAIT, args.wait)
        board.write(PI, pi)
        board.write(TONES, len(args.tones))
        board.write(MODE, args.mode)
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
        if board.read(FAULT):
            sys.exit(f"cyw43-iq: fault word {board.read(FAULT):#x}")
        if board.read(COUNT) != args.count:
            sys.exit(f"cyw43-iq: the program took {board.read(COUNT)} a run, not {args.count}")
        hz = board.read(CYCLES) / TSF_SPAN * 1e6
        firsts = [board.read(FIRST + 4 * r) for r in range(runs)]
        words = [board.read(OUT_AT + 4 * k) for k in range(4 * args.count * runs)]
    finally:
        board.write(D11_WRAPPER + AI_IOCTRL, 0)
        time.sleep(0.01)
        board.write(D11_WRAPPER + AI_RESETCTRL, 1)
        board.close()

    say(f"receive DMA stopped on fifos {stopped}; the processor at {hz / 1e6:.4f} MHz by D11's TSF"
        + (f"; the PHY's state at {pi:#x}" if pi else "") + (f"; gain held at {args.gain}" if args.gain else "; gain the AGC's"))
    out, failed = {"hz": hz, "wait": args.wait, "gain": args.gain, "runs": []}, False
    for r in range(runs):
        w = words[4 * args.count * r:4 * args.count * (r + 1)]
        rows = [(w[4 * k] & ~LATE, signed(w[4 * k + 1]), w[4 * k + 2], w[4 * k + 3], w[4 * k] & LATE)
                for k in range(args.count)]
        if args.mode & (MODES["fast"] | MODES["psm"]):
            rows = [(t, signed(pr & 0xFFFF, 16), ip & 0xFFFF, qp & 0xFFFF, lt) for t, pr, ip, qp, lt in rows]
        if args.mode & MODES["no-q"]:
            rows = [(t, pr, ip, pr * pr // ip if ip else 0, lt) for t, pr, ip, qp, lt in rows]
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
                f" I^2 + Q^2 median {statistics.median(power)}, most {max(power)};"
                f" {sum(1 for _, _, ip, qp, _ in rows if max(ip, qp) >= CLIP * CLIP)} clipped")
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
