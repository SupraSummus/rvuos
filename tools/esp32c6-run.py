#!/usr/bin/env python3
"""Boot an rvuos image on an ESP32-C6 from RAM and print what it writes.

The chip's ROM loads the image's segments into RAM over USB and jumps to it,
so nothing is written to flash.
The flash is attached before the jump, as a bootloader leaves it,
so that the kernel's window onto it reads; see kernel/board/esp32c6/board.h.
Opening the USB Serial/JTAG port resets the chip,
so the image is loaded and its output read on one connection.
The kernel's halt ends with "rvuos: halted with code <n>",
and this exits with that code, as QEMU exits with the kernel's.

Data beside the image, such as the PHY harness of user/phyblob/, goes with --ram address:file,
loaded by the ROM as the image's segments are, and bound by the same limit;
--text address:file loads a text, such as a configuration, with a NUL after it,
since RAM keeps what an earlier run left past a shorter file.
--flash address:file has the flash hold a file at an address, such as the Wi-Fi driver of user/wifi/esp32c6/:
the ROM compares the flash's MD5 with the file's on the same connection,
and only if they differ does esptool's stub write it, after which the chip is reset and connected to again.
What this reads on its standard input goes to the board, for a root task that takes commands from the console.

Usage: tools/esp32c6-run.py [--port /dev/ttyACM0] [--timeout seconds] [--ram address:file]... [--text address:file]...
                            [--flash address:file]... image.bin
"""

import argparse
import contextlib
import hashlib
import os
import re
import sys
import threading
import time

import esptool
from esptool.bin_image import LoadFirmwareImage
from esptool.util import flash_size_bytes

RAM_BASE = 0x40800000
# The ROM keeps its download buffers from here up while it loads; see kernel/board/esp32c6/board.h.
ROM_LOAD_LIMIT = 0x4086AD08

HALTED = re.compile(rb"rvuos: halted with code 0x([0-9a-f]{8})")


def ram_file(spec, end=b""):
    """An --ram argument: the address and the bytes; end follows them, --text's NUL."""
    addr, _, path = spec.partition(":")
    return int(addr, 0), open(path, "rb").read() + end


def connect(port):
    esp = esptool.detect_chip(port)
    if esp.CHIP_NAME != "ESP32-C6":
        sys.exit(f"{port} is an {esp.CHIP_NAME}, not an ESP32-C6")
    esp.flash_spi_attach(0)
    # The ROM takes the flash for 2 MiB until told, and the driver lies past that.
    esp.flash_set_parameters(flash_size_bytes(esptool.cmds.detect_flash_size(esp) or "4MB"))
    return esp


def flash(esp, port, files):
    """The flash made to hold each (address, bytes) of files; the connection to go on with."""
    stale = [(addr, data) for addr, data in files if esp.flash_md5sum(addr, len(data)) != hashlib.md5(data).hexdigest()]
    if not stale:
        print(f"the flash holds {', '.join(f'{addr:#x}' for addr, _ in files)} already")
        return esp
    stub = esp.run_stub()
    esptool.cmds.write_flash(stub, stale)
    stub._port.close()
    return connect(port)


def load(port, path, extra=(), flash_files=()):
    # esptool reports on stdout, which carries the board's output here.
    with contextlib.redirect_stdout(sys.stderr):
        esp = connect(port)
        if flash_files:
            esp = flash(esp, port, flash_files)
        image = LoadFirmwareImage(esp.CHIP_NAME, path)
        segments = [(seg.addr, seg.data) for seg in image.segments]
        for addr, data in extra:
            for seg_addr, seg in segments:
                if addr < seg_addr + len(seg) and seg_addr < addr + len(data):
                    sys.exit(f"--ram data at {addr:#x} overlaps the image's segment at {seg_addr:#x}")
        for addr, data in segments + list(extra):
            if not RAM_BASE <= addr <= addr + len(data) <= ROM_LOAD_LIMIT:
                sys.exit(f"{len(data)} bytes at {addr:#x} do not lie in RAM below the ROM's buffers at {ROM_LOAD_LIMIT:#x}")
            size = len(data)
            blocks = (size + esp.ESP_RAM_BLOCK - 1) // esp.ESP_RAM_BLOCK
            esp.mem_begin(size, blocks, esp.ESP_RAM_BLOCK, addr)
            for i in range(blocks):
                esp.mem_block(data[i * esp.ESP_RAM_BLOCK:(i + 1) * esp.ESP_RAM_BLOCK], i)
        esp.mem_finish(image.entrypoint)
    return esp._port


def forward(serial):
    """
    What comes on the standard input to the board, until it ends,
    read from the descriptor rather than a buffered file, which would hold a lock the interpreter's exit waits for.
    """
    while data := os.read(sys.stdin.fileno(), 256):
        serial.write(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--timeout", type=float, default=None,
                        help="give up after this many seconds; by default wait for the halt")
    parser.add_argument("--ram", action="append", default=[], metavar="ADDRESS:FILE",
                        help="also load a file's bytes into RAM at an address")
    parser.add_argument("--text", action="append", default=[], metavar="ADDRESS:FILE",
                        help="also load a text file into RAM at an address, a NUL after it")
    parser.add_argument("--flash", action="append", default=[], metavar="ADDRESS:FILE",
                        help="have the flash hold a file at an address, written only if it holds something else")
    parser.add_argument("image")
    args = parser.parse_args()

    extra = [ram_file(spec) for spec in args.ram] + [ram_file(spec, b"\0") for spec in args.text]
    serial = load(args.port, args.image, extra, [ram_file(spec) for spec in args.flash])
    threading.Thread(target=forward, args=(serial,), daemon=True).start()
    serial.timeout = 0.05
    deadline = None if args.timeout is None else time.monotonic() + args.timeout
    pending = b""
    while deadline is None or time.monotonic() < deadline:
        pending += serial.read(4096)
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            sys.stdout.write(line.rstrip(b"\r").decode(errors="replace") + "\n")
            sys.stdout.flush()
            halted = HALTED.search(line)
            if halted:
                sys.exit(int(halted.group(1), 16))
    sys.stdout.write(pending.decode(errors="replace"))
    sys.exit("timed out before the kernel halted")


if __name__ == "__main__":
    main()
