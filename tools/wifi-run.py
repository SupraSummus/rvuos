#!/usr/bin/env python3
"""Run the Wi-Fi system on a Pico 2 W, follow its log over the network, and check it from the host while it runs.

The command after -- boots the system and waits for its halt, tools/rp2350-run.py as `make BOARD=rp2350 wifi` runs it.
Meanwhile this asks for the log on UDP port 7070, by broadcast until the system answers, see user/wifi/wifi.h,
and prints it as it comes; the halt writes out the rest.
Once the log says the clients answer, it checks the status port, the echo, the clock,
the echo built again after a fault and after a hang, and ping.
It exits with the halt's code, or if that is 0, with 1 when a check failed
or the system had an address and its log never came.

Usage: tools/wifi-run.py -- command...
"""

import argparse
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

LOG_PORT = 7070
STATUS_PORT = 7777
ECHO_PORT = 7
CLOCK_PORT = 13
# The root task's lines the checks wait for.
ECHO_UP = "root: the echo answers on UDP port 7"
CLIENTS_UP = (ECHO_UP, "root: the clock says it is")
ECHO_FAULTED = "root: the echo faulted"
ECHO_HUNG = "root: the echo stopped answering"
HAS_ADDRESS = "root: the system answers at"


class Output:
    """Whole lines to stdout, from every thread, so that the log's lines and this tool's never cut into each other."""

    def __init__(self):
        self.lock = threading.Lock()

    def lines(self, text):
        with self.lock:
            sys.stdout.write(text)
            sys.stdout.flush()

    def say(self, *words):
        self.lines("wifi-run: " + " ".join(str(w) for w in words) + "\n")


class Log:
    """The system's log as it comes over the network, and the word back of how far it has come."""

    def __init__(self, out):
        self.out = out
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.1)
        self.device = None  # the system's address, once it answered
        self.have = 0  # the offset of the first byte of the log not had
        self.text = ""  # all of the log that came, for the checks to look in
        self.tail = ""  # the end of a line not printed yet
        self.changed = threading.Condition()
        self.stop = threading.Event()

    def ask(self):
        to = (self.device or "255.255.255.255", LOG_PORT)
        try:
            self.sock.sendto(struct.pack(">I", self.have), to)
        except OSError as e:
            self.out.say("cannot ask for the log:", e)

    def take(self, data, frm):
        if frm[1] != LOG_PORT or len(data) < 4 or (self.device and frm[0] != self.device):
            return
        if self.device is None:
            self.device = frm[0]
            self.out.say(f"the log comes from {self.device}")
        offset = struct.unpack(">I", data[:4])[0]
        body = data[4:]
        if offset > self.have:
            self.out.say(f"the log from byte {offset}" if self.have == 0
                         else f"{offset - self.have} bytes of the log were lost before they came")
            self.have = offset
        if offset + len(body) > self.have:
            new = body[self.have - offset:].decode("utf-8", "replace")
            self.have = offset + len(body)
            text = self.tail + new
            whole = text.rfind("\n") + 1
            self.tail = text[whole:]
            if whole:
                self.out.lines(text[:whole])
            with self.changed:
                self.text += new
                self.changed.notify_all()
        self.ask()

    def run(self):
        asked = 0.0
        while not self.stop.is_set():
            if time.monotonic() - asked >= (1.0 if self.device else 0.5):
                asked = time.monotonic()
                self.ask()
            try:
                data, frm = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            self.take(data, frm)
        if self.tail:
            self.out.lines(self.tail + "\n")

    def count(self, line):
        with self.changed:
            return self.text.count(line)

    def wait(self, ready, timeout):
        """Waits until ready(text) holds, for timeout seconds at most; whether it did."""
        with self.changed:
            return self.changed.wait_for(lambda: ready(self.text) or self.stop.is_set(), timeout) and ready(self.text)


