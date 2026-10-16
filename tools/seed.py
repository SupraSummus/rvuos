#!/usr/bin/env python3
"""
A seed from text, and text from a seed.

Usage: tools/seed.py TEXT SEED, TEXT being - for standard input,
       tools/seed.py --text SEED, which prints SEED as text that reads back into the same bytes,
       tools/seed.py --build DIR OUT, which writes OUT anew with a seed of every text in DIR, by the same name.

The seeds under tests/seeds are such text, and `make host-test` builds them into build/seeds.

A line is `[n] actor A OP slot S a1 X a2 Y a3 Z`, and a4 to a6 where they are not zero, as `build/host/fuzz --print` writes it:
the operation by its name in include/rvuos/abi.h or include/rvuos/replay.h, or `op N`,
and each number decimal or hexadecimal, or a name of either header whose value is a number,
or `SLOT_IN(table, index)`, or such terms joined by |, + or -, as `RIGHT_R|RIGHT_W` or `REPLAY_CAP_THIRD`.
A field left out is zero.
What follows a # is a comment, and a line with nothing else is skipped.
A record is struct replay_record of include/rvuos/abi.h, little-endian, twenty-eight bytes.

--text names what host/ops.h says each field holds:
a slot by its BOOT_CAP_ or REPLAY_CAP_ name, one the setup leaves empty as REPLAY_CAP_FREE+n,
the first past the table as REPLAY_TABLE_SLOTS, a type by its CAP_ name, rights by their RIGHT_ names,
so that a seed kept as text means the same after the ABI renumbers any of them.
--build refuses a seed written otherwise, comments aside.
"""

import re
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADERS = [ROOT / "include/rvuos/abi.h", ROOT / "include/rvuos/replay.h"]
OPS_H = ROOT / "host/ops.h"
DEFINE = re.compile(r"^#define\s+([A-Z_][A-Z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$")
TERM = re.compile(r"^(0x[0-9a-fA-F]+|[0-9]+)u?$")
SLOT_IN = re.compile(r"^SLOT_IN\s*\(([^,]+),([^)]+)\)$")
RECORD = struct.Struct("<BBHIIIIII")


def defines():
    """
    Every object-like #define of the two headers, by name, as its text,
    and BOOT_DEVICES, which a board's devices.h gives: none, since the seeds are QEMU's.
    """
    found = {"BOOT_DEVICES": "0"}
    for header in HEADERS:
        for line in header.read_text().splitlines():
            m = DEFINE.match(line)
            if m and "(" not in m.group(1):
                found[m.group(1)] = m.group(2)
    return found


def value(text, names, depth=0):
    """The number text names: terms joined by |, + or -, each a number, a name of the headers or SLOT_IN(t, i)."""
    if depth > 8:
        raise ValueError(f"{text}: names nest too deep")
    text = text.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1]
    m = SLOT_IN.match(text)
    if m:
        shift = value(names["SLOT_TABLE_SHIFT"], names, depth + 1)
        return value(m.group(1), names, depth + 1) << shift | value(m.group(2), names, depth + 1)
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


def operations(names):
    """Every operation's code, by name."""
    return {n: value(names[n], names) for n in names if n.startswith(("OP_", "REPLAY_OP_")) and n != "OP_COUNT"}


def record(line, names, ops):
    """The twenty-eight bytes of one line."""
    words = re.sub(r"SLOT_IN\s*\(\s*([^,\s]+)\s*,\s*([^)\s]+)\s*\)", r"SLOT_IN(\1,\2)", line).split()
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
    unknown = set(fields) - {"slot", "a1", "a2", "a3", "a4", "a5", "a6"}
    if unknown or len(rest) % 2:
        raise ValueError(f"expected slot and a1 to a6, found {' '.join(rest)}")
    args = [value(fields.get(k, "0"), names) for k in ("slot", "a1", "a2", "a3", "a4", "a5", "a6")]
    return RECORD.pack(op, actor, *args)


