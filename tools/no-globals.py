#!/usr/bin/env python3
"""Fail if an object file keeps a global: any data or bss section that is not empty.

A process other than the root task runs the root task's code but not with its data region,
so a global it touched would fault; this finds one at the link instead.
A const table lies in .rodata, with the code, and is fine.

Usage: tools/no-globals.py object.o...
"""

import re
import sys

from kimage import elf_sections

DATA = re.compile(r"^\.(s?data|s?bss|tdata|tbss)(\.|$)")


def main():
    bad = []
    for path in sys.argv[1:]:
        image = open(path, "rb").read()
        for name, _, _, _, _, size, _, _ in elf_sections(image):
            if DATA.match(name) and size:
                bad.append(f"{path}: {name}, {size} bytes")
    for line in bad:
        print(f"no-globals: {line}", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
