#!/usr/bin/env python3
"""Bound the kernel's stack from its image and fail when it may overflow.

The bound is the heaviest call chain, each function weighing its frame;
DESIGN.md, "Bounded stack", says why that is the whole stack.
The frames come from clang's -fstack-usage, and one of run-time size fails.
A function with no .su entry is assembly,
weighing what its own instructions take off sp.
The calls come from the disassembly.
A jal or a jump to another function's first instruction is a call,
a jump into the middle of one fails,
and a jump back to a function's own start is a loop.
A jalr or jr through a register may reach any C function whose address is taken,
formed by an lui or auipc pair or stored in the kernel's data,
so it counts as a call to each; that covers jump tables too, too many never too few.
A C function nothing calls fails,
since only assembly is entered by the hardware:
the function is dead, or reached in a way this tool does not see.

Usage: stack-depth.py [--objdump tool] kernel.elf file.su...
"""

import argparse
import bisect
import re
import struct
import subprocess
import sys

# Sections of the image the kernel does not own; see kernel/kernel.ld.S.
FOREIGN_SECTIONS = {".user_code"}

SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_PROGBITS = 1
SHT_SYMTAB = 2

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
HEADER = re.compile(r"^([0-9a-f]+) <(.+)>:$")
TARGET = re.compile(r"0x([0-9a-f]+) <[^>]+>")
BASED = re.compile(r"(-?0x[0-9a-f]+|-?\d+)\((\w+)\)")

# Instructions whose first operand is read, not written.
NO_DEST = re.compile(r"^(s[bhw]|c\.s[bhw]|b\w*|j|jr|ret|csrw|csrs|csrc|fence\S*|ecall|ebreak|"
                     r"mret|wfi|nop|unimp)$")


class Failure(Exception):
    pass


