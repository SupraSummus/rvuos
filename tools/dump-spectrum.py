#!/usr/bin/env python3
"""What the ESP32-C6's sampler captured, from a run's log: user/radio/esp32c6/dump.h, dumphex=.

Usage: tools/dump-spectrum.py LOG RATE_HZ [NFFT]

For each capture the log holds whole, its time in the run, its spread and its offset,
the gain codes and the AGC's bits it holds, its spectrum from -RATE/2 to +RATE/2 in sixteen bands,
each band's strongest bin in dB over the median, the three strongest bins,
and its power in sixteen stretches of time, in dB.
The in-phase sample is bits 10 to 19 and the quadrature bits 0 to 9, as a signal of known frequency told;
see NOTES.md, "The dump engine". RATE_HZ is the engine's clock divided: 80e6, 40e6, 26.67e6 or 16e6.
Plain Python, no numpy: a capture is a few thousand samples.
"""

import cmath
import math
import re
import sys

LINE = re.compile(r"^\s*([\d.]+) dumphex: (\d+) (\d+) (.*)$")


def s10(v):
    v &= 0x3FF
    return v - 1024 if v >= 512 else v


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even, odd = fft(x[0::2]), fft(x[1::2])
    twiddled = [cmath.exp(-2j * math.pi * k / n) * odd[k] for k in range(n // 2)]
    return [even[k] + twiddled[k] for k in range(n // 2)] + [even[k] - twiddled[k] for k in range(n // 2)]


def db(v):
    return 10 * math.log10(v + 1e-9)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    log, rate = sys.argv[1], float(sys.argv[2])
    nfft = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    if nfft < 16 or nfft & (nfft - 1):
        sys.exit("dump-spectrum: NFFT is a power of two, 16 or more")
    captures, times = {}, {}
    with open(log, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                c = int(m.group(2))
                captures.setdefault(c, []).extend(int(x, 16) for x in m.group(4).split())
                times.setdefault(c, m.group(1))
    if not captures:
        sys.exit("dump-spectrum: no dumphex lines in " + log)
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / nfft) for i in range(nfft)]
    for c in sorted(captures):
        words = captures[c]
        if len(words) < nfft:
            print(f"capture {c}: {len(words)} words, fewer than NFFT")
            continue
        i_s = [s10(w >> 10) for w in words]
        q_s = [s10(w) for w in words]
        mi, mq = sum(i_s) / len(i_s), sum(q_s) / len(q_s)
        z = [complex(i - mi, q - mq) for i, q in zip(i_s, q_s)]
        acc = [0.0] * nfft
        segments = 0
        for start in range(0, len(z) - nfft + 1, nfft // 2):
            spectrum = fft([z[start + k] * window[k] for k in range(nfft)])
            for k in range(nfft):
                acc[k] += abs(spectrum[k]) ** 2
            segments += 1
        # From -rate/2 to +rate/2.
        level = [db(acc[(k + nfft // 2) % nfft] / segments) for k in range(nfft)]
        floor = sorted(level)[nfft // 2]
        per = nfft // 16
        bands = " ".join(f"{max(level[b * per:(b + 1) * per]) - floor:3.0f}" for b in range(16))
        peaks = sorted(range(nfft), key=lambda k: -level[k])[:3]
        peaks_text = ", ".join(f"{(k - nfft // 2) * rate / nfft / 1e6:+.2f} MHz {level[k] - floor:.0f} dB" for k in peaks)
        stretch = len(z) // 16
        in_time = " ".join(
            f"{db(sum(abs(v) ** 2 for v in z[t * stretch:(t + 1) * stretch]) / stretch):3.0f}" for t in range(16))
        gains = sorted({(w >> 20) & 0xFF for w in words})
        agc = sorted({w >> 28 for w in words})
        rms = math.sqrt(sum(abs(v) ** 2 for v in z) / len(z))
        more = "+" if len(gains) > 3 else ""
        print(f"t {times[c]} capture {c}: rms {rms:6.1f}, offset I {mi:6.1f} Q {mq:6.1f}, "
              f"gain {gains[:3]}{more}, AGC bits {agc}")
        print(f"   spectrum, -fs/2 to +fs/2, dB over the median: {bands}")
        print(f"   strongest: {peaks_text}")
        print(f"   power in time, dB: {in_time}")


if __name__ == "__main__":
    main()
