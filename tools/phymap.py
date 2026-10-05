#!/usr/bin/env python3
"""Map the device registers the PHY harness of user/phyblob/, or the Wi-Fi driver, reaches, from its code and the ROM's.

libphy.a calls much of the PHY in the ROM through a table of function pointers, g_phyFuns_instance,
whose first content lies in the ROM's ELF and some of whose slots the library fills with its own fixes.
This follows every call from phyblob_main, direct, through the ROM's jump table and through those slots,
and reports each device register a load or store names by a constant address, by block,
then those that no frame in the board's DEVICE_RANGE_LIST covers, or covers without the right to write,
which the PHY tracer has to log and feign; see user/phytrace.h.

Constants are followed through lui, addi, li, mv and auipc in each function's order, past branches,
so an address computed from a table or in a loop is not seen,
nor a call through a pointer other than the table's slots; the report counts those calls.
--i2c lists the calls to the ROM's analog I2C functions with their constant arguments,
--functions every function reached and the blocks it touches.

With --all it follows from every function of the image instead,
for an image that reaches much of its code only through pointers,
as the Wi-Fi driver of user/wifi/esp32c6/ reaches the libraries' tasks and their interrupt's handler;
the linker kept only functions something names, so this is what the image may reach.
Code past the end of its function's symbol is reported apart, as the name plus an offset:
such is a library function weakened so that one of the driver's takes its place,
whose code stays behind in a section it shares with others.

Usage: tools/phymap.py [--objdump llvm-objdump] [--i2c] [--functions] [--all] image.elf rom.elf board.h
"""

import argparse
import collections
import re
import struct
import subprocess
import sys

# The ESP32-C6's peripherals from ESP-IDF's soc/esp32c6 reg_base.h,
# and the modem's blocks that no header names, as the code that drives them shows them.
BLOCKS = [
    (0x60000000, 0x1000, "UART0"), (0x60001000, 0x1000, "UART1"), (0x60002000, 0x1000, "SPI0"),
    (0x60003000, 0x1000, "SPI1"), (0x60004000, 0x1000, "I2C_EXT"), (0x60005000, 0x1000, "UHCI0"),
    (0x60006000, 0x1000, "RMT"), (0x60007000, 0x1000, "LEDC"), (0x60008000, 0x1000, "TIMERGROUP0"),
    (0x60009000, 0x1000, "TIMERGROUP1"), (0x6000A000, 0x1000, "SYSTIMER"), (0x6000B000, 0x1000, "TWAI0"),
    (0x6000C000, 0x1000, "I2S"), (0x6000D000, 0x1000, "TWAI1"), (0x6000E000, 0x1000, "APB_SARADC"),
    (0x6000F000, 0x1000, "USB_SERIAL_JTAG"), (0x60010000, 0x1000, "INTMTX"), (0x60011000, 0x1000, "ATOMIC"),
    (0x60012000, 0x1000, "PCNT"), (0x60013000, 0x1000, "SOC_ETM"), (0x60014000, 0x1000, "MCPWM"),
    (0x60015000, 0x1000, "PARL_IO"), (0x60016000, 0x1000, "HINF"), (0x60017000, 0x1000, "SLC"),
    (0x60018000, 0x1000, "SLCHOST"), (0x60019000, 0x1000, "PVT_MONITOR"), (0x60080000, 0x1000, "GDMA"),
    (0x60081000, 0x1000, "SPI2"), (0x60088000, 0x1000, "AES"), (0x60089000, 0x1000, "SHA"),
    (0x6008A000, 0x1000, "RSA"), (0x6008B000, 0x1000, "ECC_MULT"), (0x6008C000, 0x1000, "DS"),
    (0x6008D000, 0x1000, "HMAC"), (0x60090000, 0x1000, "IO_MUX"), (0x60091000, 0x1000, "GPIO"),
    (0x60092000, 0x1000, "MEM_MONITOR"), (0x60093000, 0x1000, "PAU"), (0x60095000, 0x1000, "HP_SYSTEM"),
    (0x60096000, 0x1000, "PCR"), (0x60098000, 0x1000, "TEE"), (0x60099000, 0x800, "HP_APM"),
    (0x60099800, 0x800, "LP_APM0"), (0x6009F000, 0x1000, "MISC"),
    (0x600A0000, 0x1000, "FE"),                # the RF front end: libphy.a's fe_* and the ROM's
    (0x600A2000, 0x1000, "BT_BB"),             # the Bluetooth baseband, ESP-IDF's btbb_retention_reg.h
    (0x600A3000, 0x1000, "IEEE802154"),        # ESP-IDF's ieee802154_reg.h; a bus master
    (0x600A4000, 0x2000, "WIFI_MAC"),          # the ROM's hal_mac_*; a bus master
    (0x600A7000, 0x2000, "WIFI_BB"),           # the Wi-Fi baseband: bb_reg_init, agc_reg_init and the like
    (0x600A9800, 0x400, "MODEM_SYSCON"),
    (0x600AD000, 0x1000, "WIFI_MAC_TSF"),      # the MAC's timers and power states, the ROM's tsf_hal_* and pwr_hal_*
    (0x600AF000, 0x800, "MODEM_LPCON"), (0x600AF800, 0x800, "I2C_ANA_MST"),
    (0x600B0000, 0x400, "PMU"), (0x600B0400, 0x400, "LP_CLKRST"), (0x600B0800, 0x400, "EFUSE"),
    (0x600B0C00, 0x400, "LP_TIMER"), (0x600B1000, 0x400, "LP_AON"), (0x600B1400, 0x400, "LP_UART"),
    (0x600B1800, 0x400, "LP_I2C"), (0x600B1C00, 0x400, "LP_WDT"), (0x600B2000, 0x400, "LP_IO"),
    (0x600B2400, 0x400, "LP_I2C_ANA_MST"), (0x600B2800, 0x400, "LPPERI"), (0x600B2C00, 0x400, "LP_ANALOG_PERI"),
    (0x600B3400, 0x400, "LP_TEE"), (0x600B3800, 0x400, "LP_APM"), (0x600B3C00, 0x400, "OPT_DEBUG"),
]
DEVICES = (0x60000000, 0x60100000)

