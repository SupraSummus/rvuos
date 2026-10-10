#!/usr/bin/env python3
"""Run a Wi-Fi system, follow its log while it runs, and check it from the host.

The command after -- boots the system and waits for its halt, printing what the board writes.
On a Pico 2 W, as `make BOARD=rp2350 wifi` runs it, this asks for the log on UDP port 7070, see user/wifi/wifi.h,
and once the log says the clients answer, checks the status port, the echo, the echo's throughput, the clock,
the echo built again after a fault and after a hang, and ping.
With --console, as `make BOARD=esp32c6 wifi-esp32c6` runs it, the command's output is the log
and its input reaches the root task: once the log says the clients answer, this checks the same, but for the logger,
which that system does not run, then ends the run with "end", see user/wifi/esp32c6/root.c.
Each step follows a line of the log or the command's end, never a time.

Each line goes out with its seconds since the start, into --save's file too;
a fault's or the heap's line gets the functions its addresses fall in, from the ELF files of --symbols.

It exits with the halt's code, or if that is 0, with 1 when a check failed
or the system had an address and its log never came.

Usage: tools/wifi-run.py [--console] [--save file] [--symbols elf]... -- command...
"""

import argparse
import os
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
ADDRESS = re.compile(r"root: the system answers at (\d+\.\d+\.\d+\.\d+)")
# The lines whose addresses are named, and the addresses in them.
NAMED = re.compile(r"^(fault:|heap:)|mepc=")
HEX = re.compile(r"\b(?:0x)?([0-9a-f]{8})\b")


def code_ranges(path):
    """The address ranges of a 32-bit ELF file's sections of code."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:5] != b"\x7fELF\x01":
        return []
    shoff = struct.unpack_from("<I", elf, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x2E)
    ranges = []
    for i in range(shnum):
        _, _, flags, addr, _, size = struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)
        if flags & 0x4 and size:  # SHF_EXECINSTR
            ranges.append((addr, addr + size))
    return ranges


class Symbols:
    """The functions addresses of code fall in, from ELF files, through llvm-symbolizer."""

    def __init__(self, elfs):
        found = shutil.which("llvm-symbolizer")
        self.elfs = [(elf, code_ranges(elf)) for elf in elfs if found and os.path.exists(elf)]

    def annotate(self, line):
        if not self.elfs or not NAMED.search(line):
            return line
        left, named = list(dict.fromkeys(int(h, 16) for h in HEX.findall(line))), {}
        for elf, ranges in self.elfs:
            mine = [a for a in left if any(lo <= a < hi for lo, hi in ranges)]
            if mine:
                out = subprocess.run(["llvm-symbolizer", "--obj=" + elf, "--functions=linkage", "--no-inlines",
                                      *map(hex, mine)], capture_output=True, text=True).stdout
                named.update(zip(mine, (block.split("\n")[0] for block in out.strip().split("\n\n"))))
                left = [a for a in left if a not in named]
        names = [f"{a:08x}={n}" for a, n in named.items() if n != "??"]
        return line.rstrip("\n") + "  [" + ", ".join(names) + "]\n" if names else line


class Output:
    """
    Whole lines to stdout, from every thread, so that the log's lines and this tool's never cut into each other,
    each with the seconds since the start before it, and into the file of --save too.
    """

    def __init__(self, save=None, symbols=None):
        self.lock = threading.Lock()
        self.start = time.monotonic()
        self.file = open(save, "w") if save else None
        self.symbols = symbols or Symbols([])

    def lines(self, text):
        with self.lock:
            stamp = f"{time.monotonic() - self.start:8.3f} "
            out = "".join(stamp + self.symbols.annotate(line) for line in text.splitlines(True))
            sys.stdout.write(out)
            sys.stdout.flush()
            if self.file:
                self.file.write(out)
                self.file.flush()

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
        self.ended = False  # the command that boots the system is done

    def feed(self, text):
        """The log as the command's output brings it, with --console."""
        with self.changed:
            self.text += text
            self.changed.notify_all()

    def end(self):
        with self.changed:
            self.ended = True
            self.changed.notify_all()

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

    def wait(self, ready, timeout=None):
        """Waits until ready(text) holds, or the command is done, for timeout seconds at most; whether it holds."""
        with self.changed:
            self.changed.wait_for(lambda: ready(self.text) or self.stop.is_set() or self.ended, timeout)
            return ready(self.text)


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
        ok = got is not None and got.startswith(b"rvuos on ")
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

    def throughput(self, seconds=5.0, size=1024, window=2, wait=0.5):
        """Datagrams of size bytes to the echo, window of them in flight, for seconds: the bytes that came back a
        second, each way, which the slower of the two ways bounds; one not back within wait is lost.
        Two in flight keep either board's sending busy, as one does not."""
        self.sock.setblocking(False)
        try:
            while True:
                self.sock.recvfrom(2048)
        except BlockingIOError:
            pass
        out, times, sent, back, lost = {}, [], 0, 0, 0
        start = time.perf_counter()
        while True:
            now = time.perf_counter()
            for n, t in list(out.items()):
                if now - t > wait:
                    del out[n]
                    lost += 1
            if now - start < seconds:
                while len(out) < window:
                    self.sock.sendto((b"T%07d" % sent).ljust(size, b"t"), (self.ip, ECHO_PORT))
                    out[sent] = time.perf_counter()
                    sent += 1
            elif not out:
                break
            self.sock.settimeout(0.05)
            try:
                got, frm = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            n = int(got[1:8]) if frm == (self.ip, ECHO_PORT) and len(got) == size and got[:1] == b"T" else None
            if n in out:
                times.append((time.perf_counter() - out.pop(n)) * 1000)
                back += 1
        took = time.perf_counter() - start
        detail = (f"{back} of {sent} datagrams of {size} bytes back in {took:.1f} s, "
                  f"{back * size * 8 / took / 1000:.0f} kb/s each way, {lost} lost")
        if times:
            detail += f", median {statistics.median(times):.1f} ms"
        self.result("throughput", back > 0, detail)

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

    def run(self, then):
        self.status()
        self.echo()
        self.throughput()
        self.clock()
        self.echo_back("echo after a fault", b"fault", ECHO_FAULTED, 5)
        self.echo_back("echo after a hang", b"hang", ECHO_HUNG, 6)
        self.ping()
        self.verdict(then)

    def verdict(self, then):
        if self.failed:
            self.out.say("checks failed:", ", ".join(self.failed))
        else:
            self.out.say(f"every check passed; {then}")


