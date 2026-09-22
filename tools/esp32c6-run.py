#!/usr/bin/env python3
"""Boot an rvuos image on an ESP32-C6 from RAM and print what it writes.

The chip's ROM loads the image's segments into RAM over USB and jumps to it,
so nothing is written to flash.
Opening the USB Serial/JTAG port resets the chip,
so the image is loaded and its output read on one connection.
The kernel's halt ends with "rvuos: halted with code <n>",
and this exits with that code, as QEMU exits with the kernel's.

Usage: tools/esp32c6-run.py [--port /dev/ttyACM0] [--timeout seconds] image.bin
"""

import argparse
import contextlib
import re
import sys
import time

import esptool
from esptool.bin_image import LoadFirmwareImage

# The ROM keeps its download buffers from here up while it loads; see kernel/board/esp32c6/board.h.
ROM_LOAD_LIMIT = 0x4086AD08

HALTED = re.compile(rb"rvuos: halted with code 0x([0-9a-f]{8})")


def load(port, path):
    # esptool reports on stdout, which carries the board's output here.
    with contextlib.redirect_stdout(sys.stderr):
        esp = esptool.detect_chip(port)
        if esp.CHIP_NAME != "ESP32-C6":
            sys.exit(f"{port} is an {esp.CHIP_NAME}, not an ESP32-C6")
        image = LoadFirmwareImage(esp.CHIP_NAME, path)
        for seg in image.segments:
            if seg.addr + len(seg.data) > ROM_LOAD_LIMIT:
                sys.exit(f"segment at {seg.addr:#x} reaches past the ROM's buffers at {ROM_LOAD_LIMIT:#x}")
            size = len(seg.data)
            blocks = (size + esp.ESP_RAM_BLOCK - 1) // esp.ESP_RAM_BLOCK
            esp.mem_begin(size, blocks, esp.ESP_RAM_BLOCK, seg.addr)
            for i in range(blocks):
                esp.mem_block(seg.data[i * esp.ESP_RAM_BLOCK:(i + 1) * esp.ESP_RAM_BLOCK], i)
        esp.mem_finish(image.entrypoint)
    return esp._port


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--timeout", type=float, default=None,
                        help="give up after this many seconds; by default wait for the halt")
    parser.add_argument("image")
    args = parser.parse_args()

    serial = load(args.port, args.image)
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
