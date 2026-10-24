#!/usr/bin/env python3
"""Boot an rvuos image on an ESP32-C6 from flash and print what it writes.

The image is the flash from its start, as kernel/build.mk makes it:
what the ROM loads into RAM, behind Espressif's header, and the code that runs from flash after it.
The ROM's download mode, which opening the USB Serial/JTAG port resets the chip into,
compares the flash's MD5 with each file's, the image's and those beside it,
and only if they differ does esptool's stub write it.
Then the chip is reset to boot from flash, on the same connection, and its output read there.
The kernel's halt ends with "rvuos: halted with code <n>",
and this exits with that code, as QEMU exits with the kernel's.

--flash address:file has the flash hold a file at an address, such as the Wi-Fi driver of user/wifi/esp32c6/,
and --text address:file a text, such as a configuration in the root task's input region, with a NUL after it,
since the flash keeps what an earlier run left past a shorter file.
What this reads on its standard input goes to the board, for a root task that takes commands from the console.

Usage: tools/esp32c6-run.py [--port /dev/ttyACM0] [--timeout seconds] [--flash address:file]...
                            [--text address:file]... image.bin
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
from esptool.util import flash_size_bytes

HALTED = re.compile(rb"rvuos: halted with code 0x([0-9a-f]{8})")


def flash_file(spec, end=b""):
    """A --flash argument: the address and the bytes; end follows them, --text's NUL."""
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


def flash(esp, files):
    """The flash made to hold each (address, bytes) of files."""
    stale = [(addr, data) for addr, data in files if esp.flash_md5sum(addr, len(data)) != hashlib.md5(data).hexdigest()]
    if not stale:
        print(f"the flash holds {', '.join(f'{addr:#x}' for addr, _ in files)} already")
        return
    esptool.cmds.write_flash(esp.run_stub(), stale)


def boot(port, files):
    """The flash made to hold files, and the chip booted from it; the serial port its output comes on."""
    # esptool reports on stdout, which carries the board's output here.
    with contextlib.redirect_stdout(sys.stderr):
        esp = connect(port)
        flash(esp, files)
        # RTS resets the chip and DTR, low, lets it boot from flash; the USB Serial/JTAG controller stays connected.
        esp._port.dtr = False
        esp._port.rts = True
        time.sleep(0.1)
        esp._port.rts = False
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
    parser.add_argument("--flash", action="append", default=[], metavar="ADDRESS:FILE",
                        help="have the flash hold a file at an address, written only if it holds something else")
    parser.add_argument("--text", action="append", default=[], metavar="ADDRESS:FILE",
                        help="have the flash hold a text file at an address, a NUL after it")
    parser.add_argument("image")
    args = parser.parse_args()

    # What the run flashes, into the log's own stream, so the host's tools can hold a log to the image that ran.
    for spec in args.flash:
        address, path = spec.split(":", 1)
        print("# flashed %#x sha256 %s" % (int(address, 0), hashlib.sha256(open(path, "rb").read()).hexdigest()))
    files = [flash_file(f"0:{args.image}")] + [flash_file(spec) for spec in args.flash] + \
            [flash_file(spec, b"\0") for spec in args.text]
    serial = boot(args.port, files)
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
