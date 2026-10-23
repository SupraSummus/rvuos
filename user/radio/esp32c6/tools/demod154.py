#!/usr/bin/env python3
"""IEEE 802.15.4 frames in the dump engine's captures, found and read: the O-QPSK of the 2.4 GHz band, 2 Mchip/s.

Usage: user/radio/esp32c6/tools/demod154.py LOG RATE [SHIFT_HZ]

LOG is a run's output with captures told as dumphex lines, as dump.c and prog/synth154.c's hear= tell them;
RATE their samples a second, 16e6 for dumprate= or hearrate= 3; SHIFT_HZ where the frames lie from the capture's middle,
-2e6 for 802.15.4's channel 24 heard on Wi-Fi's channel 13, as the board's I is bits 10 to 19.
For each capture: where the power rises above the capture's floor, the burst, which may fill most of it;
the sender's frequency off the expected,
from the preamble's symbols, the same one eight times, how far a symbol apart turns,
which knows it within a symbol's rate, 62.5 kHz, and of those the one symbol 0 correlates best at, found sample by sample;
then the SFD's two symbols after it,
and each symbol after the strongest of the sixteen chip sequences, without its carrier's phase:
the frame's length, its bytes and whether its FCS is right.
Plain Python: a capture is a few thousand words.
"""

import cmath
import math
import re
import sys

CHIPS = [0x744AC39B, 0x44AC39B7, 0x4AC39B74, 0xAC39B744, 0xC39B744A, 0x39B744AC, 0x9B744AC3, 0xB744AC39,
         0xDEE06931, 0xEE06931D, 0xE06931DE, 0x06931DEE, 0x6931DEE0, 0x931DEE06, 0x31DEE069, 0x1DEE0693]
CHIP_HZ = 2e6
SFD = (7, 10)  # 0xA7, its low nibble first
FLOOR_DB = 8.0  # a burst stands this far over the floor, the power a tenth of the blocks are below
BLOCK = 16  # samples a power is taken over
PREAMBLE_SYMBOLS = 6  # of the preamble's eight, the burst's first that are surely it
ALIASES = 4  # the multiples of a symbol's rate the frequency off is looked for at, each way


def s10(v):
    return v - 1024 if v & 512 else v


def captures(path):
    caps, labels = {}, {}
    label = None
    for line in open(path, errors="replace"):
        m = re.search(r"synth154: (frame \d+, setting \d+)", line)
        if m:
            label = m[1]
        m = re.search(r"dump: capture (\d+)", line)
        if m:
            labels[int(m[1])] = label
            label = None
        m = re.search(r"dumphex: (\d+) (\d+) ((?:[0-9a-f]+ ?){8})", line)
        if m:
            caps.setdefault(int(m[1]), []).extend(int(v, 16) for v in m[3].split())
    return caps, labels


def symbol_wave(s, spc):
    """A symbol's 32 chips, the even on I and the odd on Q, each a half sine two chips long."""
    n = 32 * spc
    w = [0j] * (n + spc)
    pulse = [math.sin(math.pi * k / (2 * spc)) for k in range(2 * spc)]
    for m in range(32):
        a = 1.0 if (CHIPS[s] >> m) & 1 else -1.0
        for k, p in enumerate(pulse):
            if m * spc + k < len(w):
                w[m * spc + k] += a * p if m % 2 == 0 else 1j * a * p
    return w[:n]


def corr(ref, z, at):
    return sum(r.conjugate() * z[at + k] for k, r in enumerate(ref))


def fcs(data):
    c = 0
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def burst(z):
    power = [sum(abs(v) ** 2 for v in z[k:k + BLOCK]) / BLOCK for k in range(0, len(z) - BLOCK + 1, BLOCK)]
    floor = sorted(power)[len(power) // 10] or 1e-9
    hot = [k for k, p in enumerate(power) if 10 * math.log10(p / floor + 1e-12) > FLOOR_DB]
    if not hot:
        return None
    return hot[0] * BLOCK, (hot[-1] + 1) * BLOCK


def turn(z, hz, rate):
    return [v * cmath.exp(-2j * math.pi * hz * n / rate) for n, v in enumerate(z)]


def read(z, rate):
    spc = int(round(rate / CHIP_HZ))
    span = 32 * spc
    b = burst(z)
    if b is None:
        return None
    lo, hi = b
    refs = [symbol_wave(s, spc) for s in range(16)]
    # The preamble repeats a symbol, so a symbol apart its samples turn by the sender's frequency off,
    # known so within a symbol's rate; which multiple of that it is, the one symbol 0 is strongest at.
    pre = min(hi, lo + PREAMBLE_SYMBOLS * span) - span
    turned = sum(z[n + span] * z[n].conjugate() for n in range(lo, pre))
    base = cmath.phase(turned) * rate / (2 * math.pi * span)
    t, off, top = lo, base, -1.0
    for k in range(-ALIASES, ALIASES + 1):
        hz = base + k * rate / span
        zk = turn(z[:min(len(z), lo + 3 * span)], hz, rate)
        for at in range(max(0, lo - span), min(len(zk) - span, lo + 2 * span), 2):
            c = abs(corr(refs[0], zk, at))
            if c > top:
                t, off, top = at, hz, c
    z = turn(z, off, rate)
    t = max(range(max(0, t - 2), min(len(z) - span, t + 3)), key=lambda at: abs(corr(refs[0], z, at)))

    def best(at):
        if at + span > len(z):
            return None
        return max(range(16), key=lambda s: abs(corr(refs[s], z, at)))

    # The SFD, within the preamble's eight symbols and a few past where it was found.
    at = None
    for k in range(1, 12):
        u = t + k * span
        if best(u) == SFD[0] and best(u + span) == SFD[1]:
            at = u + 2 * span
            break
    if at is None:
        return dict(off=off, start=t, sfd=None)
    symbols = []
    while len(symbols) < 2 or len(symbols) < 2 + 2 * ((symbols[0] | symbols[1] << 4) & 127):
        s = best(at)
        if s is None:
            break
        symbols.append(s)
        at += span
    data = [symbols[k] | symbols[k + 1] << 4 for k in range(0, len(symbols) - 1, 2)]
    length = data[0] & 127 if data else 0
    psdu = data[1:1 + length]
    right = len(psdu) == length >= 2 and fcs(psdu[:-2]) == (psdu[-2] | psdu[-1] << 8)
    return dict(off=off, start=t, sfd=at, length=length, psdu=psdu, right=right)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rate = float(sys.argv[2])
    shift = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    caps, labels = captures(sys.argv[1])
    for c, words in sorted(caps.items()):
        z = [complex(s10((w >> 10) & 1023), s10(w & 1023)) for w in words]
        mean = sum(z) / len(z)
        z = turn([v - mean for v in z], shift, rate)
        r = read(z, rate)
        name = f"capture {c}" + (f" ({labels[c]})" if labels.get(c) else "")
        if r is None:
            print(f"{name}: nothing over the floor")
        elif r["sfd"] is None:
            print(f"{name}: a burst at {r['start'] / rate * 1e6:.0f} us, {r['off'] / 1e3:+.0f} kHz off, no SFD")
        else:
            print(f"{name}: a frame at {r['start'] / rate * 1e6:.0f} us, {r['off'] / 1e3:+.0f} kHz off, "
                  f"{r['length']} bytes: {' '.join(f'{b:02x}' for b in r['psdu'])}, FCS {'right' if r['right'] else 'wrong'}")


main()
