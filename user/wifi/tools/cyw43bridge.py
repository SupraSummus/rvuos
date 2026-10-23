"""The host's side of the Wi-Fi system's bridge, user/wifi/bridge.h: the CYW43439's backplane, read and written from here.

    with Bridge() as b:
        b.write(0x19640894, 3)
        print(hex(b.read(0x19000000)))

Bridge() builds the Wi-Fi system's bridge, make BOARD=rp2350 WIFI_BRIDGE=1, or brings it up to date,
boots it on a Pico 2 W in its BOOTSEL mode, through tools/rp2350-run.py --stdin,
with the firmware blob and a configuration of mode=bridge, or conf's lines, whose mode's scan, join or access point
comes first, and waits for the driver to open the bridge;
each read and write is then a line to the board and its answer, about half a millisecond;
post() queues a write instead, which goes on the line of the next ask, so that it costs no wait of its own.
Once sniff() has named two words of the chip's, every answer brings what they collected, see user/wifi/bridge.h,
and take_sniffed() hands it over.
The board's log, its lines besides the answers, goes to `log`, a callable, by default nowhere.
Leaving the `with` ends the run, which halts the board, back in BOOTSEL for the next;
should this program die without, the board's run ends by itself after run_s seconds, RUN_S by default.
"""

import os
import select
import signal
import subprocess
import sys
import tempfile
import time

BLOB = "build/cyw43/blob.bin"
BLOB_AT = 0x20040000  # FREE_RAM_BASE, kernel/board/rp2350/board.h
INPUT_AT = 0x20038000  # INPUT_BASE
IMAGE = "build/rp2350-bridge/kernel-wifi.elf"  # make BOARD=rp2350 WIFI_BRIDGE=1
BLOCK = 256  # BRIDGE_BYTES, the most bytes of one write
WRITES = 16  # BRIDGE_WRITES, the most writes queued before one ask
RUN_S = 1800


class BridgeError(Exception):
    pass


class Bridge:
    def __init__(self, image=IMAGE, blob=BLOB, run_s=RUN_S, log=None, timeout=30.0, conf=""):
        self.log = log or (lambda line: None)
        if image == IMAGE:
            subprocess.run(["make", "-s", "BOARD=rp2350", "WIFI_BRIDGE=1", IMAGE], check=True)
        self.conf = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
        # conf, lines of the Wi-Fi system's configuration, has the bridge open once their mode's work is up.
        self.conf.write((conf.rstrip("\n") + "\n" if conf else "mode=bridge\n") + f"run={run_s}\n")
        self.conf.close()
        self.proc = subprocess.Popen(
            [sys.executable, "tools/rp2350-run.py", "--stdin", "--ram", f"{BLOB_AT:#x}:{blob}",
             "--text", f"{INPUT_AT:#x}:{self.conf.name}", image],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.pending = b""
        self.asks = 0
        self.queued = []  # the writes posted and not yet sent
        self.sniffed = 0  # what the sniffed words collected, both of them, since take_sniffed()
        try:
            self.expect(lambda line: "the bridge is open" in line, timeout)
        except BaseException:  # the board let go and the configuration removed
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def line(self, timeout):
        end = time.monotonic() + timeout
        while b"\n" not in self.pending:
            left = end - time.monotonic()
            if left <= 0:
                return None
            ready, _, _ = select.select([self.proc.stdout], [], [], left)
            if ready:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if not chunk:
                    return None
                self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return line.decode(errors="replace").rstrip("\r")

    def expect(self, want, timeout):
        """The first line `want` takes, the others to the log; BridgeError if none comes in time."""
        end = time.monotonic() + timeout
        while True:
            line = self.line(max(0.0, end - time.monotonic()))
            if line is None:
                raise BridgeError("the board said nothing more" if self.proc.poll() is not None else "no answer in time")
            if want(line):
                return line
            self.log(line)

    def ask(self, text):
        """The answer to text, the writes queued before it going first, on its line, unless it is a block."""
        if self.queued and text.startswith("W "):
            self.ask(self.queued.pop())
        line = ";".join(self.queued + [text])
        self.queued = []
        self.proc.stdin.write(line.encode() + b"\n")
        self.asks += 1
        answer = self.expect(lambda l: l.startswith("bridge: "), 5.0)
        if answer in ("bridge: refused", "bridge: what?", "bridge: busy"):
            raise BridgeError(f"{line}: {answer}")
        answer, _, sniffed = answer.partition(" ! ")
        if sniffed:
            lo, hi = (int(v, 16) for v in sniffed.split())
            self.sniffed |= lo | hi << 32
        return answer

    def read(self, addr):
        line = self.ask(f"r {addr:x}")
        return int(line.rsplit("=", 1)[1], 16)

    def read_after(self, addr, spare):
        """The word at addr, read after the one at spare, and how many reads of addr it took; see bridge.h."""
        value, reads = self.ask(f"R {addr:x} {spare:x}").rsplit("=", 1)[1].split()
        return int(value, 16), int(reads)

    def write(self, addr, value):
        self.ask(f"w {addr:x} {value & 0xFFFFFFFF:x}")

    def post(self, addr, value):
        """A write, queued to go before the next ask; sent at once, as its own ask, when the queue is full."""
        self.queued.append(f"w {addr:x} {value & 0xFFFFFFFF:x}")
        if len(self.queued) > WRITES:
            self.ask(self.queued.pop())

    def sniff(self, addr, spare):
        """Every answer from now on brings the two words at addr, read after spare, which the bridge then clears."""
        self.ask(f"S {addr:x} {spare:x}")

    def take_sniffed(self):
        bits, self.sniffed = self.sniffed, 0
        return bits

    def read16(self, addr):
        """The halfword at addr, an even address."""
        line = self.ask(f"h {addr:x}")
        return int(line.split("=")[1].split()[0], 16)

    def write16(self, addr, value):
        """A halfword written at addr, alone: for a register of two halves, which a word's write would both set."""
        self.ask(f"h {addr:x} {value & 0xFFFF:x}")

    def write_block(self, addr, data):
        """Bytes at addr, a multiple of four of them at a multiple of four, in blocks of the bridge's most."""
        for at in range(0, len(data), BLOCK):
            self.ask(f"W {addr + at:x} {data[at:at + BLOCK].hex()}")

    def close(self):
        # An interrupt now would leave the board running its bridge, not halted for the next run.
        previous = {sig: signal.signal(sig, signal.SIG_IGN) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            self.end()
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)

    def end(self):
        if self.proc.poll() is None:
            try:
                if self.queued:
                    self.ask(self.queued.pop())
            except (BrokenPipeError, BridgeError):
                pass
            try:  # the end goes whatever became of the writes, so that the board halts
                self.proc.stdin.write(b"end\n")
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            try:
                while self.line(20.0) is not None:
                    pass
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        os.unlink(self.conf.name)
