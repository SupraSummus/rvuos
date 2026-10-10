#!/usr/bin/env python3
"""Boot an rvuos image on an nRF52840 from RAM and print what it writes.

The chip is reached over SWD through an ST-Link and OpenOCD, which this starts and drives through its Tcl port.
It resets the chip and holds the core before its first instruction, so whatever the flash holds never runs,
writes the ELF's segments into RAM, reads them back, and starts the core at _start; nothing is written to flash.
It then turns halting debug off, so a breakpoint a thread runs is taken by the kernel rather than stopping the core.

The root task's console is a ring in RAM, which this follows as the core runs, see user/board/nrf52840/console.h;
the halt only says that the machine halted, and with which code, in the kernel's halt_record, see kernel/board/nrf52840/halt.c.
This then writes out what no logger took from the kernel's log, under the line QEMU's halt writes,
and "rvuos: halted with code <n>", and exits with that code, as QEMU exits with the kernel's.
A program that never halts runs on once this is stopped, until the chip is reset.

Usage: tools/nrf52840-run.py image.elf
"""

import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

# The console's block, see UART_BASE and UART_SIZE in kernel/board/nrf52840/board.h: the count of bytes written, then the ring.
CONSOLE_BASE = 0x20000000
CONSOLE_RING = CONSOLE_BASE + 16
CONSOLE_ROOM = 0x2000 - 16

# "HALT", which the halt writes into halt_record's first word once the rest is written.
HALT_MAGIC = 0x544C4148
# struct rvuos_log's header, see include/rvuos/abi.h: the ring follows it.
RVUOS_LOG_HEADER = 32

# The Debug Halting Control and Status Register: its key, written alone, turns halting debug off.
DHCSR = 0xE000EDF0
DHCSR_KEY = 0xA05F0000

# How long a look at a console that has not moved waits before the next.
POLL_SECONDS = 0.02

TCL_END = b"\x1a"


