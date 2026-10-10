#!/usr/bin/env python3
"""The medium shared: a Pico 2 W and an ESP32-C6 at one access point, each one's echo driven from the host,
alone and both at once, so that a change to how the C6 sends shows whether it takes more than its share.

--pico runs the Pico 2 W, `make BOARD=rp2350 wifi` with its WIFI_CONFIG, whose own checks come first;
--c6 boots the C6 with its console, as `make BOARD=esp32c6 wifi-esp32c6` does, and may be given again,
for other images to hold against the first, each booted in turn beside the one Pico run.
Both configurations name the same access point by its bssid, and a run= that outlasts the rounds;
the Pico's run lasts its run= however soon the rounds end, as nothing from the host ends it.
Each round drives the Pico's echo alone, the C6's alone, then both at once,
each from a process of its own: two threads of one interpreter slow each other as a shared medium would.
The summary gives, for each C6, what the Pico kept beside it of its own throughput alone, and its 90th percentile;
the channel's other traffic moves a single round a lot, so hold four or more against four or more.

Usage: tools/wifi-share.py [--rounds 4] [--seconds 8] [--window 8] --pico CMD --c6 CMD [--c6 CMD]...
"""

import argparse
import importlib.util
import multiprocessing
import os
import re
import shlex
import signal
import socket
import statistics
import subprocess
import threading
import time

spec = importlib.util.spec_from_file_location("wifi_run", os.path.join(os.path.dirname(__file__), "wifi-run.py"))
wr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wr)

SIZE = 1024
WAIT = 0.5  # a datagram not back within this is lost


def burst(ip, seconds, window):
    """The echo at ip, window datagrams in flight for seconds: kb/s each way, and the 90th percentile in ms."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.01)
    out, times, sent = {}, [], 0
    start = time.perf_counter()
    while True:
        now = time.perf_counter()
        for n in [n for n, t in out.items() if now - t > WAIT]:
            del out[n]
        if now - start < seconds:
            while len(out) < window:
                s.sendto((b"T%07d" % sent).ljust(SIZE, b"t"), (ip, wr.ECHO_PORT))
                out[sent] = time.perf_counter()
                sent += 1
        elif not out:
            break
        try:
            got, frm = s.recvfrom(4096)
        except socket.timeout:
            continue
        n = int(got[1:8]) if frm == (ip, wr.ECHO_PORT) and len(got) == SIZE and got[:1] == b"T" else None
        if n in out:
            times.append((time.perf_counter() - out.pop(n)) * 1000)
    times.sort()
    kbps = len(times) * SIZE * 8 / (time.perf_counter() - start) / 1000
    return kbps, times[int(len(times) * 0.9)] if times else float("inf")


def _burst(q, key, args):
    q.put((key, burst(*args)))


def together(targets, seconds, window):
    """Bursts at once, each in a process of its own."""
    q = multiprocessing.Queue()
    procs = [multiprocessing.Process(target=_burst, args=(q, key, (ip, seconds, window))) for key, ip in targets]
    for p in procs:
        p.start()
    got = dict(q.get() for _ in procs)
    for p in procs:
        p.join()
    return got


def start(cmd, out, name):
    """A board's command, its lines into the output under name and into a log the caller waits on."""
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
    proc = subprocess.Popen(shlex.split(cmd), stdout=subprocess.PIPE, stdin=subprocess.PIPE, text=True,
                            errors="replace", env=env, start_new_session=True)
    log = wr.Log(out)

    def through():
        for line in proc.stdout:
            out.lines(f"{name} {line}")
            log.feed(line)
        log.end()

    threading.Thread(target=through, daemon=True).start()
    return proc, log


def stop(proc):
    if proc.poll() is None:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pico", required=True, help="the command that runs the Pico 2 W")
    ap.add_argument("--c6", required=True, action="append", help="a command that boots the C6, once per image")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--window", type=int, default=8)
    a = ap.parse_args()
    out = wr.Output()

    pico, pico_log = start(a.pico, out, "pico")
    code = share(a, out, pico_log)
    out.say("share: the Pico 2 W runs on until its run= is over, which nothing from here can end")
    pico.wait()
    return code


def share(a, out, pico_log):
    if not pico_log.wait(lambda t: "every check passed" in t or "checks failed" in t, 180):
        out.say("share: the Pico 2 W's checks never ended")
        return 1
    pico_ip = wr.ADDRESS.search(pico_log.text).group(1)
    kept = {}
    for n, cmd in enumerate(a.c6):
        c6, c6_log = start(cmd, out, f"c6.{n}")
        try:
            if not c6_log.wait(lambda t: all(s in t for s in wr.CLIENTS_UP), 90):
                out.say(f"share: c6.{n}'s clients never answered")
                continue
            c6_ip = wr.ADDRESS.search(c6_log.text).group(1)
            for r in range(a.rounds):
                alone = burst(pico_ip, a.seconds, a.window)
                c6_alone = burst(c6_ip, a.seconds, a.window)
                both = together([("pico", pico_ip), ("c6", c6_ip)], a.seconds, a.window)
                kept.setdefault(n, []).append((both["pico"][0] / alone[0], alone[1], both["pico"][1]))
                out.say(f"share: c6.{n} round {r}: pico {alone[0]:.0f} kb/s alone, {both['pico'][0]:.0f} beside it; "
                        f"c6 {c6_alone[0]:.0f} alone, {both['c6'][0]:.0f} beside the Pico")
        finally:
            wr.tell(c6, "end", out)
            try:
                c6.wait(timeout=30)
            except subprocess.TimeoutExpired:
                stop(c6)
    for n, rows in kept.items():
        out.say(f"share: beside c6.{n}, over {len(rows)} rounds, the Pico kept "
                f"{statistics.mean(r[0] for r in rows):.0%} of its throughput alone, "
                f"its 90th percentile {statistics.mean(r[1] for r in rows):.0f} ms alone and "
                f"{statistics.mean(r[2] for r in rows):.0f} beside it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
