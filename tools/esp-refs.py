#!/usr/bin/env python3
"""The symbols of Espressif's closed libraries the ESP32-C6 Wi-Fi driver's own objects reach directly.

A library is its archive and, if it has one, the ROM's linker script that places the rest of it,
as esp32c6.rom.pp.ld places pp's functions the ROM holds.
A symbol is the library's if the archive defines it or the script places it,
and the driver reaches it if one of the objects given refers to it and none of them defines it.
For each library this prints how many it reaches, then each with the objects that refer to it,
so that the count can be held against the driver's from one commit to the next; see TODO.md.

Usage: tools/esp-refs.py [--nm NM] --lib NAME=ARCHIVE[:ROM.ld]... OBJECT...
"""

import argparse
import os
import re
import subprocess
import sys

ROM_PLACED = re.compile(r"^\s*(?:PROVIDE\s*\(\s*)?([A-Za-z_][A-Za-z0-9_.$]*)\s*=\s*0x[0-9a-fA-F]+", re.M)


def symbols(nm, path, defined):
    """The global symbols an object or an archive defines, or those it refers to and leaves undefined."""
    flags = ["--defined-only", "--extern-only"] if defined else ["--undefined-only"]
    out = subprocess.run([nm, *flags, "--format=just-symbols", path], capture_output=True, text=True, check=True)
    return {s for s in out.stdout.split() if not s.endswith(":")}


def rom_placed(path):
    """The symbols a ROM linker script places, but for those it leaves commented out."""
    with open(path) as f:
        text = re.sub(r"/\*.*?\*/", "", f.read(), flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return set(ROM_PLACED.findall(text))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", default="llvm-nm")
    parser.add_argument("--lib", action="append", default=[], metavar="NAME=ARCHIVE[:ROM.ld]")
    parser.add_argument("objects", nargs="+")
    args = parser.parse_args()

    defined, wanted = set(), {}
    for obj in args.objects:
        defined |= symbols(args.nm, obj, True)
        for s in symbols(args.nm, obj, False):
            wanted.setdefault(s, set()).add(os.path.basename(obj))
    reached = {s: objs for s, objs in wanted.items() if s not in defined}

    for spec in args.lib:
        name, _, paths = spec.partition("=")
        archive, _, rom = paths.partition(":")
        theirs = symbols(args.nm, archive, True) | (rom_placed(rom) if rom else set())
        mine = sorted(s for s in reached if s in theirs)
        print(f"{name}: {len(mine)}")
        for s in mine:
            print(f"  {s}: {' '.join(sorted(reached[s]))}")


if __name__ == "__main__":
    sys.exit(main())