def elf_image(path):
    """The loadable segments of a 32-bit little-endian ARM ELF as (address, bytes), and its symbols by name."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1 or struct.unpack_from("<H", data, 18) != (40,):
        sys.exit(f"{path} is not a 32-bit little-endian ARM ELF")
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 42)
    segments = []
    for i in range(phnum):
        p_type, p_offset, _, p_paddr, p_filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1 and p_filesz > 0:
            segments.append((p_paddr, data[p_offset:p_offset + p_filesz]))
    sections = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    symbols = {}
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        names = data[strtab[4]:strtab[4] + strtab[5]]
        for off in range(sh[4], sh[4] + sh[5], sh[9]):
            st_name, st_value = struct.unpack_from("<II", data, off)
            symbols.setdefault(names[st_name:names.index(b"\0", st_name)].decode(), st_value)
    return segments, symbols


class OpenOCD:
    """OpenOCD on the ST-Link and the chip, and its Tcl port, through which every command goes."""

    def __init__(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            ["openocd", "-f", "interface/stlink-dap.cfg", "-c", "transport select dapdirect_swd",
             "-f", "target/nrf52.cfg",
             # No flash algorithm runs, so nothing needs the work area, which lies where the console and kernel go.
             "-c", "nrf52.cpu configure -work-area-size 0",
             "-c", f"tcl_port {port}", "-c", "gdb_port disabled", "-c", "telnet_port disabled", "-c", "init"],
            stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = None
        deadline = time.monotonic() + 10
        while self.sock is None:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
            except OSError:
                if self.process.poll() is not None or time.monotonic() > deadline:
                    self.fail("OpenOCD did not start")
                time.sleep(0.05)
        self.pending = b""

    def fail(self, why):
        self.log.seek(0)
        sys.stderr.write(self.log.read().decode(errors="replace"))
        self.close()
        sys.exit(f"nrf52840-run: {why}")

    def __call__(self, command):
        """A command's result; its error, caught in Tcl, fails the run."""
        wrapped = f"if {{[catch {{{command}}} r]}} {{return \"error: $r\"}} else {{return $r}}"
        self.sock.sendall(wrapped.encode() + TCL_END)
        while TCL_END not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                self.fail(f"OpenOCD went away during: {command}")
            self.pending += chunk
        reply, _, self.pending = self.pending.partition(TCL_END)
        reply = reply.decode(errors="replace")
        if reply.startswith("error: "):
            self.fail(f"{command}: {reply[7:]}")
        return reply

    def words(self, address, count):
        return [int(w, 16) for w in self(f"read_memory {address:#x} 32 {count}").split()]

    def bytes(self, address, count):
        """count bytes from address, read as the words that hold them, which the probe moves faster."""
        if count == 0:
            return b""
        first = address & ~3
        words = self.words(first, (address + count - first + 3) // 4)
        return struct.pack(f"<{len(words)}I", *words)[address - first:address - first + count]

    def close(self):
        if self.sock is not None:
            try:
                self.sock.sendall(b"shutdown" + TCL_END)
            except OSError:
                pass
            self.sock.close()
            self.sock = None
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


class Console:
    """The root task's ring, followed from the first byte, and what the writer lapped before it was read."""

    def __init__(self, ocd):
        self.ocd = ocd
        self.seen = 0

    def ring(self, start, end):
        at = start % CONSOLE_ROOM
        first = min(end - start, CONSOLE_ROOM - at)
        return self.ocd.bytes(CONSOLE_RING + at, first) + self.ocd.bytes(CONSOLE_RING, end - start - first)

    def follow(self, head):
        start = max(self.seen, head - CONSOLE_ROOM)
        data = self.ring(start, head)
        # What the writer lapped while it was read is the next round's bytes, not these.
        after, = self.ocd.words(CONSOLE_BASE, 1)
        kept = max(start, after - CONSOLE_ROOM)
        lost = kept - self.seen
        self.seen = max(head, kept)
        note = f"\nrvuos: the console lost {lost:#010x} bytes\n".encode() if lost else b""
        return note + data[kept - start:]


def log_untaken(ocd, base):
    """What no logger took from the kernel's log, oldest first, as klog_dump hands it to a halt."""
    head, size, taken = ocd.words(base, 3)
    n = (head - taken) & 0xFFFFFFFF
    n = 0 if n >= 0x80000000 else min(n, size)
    ring = ocd.bytes(base + RVUOS_LOG_HEADER, size)
    return bytes(ring[(head - n + i) % size] for i in range(n))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    image = sys.argv[1]
    segments, symbols = elf_image(image)
    if "_start" not in symbols or "halt_record" not in symbols:
        sys.exit(f"{image} is not an rvuos kernel for nRF52840")
    record = symbols["halt_record"]

    ocd = OpenOCD()
    # timeout(1) ends a run with SIGTERM, which leaves the probe free for the next.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(124))
    out = sys.stdout.buffer
    try:
        ocd("reset halt")
        ocd(f"load_image {{{os.path.abspath(image)}}} 0 elf")
        for address, data in segments:
            back = ocd.bytes(address, len(data))
            if back != data:
                at = next(i for i in range(len(data)) if back[i] != data[i])
                ocd.fail(f"RAM at {address + at:#x} did not take the image")
        # RAM keeps what the last run left: the console starts empty and the halt unsaid.
        ocd(f"write_memory {CONSOLE_BASE:#x} 32 {{0}}")
        ocd(f"write_memory {record:#x} 32 {{0}}")
        ocd("reg xPSR 0x01000000")
        ocd(f"reg pc {symbols['_start'] & ~1:#x}")
        ocd("resume")
        ocd(f"write_memory {DHCSR:#x} 32 {DHCSR_KEY:#x}")

        console = Console(ocd)
        while True:
            head, = ocd.words(CONSOLE_BASE, 1)
            magic, code, log = ocd.words(record, 3)
            if magic == HALT_MAGIC:
                break
            # A console that moved is looked at again at once, as it may be writing faster than this reads.
            if head > console.seen:
                out.write(console.follow(head))
                out.flush()
            else:
                time.sleep(POLL_SECONDS)

        out.write(console.follow(ocd.words(CONSOLE_BASE, 1)[0]))
        out.write(b"\nrvuos: halting, the log follows\n")
        out.write(log_untaken(ocd, log))
        out.write(f"rvuos: halted with code {code:#010x}\n".encode())
        out.flush()
        return code & 0xFF
    finally:
        ocd.close()


if __name__ == "__main__":
    sys.exit(main())
