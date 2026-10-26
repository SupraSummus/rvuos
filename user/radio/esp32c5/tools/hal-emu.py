#!/usr/bin/env python3
"""Run a function of Espressif's closed Wi-Fi library under unicorn, and list its device accesses as replays list them.

The MAC's bring-up of user/radio/esp32c6/macstart.c is held, on the host, to what the libraries' own did:
on the ESP32-C6 a traced run of them on the chip gave the accesses, test/replay/config.txt there.
No such run exists on the ESP32-C5, whose radio never ran Espressif's libraries under rvuos,
so this runs the library's function itself: libpp.a linked alone with the ROM, at the chip's addresses,
each symbol it lacks a stub that returns 0 or zeroed data, the OS adapter's table a slot per function,
and every load and store of the devices, 0x60000000 up, answered from a model of the registers:
what was written last, or 0, with --set's bits always set, as a ready bit a poll waits for.
With --reads FILE, a replay's file, a read takes the value FILE's next read has where that read is of the same address,
so that a run on the ESP32-C6's library reads what the chip read.
Run on the ESP32-C6's library with --check against the chip's config.txt, it must give the chip's accesses,
the same addresses in the same order, which is how far it may be trusted; see user/radio/esp32c5/README.md.

A function the driver calls on its own, as the PHY's, is stubbed by --stub, the ROM's or the library's alike,
so that its accesses are not the function's: the PHY's leave the MAC's replay, as they left the chip's.
A stub that answers through a pointer, as phy_get_max_pwr does, writes zeros there, --zero NAME=ARG:BYTES,
or a byte of a table its first argument indexes and a zero, --table NAME=ARG:HEX,
so that the run answers as the host's replay test answers the driver's own code.

The adapter's coex_pti_get, whose answer goes through its second argument, takes its bytes from --pti,
the coexistence library's coex_pti_tab, as the driver's osi_coex_pti does.

Usage: hal-emu.py --chip esp32c5|esp32c6 --cache DIR --rom ROM.elf [--set ADDR=BITS]... [--osi SLOT=VALUE]...
                  [--stub NAME]... [--zero NAME=ARG:BYTES]... [--table NAME=ARG:HEX]... [--reads FILE] [--pti SLOT=HEX] [--lib ARCHIVE]... [--calls] [--check FILE] FUNC [ARG]...
It prints the accesses, "R4 0xaddress 0xvalue" and "W4 0xaddress 0xvalue", and "#" lines of what else it saw:
calls to stubs and to the adapter's slots, with their caller, and with --calls, every call to a named function.
With --check FILE it prints nothing but whether the addresses and their order match FILE's, and fails if not.
"""

import argparse
import bisect
import os
import re
import struct
import subprocess
import sys
import tempfile

from unicorn import UC_ARCH_RISCV, UC_HOOK_CODE, UC_MODE_RISCV32, Uc, UcError
from unicorn.riscv_const import UC_RISCV_REG_A0, UC_RISCV_REG_PC, UC_RISCV_REG_RA, UC_RISCV_REG_SP

MAGIC_RET = 0x00000F00
OSI_TABLE, OSI_CODE, OSI_SLOTS = 0x40878000, 0x2000, 256  # slot i's code at OSI_CODE + 16 i
MMIO_BASE, MMIO_SIZE = 0x60000000, 0x00100000
ROM_LD = ("rom", "rom.pp", "rom.net80211", "rom.libgcc", "rom.libc", "rom.phy", "rom.coexist", "rom.version")

# Code in flash, data in the RAM the ROM's data lies above, as the driver's are.
LINKER_SCRIPT = """SECTIONS {
  . = 0x42000000;
  .text : { *(.text .text.* .iram1 .iram1.* .wifi0iram* .wifirxiram* .wifislpiram* .wifislprxiram* .wifiextrairam*
             .wifiorslpiram*) }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.* .rodata_wlog*) }
  . = 0x40810000;
  .data : { *(.data .data.* .sdata .sdata.* .dram1 .dram1.*) }
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
}
"""

