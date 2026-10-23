#!/usr/bin/env python3
"""The CYW43439's WLAN code in Ghidra: its RAM firmware and its ROM as one program, and what that code does to the PHY and the radio.

Usage: user/wifi/tools/cyw43-code.py import [--rom FILE]
       user/wifi/tools/cyw43-code.py regs OUT
       user/wifi/tools/cyw43-code.py decompile ADDR...
       user/wifi/tools/cyw43-code.py refs ADDR...

import builds a Ghidra project in build/cyw43/ghidra: the RAM firmware, build/cyw43/43439A0.bin, at 0,
where its vector table says it runs, and the ROM, --rom, at 0x800000, where the code's pointers to its own strings point;
the ROM is read off the chip with user/wifi/tools/cyw43-regmap.py dump 0x1e000000 0xa0000 FILE.
Ghidra analyses both, then names each function after the one identifier string only it refers to, its __func__ as a rule.
It takes some minutes.
regs lists every call of the PHY's and the radio's access functions whose register is a constant, with its arguments
as Ghidra's decompiler gives them, and decodes the register lists the code plays through 0x86bfa8, into OUT as JSON;
see user/wifi/hw/phy.md.
decompile prints the C of the function holding each ADDR, refs every reference to each ADDR.

GHIDRA names Ghidra's analyzeHeadless, /opt/ghidra/support/analyzeHeadless by default.
The scripts are in ghidra/ beside this file.
"""

import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS, "../../.."))
SCRIPTS = os.path.join(TOOLS, "ghidra")
PROJECT = os.path.join(ROOT, "build/cyw43/ghidra")
NAME = "wlan"
RAM_IMAGE = os.path.join(ROOT, "build/cyw43/43439A0.bin")
ROM_IMAGE = os.path.join(ROOT, "build/cyw43/regmap/wlan-1e000000.bin")
ROM_BASE = 0x800000

# The access functions of the ROM's PHY code, see user/wifi/hw/phy.md.
HELPERS = {0x86BF20: "phy and", 0x86BF38: "phy mod", 0x86BF5C: "phy or", 0x86BF78: "phy read", 0x86BF9C: "phy write",
           0x86BFA8: "list", 0x86BEC0: "radio and", 0x86BEDC: "radio mod", 0x86BF00: "radio or",
           0x86C064: "radio read", 0x86E2F8: "radio write"}
LIST_OPS = {0: ("mod", 2), 1: ("and", 1), 2: ("write", 1), 3: ("or", 1)}


def ghidra(*args):
    headless = os.environ.get("GHIDRA", "/opt/ghidra/support/analyzeHeadless")
    p = subprocess.run([headless, PROJECT, NAME, *args], capture_output=True, text=True)
    if p.returncode != 0 or "ERROR" in p.stdout:
        sys.stderr.write(p.stdout[-4000:] + p.stderr[-2000:])
        sys.exit("cyw43-code: Ghidra failed")


def process(*scripts):
    """Runs post-scripts over the project's program, changing nothing but what a script saves."""
    ghidra("-process", os.path.basename(RAM_IMAGE), "-noanalysis", "-scriptPath", SCRIPTS, *scripts)


def scratch():
    f = tempfile.NamedTemporaryFile("r", suffix=".txt", delete=False)
    f.close()
    return f.name


def cmd_import(args):
    os.makedirs(PROJECT, exist_ok=True)
    ghidra("-import", RAM_IMAGE, "-processor", "ARM:LE:32:Cortex", "-loader", "BinaryLoader", "-loader-baseAddr", "0",
           "-scriptPath", SCRIPTS, "-preScript", "AddBlock.java", "rom", os.path.abspath(args.rom), hex(ROM_BASE),
           "-postScript", "ReadOnly.java", "-overwrite")
    names = scratch()
    process("-postScript", "NameFromStrings.java", names)
    with open(names) as f:
        print("cyw43-code:", f.read().splitlines()[-1].lstrip("/ "))
    os.unlink(names)


def image_bytes(rom, ram, addr, n):
    if ROM_BASE <= addr and addr + n <= ROM_BASE + len(rom):
        return rom[addr - ROM_BASE:addr - ROM_BASE + n]
    if addr + n <= len(ram):
        return ram[addr:addr + n]
    return None


def decode_list(data):
    out, hw, i = [], struct.unpack(f"<{len(data) // 2}H", data), 0
    while i < len(hw):
        w = hw[i]
        op, k = LIST_OPS[w >> 13 & 3]
        out.append({"dev": "radio" if w & 0x8000 else "phy", "op": op, "reg": w & 0x1FFF, "args": list(hw[i + 1:i + 1 + k])})
        i += 1 + k
    return out


def cmd_regs(args):
    calls = scratch()
    process("-postScript", "RegCalls.java", calls, *(f"{a:#x}={n.replace(' ', '_')}" for a, n in HELPERS.items()))
    with open(RAM_IMAGE, "rb") as f:
        ram = f.read()
    with open(args.rom, "rb") as f:
        rom = f.read()
    regs = {"phy": collections.defaultdict(list), "radio": collections.defaultdict(list)}
    lists = []
    with open(calls) as f:
        for line in f:
            if line.startswith("//"):
                continue
            at, fn, helper, text = line.rstrip("\n").split(" ", 3)
            a = [x.strip() for x in text.split(",")]
            if helper == "list":
                m = re.fullmatch(r"DAT_([0-9a-f]{8})", a[1]) if len(a) > 2 else None
                if not m or not re.fullmatch(r"0x[0-9a-f]+|\d+", a[2]):
                    continue
                word = image_bytes(rom, ram, int(m.group(1), 16), 4)
                where = struct.unpack("<I", word)[0] if word else None
                data = image_bytes(rom, ram, where, 2 * int(a[2], 0)) if where is not None else None
                lists.append({"fn": fn, "at": at, "list": where, "writes": decode_list(data) if data else None})
            elif len(a) > 1 and re.fullmatch(r"0x[0-9a-f]+|\d+", a[1]):
                dev, op = helper.split("_")
                regs[dev][f"{int(a[1], 0):#05x}"].append({"fn": fn, "at": at, "op": op, "args": a[2:]})
    os.unlink(calls)
    for dev in regs:
        written = sum(1 for v in regs[dev].values() if any(c["op"] != "read" for c in v))
        print(f"cyw43-code: {dev}: {len(regs[dev])} registers named, {written} written")
    print(f"cyw43-code: {len(lists)} lists")
    with open(args.out, "w") as f:
        json.dump({"phy": dict(sorted(regs["phy"].items())), "radio": dict(sorted(regs["radio"].items())), "lists": lists},
                  f, indent=0)


def cmd_print(script):
    def run(args):
        out = scratch()
        process("-postScript", "ReadOnly.java", "-postScript", script, out, *args.addr)
        with open(out) as f:
            sys.stdout.write(f.read())
        os.unlink(out)
    return run


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("import")
    i.add_argument("--rom", default=ROM_IMAGE)
    r = sub.add_parser("regs")
    r.add_argument("out")
    r.add_argument("--rom", default=ROM_IMAGE)
    for name in ("decompile", "refs"):
        sub.add_parser(name).add_argument("addr", nargs="+")
    args = p.parse_args()
    {"import": cmd_import, "regs": cmd_regs, "decompile": cmd_print("Decompile.java"),
     "refs": cmd_print("RefsTo.java")}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
