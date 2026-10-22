#!/usr/bin/env python3
"""What the ESP32-C6's sampler captured, from a run's log: user/radio/esp32c6/prog/sampler.c and dump.h, dumphex=.

Usage: tools/dump-spectrum.py LOG RATE_HZ [NFFT] [--scan [MHZ]] [--tail [US]]

For each capture the log holds whole, its lines following on from word 0, its time in the run, its spread and its offset,
the gain codes and the AGC's bits it holds, its spectrum from -RATE/2 to +RATE/2 in sixteen bands,
each band's strongest bin in dB over the median, the three strongest bins,
and its power in sixteen stretches of time, in dB.
With --tail, also its last US microseconds, 10 by default, a microsecond at a time, and the gain's and the AGC's steps there:
the end of a ring of user/radio/esp32c6/prog/ring.c, which its source's event stopped.
The in-phase sample is bits 10 to 19 and the quadrature bits 0 to 9, as a signal of known frequency told;
see user/radio/esp32c6/hw/dump.md. RATE_HZ is the engine's clock divided: 80e6, 40e6, 26.67e6 or 16e6;
below 80e6 a capture holds what lies past half the rate folded in, as dump.md says.
Plain Python, no numpy: a capture is a few thousand samples.

With --scan, the captures of user/radio/esp32c6/prog/scan.c, each after the line naming its step's frequency
and whether its LO was divided, a step each way:
each step's own band, half a step either side of the frequency it was tuned to, the 100 kHz about its middle left out,
as dB over the scan's floor, the median of every step's median, its strongest in any pass and its median over the passes,
with the PLL's state, its capacitance and the gains; then the strongest frequencies of the whole scan;
and with MHZ, the level there in every capture whose band holds it, in the order of the run, to tell when a signal was.
"""

import cmath
import math
import re
import sys

LINE = re.compile(r"^\s*([\d.]+) dumphex: (\d+) (\d+) (.*)$")
HEX = re.compile(r"[0-9a-f]{1,8}")
CAPTURE = re.compile(r"^\s*[\d.]+ dump: capture (\d+) ")
STEP = re.compile(r"^\s*[\d.]+ scan: pass (\d+) at (\d+) kHz, PLL (\w+), divided (\d), capacitance (-?\d+)")


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


def read(log):
    """Each capture's words, its time, whether a line of it is missing, and the scan's step it was taken at."""
    captures, times, broken, steps = {}, {}, set(), {}
    step = None
    with open(log, errors="replace") as f:
        for line in f:
            m = STEP.search(line)
            if m:
                step = (int(m.group(1)), int(m.group(2)), int(m.group(3), 16), int(m.group(5)), int(m.group(4)))
                continue
            m = CAPTURE.search(line)
            if m and step is not None:
                steps[int(m.group(1))] = step
                step = None
                continue
            m = LINE.search(line)
            if m:
                c = int(m.group(2))
                words = captures.setdefault(c, [])
                # A line lost from the kernel's log leaves the next one's index past the words so far,
                # and a line cut short there runs into the next one's text.
                fields = m.group(4).split()
                if int(m.group(3)) != len(words) or len(fields) != 8 or not all(HEX.fullmatch(x) for x in fields):
                    broken.add(c)
                    continue
                words.extend(int(x, 16) for x in fields)
                times.setdefault(c, m.group(1))
    if not captures:
        sys.exit("dump-spectrum: no dumphex lines in " + log)
    return captures, times, broken, steps


def samples(words):
    i_s = [s10(w >> 10) for w in words]
    q_s = [s10(w) for w in words]
    mi, mq = sum(i_s) / len(i_s), sum(q_s) / len(q_s)
    return [complex(i - mi, q - mq) for i, q in zip(i_s, q_s)], mi, mq