# The C library calls the libraries make that a stub returning 0 would break.
LIBC = r"""
typedef __SIZE_TYPE__ size_t;
void free(void *p) { (void)p; }
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    char *a = d; const char *b = s;
    if (a < b) { while (n--) *a++ = *b++; } else { while (n--) a[n] = b[n]; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
"""


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def link(chip, cache, func, tmp, libs=("libpp.a",)):
    """The library's FUNC linked alone, with a stub for each symbol it lacks; the image's path and the stubs' names."""
    cc = ["clang", "--target=riscv32-unknown-elf", "-march=rv32imac", "-mabi=ilp32", "-O1", "-ffreestanding",
          "-fno-builtin", "-w", "-c"]
    with open(os.path.join(tmp, "libc.c"), "w") as f:
        f.write(LIBC)
    run(cc + [os.path.join(tmp, "libc.c"), "-o", os.path.join(tmp, "libc.o")])
    with open(os.path.join(tmp, "image.ld"), "w") as f:
        f.write(LINKER_SCRIPT)
    scripts = [os.path.join(cache, f"{chip}.{s}.ld") for s in ROM_LD]
    lib = [os.path.join(cache, l) for l in libs]
    base = ["ld.lld", "-m", "elf32lriscv", "-T", os.path.join(tmp, "image.ld")]
    for s in scripts:
        if os.path.exists(s):
            base += ["-T", s]
    base += ["-u", func, "-e", func, "--no-relax"]
    _, out = run(base + ["--error-limit=0", os.path.join(tmp, "libc.o")] + lib + ["-o", "/dev/null"])
    missing = sorted(set(re.findall(r"undefined symbol: (\S+)", out)))
    _, relocs = run(["llvm-objdump", "-r"] + lib)
    called = set(re.findall(r"R_RISCV_CALL(?:_PLT)?\s+(\S+)", relocs))
    lines = []
    for s in missing:
        if s in called:
            lines.append(f'__attribute__((noinline, section(".text.stub.{s}"))) int {s}(void) '
                         '{ __asm__ volatile("nop"); return 0; }')
        else:
            lines.append(f"__attribute__((aligned(16))) unsigned char {s}[8192];")
    with open(os.path.join(tmp, "stubs.c"), "w") as f:
        f.write("\n".join(lines) + "\n")
    run(cc + [os.path.join(tmp, "stubs.c"), "-o", os.path.join(tmp, "stubs.o")])
    image = os.path.join(tmp, "image.elf")
    status, out = run(base + [os.path.join(tmp, "libc.o"), os.path.join(tmp, "stubs.o")] + lib + ["-o", image])
    if status != 0:
        sys.exit(f"hal-emu: the link failed:\n{out}")
    return image, set(missing)


def segments(path):
    d = open(path, "rb").read()
    phoff = struct.unpack_from("<I", d, 28)[0]
    phentsize, phnum = struct.unpack_from("<HH", d, 42)
    for i in range(phnum):
        t, off, va, _, fs, ms = struct.unpack_from("<6I", d, phoff + i * phentsize)
        if t == 1 and ms:
            yield va, d[off:off + fs]


def symbols(path):
    _, out = run(["llvm-nm", "-n", "--defined-only", path])
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "TtWwA":
            syms.append((int(p[0], 16), p[2]))
    return syms