def kinds():
    """What host/ops.h says each operation's a1 to a3 hold, by the operation's name: (the invoked type, [kinds])."""
    table = {}
    for m in re.finditer(r"(?:OP|DESCRIBE)\((OP_\w+),\s*(\w+),(?:\s*\w+,)?\s*(ARG_\w+),\s*(ARG_\w+),\s*(ARG_\w+)\)",
                         OPS_H.read_text()):
        table[m.group(1)] = (m.group(2), [m.group(3), m.group(4), m.group(5)])
    for m in re.finditer(r'\{\s*"(REPLAY_OP_\w+)",\s*(\w+),\s*\w+,\s*\{\s*(ARG_\w+),\s*(ARG_\w+),\s*(ARG_\w+)\s*\}',
                         OPS_H.read_text()):
        table[m.group(1)] = (m.group(2), [m.group(3), m.group(4), m.group(5)])
    missing = set(operations(defines())) - set(table)
    if missing:
        sys.exit(f"{OPS_H}: no fields found for {', '.join(sorted(missing))}")
    return table


SLOT_KINDS = {"ARG_DST", "ARG_DST_INVOKED", "ARG_SLOT_INVOKED", "ARG_ANY", "ARG_FRAME", "ARG_POOL",
              "ARG_NOTIFICATION", "ARG_THREAD", "ARG_CAPTABLE"}


class Namer:
    """Names for the values of each kind of field, from the headers."""

    def __init__(self, names):
        num = {n: value(t, names) for n, t in names.items() if TERM.match(t.strip()) or n.startswith(("BOOT_CAP_", "REPLAY_CAP_"))}
        self.shift = num["SLOT_TABLE_SHIFT"]
        # The replay's names first, since BOOT_CAP_DEVICES and BOOT_CAP_COUNT are where its slots begin.
        self.slots = {}
        for prefix in ("REPLAY_CAP_", "BOOT_CAP_"):
            for n, v in num.items():
                if n.startswith(prefix) and n not in ("BOOT_CAP_COUNT", "BOOT_CAP_NULL", "BOOT_CAP_DEVICES"):
                    self.slots.setdefault(v, n)
        self.types = {v: n for n, v in num.items()
                      if n.startswith("CAP_") and n != "CAP_NONE" and re.fullmatch(r"CAP_[A-Z_]+", n)}
        self.free = num["REPLAY_CAP_FREE"]
        # One past the table, as a seed names a slot it means to be out of range; a larger number is a larger table's.
        self.past = value(names["REPLAY_TABLE_SLOTS"], names)
        self.slots[self.past] = "REPLAY_TABLE_SLOTS"
        self.rights = [(num[n], n) for n in ("RIGHT_R", "RIGHT_W", "RIGHT_X")]
        self.all_rights = num["RIGHT_ALL"]
        self.num = num

    @staticmethod
    def number(v):
        return str(v) if v < 1024 else f"0x{v:x}"

    def slot(self, v):
        table, index = v >> self.shift, v & ((1 << self.shift) - 1)
        name = self.slots.get(index)
        if name is None:
            free = index - self.free
            name = f"REPLAY_CAP_FREE+{free}" if 0 <= free < self.past - self.free else str(index)
        return f"SLOT_IN({table}, {name})" if table else name

    def name(self, kind, v, op, a1):
        if kind in SLOT_KINDS or (kind == "ARG_ALLOC" and a1 in (self.num["CAP_PROCESS"], self.num["CAP_THREAD"])):
            return self.slot(v)
        if kind == "ARG_TYPE" and v in self.types:
            return self.types[v]
        if kind == "ARG_RIGHTS" and v and not v & ~self.all_rights:
            if v == self.all_rights:
                return "RIGHT_ALL"
            return "|".join(n for bit, n in self.rights if v & bit)
        if kind == "ARG_FLAGS" and v == self.num["IRQ_SET_PERIOD"]:
            return "IRQ_SET_PERIOD"
        if kind == "ARG_END" and op == "OP_ROUTE_END" and v in (self.num["ROUTE_EVENT"], self.num["ROUTE_TASK"]):
            return "ROUTE_EVENT" if v == self.num["ROUTE_EVENT"] else "ROUTE_TASK"
        if kind == "ARG_BITS" and v == self.num["NOTIFY_ALL_BITS"]:
            return "NOTIFY_ALL_BITS"
        if kind == "ARG_OFFSET" and op == "OP_THREAD_READ_REG" and v == self.num["THREAD_REG_PC"]:
            return "THREAD_REG_PC"
        if kind in ("ARG_WORD", "ARG_ADDRESS", "ARG_BITS", "ARG_NONE"):
            return f"0x{v:x}" if v else "0"
        return self.number(v)


