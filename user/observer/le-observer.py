#!/usr/bin/env python3
"""The observer on the nRF52840, user/observer/root.c: BLE's packets of an access address on one channel, counted.

    with Observer(channel=19) as o:
        start = time.time(); ...; end = time.time()
        print(o.heard(start, end))   # {"ok": ..., "bad": ..., "rssi": mean dBm or None, "measured": ...}

Observer() builds and runs the program, make BOARD=nrf52840 observer, with a configuration of the arguments,
and returns once the program listens; each line it says then carries a window's end on the board's clock,
which goes onto the host's clock by the least difference between a line's arrival and the time it carries,
since a line can only arrive late.
heard(t0, t1) sums the windows that lie wholly between two times of the host's, time.time()'s;
a window's length, window_ms, bounds how finely a span is told.
Leaving the `with` waits for the run to end, run_s after it began, or stops the runner,
which leaves the radio listening until the chip is reset; it sends nothing either way.

Usage: user/observer/le-observer.py [--channel N] [--aa HEX] [--crcinit HEX] [--window MS] [--seconds S]
prints each window and then the totals.
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
LINE = re.compile(r"observer: at (\d+) ok (\d+) bad (\d+) rssi (-?\d+) of (\d+), best -(\d+)")


class Observer:
    def __init__(self, channel=19, aa=0x71764129, crcinit=0x555555, window_ms=100, run_s=60, log=None, timeout=60.0):
        self.window_ms, self.log = window_ms, log or (lambda line: None)
        self.windows = []  # (arrival, board's seconds, ok, bad, rssi sum, measured, best)
        self.ready = threading.Event()
        self.conf = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
        self.conf.write(f"channel={channel}\naa={aa:x}\ncrcinit={crcinit:x}\nwindow={window_ms}\nrun={run_s}\n")
        self.conf.close()
        self.proc = subprocess.Popen(["make", "-s", "-C", ROOT, "BOARD=nrf52840", "observer",
                                      f"OBSERVER_CONFIG={self.conf.name}"],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        if not self.ready.wait(timeout):
            self.close()
            raise RuntimeError("le-observer: the observer did not start")

    def _read(self):
        for line in self.proc.stdout:
            now = time.time()
            self.log(line.rstrip("\n"))
            if "observer: BLE" in line:
                self.ready.set()
            m = LINE.search(line)
            if m:
                at, ok, bad, rssi, k, best = (int(x) for x in m.groups())
                self.windows.append((now, at / 1e6, ok, bad, rssi, k, best))

    def heard(self, t0, t1):
        """What the windows wholly between t0 and t1, the host's times, heard."""
        ws = list(self.windows)
        if not ws:
            return {"ok": 0, "bad": 0, "rssi": None, "measured": 0, "windows": 0}
        offset = min(arrival - board for arrival, board, *_ in ws)
        span = self.window_ms / 1000.0
        inside = [w for w in ws if t0 <= w[1] + offset - span and w[1] + offset <= t1]
        measured = sum(w[5] for w in inside)
        return {"ok": sum(w[2] for w in inside), "bad": sum(w[3] for w in inside),
                "rssi": sum(w[4] for w in inside) / measured if measured else None,
                "measured": measured, "windows": len(inside)}

    def close(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        os.unlink(self.conf.name)

    def wait(self, timeout=None):
        """Until the run ends by itself."""
        self.proc.wait(timeout=timeout)
        self.reader.join(5)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--channel", type=int, default=19)
    p.add_argument("--aa", type=lambda s: int(s, 16), default=0x71764129)
    p.add_argument("--crcinit", type=lambda s: int(s, 16), default=0x555555)
    p.add_argument("--window", type=int, default=1000)
    p.add_argument("--seconds", type=int, default=10)
    args = p.parse_args()
    o = Observer(args.channel, args.aa, args.crcinit, args.window, args.seconds, log=print)
    try:
        o.wait(timeout=args.seconds + 60)
    finally:
        o.close()
    ok, bad = sum(w[2] for w in o.windows), sum(w[3] for w in o.windows)
    k = sum(w[5] for w in o.windows)
    rssi = f", {sum(w[4] for w in o.windows) / k:.1f} dBm on average" if k else ""
    print(f"le-observer: {ok} packets whole, {bad} with a bad CRC, in {len(o.windows)} windows{rssi}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