def spectrum(z, nfft):
    """The power from -rate/2 to +rate/2, in dB, averaged over half-overlapping windowed stretches."""
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / nfft) for i in range(nfft)]
    acc = [0.0] * nfft
    segments = 0
    for start in range(0, len(z) - nfft + 1, nfft // 2):
        s = fft([z[start + k] * window[k] for k in range(nfft)])
        for k in range(nfft):
            acc[k] += abs(s[k]) ** 2
        segments += 1
    return [db(acc[(k + nfft // 2) % nfft] / segments) for k in range(nfft)]


def captures_of(log, nfft):
    captures, times, broken, steps = read(log)
    for c in sorted(captures):
        words = captures[c]
        if c in broken:
            print(f"capture {c}: a line of its words is missing from the log")
        elif len(words) < nfft:
            print(f"capture {c}: {len(words)} words, fewer than NFFT")
        else:
            yield c, words, times[c], steps.get(c)


def tail_of(words, z, rate, us):
    per = int(rate / 1e6)
    n = len(z)
    if n < us * per:
        return
    power = " ".join(f"{db(sum(abs(v) ** 2 for v in z[i:i + per]) / per):3.0f}" for i in range(n - us * per, n, per))
    steps = " ".join(f"{(i - n) / per:.1f} us {(words[i] >> 20) & 0xFF}/{words[i] >> 28:x}"
                     for i in range(n - us * per, n) if words[i] >> 20 != words[i - 1] >> 20)
    print(f"   its last {us} us, dB a microsecond: {power}")
    print(f"   gain/AGC bits there: {steps or 'no step'}")


def one_by_one(log, rate, nfft, tail):
    for c, words, t, _ in captures_of(log, nfft):
        z, mi, mq = samples(words)
        level = spectrum(z, nfft)
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
        print(f"t {t} capture {c}: rms {rms:6.1f}, offset I {mi:6.1f} Q {mq:6.1f}, "
              f"gain {gains[:3]}{more}, AGC bits {agc}")
        print(f"   spectrum, -fs/2 to +fs/2, dB over the median: {bands}")
        print(f"   strongest: {peaks_text}")
        print(f"   power in time, dB: {in_time}")
        if tail:
            tail_of(words, z, rate, tail)


def scan(log, rate, nfft, at_mhz):
    bin_khz = rate / nfft / 1e3
    taken = [x for x in captures_of(log, nfft) if x[3] is not None]
    if not taken:
        sys.exit("dump-spectrum: no capture of the scan's steps in " + log)
    steps = sorted({(x[3][1], x[3][4]) for x in taken})
    freqs = sorted({khz for khz, _ in steps})
    step_khz = min((b - a for a, b in zip(freqs, freqs[1:])), default=rate / 2e3)
    half = min(step_khz / 2, rate / 2e3)
    # Each step's bins, by their frequency in kHz, a list of levels, one a pass.
    by_step, notes, at_levels = {}, {}, []
    for c, words, t, (pss, khz, state, cap, divided) in taken:
        z, _, _ = samples(words)
        level = spectrum(z, nfft)
        bins = {}
        for k in range(nfft):
            off = (k - nfft // 2) * bin_khz
            if abs(off) <= half and abs(off) >= 100:
                bins[khz + off] = level[k]
        by_step.setdefault((khz, divided), []).append(bins)
        note = notes.setdefault((khz, divided), [set(), set(), set()])
        note[0].add(state)
        note[1].add(cap)
        note[2].update((w >> 20) & 0xFF for w in words)
        if at_mhz is not None and abs(at_mhz * 1e3 - khz) <= half:
            k = round((at_mhz * 1e3 - khz) / bin_khz) + nfft // 2
            near = level[max(k - 2, 0):k + 3]
            at_levels.append((t, pss, khz, divided, max(near)))
    medians = [sorted(b.values())[len(b) // 2] for bs in by_step.values() for b in bs if b]
    floor = sorted(medians)[len(medians) // 2]
    print(f"{len(taken)} captures of {len(steps)} steps, {step_khz / 1e3:g} MHz apart, "
          f"a bin {bin_khz:.1f} kHz; dB over the scan's floor")
    print(f"{'step MHz':>10} {'LO':>3} {'passes':>6} {'PLL':>4} {'capacitance':>12} {'gains':>10} {'strongest':>9} "
          f"{'median':>7}")
    strongest = {}
    for khz, divided in steps:
        bs = by_step[(khz, divided)]
        best = {f: max(b.get(f, -999) for b in bs) for f in bs[0]}
        med = {f: sorted(b.get(f, -999) for b in bs)[len(bs) // 2] for f in bs[0]}
        state, caps, gains = notes[(khz, divided)]
        top = max(best.values()) - floor if best else 0
        middle = sorted(med.values())[len(med) // 2] - floor if med else 0
        print(f"{khz / 1e3:10.3f} {'5/6' if divided else '1':>3} {len(bs):6} {','.join(f'{s:x}' for s in sorted(state)):>4} "
              f"{','.join(str(x) for x in sorted(caps)):>12} {min(gains)}-{max(gains):<6} {top:9.0f} {middle:7.0f}")
        for f, v in best.items():
            strongest[f] = max(v, strongest.get(f, v))
    print("strongest of the scan:")
    shown = []
    for f in sorted(strongest, key=lambda f: -strongest[f]):
        if all(abs(f - g) > 500 for g in shown):
            shown.append(f)
            print(f"   {f / 1e3:10.3f} MHz {strongest[f] - floor:4.0f} dB")
        if len(shown) == 10:
            break
    if at_mhz is not None:
        print(f"at {at_mhz} MHz, the strongest of five bins, dB over the floor, in the order of the run:")
        for t, pss, khz, divided, v in at_levels:
            print(f"   t {t} pass {pss} step {khz / 1e3:.3f} MHz, LO {'5/6' if divided else '1'}: {v - floor:4.0f}")


def main():
    argv, tail = sys.argv[1:], None
    if "--tail" in argv:
        at = argv.index("--tail")
        tail = int(argv.pop(at + 1)) if at + 1 < len(argv) and argv[at + 1].isdigit() else 10
        argv.pop(at)
    args = [a for a in argv if a != "--scan"]
    if len(args) < 2:
        sys.exit(__doc__)
    log, rate = args[0], float(args[1])
    nfft = int(args[2]) if len(args) > 2 else 256
    if nfft < 16 or nfft & (nfft - 1):
        sys.exit("dump-spectrum: NFFT is a power of two, 16 or more")
    if "--scan" in sys.argv:
        scan(log, rate, nfft, float(args[3]) if len(args) > 3 else None)
    else:
        one_by_one(log, rate, nfft, tail)


if __name__ == "__main__":
    main()