# The ROM's analog I2C functions: (block, host, register[, msb, lsb][, data]).
I2C_CALLS = {
    "rom_i2c_readReg": 3, "rom_i2c_readReg_Mask": 5, "rom_chip_i2c_readReg": 3,
    "rom_i2c_writeReg": 4, "rom_i2c_writeReg_Mask": 6, "rom_chip_i2c_writeReg": 4,
}

CALLER_SAVED = {"ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"}
NO_DEST = {"ret", "fence", "fence.i", "ebreak", "ecall", "nop", "mret", "wfi", "sw", "sh", "sb", "j", "jr"}
LOADS = {"lw": 4, "lh": 2, "lhu": 2, "lb": 1, "lbu": 1}
STORES = {"sw": 4, "sh": 2, "sb": 1}
FUNC = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$")
MEM = re.compile(r"(-?0x[0-9a-f]+|-?\d+)?\((\w+)\)")
TARGET = re.compile(r"0x([0-9a-f]+)")

TABLE = "table"  # the value of g_phyFuns: the address of the ROM's table


def block_of(addr):
    for base, size, name in BLOCKS:
        if base <= addr < base + size:
            return name
    return f"page {addr & ~0xfff:#x}"


class Elf:
    """An ELF32's sections and symbols, as far as this needs them."""

    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:5] != b"\x7fELF\x01":
            sys.exit(f"phymap: {path} is no ELF32")
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2e)
        headers = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
        names = headers[shstrndx]

        def string(table, offset):
            start = table[4] + offset
            return data[start:data.index(b"\0", start)].decode()

        self.sections = {string(names, h[0]): h for h in headers}
        self.data = data
        self.symbols = {}
        self.sizes = {}  # of each function, by its address
        for h in headers:
            if h[1] != 2:  # SHT_SYMTAB
                continue
            strtab = headers[h[6]]
            for off in range(h[4], h[4] + h[5], 16):
                name, value, size, info = struct.unpack_from("<IIIB", data, off)
                if name:
                    self.symbols.setdefault(string(strtab, name), value)
                if info & 0xf == 2 and size:  # STT_FUNC
                    self.sizes[value] = size

    def words(self, section):
        h = self.sections[section]
        return h[3], struct.unpack_from(f"<{h[5] // 4}I", self.data, h[4])


def disassemble(objdump, path):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=True).stdout
    funcs, body = {}, None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            body = funcs.setdefault(int(m.group(1), 16), (m.group(2), []))[1]
            continue
        m = INSN.match(line)
        if m and body is not None:
            body.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs


