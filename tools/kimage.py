"""The kernel image as the tools that check it read it.

tools/stack-depth.py and tools/loop-bounds.py share this:
the ELF's sections and symbols, the disassembly, and the call graph,
in which a call through a register reaches every C function whose address is taken.
The instructions read here are RISC-V's; tools/kthumb.py reads ARMv7-M's Thumb-2,
and arch says which of the two an image holds.
"""

import bisect
import re
import struct
import subprocess

# Sections of the image the kernel does not own; see kernel/kernel.ld.S.
FOREIGN_SECTIONS = {".user_code"}

SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_PROGBITS = 1
SHT_SYMTAB = 2
STT_FUNC = 2

EM_ARM = 40
EM_RISCV = 243

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
# A Thumb instruction's line: the address, then the mnemonic and operands after tabs,
# where data in the code, a literal or a table, has its bytes before the tab and a directive after it.
LINE_ARM = re.compile(r"^\s*([0-9a-f]+):\s*\t(\S+)\s*(.*)$")
HEADER = re.compile(r"^([0-9a-f]+) <(.+)>:$")
TARGET = re.compile(r"0x([0-9a-f]+) <[^>]+>")
BASED = re.compile(r"(-?0x[0-9a-f]+|-?\d+)\((\w+)\)")

# Instructions whose first operand is read, not written.
NO_DEST = re.compile(r"^(s[bhw]|c\.s[bhw]|b\w*|j|jr|ret|csrw|csrs|csrc|fence\S*|ecall|ebreak|"
                     r"mret|wfi|nop|unimp)$")


class Failure(Exception):
    pass


def arch(image: bytes) -> str:
    """The instruction set the image holds: "riscv" or "arm"."""
    machine, = struct.unpack_from("<H", image, 0x12)
    if machine == EM_RISCV:
        return "riscv"
    if machine == EM_ARM:
        return "arm"
    raise Failure(f"the image is for machine {machine}, which no tool here reads")


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


