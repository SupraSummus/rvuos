#!/usr/bin/env python3
"""What the nRF52840's watch heard, from a run's output: user/thread/root.c, watch=.

Usage: user/thread/tools/watch-spectrum.py LOG [HZ ...]

The watch keeps the mean power of each block, or with watchmax=1 its strongest sample, in sixteenths of a dB below a milliwatt;
a sender switched on and off in a rhythm stands out as a line at its rate in the blocks' spectrum,
however far below a block's own spread it lies, as the spectrum gathers it over every period.
This tells the series' mean and spread, its strongest rates from 0.5 Hz up to half the blocks' rate,
and for each HZ the line at the rate nearest it, as dB over the median of the rates within 3 Hz of it,
the 0.5 Hz about it left out: a line of a few dB over its neighbours is a rhythm the watch heard.
Plain Python, no numpy: a watch is a few thousand blocks.
"""

import cmath
import math
import re
import sys

NEAR_HZ = 3.0
GAP_HZ = 0.5
LOWEST_HZ = 0.5
NONE = 0xFFFF  # a block no sample ended in, root.c's WATCH_NONE


def read(path):
    head = None
    blocks = {}
    for line in open(path, errors="replace"):
        m = re.search(r"watch: channel (\d+), (\d+) MHz, (\d+) blocks of (\d+) us in (\d+) us, \d+ samples, "
                      r"(?:\d+ missed, )?(means|the strongest) in 1/(\d+) dB", line)
        if m:
            head = dict(channel=int(m[1]), mhz=int(m[2]), blocks=int(m[3]), block_us=int(m[4]), took_us=int(m[5]),
                        kind=m[6], scale=int(m[7]))
            continue
        m = re.search(r"watchhex: (\d+)((?: 0x[0-9a-f]+)+)", line)
        if m:
            first = int(m[1])
            for k, v in enumerate(m[2].split()):
                blocks[first + k] = int(v, 16)
    if head is None:
        sys.exit("watch-spectrum: no watch in the log")
    n = head["blocks"]
    missing = [k for k in range(n) if k not in blocks]
    if missing:
        sys.exit(f"watch-spectrum: {len(missing)} of {n} blocks missing, the first {missing[0]}")
    empty = [k for k in range(n) if blocks[k] == NONE]
    if empty:
        sys.exit(f"watch-spectrum: {len(empty)} of {n} blocks took no sample, the first {empty[0]}")
    # A block's value is dB below a milliwatt, so its power in dBm is the negative.
    return head, [-blocks[k] / head["scale"] for k in range(n)]


def amplitude(x, rate, hz):
    w = -2j * math.pi * hz / rate
    return abs(sum(v * cmath.exp(w * k) for k, v in enumerate(x))) * 2 / len(x)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    head, x = read(sys.argv[1])
    asked = [float(a) for a in sys.argv[2:]]
    rate = 1e6 / head["block_us"]
    n = len(x)
    mean = sum(x) / n
    x = [v - mean for v in x]
    spread = math.sqrt(sum(v * v for v in x) / n)
    step = rate / n
    rates = [k * step for k in range(1, n // 2) if k * step >= LOWEST_HZ]
    amp = {f: amplitude(x, rate, f) for f in rates}
    print(f"channel {head['channel']}, {head['mhz']} MHz: {n} blocks of {head['block_us']} us, {head['kind']}, "
          f"mean {mean:.2f} dBm, spread {spread:.3f} dB, a rate every {step:.3f} Hz")
    top = sorted(rates, key=lambda f: -amp[f])[:8]
    print("strongest: " + ", ".join(f"{f:.2f} Hz {amp[f] * 1000:.1f} mdB" for f in top))
    for want in asked:
        f = min(rates, key=lambda r: abs(r - want))
        near = sorted(amp[r] for r in rates if GAP_HZ < abs(r - f) <= NEAR_HZ)
        median = near[len(near) // 2] if near else 0.0
        over = f"{20 * math.log10(amp[f] / median):+.1f} dB" if amp[f] > 0 and median > 0 else "no level"
        print(f"at {want:.2f} Hz ({f:.3f}): {amp[f] * 1000:.1f} mdB, {over} "
              f"over the median of {len(near)} rates within {NEAR_HZ:.0f} Hz")


main()
