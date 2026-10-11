#!/usr/bin/env python3
"""
A seed from text: the lines `build/host/fuzz --print` writes, read back into the records the harnesses take.

Usage: tools/seed.py TEXT SEED, TEXT being - for standard input.

A line is `[n] actor A OP slot S a1 X a2 Y a3 Z`, as --print writes it:
the operation by its name in include/rvuos/abi.h or include/rvuos/replay.h, or `op N`,
and each number decimal or hexadecimal, or a name of either header whose value is a number,
or such names and numbers joined by |, + or -, as `RIGHT_R|RIGHT_W` or `REPLAY_CAP_THIRD`.
What follows a # is a comment, and a line with nothing else is skipped.
A record is struct replay_record of include/rvuos/abi.h, little-endian, sixteen bytes.
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADERS = [ROOT / "include/rvuos/abi.h", ROOT / "include/rvuos/replay.h"]
DEFINE = re.compile(r"^#define\s+([A-Z_][A-Z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$")
TERM = re.compile(r"^(0x[0-9a-fA-F]+|[0-9]+)u?$")


def defines():
    """Every object-like #define of the two headers, by name, as its text."""
    found = {}
    for header in HEADERS:
        for line in header.read_text().splitlines():
            m = DEFINE.match(line)
            if m and "(" not in m.group(1):
                found[m.group(1)] = m.group(2)
    return found


def value(text, names, depth=0):
    """The number text names: terms joined by |, + or -, each a number or a name of the headers."""
    if depth > 8:
        raise ValueError(f"{text}: names nest too deep")
    text = text.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    total, op = 0, "+"
    for part in re.split(r"\s*([|+-])\s*", text):
        if part in "|+-" and part != "":
            op = part
            continue
        m = TERM.match(part)
        if m:
            v = int(m.group(1), 0)
        elif part in names:
            v = value(names[part], names, depth + 1)
        else:
            raise ValueError(f"{part}: not a number, nor a name of {', '.join(h.name for h in HEADERS)}")
        total = total | v if op == "|" else total + v if op == "+" else total - v
    return total


def record(line, names, ops):
    """The sixteen bytes of one line."""
    words = line.split()
    if words and words[0].isdigit():
        words = words[1:]
    if len(words) < 3 or words[0] != "actor":
        raise ValueError("a record starts with `actor A`")
    actor = value(words[1], names)
    rest = words[2:]
    if rest[0] == "op":
        op, rest = value(rest[1], names), rest[2:]
    elif rest[0] in ops:
        op, rest = ops[rest[0]], rest[1:]
    else:
        raise ValueError(f"{rest[0]}: not an operation")
    fields = dict(zip(rest[0::2], rest[1::2]))
    unknown = set(fields) - {"slot", "a1", "a2", "a3"}
    if unknown or len(rest) % 2:
        raise ValueError(f"expected slot, a1, a2 and a3, found {' '.join(rest)}")
    args = [value(fields.get(k, "0"), names) for k in ("slot", "a1", "a2", "a3")]
    return struct.pack("<BBHIII", op, actor, *args)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    names = defines()
    ops = {n: value(names[n], names) for n in names if n.startswith(("OP_", "REPLAY_OP_")) and n != "OP_COUNT"}
    text = sys.stdin.read() if sys.argv[1] == "-" else Path(sys.argv[1]).read_text()
    out = b""
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            out += record(line, names, ops)
        except (ValueError, IndexError, struct.error) as e:
            sys.exit(f"{sys.argv[1]}:{n}: {e}")
    Path(sys.argv[2]).write_bytes(out)


if __name__ == "__main__":
    main()