def tell(boot, line, out):
    """A line to the root task, through the command's input, with --console."""
    try:
        boot.stdin.write(line + "\n")
        boot.stdin.flush()
    except (BrokenPipeError, ValueError):
        out.say(f"could not say '{line}': the command is done")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--console", action="store_true",
                        help="the command's output is the log, and its input reaches the root task")
    parser.add_argument("--save", metavar="FILE", help="also write every line into a file")
    parser.add_argument("--symbols", action="append", default=[], metavar="ELF",
                        help="name the functions the addresses of fault lines fall in, from this ELF file")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- and the command that boots the system")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("the command that boots the system comes after --")

    out = Output(args.save, Symbols(args.symbols))
    log = Log(out)
    follower = None
    if not args.console:
        follower = threading.Thread(target=log.run, daemon=True)
        follower.start()
    boot = subprocess.Popen(command, stdout=subprocess.PIPE, stdin=subprocess.PIPE if args.console else None,
                            text=True, errors="replace")
    halt_text = []

    def through():
        for line in boot.stdout:
            halt_text.append(line)
            out.lines(line)
            if args.console:
                log.feed(line)
        log.end()

    piper = threading.Thread(target=through, daemon=True)
    piper.start()

    checks = None
    try:
        if args.console:
            if log.wait(lambda t: all(s in t for s in CLIENTS_UP)):
                checks = Checks(out, log, ADDRESS.search(log.text).group(1))
                checks.run("the run ends")
                tell(boot, "end", out)
        elif log.wait(lambda t: all(s in t for s in CLIENTS_UP)):
            checks = Checks(out, log, log.device)
            checks.run("the system runs on until its run is over")
        code = boot.wait()
    except KeyboardInterrupt:
        if args.console:
            tell(boot, "end", out)
            out.say("stopped; the run ends")
        else:
            boot.terminate()
            out.say("stopped; the system runs on until its run is over or the chip is unplugged")
        code = boot.wait()
    piper.join()
    log.stop.set()
    if follower:
        follower.join()

    if code != 0:
        sys.exit(code)
    if checks is not None and checks.failed:
        sys.exit(1)
    if args.console and checks is None and "root: joining" in log.text:
        out.say("the system's clients never answered" if HAS_ADDRESS in log.text else "the system never had an address")
        sys.exit(1)
    if not args.console and log.device is None and any(HAS_ADDRESS in line for line in halt_text):
        out.say("the system had an address, and its log never came over the network")
        sys.exit(1)


if __name__ == "__main__":
    main()