class Map:
    def __init__(self, objdump, image, rom, everything):
        self.funcs = {}
        h, r = Elf(image), Elf(rom)
        for origin, path, elf in (("rom", rom, r), ("lib", image, h)):
            for addr, (name, body) in disassemble(objdump, path).items():
                end = addr + elf.sizes.get(addr, 1 << 32)
                rest = [i for i in body if i[0] >= end]
                body = [i for i in body if i[0] < end]
                if body:
                    self.funcs[addr] = (name, body, origin)
                if rest:
                    self.funcs[rest[0][0]] = (f"{name}+{rest[0][0] - addr:#x}", rest, origin)
        self.table_vars = {h.symbols["g_phyFuns"], r.symbols["rom_phyFuns"]}
        self.table_base = r.symbols["g_phyFuns_instance"]
        self.table_slots = (r.symbols["rom_phyFuns"] - self.table_base) // 4
        base, words = r.words(".data_phyrom")
        first = (self.table_base - base) // 4
        self.slots = {i: {words[first + i]} - {0} for i in range(self.table_slots)}
        self.own_slots = set()
        self.roots = ([a for a, f in self.funcs.items() if f[2] == "lib"] if everything
                      else [h.symbols["phyblob_main"]])

    def name(self, addr):
        return self.funcs[addr][0] if addr in self.funcs else f"{addr:#x}"

    def through_jump_table(self, addr):
        """A ROM interface entry, __call_<name>, is a j to the function."""
        f = self.funcs.get(addr)
        if f and f[0].startswith("__call_") and f[1][0][1] == "j":
            return int(TARGET.search(f[1][0][2]).group(1), 16)
        return addr

    def slot_of(self, base, offset):
        """The table slot a load or store at base + offset names, if any."""
        if base == TABLE:
            index = offset // 4
        elif isinstance(base, int) and 0 <= base + offset - self.table_base < 4 * self.table_slots:
            index = (base + offset - self.table_base) // 4
        else:
            return None
        return index if 0 <= index < self.table_slots else None

    def analyse(self, addr):
        name, body, _ = self.funcs[addr]
        regs = {"zero": 0}
        result = {"calls": set(), "dev": [], "i2c": [], "lost": 0}
        first, last = body[0][0], body[-1][0]

        def value(r):
            return regs.get(r)

        def assign(r, v):
            if r != "zero":
                if v is None:
                    regs.pop(r, None)
                else:
                    regs[r] = v

        def call(target, at):
            if isinstance(target, int):
                targets = {self.through_jump_table(target)}
            elif isinstance(target, tuple):
                targets = self.slots[target[1]]
            else:
                targets = set()
                result["lost"] += 1
            for t in targets:
                result["calls"].add(t)
                n = self.name(t)
                if n in I2C_CALLS:
                    result["i2c"].append((at, n, [value(f"a{i}") for i in range(I2C_CALLS[n])]))
            returns_table = any(self.name(t) == "phy_get_romfuncs" for t in targets)
            for r in CALLER_SAVED:
                regs.pop(r, None)
            if returns_table:
                regs["a0"] = TABLE

        for at, op, args in body:
            o = [a.strip() for a in args.split(",")] if args else []
            if op == "lui":
                assign(o[0], (int(o[1], 0) << 12) & 0xffffffff)
            elif op == "auipc":
                assign(o[0], (at + (int(o[1], 0) << 12)) & 0xffffffff)
            elif op == "li":
                assign(o[0], int(o[1], 0) & 0xffffffff)
            elif op == "mv":
                assign(o[0], value(o[1]))
            elif op == "addi":
                v = value(o[1])
                assign(o[0], (v + int(o[2], 0)) & 0xffffffff if isinstance(v, int) else None)
            elif op in LOADS or op in STORES:
                m = MEM.search(args)
                base, offset = value(m.group(2)), int(m.group(1) or "0", 0)
                address = (base + offset) & 0xffffffff if isinstance(base, int) else None
                if address is not None and DEVICES[0] <= address < DEVICES[1]:
                    result["dev"].append((at, "W" if op in STORES else "R", address))
                slot = self.slot_of(base, offset)
                if op in STORES:
                    if slot is not None and isinstance(value(o[0]), int):
                        self.slots[slot].add(value(o[0]))
                        self.own_slots.add(slot)
                elif address in self.table_vars:
                    assign(o[0], TABLE)
                elif slot is not None:
                    assign(o[0], ("slot", slot))
                else:
                    assign(o[0], None)
            elif op in ("jal", "call"):
                call(int(TARGET.search(o[-1]).group(1), 16), at)
            elif op == "jalr":
                m = MEM.search(args)
                if m:
                    base, offset = value(m.group(2)), int(m.group(1) or "0", 0)
                else:
                    base, offset = value(o[0] if len(o) == 1 else o[1]), int(o[2], 0) if len(o) == 3 else 0
                call((base + offset) & 0xffffffff if isinstance(base, int) else base if offset == 0 else None, at)
            elif op in ("j", "tail"):
                target = int(TARGET.search(o[0]).group(1), 16)
                if not first <= target <= last:
                    call(target, at)
            elif op == "jr":
                if o[0] != "ra":
                    call(value(o[0]), at)
            elif op.startswith("b") or op in NO_DEST:
                pass
            elif o:
                assign(o[0], None)
        return result

    def reach(self):
        """Every function reached from the roots, until the slots the library fills stop growing."""
        while True:
            filled = {k: set(v) for k, v in self.slots.items()}
            self.found, todo = {}, list(self.roots)
            while todo:
                a = todo.pop()
                if a in self.found or a not in self.funcs:
                    continue
                self.found[a] = self.analyse(a)
                todo.extend(self.found[a]["calls"])
            if filled == self.slots:
                return