def elf_sizes(image: bytes, sections) -> dict[int, int]:
    """The size of every function symbol, by its address, bit 0 of a Thumb address taken off."""
    sizes = {}
    for _, kind, _, _, off, size, _, entsize in sections:
        if kind != SHT_SYMTAB:
            continue
        for i in range(size // entsize):
            _, value, length, info = struct.unpack_from("<IIIB", image, off + i * entsize)
            if info & 0xF == STT_FUNC and length:
                sizes[value & ~1] = max(sizes.get(value & ~1, 0), length)
    return sizes


def literal_ranges(image: bytes, sections) -> list[tuple[int, int]]:
    """Where ARM code holds data, as (start, end):
    from each $d mapping symbol to the next mapping symbol of its section, or the section's end.
    The ARM ELF ABI has the assembler mark every switch between instructions and data in code,
    the literals clang places after a function and a .word in assembly alike."""
    marks = {}
    for _, kind, _, _, off, size, link, entsize in sections:
        if kind != SHT_SYMTAB:
            continue
        strtab = sections[link][4]
        for i in range(size // entsize):
            name_off, value, _, _, _, shndx = struct.unpack_from("<IIIBBH", image, off + i * entsize)
            end = image.index(b"\0", strtab + name_off)
            name = image[strtab + name_off:end].decode()
            kind_of = name[:2]
            if kind_of in ("$d", "$t", "$a") and (len(name) == 2 or name[2] == "."):
                marks.setdefault(shndx, []).append((value, kind_of))
    ranges = []
    for shndx, found in marks.items():
        found.sort()
        section_end = sections[shndx][3] + sections[shndx][5]
        for i, (at, kind_of) in enumerate(found):
            if kind_of == "$d":
                ranges.append((at, found[i + 1][0] if i + 1 < len(found) else section_end))
    return sorted(ranges)


def in_ranges(ranges, start, end) -> bool:
    """Whether [start, end) lies within one of the sorted, disjoint ranges."""
    i = bisect.bisect_right(ranges, (start, 0xFFFFFFFF)) - 1
    return i >= 0 and ranges[i][0] <= start and end <= ranges[i][1]


def data_words(image: bytes, sections):
    """Every aligned word of the kernel's own data.
    On ARM the code holds data too, the literals a function loads, and a code address carries bit 0,
    so there every word of the code's literals counts as well, and each word with bit 0 taken off beside it;
    an instruction is not one, though two halfwords of Thumb may read as a function's address."""
    thumb = arch(image) == "arm"
    literals = literal_ranges(image, sections) if thumb else []
    for name, kind, flags, addr, off, size, _, _ in sections:
        if (kind == SHT_PROGBITS and flags & SHF_ALLOC and (thumb or not flags & SHF_EXECINSTR)
                and name not in FOREIGN_SECTIONS):
            start = (-addr) % 4
            for i in range(start, size - 3, 4):
                if flags & SHF_EXECINSTR and not in_ranges(literals, addr + i, addr + i + 4):
                    continue
                word = struct.unpack_from("<I", image, off + i)[0]
                yield word
                if thumb and word & 1:
                    yield word & ~1


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


def disassemble(objdump: str, elf: str, thumb: bool = False):
    """Every function as (address, name, [(address, mnemonic, operands)]).
    In Thumb code the data between instructions, literals and tables, is left out,
    and so is the vector table, which the disassembler shows as bytes."""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    functions = []
    line_re = LINE_ARM if thumb else LINE
    for line in out.splitlines():
        m = HEADER.match(line)
        if m:
            functions.append((int(m.group(1), 16), m.group(2), []))
            continue
        m = line_re.match(line)
        if m and functions and not (thumb and m.group(2).startswith(".")):
            functions[-1][2].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return functions


def imm(text: str) -> int:
    return int(text, 0)


class Block:
    """A basic block of a function, for tools/loop-bounds.py."""

    def __init__(self, start):
        self.start = start
        self.end = start
        self.succ = set()
        self.calls = []  # (pc, callee)
        self.leaves = False  # returns to the caller, itself or by a tail call
        self.tails = []  # (pc, function) of the jumps to functions that return for it
        self.indirect = False  # calls or jumps through a register to another function


class Function:
    def __init__(self, addr, name):
        self.addr = addr
        self.name = name
        self.frame = 0
        self.calls = set()
        self.indirect_call = False
        self.indirect_jump = False


def analyse(functions, end, frames, image, sections):
    """Frames, edges and taken addresses, from the disassembly."""
    starts = [f[0] for f in functions]
    by_addr = {}
    taken = set()

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 and addr < end else None

    for addr, name, _ in functions:
        by_addr[addr] = Function(addr, name)
    if arch(image) == "arm":
        import kthumb  # which builds on this module, so it comes in only here
        return by_addr, kthumb.analyse(functions, end, frames, image, sections, owner, by_addr)
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


def load(objdump: str, elf: str, su: list[str]):
    """The image, its sections and symbols, the frames, the functions, the call graph,
    and the C functions a call through a register may reach."""
    image = open(elf, "rb").read()
    sections = elf_sections(image)
    symbols = elf_symbols(image, sections)
    frames = read_frames(su)
    thumb = arch(image) == "arm"
    functions = disassemble(objdump, elf, thumb)
    if thumb:
        # The linker pads between functions with bytes that read as a branch; a function is its symbol's size.
        sizes = elf_sizes(image, sections)
        functions = [(a, n, [i for i in insns if a not in sizes or i[0] < a + sizes[a]])
                     for a, n, insns in functions]
    graph, taken = analyse(functions, text_end(sections), frames, image, sections)

    taken.update(data_words(image, sections))
    pointed = sorted(a for a in taken & graph.keys() if graph[a].name in frames)
    for fn in graph.values():
        if fn.indirect_call:
            fn.calls.update(pointed)
        elif fn.indirect_jump:
            # A jr to itself is a tail call, which gives its frame back first.
            fn.calls.update(p for p in pointed if p != fn.addr)
    return image, sections, symbols, frames, functions, graph, pointed