def emulate(image, rom, stubbed, func, args, always, osi, calls, stub, pti, zero, table, reads):
    mu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
    mu.mem_map(0, 0x10000)
    mu.mem_map(0x40000000, 0x80000)
    mu.mem_map(0x40800000, 0x80000)
    mu.mem_map(0x42000000, 0x200000)
    for path in (rom, image):
        for va, data in segments(path):
            if 0x40000000 <= va < 0x40080000 or 0x40800000 <= va < 0x40880000 or 0x42000000 <= va < 0x42200000:
                mu.mem_write(va, data)
    syms = symbols(image) + symbols(rom)
    byaddr = {}
    for addr, name in syms:
        byaddr.setdefault(addr, name)
    order = sorted(byaddr)

    def name_of(pc):
        i = bisect.bisect_right(order, pc) - 1
        return f"{byaddr[order[i]]}+{pc - order[i]:#x}" if i >= 0 else hex(pc)

    model, log = {}, []
    recorded = [(int(m[1], 16), int(m[2], 16)) for m in
                (re.match(r"R\d (0x\w+) (0x\w+)", l) for l in (open(reads) if reads else [])) if m]
    rpos = [0]

    def mmio_read(uc, offset, size, user):
        addr = MMIO_BASE + offset
        v = model.get(addr & ~3, 0) | always.get(addr & ~3, 0)
        if rpos[0] < len(recorded) and recorded[rpos[0]][0] == addr and size == 4:
            v = recorded[rpos[0]][1]
            rpos[0] += 1
        v = (v >> (8 * (addr & 3))) & ((1 << (8 * size)) - 1)
        log.append(f"R{size} {addr:#010x} {v:#010x}")
        return v

    def mmio_write(uc, offset, size, value, user):
        addr = MMIO_BASE + offset
        sh = 8 * (addr & 3)
        mask = ((1 << (8 * size)) - 1) << sh
        model[addr & ~3] = (model.get(addr & ~3, 0) & ~mask) | ((value << sh) & mask)
        log.append(f"W{size} {addr:#010x} {value:#010x}")

    mu.mmio_map(MMIO_BASE, MMIO_SIZE, mmio_read, None, mmio_write, None)
    # The adapter's table: slot i's code at OSI_CODE + 16 i, which loads the slot's value into a0 and returns.
    for i in range(OSI_SLOTS):
        v = osi.get(i, 0)
        hi, lo = (v + 0x800) >> 12 & 0xfffff, v & 0xfff
        mu.mem_write(OSI_CODE + 16 * i, struct.pack("<IIII", 0x00000537 | (hi << 12), 0x00050513 | (lo << 20),
                                                    0x00008067, 0))  # lui a0; addi a0, a0; ret
        mu.mem_write(OSI_TABLE + 4 * i, struct.pack("<I", OSI_CODE + 16 * i))
    for addr, name in syms:
        if name == "g_osi_funcs_p":
            mu.mem_write(addr, struct.pack("<I", OSI_TABLE))
    mu.mem_write(MAGIC_RET, bytes([0x01, 0x00, 0x01, 0x00]))
    # A call through a pointer the run left 0, as an unset callback, returns 0 and is told.
    mu.mem_write(0, bytes([0x01, 0x45, 0x82, 0x80]))  # c.li a0, 0; c.ret
    stub_at = {addr: n for addr, n in syms if n in stubbed or n in stub}
    for addr, name in syms:
        if name in stub and addr in stub_at:
            mu.mem_write(addr, bytes([0x01, 0x45, 0x82, 0x80]))  # c.li a0, 0; c.ret

    def on_code(uc, pc, size, user):
        if pc == MAGIC_RET:
            uc.emu_stop()
        elif pc == 0:
            log.append(f"# call through a null pointer from {name_of(uc.reg_read(UC_RISCV_REG_RA))}")
        elif OSI_CODE <= pc < OSI_CODE + 16 * OSI_SLOTS:
            if (pc - OSI_CODE) % 16 == 0:
                slot = (pc - OSI_CODE) // 16
                log.append(f"# osi slot {slot} from {name_of(uc.reg_read(UC_RISCV_REG_RA))}")
                if pti and slot == pti[0]:
                    event = uc.reg_read(UC_RISCV_REG_A0)
                    uc.mem_write(uc.reg_read(UC_RISCV_REG_A0 + 1), bytes([pti[1][event] if event < len(pti[1]) else 0]))
        elif pc in stub_at:
            log.append(f"# stub {stub_at[pc]} from {name_of(uc.reg_read(UC_RISCV_REG_RA))}")
            if stub_at[pc] in zero:
                arg, n = zero[stub_at[pc]]
                uc.mem_write(uc.reg_read(UC_RISCV_REG_A0 + arg), bytes(n))
            if stub_at[pc] in table:
                arg, t = table[stub_at[pc]]
                i = uc.reg_read(UC_RISCV_REG_A0)
                uc.mem_write(uc.reg_read(UC_RISCV_REG_A0 + arg), bytes([t[i] if i < len(t) else 0, 0]))
        elif calls and pc in byaddr and not byaddr[pc].startswith("."):
            log.append(f"# call {byaddr[pc]}")

    mu.hook_add(UC_HOOK_CODE, on_code, begin=MAGIC_RET, end=MAGIC_RET + 4)
    mu.hook_add(UC_HOOK_CODE, on_code, begin=0, end=2)
    mu.hook_add(UC_HOOK_CODE, on_code, begin=OSI_CODE, end=OSI_CODE + 16 * OSI_SLOTS)
    for addr in stub_at:
        mu.hook_add(UC_HOOK_CODE, on_code, begin=addr, end=addr + 2)
    if calls:
        mu.hook_add(UC_HOOK_CODE, on_code, begin=0x40000000, end=0x42200000)
    start = next((addr for addr, n in syms if n == func), None)
    if start is None:
        sys.exit(f"hal-emu: no {func} in the image")
    for i, v in enumerate(args):
        mu.reg_write(UC_RISCV_REG_A0 + i, v)
    mu.reg_write(UC_RISCV_REG_SP, 0x4087F000)
    mu.reg_write(UC_RISCV_REG_RA, MAGIC_RET)
    try:
        mu.emu_start(start, MAGIC_RET, count=50_000_000)
        if mu.reg_read(UC_RISCV_REG_PC) != MAGIC_RET:
            sys.exit(f"hal-emu: {func} did not return, at {name_of(mu.reg_read(UC_RISCV_REG_PC))}")
    except UcError as e:
        sys.exit(f"hal-emu: {e} at {name_of(mu.reg_read(UC_RISCV_REG_PC))}")
    return log


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chip", required=True, choices=("esp32c5", "esp32c6"))
    ap.add_argument("--cache", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--set", action="append", default=[], metavar="ADDR=BITS")
    ap.add_argument("--osi", action="append", default=[], metavar="SLOT=VALUE")
    ap.add_argument("--stub", action="append", default=[], metavar="NAME")
    ap.add_argument("--zero", action="append", default=[], metavar="NAME=ARG:BYTES")
    ap.add_argument("--table", action="append", default=[], metavar="NAME=ARG:HEX")
    ap.add_argument("--pti", metavar="SLOT=HEX")
    ap.add_argument("--reads")
    ap.add_argument("--lib", action="append", default=[], metavar="ARCHIVE",
                    help="the cache's archives to link, libpp.a unless named")
    ap.add_argument("--calls", action="store_true")
    ap.add_argument("--check")
    ap.add_argument("func")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    always = {int(k, 0): int(v, 0) for k, v in (o.split("=") for o in a.set)}
    osi = {int(k, 0): int(v, 0) for k, v in (o.split("=") for o in a.osi)}
    with tempfile.TemporaryDirectory() as tmp:
        image, stubbed = link(a.chip, a.cache, a.func, tmp, tuple(a.lib) or ("libpp.a",))
        log = emulate(image, a.rom, stubbed, a.func, [int(x, 0) for x in a.args], always, osi, a.calls, set(a.stub),
                      (int(a.pti.split("=")[0], 0), bytes.fromhex(a.pti.split("=")[1])) if a.pti else None,
                      {z.split("=")[0]: tuple(int(x, 0) for x in z.split("=")[1].split(":")) for z in a.zero},
                      {t.split("=")[0]: (int(t.split("=")[1].split(":")[0]), bytes.fromhex(t.split("=")[1].split(":")[1]))
                       for t in a.table}, a.reads)
    if a.check:
        theirs = [tuple(l.split()[:2]) for l in open(a.check) if re.match(r"[RW]\d ", l)]
        ours = [tuple(l.split()[:2]) for l in log if re.match(r"[RW]\d ", l)]
        if theirs != ours:
            for i, (t, o) in enumerate(zip(theirs + [None] * len(ours), ours + [None] * len(theirs))):
                if t != o:
                    sys.exit(f"hal-emu: access {i + 1}: {' '.join(o) if o else 'none'}, where {a.check} has "
                             f"{' '.join(t) if t else 'none'}")
        print(f"hal-emu: {a.func}'s {len(ours)} accesses are {a.check}'s, in order")
        return
    print("\n".join(log))


if __name__ == "__main__":
    main()