class Checks:
    """The system's services asked from the host, each named with what came of it."""

    def __init__(self, out, log, ip):
        self.out, self.log, self.ip = out, log, ip
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.failed = []

    def ask(self, port, data, timeout=0.5, want=None):
        """The answer from port to data, and how long it took in ms, or None; with want, only an answer that is want."""
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recvfrom(2048)
        except BlockingIOError:
            pass
        start = time.perf_counter()
        self.sock.sendto(data, (self.ip, port))
        while True:
            left = start + timeout - time.perf_counter()
            if left <= 0:
                return None, None
            self.sock.settimeout(left)
            try:
                got, frm = self.sock.recvfrom(2048)
            except socket.timeout:
                return None, None
            if frm == (self.ip, port) and (want is None or got == want):
                return got, (time.perf_counter() - start) * 1000

    def ask_again(self, port, data, tries, want=None):
        for _ in range(tries):
            got, ms = self.ask(port, data, want=want)
            if got is not None:
                return got, ms
        return None, None

    def result(self, name, ok, detail):
        self.out.say(f"{name}: {'ok' if ok else 'FAIL'}, {detail}")
        if not ok:
            self.failed.append(name)

    def status(self):
        got, _ = self.ask_again(STATUS_PORT, b"status?", 5)
        ok = got is not None and got.startswith(b"rvuos on a Pico 2 W")
        self.result("status", ok, got.decode(errors="replace").strip() if got else "no answer on UDP port 7777")

    def echo(self, count=20, size=32):
        times, lost = [], 0
        for n in range(count):
            payload = (b"%06d" % n).ljust(size, b"x")
            got, ms = self.ask_again(ECHO_PORT, payload, 3, want=payload)
            if got is None:
                lost += 1
            else:
                times.append(ms)
        detail = f"{count - lost} of {count} datagrams of {size} bytes back"
        if times:
            detail += f", median {statistics.median(times):.1f} ms"
        self.result("echo", lost == 0, detail)

    def clock(self):
        got, _ = self.ask_again(CLOCK_PORT, b"time?", 5)
        ok = got is not None and re.match(rb"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC\n$", got) is not None
        self.result("clock", ok, got.decode(errors="replace").strip() if got else "no answer on UDP port 13")

    def echo_back(self, name, word, said, within):
        """The echo sent word, which stops it, and back once the root task has said said and built it again."""
        before_said, before_up = self.log.count(said), self.log.count(ECHO_UP)
        start = time.monotonic()
        self.sock.sendto(word, (self.ip, ECHO_PORT))
        heard = self.log.wait(lambda t: t.count(said) > before_said and t.count(ECHO_UP) > before_up, within)
        back = None
        while heard and back is None and time.monotonic() - start < within:
            back, _ = self.ask(ECHO_PORT, b"back?", 0.2, want=b"back?")
        took = (time.monotonic() - start) * 1000
        detail = f"back after {took:.0f} ms" if back else f"not back within {within} s"
        if not heard:
            detail += f"; the log did not say '{said}' and that the echo answers again"
        self.result(name, back is not None, detail)

    def ping(self):
        if shutil.which("ping") is None:
            self.out.say("ping: not run, no ping here")
            return
        out = subprocess.run(["ping", "-c", "5", "-i", "0.2", "-W", "1", self.ip], capture_output=True, text=True)
        m = re.search(r"(\d+) received", out.stdout)
        received = int(m.group(1)) if m else 0
        self.result("ping", received >= 4, f"{received} of 5 answered")

    def run(self):
        self.status()
        self.echo()
        self.clock()
        self.echo_back("echo after a fault", b"fault", ECHO_FAULTED, 5)
        self.echo_back("echo after a hang", b"hang", ECHO_HUNG, 6)
        self.ping()
        if self.failed:
            self.out.say("checks failed:", ", ".join(self.failed))
        else:
            self.out.say("every check passed; the system runs on until its run is over")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- and the command that boots the system")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("the command that boots the system comes after --")

    out = Output()
    log = Log(out)
    follower = threading.Thread(target=log.run, daemon=True)
    follower.start()
    boot = subprocess.Popen(command, stdout=subprocess.PIPE, text=True, errors="replace")
    halt_text = []

    def through():
        for line in boot.stdout:
            halt_text.append(line)
            out.lines(line)

    piper = threading.Thread(target=through, daemon=True)
    piper.start()

    checks = None
    try:
        while boot.poll() is None and not log.wait(lambda t: all(s in t for s in CLIENTS_UP), 1.0):
            pass
        if boot.poll() is None:
            checks = Checks(out, log, log.device)
            checks.run()
        code = boot.wait()
    except KeyboardInterrupt:
        boot.terminate()
        code = boot.wait()
        out.say("stopped; the system runs on until its run is over or the chip is unplugged")
    piper.join()
    log.stop.set()
    follower.join()

    if code != 0:
        sys.exit(code)
    if checks is not None and checks.failed:
        sys.exit(1)
    if log.device is None and any(HAS_ADDRESS in line for line in halt_text):
        out.say("the system had an address, and its log never came over the network")
        sys.exit(1)


if __name__ == "__main__":
    main()