def frames(board):
    """DEVICE_RANGE_LIST of a board.h: (base, size, writable) for each frame."""
    text = open(board).read()
    m = re.search(r"#define DEVICE_RANGE_LIST((?:.*\\\n)*.*)", text)
    if not m:
        sys.exit(f"phymap: {board} defines no DEVICE_RANGE_LIST")
    return [(int(b, 16), int(s, 16), "RIGHT_W" in r)
            for b, s, r in re.findall(r"\{\s*U32\((0x[0-9A-Fa-f]+)\),\s*U32\((0x[0-9A-Fa-f]+)\),\s*([A-Z_| ]+)\}",
                                      m.group(1))]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--i2c", action="store_true", help="list the analog I2C calls")
    ap.add_argument("--functions", action="store_true", help="list every function reached")
    ap.add_argument("--all", action="store_true", help="follow from every function of the image")
    ap.add_argument("image")
    ap.add_argument("rom")
    ap.add_argument("board")
    args = ap.parse_args()

    m = Map(args.objdump, args.image, args.rom, args.all)
    m.reach()
    found = sorted(m.found)
    lib = sum(m.funcs[a][2] == "lib" for a in found)
    origin, image = (("every function of the image", "the image") if args.all
                     else ("phyblob_main", "the harness and libphy.a"))
    print(f"phymap: {len(found)} functions reached from {origin}, {lib} of {image}, {len(found) - lib} of the ROM;")
    print(f"  libphy.a fills {len(m.own_slots)} of the ROM's {m.table_slots} slots with its own functions, "
          f"and {sum(f['lost'] for f in m.found.values())} calls go through pointers this does not follow")

    regs = collections.defaultdict(lambda: collections.defaultdict(set))
    for a in found:
        for _, rw, address in m.found[a]["dev"]:
            regs[address][rw].add(m.name(a))
    blocks = collections.defaultdict(list)
    for address in regs:
        blocks[block_of(address)].append(address)

    print("\nBlocks, with the registers named in each and the functions that name them:")
    for block, addresses in sorted(blocks.items(), key=lambda b: min(b[1])):
        fns = set().union(*(regs[a]["R"] | regs[a]["W"] for a in addresses))
        rw = "".join(k for k in "RW" if any(regs[a][k] for a in addresses))
        listed = ", ".join(sorted(fns)[:6]) + (", ..." if len(fns) > 6 else "")
        print(f"  {block:14} {min(addresses):#x}-{max(addresses):#x} {len(addresses):3} {rw:2} "
              f"{len(fns):3} functions: {listed}")

    granted = frames(args.board)
    print(f"\nOutside the {len(granted)} frames of {args.board}, or written where a frame lets no program write:")
    outside = 0
    for address in sorted(regs):
        cover = [w for b, s, w in granted if b <= address < b + s]
        if cover and (cover[0] or not regs[address]["W"]):
            continue
        outside += 1
        fns = sorted(regs[address]["R"] | regs[address]["W"])
        rw = "".join(k for k in "RW" if regs[address][k])
        why = "read only" if cover else block_of(address)
        print(f"  {address:#x} {rw:2} {why:14} {', '.join(fns)}")
    if not outside:
        print("  none")

    if args.i2c:
        print("\nAnalog I2C calls, (block, host, register[, msb, lsb][, data]), ? where not a constant:")
        for a in found:
            for at, fn, values in m.found[a]["i2c"]:
                shown = " ".join(f"{v:#x}" if isinstance(v, int) else "?" for v in values)
                print(f"  {m.name(a):34} {at:#x} {fn:22} {shown}")

    if args.functions:
        print("\nFunctions reached, with where they lie and the blocks they name:")
        for a in found:
            name, body, origin = m.funcs[a]
            touched = sorted({block_of(address) for _, _, address in m.found[a]["dev"]})
            print(f"  {a:#010x} {origin:3} {name:34} {', '.join(touched)}")


if __name__ == "__main__":
    main()