def text(data, names):
    """The lines of a seed's records, each field by what it holds."""
    ops = {v: n for n, v in operations(names).items()}
    table = kinds()
    namer = Namer(names)
    lines = []
    for off in range(0, len(data) - len(data) % RECORD.size, RECORD.size):
        op, actor, slot, *args = RECORD.unpack_from(data, off)
        name = ops.get(op)
        if name not in table:
            words = [f"op {op}", f"slot {slot}"] + [f"a{i + 1} 0x{a:x}" for i, a in enumerate(args[:3])]
        else:
            cap, arg = table[name]
            words = [name]
            if slot or cap != "CAP_NONE":
                words.append(f"slot {namer.slot(slot) if cap != 'CAP_NONE' else namer.number(slot)}")
            for i, (kind, a) in enumerate(zip(arg, args)):
                if a or kind != "ARG_NONE":
                    words.append(f"a{i + 1} {namer.name(kind, a, name, args[0])}")
        # a4 to a6, which no operation reads, only where they are not zero.
        words += [f"a{i + 4} 0x{a:x}" for i, a in enumerate(args[3:]) if a]
        lines.append(f"actor {actor} " + " ".join(words))
    return "".join(line + "\n" for line in lines)


def seed(source, where, names, ops):
    """The bytes of a seed's text; where names it in an error."""
    out = b""
    for n, line in enumerate(source.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            out += record(line, names, ops)
        except (ValueError, IndexError, struct.error) as e:
            sys.exit(f"{where}:{n}: {e}")
    return out


def canon(source, data, names):
    """Fail unless source reads as --text writes data, comments aside, so that no seed calls a slot by its number."""
    written = [(n, " ".join(line.split("#", 1)[0].split())) for n, line in enumerate(source.read_text().splitlines(), 1)]
    for (n, line), want in zip([w for w in written if w[1]], text(data, names).splitlines()):
        if line != want:
            sys.exit(f"{source}:{n}: tools/seed.py --text writes this record as\n{want}")


def main():
    names = defines()
    if len(sys.argv) == 3 and sys.argv[1] == "--text":
        sys.stdout.write(text(Path(sys.argv[2]).read_bytes(), names))
        return
    ops = operations(names)
    if len(sys.argv) == 4 and sys.argv[1] == "--build":
        # Anew, so that a seed renamed or removed leaves nothing behind to replay,
        # and whole or not at all, so that a seed that does not build leaves no directory make takes as built.
        out = Path(sys.argv[3])
        new = out.with_name(out.name + ".new")
        shutil.rmtree(new, ignore_errors=True)
        new.mkdir(parents=True)
        for source in sorted(Path(sys.argv[2]).iterdir()):
            data = seed(source.read_text(), source, names, ops)
            canon(source, data, names)
            (new / source.name).write_bytes(data)
        shutil.rmtree(out, ignore_errors=True)
        new.rename(out)
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    source = sys.stdin.read() if sys.argv[1] == "-" else Path(sys.argv[1]).read_text()
    Path(sys.argv[2]).write_bytes(seed(source, sys.argv[1], names, ops))


if __name__ == "__main__":
    main()
