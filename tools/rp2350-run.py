#!/usr/bin/env python3
"""Boot an rvuos image on an RP2350 from RAM and print what it writes.

The chip must be in its BOOTSEL mode, as it is when plugged in with BOOTSEL held,
after the last image this ran has halted and been read,
or seventeen seconds after one that hung, when its watchdog reboots it there.
The bootrom's PICOBOOT interface takes the ELF's segments into SRAM over USB,
which this reads back and checks every command's status,
and reboots into them, on the RISC-V cores or the Arm ones as the ELF's machine says,
so nothing is written to flash.
The kernel has no serial port while it runs; its halt makes one of the chip's USB controller,
and writes out the root task's console and the kernel's log;
see kernel/board/rp2350/halt.c.
The halt ends with "rvuos: halted with code <n>",
and this exits with that code, as QEMU exits with the kernel's.
Closing the port then reboots the chip into BOOTSEL for the next image.

Data beside the image, such as the Wi-Fi system's firmware blob, goes with --ram address:file,
into SRAM alone and never into flash, checked as the segments are;
--text address:file writes a text, such as a configuration, with a NUL after it,
since SRAM keeps what an earlier run left past a shorter file.

Usage: tools/rp2350-run.py [--timeout seconds] [--ram address:file]... [--text address:file]... image.elf
"""

import argparse
import glob
import os
import re
import select
import struct
import sys
import termios
import time
import tty

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("tools/rp2350-run.py needs pyusb: pacman -S python-pyusb, or pip install pyusb")

BOOTSEL = (0x2E8A, 0x000F)
# The halt's serial port; see kernel/board/rp2350/cdc.c.
HALT_PORT = (0x1209, 0x0001)

# Where the bootrom looks for the image's block when it reboots into RAM, and how far;
# see kernel/board/rp2350/board.h and image.S.
RAM_BASE = 0x20000000
RAM_SEARCH = 0x80000
# All of SRAM, SRAM8 and SRAM9 with it, where --ram may write.
SRAM_END = 0x20082000

PICOBOOT_MAGIC = 0x431FD10B
PC_EXCLUSIVE_ACCESS = 0x01
PC_READ = 0x84
PC_WRITE = 0x05
PC_REBOOT2 = 0x0A
PC_GET_INFO = 0x8B
GET_INFO_SYS = 1
SYS_INFO_CPU_INFO = 0x4
PC_INTERFACE_RESET = 0x41
PC_COMMAND_STATUS = 0x42
EXCLUSIVE = 1
REBOOT2_RAM_IMAGE = 0x3
REBOOT2_TO_ARM = 0x10
REBOOT2_TO_RISCV = 0x20
# The cores to reboot into, by the ELF's e_machine.
EM_ARM = 40
EM_RISCV = 243
REBOOT2_CORES = {EM_ARM: REBOOT2_TO_ARM, EM_RISCV: REBOOT2_TO_RISCV}

HALTED = re.compile(rb"rvuos: halted with code 0x([0-9a-f]{8})")


def say(*words):
    print("rp2350-run:", *words, file=sys.stderr, flush=True)


def elf_image(path):
    """The machine of a 32-bit little-endian ELF, and its loadable segments as (address, bytes)."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path} is not a 32-bit little-endian ELF")
    machine, = struct.unpack_from("<H", data, 18)
    if machine not in REBOOT2_CORES:
        sys.exit(f"{path} is for machine {machine}, which RP2350 has no cores for")
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segments = []
    for i in range(phnum):
        p_type, p_offset, _, p_paddr, p_filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1 and p_filesz > 0:
            segments.append((p_paddr, data[p_offset:p_offset + p_filesz]))
    return machine, segments


def usb_device(ids):
    return usb.core.find(idVendor=ids[0], idProduct=ids[1])


def halt_port():
    """The tty of the halt's serial port, or None while there is none."""
    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            vid = int(open(f"{dev}/idVendor").read(), 16)
            pid = int(open(f"{dev}/idProduct").read(), 16)
        except OSError:
            continue
        if (vid, pid) == HALT_PORT:
            ttys = glob.glob(f"{dev}/{os.path.basename(dev)}:1.0/tty/tty*")
            if ttys:
                return "/dev/" + os.path.basename(ttys[0])
    return None


def wait_for(find, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = find()
        if found is not None:
            return found
        time.sleep(0.05)
    return None


def open_port(path):
    """The halt's port, raw, opened: which raises DTR and so starts the halt writing."""
    for _ in range(40):
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
            break
        except OSError:
            time.sleep(0.05)
    else:
        sys.exit(f"cannot open {path}")
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] |= termios.HUPCL | termios.CLOCAL  # closing drops DTR, which reboots the chip
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def to_bootsel():
    """The chip in BOOTSEL, rebooting it there if an image before has halted and waits for a reader."""
    dev = usb_device(BOOTSEL)
    if dev is not None:
        return dev
    port = halt_port()
    if port is not None:
        say(f"an earlier halt waits on {port}; letting it go")
        os.close(open_port(port))
    dev = wait_for(lambda: usb_device(BOOTSEL), 5.0)
    if dev is None:
        sys.exit("no RP2350 in BOOTSEL mode: hold BOOTSEL while plugging it in")
    return dev