def elf_sections(image: bytes):
    """Every section as (name, type, flags, address, offset, size, link, entsize)."""
    if image[:4] != b"\x7fELF" or image[4] != 1 or image[5] != 1:
        raise Failure("not a little-endian 32-bit ELF file")
    shoff, = struct.unpack_from("<I", image, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", image, 0x2E)
    raw = [struct.unpack_from("<IIIIIIIIII", image, shoff + i * shentsize) for i in range(shnum)]
    names = raw[shstrndx][4]

    def name(off):
        return image[names + off:image.index(b"\0", names + off)].decode()

    return [(name(s[0]), s[1], s[2], s[3], s[4], s[5], s[6], s[9]) for s in raw]


def elf_symbols(image: bytes, sections) -> dict[str, int]:
    """Every named symbol's value."""
    symbols = {}
    for _, kind, _, _, off, size, link, entsize in sections:
        if kind != SHT_SYMTAB:
            continue
        strtab = sections[link][4]
        for i in range(size // entsize):
            name_off, value = struct.unpack_from("<II", image, off + i * entsize)
            if name_off:
                end = image.index(b"\0", strtab + name_off)
                symbols[image[strtab + name_off:end].decode()] = value
    return symbols


def data_words(image: bytes, sections):
    """Every aligned word of the kernel's own data."""
    for name, kind, flags, addr, off, size, _, _ in sections:
        if (kind == SHT_PROGBITS and flags & SHF_ALLOC and not flags & SHF_EXECINSTR
                and name not in FOREIGN_SECTIONS):
            start = (-addr) % 4
            for i in range(start, size - 3, 4):
                yield struct.unpack_from("<I", image, off + i)[0]


def text_end(sections) -> int:
    return max(addr + size for _, _, flags, addr, _, size, _, _ in sections
               if flags & SHF_EXECINSTR)


def read_frames(paths: list[str]) -> dict[str, int]:
    frames = {}
    for path in paths:
        try:
            lines = open(path).read().splitlines()
        except FileNotFoundError:
            raise Failure(f"no {path}; the object predates -fstack-usage, run make clean")
        for line in lines:
            where, size, kind = line.split("\t")
            name = where.rsplit(":", 1)[1]
            if kind != "static":
                raise Failure(f"{where} has a {kind} frame; the kernel's frames must be fixed")
            frames[name] = max(frames.get(name, 0), int(size))
    return frames


def disassemble(objdump: str, elf: str):
    """Every function as (address, name, [(address, mnemonic, operands)])."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    functions = []
    for line in out.splitlines():
        m = HEADER.match(line)
        if m:
            functions.append((int(m.group(1), 16), m.group(2), []))
            continue
        m = LINE.match(line)
        if m and functions:
            functions[-1][2].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return functions


def imm(text: str) -> int:
    return int(text, 0)


class Function:
    def __init__(self, addr, name):
        self.addr = addr
        self.name = name
        self.frame = 0
        self.calls = set()
        self.indirect_call = False
        self.indirect_jump = False


def analyse(functions, end, frames):
    """Frames, edges and taken addresses, from the disassembly."""
    starts = [f[0] for f in functions]
    by_addr = {}
    taken = set()

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 and addr < end else None

    for addr, name, _ in functions:
        by_addr[addr] = Function(addr, name)
    for addr, name, insns in functions:
        fn = by_addr[addr]
        measured = 0
        high = {}  # register -> the value an lui or auipc left in it
        for pc, mnem, ops in insns:
            args = [a.strip() for a in ops.split(",")] if ops else []
            m = TARGET.search(ops)
            if m:
                target = int(m.group(1), 16)
                home = owner(target)
                if home is None:
                    raise Failure(f"{name} at {pc:#x} jumps out of the kernel's code")
                if target == addr and mnem == "jal":
                    fn.calls.add(target)
                elif home != addr:
                    if target != home:
                        raise Failure(f"{name} at {pc:#x} jumps into the middle of "
                                      f"{by_addr[home].name}")
                    fn.calls.add(target)
            if mnem in ("lui", "auipc"):
                value = (imm(args[1]) << 12) & 0xFFFFFFFF
                if mnem == "auipc":
                    value = (pc + value) & 0xFFFFFFFF
                high[args[0]] = value
                continue
            if mnem in ("addi", "mv") and args[1] in high:
                offset = imm(args[2]) if mnem == "addi" else 0
                taken.add((high[args[1]] + offset) & 0xFFFFFFFF)
            elif mnem == "addi" and args[0] == "sp" and args[1] == "sp" and imm(args[2]) < 0:
                measured -= imm(args[2])
            if mnem in ("jalr", "jr"):
                b = BASED.search(ops)
                base, offset = (b.group(2), imm(b.group(1))) if b else (args[-1], 0)
                if base in high:
                    target = (high[base] + offset) & 0xFFFFFFFF
                    if owner(target) != target:
                        raise Failure(f"{name} at {pc:#x} jumps to {target:#x}, "
                                      "which starts no function")
                    fn.calls.add(target)
                elif mnem == "jalr":
                    fn.indirect_call = True
                else:
                    fn.indirect_jump = True
            if args and not NO_DEST.match(mnem):
                high.pop(args[0], None)
        fn.frame = frames.get(name, measured)
    return by_addr, taken


def heaviest(graph, entries):
    """The heaviest chain from an entry, as its weight and its names."""
    depth = {}
    via = {}
    active = []

    def visit(addr):
        if addr in active:
            cycle = active[active.index(addr):] + [addr]
            raise Failure("the kernel recurses: " + " > ".join(graph[a].name for a in cycle))
        if addr in depth:
            return
        active.append(addr)
        depth[addr], via[addr] = graph[addr].frame, None
        for callee in sorted(graph[addr].calls):
            visit(callee)
            if graph[addr].frame + depth[callee] > depth[addr]:
                depth[addr], via[addr] = graph[addr].frame + depth[callee], callee
        active.pop()

    for addr in graph:
        visit(addr)
    top = max(entries, key=lambda a: depth[a])
    names = []
    a = top
    while a is not None:
        names.append(f"{graph[a].name} {graph[a].frame}")
        a = via[a]
    return depth[top], " > ".join(names)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("elf")
    ap.add_argument("su", nargs="+")
    args = ap.parse_args()

    try:
        image = open(args.elf, "rb").read()
        sections = elf_sections(image)
        symbols = elf_symbols(image, sections)
        size = symbols["__kernel_stack_top"] - symbols["__kernel_stack_bottom"]
        frames = read_frames(args.su)
        functions = disassemble(args.objdump, args.elf)
        graph, taken = analyse(functions, text_end(sections), frames)

        taken.update(data_words(image, sections))
        pointed = sorted(a for a in taken & graph.keys() if graph[a].name in frames)
        for fn in graph.values():
            if fn.indirect_call:
                fn.calls.update(pointed)
            elif fn.indirect_jump:
                # A jr to itself is a tail call, which gives its frame back first.
                fn.calls.update(p for p in pointed if p != fn.addr)

        called = set().union(*(fn.calls for fn in graph.values()))
        entries = [a for a in graph if a not in called]
        for a in entries:
            if graph[a].name in frames:
                raise Failure(f"nothing calls {graph[a].name}; "
                              "it is dead code, or called in a way this tool does not see")

        worst, route = heaviest(graph, entries)
        if worst > size:
            raise Failure(f"the kernel may take {worst} bytes of stack, "
                          f"and KERNEL_STACK_SIZE in kernel/kernel.ld.S gives {size}: {route}")
        print(f"kernel stack: at most {worst} of {size} bytes, through {route}")
        return 0
    except Failure as e:
        print(f"stack-depth: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