class Picoboot:
    def __init__(self, dev):
        self.dev = dev
        cfg = dev.get_active_configuration()
        self.intf = usb.util.find_descriptor(cfg, bInterfaceClass=0xFF)
        if self.intf is None:
            sys.exit("the chip's BOOTSEL mode shows no PICOBOOT interface")
        try:
            usb.util.claim_interface(dev, self.intf)
        except usb.core.USBError as e:
            sys.exit(f"cannot claim PICOBOOT ({e}); the device needs a udev rule giving you access, see manual/targets.md")
        eps = self.intf.endpoints()
        self.out = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        self.inp = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.token = 1
        dev.ctrl_transfer(0x41, PC_INTERFACE_RESET, 0, self.intf.bInterfaceNumber, None)

    def command(self, cmd, args=b"", data=b"", read=0, check=True):
        """Send a command with data out or read data in, and fail unless the bootrom says it succeeded."""
        packet = struct.pack("<IIBBHI", PICOBOOT_MAGIC, self.token, cmd, len(args), 0, len(data) or read)
        packet += args.ljust(16, b"\0")
        self.out.write(packet)
        if read:
            got = bytes(self.inp.read(read, timeout=10000))
            self.out.write(b"")  # the empty packet that acknowledges data in
        else:
            got = b""
            if data:
                self.out.write(data, timeout=10000)
            self.inp.read(64, timeout=10000)  # the empty packet that says the command is done
        if not check:
            return got
        status = bytes(self.dev.ctrl_transfer(0xC1, PC_COMMAND_STATUS, 0, self.intf.bInterfaceNumber, 16))
        token, code = struct.unpack_from("<II", status)
        if token != self.token or code != 0:
            sys.exit(f"PICOBOOT command {cmd:#04x} failed: status {code}, token {token} of {self.token}")
        self.token += 1
        return got

    def write(self, addr, data):
        self.command(PC_WRITE, struct.pack("<II", addr, len(data)), data)

    def read(self, addr, size):
        return self.command(PC_READ, struct.pack("<II", addr, size), read=size)

    def cores(self):
        """The cores the bootrom runs on now: arm or riscv."""
        info = self.command(PC_GET_INFO, struct.pack("<BBHIII", GET_INFO_SYS, 0, 0, SYS_INFO_CPU_INFO, 0, 0), read=16)
        return "riscv" if struct.unpack_from("<II", info)[1] else "arm"

    def reboot_ram(self, machine, base, size):
        args = struct.pack("<IIII", REBOOT2_RAM_IMAGE | REBOOT2_CORES[machine], 10, base, size)
        self.command(PC_REBOOT2, args, check=False)  # the chip is gone before a status could be asked for


def ram_file(spec, end=b""):
    """An --ram argument: the address and the bytes, which must lie in SRAM; end follows them, --text's NUL."""
    addr, _, path = spec.partition(":")
    addr = int(addr, 0)
    data = open(path, "rb").read() + end
    if not (RAM_BASE <= addr and addr + len(data) <= SRAM_END):
        sys.exit(f"--ram {spec}: {len(data)} bytes at {addr:#010x} do not lie in SRAM")
    return addr, data


def load(path, extra=()):
    boot = Picoboot(to_bootsel())
    boot.command(PC_EXCLUSIVE_ACCESS, bytes([EXCLUSIVE]))
    machine, segments = elf_image(path)
    for addr, data in extra:
        for seg_addr, seg in segments:
            if addr < seg_addr + len(seg) and seg_addr < addr + len(data):
                sys.exit(f"--ram data at {addr:#010x} overlaps the image's segment at {seg_addr:#010x}")
    segments = segments + list(extra)
    say(f"the bootrom runs on the {boot.cores()} cores")
    for addr, data in segments:
        say(f"writing {len(data)} bytes at {addr:#010x}")
        boot.write(addr, data)
    for addr, data in segments:
        got = boot.read(addr, len(data))
        if got != data:
            bad = next(i for i in range(len(data)) if i >= len(got) or got[i] != data[i])
            sys.exit(f"the {len(data)} bytes at {addr:#010x} read back {len(got)} bytes, differing from {addr + bad:#010x}; "
                     "not rebooting into them")
    boot.reboot_ram(machine, RAM_BASE, RAM_SEARCH)
    usb.util.dispose_resources(boot.dev)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--timeout", type=float, default=None,
                        help="give up after this many seconds; by default wait for the halt")
    parser.add_argument("--ram", action="append", default=[], metavar="ADDRESS:FILE",
                        help="also write a file's bytes into SRAM at an address")
    parser.add_argument("--text", action="append", default=[], metavar="ADDRESS:FILE",
                        help="also write a text file into SRAM at an address, a NUL after it")
    parser.add_argument("image")
    args = parser.parse_args()

    deadline = None if args.timeout is None else time.monotonic() + args.timeout
    load(args.image, [ram_file(spec) for spec in args.ram] + [ram_file(spec, b"\0") for spec in args.text])
    say("booted; waiting for the halt's serial port")
    port = wait_for(halt_port, 1e9 if deadline is None else deadline - time.monotonic())
    if port is None:
        sys.exit("timed out before the kernel halted")
    fd = open_port(port)
    pending = b""
    while deadline is None or time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        pending += chunk
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            sys.stdout.write(line.rstrip(b"\r").decode(errors="replace") + "\n")
            sys.stdout.flush()
            halted = HALTED.search(line)
            if halted:
                os.close(fd)
                sys.exit(int(halted.group(1), 16))
    sys.stdout.write(pending.decode(errors="replace"))
    sys.exit("the port closed, or the time ran out, before the kernel halted")


if __name__ == "__main__":
    main()
