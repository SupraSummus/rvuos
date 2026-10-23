#!/usr/bin/env python3
"""Boot the CYW43439's Bluetooth core from its reset off the board, and list every register access it makes.

Usage: user/radio/cyw43439/tools/cyw43-rehost.py [--log FILE] [--max N] [--reg ADDR=VALUE]... [--auto] [--until ADDR] [--seconds S]
                             [--board [--read-rom] [--clear ADDR=BITS]... [--local ADDR]... [--w1c ADDR]...
                              [--kick ADDR=VALUE]... [--poke ADDR=VALUE]... [--hci OPCODE[:PARAMETERS]]...
                              [--record FILE] [--idle S]]
                             [--show ADDR]... [--show-irq N]... [--watch ADDR]... [--trace N] [--profile] [--ram FILE]

The core is a Cortex-M3.
Its memory here is the ROM, everything below 0xd0000, out of build/cyw43/bt-rom-orig.bin, read with --read-rom,
or else out of build/cyw43/bt-mem.bin, a dump of a running core whose patched words the remapper has swapped in,
and RAM of zeros above it.
The vendor patch's records, out of build/cyw43/43439A0_btfw.bin, go in as the host writes them, see user/wifi/bt.c,
when the ROM first reaches its idle loop, IDLE_HOOK, as on the chip, where the ROM is up before the host writes them;
the ROM's own start clears the RAM.
The patch then has the remapper put its words over the ROM's, REMAP, and the run swaps them in as it does.
user/wifi/tools/cyw43-blob.py --bt fetches the patch; the dump was read off the board through the backplane's window.
The files stay in build/, out of the tree.

Everything from 0x220000 up is the register space, the modem's windows and the Cortex-M's own:
a read gives what was last written there, or 0, or what --reg says,
and every access goes to the log with the instruction that made it,
each read and each write at one instruction only the first few times.
Two registers move by themselves, as COUNTERS says.
A read one instruction repeats is a poll on hardware that is not there:
with --auto it is answered with each of TRIES in turn, every POLL reads, and the answer goes to the log,
so a loop waiting for a bit goes on, though an answer it takes is a guess, not the chip's.

With --board the registers are the chip's own, on a Pico 2 W in its BOOTSEL mode, through the Wi-Fi system's bridge,
user/wifi/bridge.h and user/wifi/tools/cyw43bridge.py: each access below BOARD_TOP is the same access to the backplane's window
onto the core, a word at a time, so a poll waits on what the chip says and nothing is guessed.
A read comes after one of SPARE, a word the run knows, so that a slow register's answer of the read before is known
and the read made again; see read() in user/radio/cyw43439/tools/cyw43park.py.
The core's own processor must not run meanwhile, so it is parked first:
the vendor patch goes in as the host writes it, but its code a slide of `cpsid i` into SNIFFER,
so the ROM checks the patch's signature, installs it and, at its first call into the patch, sleeps there for good,
its interrupts masked; see park() in user/radio/cyw43439/tools/cyw43park.py.
Parked so, the processor looks about every millisecond, on its own SysTick,
and SNIFFER writes the lines it sees pending into a mailbox in the core's RAM, SNIFFED;
between two looks it sleeps in `wfi` while the emulated core waits in one, ASLEEP, and spins otherwise,
as the chip's power manager watches the processor's sleep;
every answer of the bridge brings what it collected, and the run pends each line the emulated core has enabled,
so the chip's interrupts reach the code that answers them.
The emulated core is far slower than the chip, a forwarded access taking about half a millisecond,
so lines pend in rounds IRQ_GAP accesses apart, as a slow processor misses ticks,
and a line taken waits out its round until every other raised has been taken once,
so that one the chip raises faster than the emulation can answer keeps none of the others out.
What it would have answered meanwhile waits in the device's status, and the threads get their turn.
Writes go on the line of the next read, queued, and cost no wait of their own; see user/wifi/tools/cyw43bridge.py.
The run then boots the core from its reset in the emulator, again, over the chip the ROM has set up once.
Only the Cortex-M's own registers, at 0xe000e000, stay the emulator's.

Unicorn leaves a Cortex-M's exceptions to its user, so this takes them:
SVC, and PendSV and SysTick as the code pends them in the ICSR,
enter through the vector table with the frame pushed, and a branch to EXC_RETURN pops it.

Where the emulation's slowness or its place behind the backplane makes the chip answer as it never would the core,
the run departs from the firmware, each departure but the last named on the command line, see BT.md:
--clear keeps bits of a register clear on the chip, with a note in the log,
as bit 17 of 0x321520, which cuts the host off the core's registers;
--local keeps a register in the emulator, as the watchdog's control, 0x329008, whose second the emulation never keeps;
--w1c has a status cleared by writing ones leave those ones out of its reads until the handler that cleared them returns,
or out of its next read if no handler did, as the link's, 0x314004, whose events latch again faster than the emulation reads them:
a handler's pass takes the chip microseconds, and tens of the link's slots emulated;
and two reads of one register by instructions in a row are one instant, as a counter read twice to see it settle.
--poke writes words of the core's memory once it is ready,
0x201f60 among them, which keeps the controller from sleeping.

Once the core is ready, the run stands where user/wifi/bt.c stands, the host:
--hci sends each command through the ring in the WLAN core's RAM, rings the core's doorbell, and logs its events;
the run ends once the last command has its answer, or has gone without one HCI_WAIT accesses,
or if the core is not ready within READY_WITHIN, or a register stops answering.
--show, --show-irq, --watch and --trace log a register's every access, a line's every interrupt,
who touches a word of memory, and the blocks run once the patch is in, each with how far the run had come;
--profile counts the blocks run once the first command is sent, and where a pending interrupt waited.
The run logs when the parked processor's ticks slow, which leaves its sniffed lines unseen.

It ends after --max instructions, at --until, at a fault, or after --seconds, and says where,
then lists the register windows the run touched and the polls it answered.
Without --board nothing runs on the chip; with it, the firmware's accesses do, and its carrier only within the band.
"""

import argparse
import collections
import os
import signal
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(1, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../wifi/tools"))  # cyw43bridge, beside the bridge it is the host side of
from cyw43bridge import Bridge, BridgeError  # noqa: E402, the tools' own directory first
from cyw43park import (ASLEEP, BOARD_WINDOW, BT_CTRL, BT_FW_RDY, PATCH, SILENT, REREAD, SNIFFED, SPARE,  # noqa: E402
                       host_read, park, patch_records, read)

from unicorn import (UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_READ, UC_HOOK_MEM_UNMAPPED,
                     UC_HOOK_MEM_WRITE, UC_MEM_WRITE, UC_MODE_MCLASS, UC_MODE_THUMB, UC_PROT_ALL, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_CONTROL, UC_ARM_REG_LR, UC_ARM_REG_MSP, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK,
                               UC_ARM_REG_PSP, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_XPSR)

IMAGE = "build/cyw43/bt-mem.bin"
# The ROM as it is before a patch remaps any of it, read through the bridge with --read-rom; bt-mem.bin's ROM was read
# with the patch in place, and nineteen of its words are the patch's branches.
ROM_IMAGE = "build/cyw43/bt-rom-orig.bin"
ROM_TOP = 0xD0000
RAM = 0x200000
IMAGE_TOP = 0x220000
REG_PAGE = 0x1000
IDLE_HOOK = 0x9834  # called on each turn of the ROM's idle loop; runs the patch's trigger once written
LOGGED = 2  # accesses at one instruction to one register that go to the log
POLL = 64  # reads at one instruction of one register before --auto answers them
# What --auto answers a poll with, in turn: the value last read with every bit flipped (None), then these.
TRIES = (None, 0xFFFFFFFF, *range(16), *(1 << b for b in range(4, 32)))
# Registers that move by themselves, and the step each read moves them by:
# the firmware's microsecond timer counts down, and 0x318084, read after 0x318088, counts up, a Bluetooth clock.
COUNTERS = {0x32A004: -1, 0x318084: 1}
BOARD_TOP = 0x1000000  # the core's addresses the backplane's window reaches, at BOARD_WINDOW and up
IRQ_POLL = 32  # forwarded accesses between two looks at SNIFFED
IRQ_GAP = 256  # forwarded accesses after a line is taken before the next is
NVIC_ISER, NVIC_ICER, NVIC_ISPR, NVIC_ICPR = 0xE000E100, 0xE000E180, 0xE000E200, 0xE000E280
NVIC_IPR = 0xE000E400  # a byte a line, its priority
SHPR = {11: 0xE000ED1F, 14: 0xE000ED22, 15: 0xE000ED23}  # SVC's, PendSV's and SysTick's priority bytes
# The host's side of HCI, as user/wifi/bt.c has it: the ring's address, and the host's control word with its bits.
BT_RING_BASE, BT_HOST_CTRL = 0x18000D68, 0x18000D6C
BT_DATA_VALID, BT_WAKE, BT_SW_RDY = 1 << 1, 1 << 17, 1 << 24
RING, H2B_IN, B2H_IN, B2H_OUT = 0x1000, 0x2000, 0x2008, 0x200C  # each way's bytes, and the pointers past both
HCI_WAIT = 20000  # forwarded accesses a command waits for its event before the next goes anyway
READY_WITHIN = 200000  # forwarded accesses the core has to get ready in; it took about 60000
HCI_LINGER = 5000  # forwarded accesses the run goes on after the last command's answer
SVC, PENDSV, SYSTICK = 11, 14, 15
ICSR = 0xE000ED04
# The patch's remapper: slot n's word at REMAP + 4n holds the ROM's word address it covers, and bit n of the words
# from REMAP_ON turns it on; a fetch from a covered word then gives the patch's word, a branch into its code.
# The emulator stands in for it, with the words bt-mem.bin, read with the patch in place, holds there.
REMAP, REMAP_ON = 0x310000, 0x310204
EXC_RETURN = 0xFFFFFFE0  # and up: a branch there ends an exception
# The exception numbers unicorn's interrupt hook gives, QEMU's: a supervisor call, and a return from an exception.
EXCP_SWI, EXCP_EXCEPTION_EXIT = 2, 8
FRAME = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12, UC_ARM_REG_LR)


CTRL_EVERY = 4096  # forwarded accesses between two reads of the host's BT_CTRL
TICKS_LEAST = 200  # ticks a second below which the parked processor's clock has been slowed; it ticks 1200
HOST_EVERY = 256  # forwarded accesses between two looks of the host's at the ring


class Host:
    """The host's side of HCI, as user/wifi/bt.c is: the ring in the WLAN core's RAM, and the core's doorbell."""

    def __init__(self, core, commands):
        self.core, self.commands = core, list(commands)
        self.ring = None  # where the ring lies on the backplane, once the core is ready
        self.h2b = self.b2h = self.host = 0
        self.waiting = None  # the forwarded access by which the command sent last has had its time
        self.broken = False  # the ring read as no core writes it
        self.sent = 0  # HCI commands sent
        self.last = None  # the command sent last
        self.end_at = None  # once every command is answered: when the run ends, a little later for what comes after

    def word(self, at):
        return host_read(self.core.board, self.ring + at)

    def set_word(self, at, value):
        self.core.board.write(self.ring + at, value)

    def control(self, bits):
        self.host = bits
        self.core.board.write(BT_HOST_CTRL, bits)
        self.core.log(f"host  BT_HOST_CTRL = {bits:#x} [{self.core.total} {self.core.waits}]")

    def step(self):
        core = self.core
        if self.ring is None:
            if not core.ctrl & BT_FW_RDY:
                if core.total > READY_WITHIN:
                    core.ask("unready", core.total, core.pc())
                return
            self.ring = host_read(core.board, BT_RING_BASE)
            core.log(f"host  ready, the ring at {self.ring:#x} [{core.total} {core.waits}]")
            for addr, value in core.args.poke:
                core.uc.mem_write(addr, struct.pack("<I", value))
                core.log(f"poke  {addr:06x} = {value:#x}")
            for off in range(H2B_IN, B2H_OUT + 4, 4):
                self.set_word(off, 0)
            self.control(self.host | BT_SW_RDY)
            self.control(self.host ^ BT_DATA_VALID)
            self.waiting = core.total + HCI_WAIT  # the core's first event, before any command
            return
        while not self.broken and self.word(B2H_IN) != self.b2h:
            into, head = self.word(B2H_IN), self.word(RING + self.b2h)
            n, kind = head & 0xFFFFFF, head >> 24
            if into % 4 or into >= RING or n > 0x400:  # not what the core writes: the ring is not read further
                core.log(f"host  the ring makes no sense: in {into:#x}, a head {head:#x} [{core.total} {core.waits}]")
                self.broken = True
                break
            words = (n + 3) // 4
            data = b"".join(struct.pack("<I", self.word(RING + (self.b2h + 4 + 4 * i) % RING)) for i in range(words))
            self.b2h = (self.b2h + 4 + 4 * words) % RING
            self.set_word(B2H_OUT, self.b2h)
            core.log(f"hci   event type {kind} len {n}: {data[:n].hex()} [{core.total} {core.waits}] at {time.time():.3f}")
            if core.record:
                print(f"# hci event {kind} {data[:n].hex()} {core.total}", file=core.record)
            core.events.append((kind, data[:n]))
            if kind == 4 and n >= 1 and data[0] in (0x0E, 0x0F):  # a command's completion or status
                self.waiting = None
        if not self.commands and self.sent:  # the run ends once the last command has had its answer, or its time
            if self.waiting is not None and core.total >= self.waiting:
                core.ask("stall", self.last, core.pc())
            elif self.waiting is None and self.end_at is None:
                self.end_at = core.total + HCI_LINGER
            elif self.end_at is not None and core.total >= self.end_at:
                core.ask("done", self.last, core.pc())
            return
        if self.waiting is not None and core.total < self.waiting or not self.commands:
            return
        op, par = self.commands.pop(0)
        self.last = op
        if not self.host & BT_WAKE:
            self.control(self.host | BT_WAKE)
        packet = struct.pack("<IHB", (3 + len(par)) | 1 << 24, op, len(par)) + par
        packet += bytes(-len(packet) % 4)
        for i in range(0, len(packet), 4):
            self.set_word((self.h2b + i) % RING, struct.unpack_from("<I", packet, i)[0])
        self.h2b = (self.h2b + len(packet)) % RING
        self.set_word(H2B_IN, self.h2b)
        core.log(f"hci   command {op:04x} {par.hex()} [{core.total} {core.waits}] at {time.time():.3f}")
        if core.record:
            print(f"# hci command {op:04x} {par.hex()} {core.total}", file=core.record)
        self.sent += 1
        self.control(self.host ^ BT_DATA_VALID)
        self.waiting = core.total + HCI_WAIT


class Core:
    def __init__(self, args, log, board=None):
        self.args, self.log, self.board = args, log, board
        self.regs, self.reads, self.writes, self.answers = {}, collections.Counter(), collections.Counter(), {}
        self.windows = collections.Counter()
        self.irqs = collections.Counter()
        self.waits = 0
        self.pending, self.active, self.asked = set(), [], []
        self.enabled, self.forwarded = 0, 0  # the NVIC's enabled lines, a bit each; accesses since SNIFFED was read
        self.total = 0  # forwarded accesses in all
        self.raised, self.again = set(), 0  # lines the chip raised, not yet taken; when the next may be taken
        self.served = set()  # the lines pended this round
        self.entries = collections.Counter()
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        image = open(IMAGE, "rb").read()
        uc.mem_map(0, IMAGE_TOP, UC_PROT_ALL)
        uc.mem_write(0, image[:ROM_TOP])
        if os.path.exists(ROM_IMAGE):
            uc.mem_write(0, open(ROM_IMAGE, "rb").read()[:ROM_TOP])
        self.remapped = {at: image[at:at + 4] for at in range(0, ROM_TOP, 4)
                         if image[at:at + 4] != bytes(uc.mem_read(at, 4))}
        self.original = {at: bytes(uc.mem_read(at, 4)) for at in self.remapped}
        self.slots, self.slots_on = {}, 0
        self.patched = False
        self.ctrl, self.next_ctrl = 0, 0  # the host's BT_CTRL as last read, and when it is read next
        self.next_host = 0  # when the host looks at the ring next
        self.ticks, self.slow = None, False  # the parked processor's ticks when last read, and the time; and if too few
        self.events = []  # the HCI events the core has sent the host
        self.host = Host(self, args.hci) if board else None
        self.traced = 0  # blocks logged since the patch was written
        uc.mem_map(EXC_RETURN & ~(REG_PAGE - 1), REG_PAGE, UC_PROT_ALL)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        uc.hook_add(UC_HOOK_INTR, self.on_intr)
        uc.hook_add(UC_HOOK_CODE, self.on_return, begin=EXC_RETURN, end=0xFFFFFFFF)
        uc.hook_add(UC_HOOK_BLOCK, self.on_block)
        uc.hook_add(UC_HOOK_CODE, self.write_patch, begin=IDLE_HOOK, end=IDLE_HOOK)
        self.record = open(args.record, "w") if args.record else None  # every forwarded access, with --record
        self.cleared = {}  # each --w1c status: the ones written into it, by how many exceptions were active then
        self.last_read = None  # the last forwarded read: word, instruction, number, value, and if it was the one before
        self.instants = 0  # reads answered as the read before them, at the same instant
        self.held = collections.Counter()  # blocks a pending exception waited at, with --profile
        self.profile = collections.Counter()  # blocks run since the first HCI command, with --profile
        self.recent = collections.deque(maxlen=CTRL_EVERY)  # the last forwarded accesses, logged if something stops
        self.watched = collections.Counter()
        for at in args.watch:
            uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.on_watch, begin=at, end=at + 3)
        if args.until is not None:
            uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: self.ask("until", a, a), begin=args.until, end=args.until)
        self.sp, self.reset = struct.unpack("<II", image[:8])

    def on_watch(self, uc, access, addr, size, value, data):
        kind = "write" if access == UC_MEM_WRITE else "read"
        if access != UC_MEM_WRITE:
            value = int.from_bytes(uc.mem_read(addr, size), "little")
        key = (kind, addr, self.pc())
        self.watched[key] += 1
        if self.watched[key] <= LOGGED:
            self.log(f"watch {kind} {addr:06x}/{size} = {value:#x} at {self.pc():06x}")

    def when(self):
        """Where a shown line falls in the run: the forwarded accesses so far, and the waits."""
        return f" [{self.total} {self.waits}]" if self.args.show or self.args.show_irq else ""

    def pc(self):
        return self.uc.reg_read(UC_ARM_REG_PC)

    # The register space. Its pages are MMIO, mapped as first touched: a memory hook instead (Unicorn 2) drops the
    # Thumb IT state of the instruction after a hooked access, and a conditional branch there then faults.

    def unmapped(self, uc, access, addr, size, value, data):
        if addr < IMAGE_TOP:
            return False
        base = addr & ~(REG_PAGE - 1)
        try:
            uc.mmio_map(base, REG_PAGE, self.reg_read, base, self.reg_write, base)
        except UcError:
            return False
        return True

    def reg_read(self, uc, off, size, base):
        addr = base + off
        word, shift, mask = addr & ~3, 8 * (addr & 3), (1 << 8 * size) - 1
        at = self.pc()
        self.windows[addr & ~(REG_PAGE - 1)] += 1
        if self.board and addr < BOARD_TOP and word not in self.args.local:
            self.forwarded += 1
            self.total += 1
            if time.monotonic() > self.deadline:
                self.ask("time", word, at)
            n = self.reads[at, word] = self.reads[at, word] + 1
            silent = sum(SILENT.values())
            last = self.last_read
            if last and last[0] == word and last[1] != at and last[2] == self.total - 1 and not last[4]:
                # Two instructions in a row reading one register are one instant on the chip, where a running counter
                # reads the same twice; through the bridge half a millisecond apart, it never would.
                self.instants += 1
                full, same = last[3], True
            else:
                try:
                    full, same = read(self.board, word), False
                except BridgeError as e:  # a queued write that went before it refused: the run ends here
                    self.log(f"read  {addr:08x}/{size} at {at:06x}: {e}")
                    self.ask("refused-read", word, at)
                    return 0
            if word in self.cleared:
                # A status the core cleared by writing ones: on the chip the handler that cleared it reads it again,
                # and returns, before its events latch again, where through the bridge they would have, every time;
                # a clear made outside any handler holds for the next read alone.
                by_depth = self.cleared[word]
                for ones in by_depth.values():
                    full &= ~ones
                by_depth.pop(0, None)
                if not by_depth:
                    del self.cleared[word]
            self.last_read = (word, at, self.total, full, same)
            v = full >> shift & mask
            self.recent.append(f"read  {addr:08x}/{size} = {v:#x} at {at:06x} [{self.total}]")
            if self.record:
                print(f"r {addr:06x} {size} {v:x} {at:06x} {self.total}", file=self.record)
            if sum(SILENT.values()) != silent:
                self.check_alive(word, at)
            if n <= LOGGED or word in self.args.show:
                self.log(f"read  {addr:08x}/{size} = {v:#x} at {at:06x}{self.when()}")
            return v
        if word in COUNTERS:
            self.regs[word] = (self.regs.get(word, 0x7FFFFFFF) + COUNTERS[word]) & 0x7FFFFFFF
            return self.regs[word] >> shift & mask
        n = self.reads[at, word] = self.reads[at, word] + 1
        v = self.args.reg.get(word, self.regs.get(word, 0))
        if self.args.auto and n % POLL == 0 and word not in self.args.reg:
            k = self.answers[at, word] = self.answers.get((at, word), 0) + 1
            try_ = TRIES[(k - 1) % len(TRIES)]
            v = self.regs[word] = ~v & 0xFFFFFFFF if try_ is None else try_
            if k <= len(TRIES):
                self.log(f"poll  {addr:08x} at {at:06x}: answered {v:#x}")
        v = v >> shift & mask
        if n <= LOGGED or word in self.args.show:
            self.log(f"read  {addr:08x}/{size} = {v:#x} at {at:06x}{self.when()}")
        return v

    def reg_write(self, uc, off, size, value, base):
        addr = base + off
        word, shift = addr & ~3, 8 * (addr & 3)
        mask = ((1 << 8 * size) - 1) << shift
        at = self.pc()
        self.windows[addr & ~(REG_PAGE - 1)] += 1
        if self.board and addr < BOARD_TOP and word not in self.args.local:
            self.forwarded += 1
            self.total += 1
            if time.monotonic() > self.deadline:
                self.ask("time", word, at)
            held = self.args.clear.get(word, 0) & (value << shift)
            if held:  # a bit the run keeps clear on the chip, as the core it stands in for would not
                value &= ~(held >> shift)
                self.log(f"note  {addr:08x}/{size}: {held:#x} kept clear")
            if size == 4:
                try:
                    self.board.post(BOARD_WINDOW + word, value)
                except BridgeError as e:  # the bridge refused it, or one queued before it: the run ends here
                    self.log(f"write {addr:08x}/{size} = {value:#x} at {at:06x}: {e}")
                    self.ask("refused", word, at)
                    return
            else:  # the bridge writes words: the rest of the word as the chip has it, which a read may change
                old = read(self.board, word)
                self.board.post(BOARD_WINDOW + word, old & ~mask | (value << shift) & mask)
                self.log(f"note  {addr:08x}/{size} written as a word, read first")
            self.recent.append(f"write {addr:08x}/{size} = {value:#x} at {at:06x} [{self.total}]")
            if self.record:
                print(f"w {addr:06x} {size} {value:x} {at:06x} {self.total}", file=self.record)
            if word in self.args.w1c:
                by_depth = self.cleared.setdefault(word, {})
                by_depth[len(self.active)] = by_depth.get(len(self.active), 0) | value << shift
        self.regs[word] = self.regs.get(word, 0) & ~mask | (value << shift) & mask
        n = self.writes[at, word] = self.writes[at, word] + 1
        if n <= LOGGED or word in self.args.show:
            self.log(f"write {addr:08x}/{size} = {value & mask >> shift:#x} at {at:06x}{self.when()}")
        if REMAP <= word < REMAP + 0x200 or REMAP_ON <= word < REMAP_ON + 0x20:
            self.remap(word, value)
        if word == ICSR:
            if value & 1 << 28:
                self.pending.add(PENDSV)
            if value & 1 << 26:
                self.pending.add(SYSTICK)
        for base, act in ((NVIC_ISER, "set"), (NVIC_ICER, "clear"), (NVIC_ISPR, "pend"), (NVIC_ICPR, "unpend")):
            if base <= word < base + 8:
                bits = (value & 0xFFFFFFFF) << 32 * ((word - base) // 4)
                if act == "set":
                    self.enabled |= bits
                elif act == "clear":
                    self.enabled &= ~bits
                elif act == "pend":
                    self.pending |= {16 + n for n in range(64) if bits >> n & 1}
                else:
                    self.pending -= {16 + n for n in range(64) if bits >> n & 1}

    def check_alive(self, word, at):
        """The run ends at a register that does not answer, its last accesses to the log."""
        self.log(f"silent: {word:08x} at {at:06x} does not answer; the accesses before it:")
        for line in self.recent:
            self.log(f"  {line}")
        self.ask("silent", word, at)

    def remap(self, word, value):
        """A write to the remapper: the emulator's ROM made to read as the chip's now does."""
        if word < REMAP + 0x200:
            self.slots[(word - REMAP) // 4] = 4 * (value & 0xFFFFFFFF)
        else:
            shift = 32 * ((word - REMAP_ON) // 4)
            self.slots_on = self.slots_on & ~(0xFFFFFFFF << shift) | (value & 0xFFFFFFFF) << shift
        covered = {at for n, at in self.slots.items() if self.slots_on >> n & 1}
        for at, patched in self.remapped.items():
            want = patched if at in covered else self.original[at]
            if bytes(self.uc.mem_read(at, 4)) != want:
                self.uc.mem_write(at, want)
                self.uc.ctl_remove_cache(at, at + 4)
                self.log(f"remap {at:06x} {'on' if at in covered else 'off'}")
        unknown = covered - set(self.remapped)
        if unknown:
            self.log(f"remap: no patched word known for {sorted(hex(a) for a in unknown)}")

    # Exceptions: a hook asks, emu_stop ends the run, and the loop in run() acts between runs.

    def ask(self, kind, n, at):
        self.asked.append((kind, n, at))
        self.uc.emu_stop()

    def on_intr(self, uc, intno, data):
        self.ask("intr", intno, self.pc())

    def on_return(self, uc, addr, size, data):
        self.ask("return", addr, addr)

    def sniff(self, poll=False):
        """The lines the chip raised since the last look, as the parked processor saw them, pended if enabled.
        Every answer of the bridge brings them; with poll, an ask is made for them."""
        self.forwarded = 0
        if poll:
            self.board.read(BOARD_WINDOW + SPARE)
        if self.total >= self.next_ctrl:  # what the host sees of the core, now and then
            self.next_ctrl = self.total + CTRL_EVERY
            ctrl = host_read(self.board, BT_CTRL)
            if ctrl != self.ctrl:
                self.log(f"host  BT_CTRL {self.ctrl:#010x} -> {ctrl:#010x} [{self.total} {self.waits}]")
                self.ctrl = ctrl
            # The parked processor's ticks, which the sniffed lines depend on: a clock of its stopped stops them.
            ticks, now = read(self.board, SNIFFED + 8), time.monotonic()
            if self.ticks is not None:
                rate = (ticks - self.ticks[0]) / (now - self.ticks[1])
                if rate < TICKS_LEAST and not self.slow:
                    self.log(f"ticks the parked processor ticks {rate:.0f} a second [{self.total} {self.waits}];"
                             f" the accesses before:")
                    for line in self.recent:
                        self.log(f"  {line}")
                self.slow = rate < TICKS_LEAST
            self.ticks = (ticks, now)
        if self.total >= self.next_host:
            self.next_host = self.total + HOST_EVERY
            self.host.step()
        lines = self.board.take_sniffed()
        if lines:
            self.raised |= {16 + n for n in range(64) if lines >> n & 1}
        if self.total < self.again:
            return lines
        # The lines raised and enabled pend in rounds: one taken waits out the round until every other raised has been
        # taken once, so that a line the chip raises faster than this slow core can answer keeps none of the others
        # out, as on the chip, where its handler takes microseconds. The gap spaces the rounds, as missed ticks.
        ready = {e for e in self.raised if self.enabled >> (e - 16) & 1 and e not in self.pending}
        if not ready - self.served:
            self.served = set()
        for exc in sorted(ready - self.served):
            self.served.add(exc)
            self.raised.discard(exc)
            self.pending.add(exc)
            self.again = self.total + IRQ_GAP
            self.irqs[exc] += 1
            if self.irqs[exc] <= LOGGED or exc - 16 in self.args.show_irq:
                self.log(f"irq   {exc - 16} raised by the chip{self.when()}")
        return lines

    def priority(self, exc):
        """An exception's priority as the code set it, lower first: the NVIC's IPR bytes, the SHPRs for SVC and on."""
        if exc >= 16:
            at = NVIC_IPR + exc - 16
        elif exc in SHPR:
            at = SHPR[exc]
        else:
            return -1
        return self.regs.get(at & ~3, 0) >> 8 * (at & 3) & 0xFF

    def next_exception(self, masked=True):
        """The pending exception to take now, if any: the most urgent, and only if it preempts what runs;
        with masked false, whether one would but for PRIMASK, which is what ends a wfi."""
        if not self.pending or masked and self.uc.reg_read(UC_ARM_REG_PRIMASK) & 1:
            return None
        exc = min(self.pending, key=lambda e: (self.priority(e), e))
        running = min((self.priority(a) for a in self.active), default=256)
        return exc if self.priority(exc) < running else None

    def write_patch(self, uc, at, size, data):
        """The ROM's idle loop is reached: the host writes the patch now, record by record in the file's order."""
        # Written before the reset, the ROM's own start would clear the last two, the pointers its idle loop
        # looks at, and never take the patch.
        if self.patched:
            return
        for addr, data in patch_records(PATCH):
            uc.mem_write(addr, data)
        if self.board:
            for addr, value in self.args.kick:
                self.log(f"kick  {addr:08x} = {value:#x}")
                self.board.write(addr, value)
        self.patched = True
        self.log(f"patch written, as the host writes it [{self.total} {self.waits}]")

    def wait_for_interrupt(self, at):
        """At a wfi: the chip's interrupts looked for until one the core takes comes; why the run ends, if it does."""
        self.waits += 1
        if self.waits <= LOGGED:
            self.log(f"wfi   at {at - 2:06x}")
        end = time.monotonic() + self.args.idle
        asleep = False
        try:
            while True:
                self.again = self.total  # time passes while the core sleeps: no gap holds a line back
                if self.board:
                    self.sniff(poll=True)
                if self.next_exception(masked=False) is not None:
                    return None
                if not self.board or time.monotonic() > end:
                    return (f"waited at the wfi at {at - 2:06x} for an interrupt that did not come; lines enabled"
                            f" {self.enabled:#x}, raised and not taken {sorted(e - 16 for e in self.raised)}")
                if not asleep:  # the chip's processor sleeps with it, for the power manager to see
                    self.board.write(BOARD_WINDOW + ASLEEP, 1)
                    asleep = True
                time.sleep(0.001)
        finally:
            if asleep:
                self.board.write(BOARD_WINDOW + ASLEEP, 0)

    def on_block(self, uc, addr, size, data):
        if self.args.profile and self.host and self.host.sent:
            self.profile[addr] += 1
        if self.patched and self.traced < self.args.trace:
            self.traced += 1
            self.log(f"block {addr:06x}")
        if self.board and self.forwarded >= IRQ_POLL:
            self.sniff()
        exc = self.next_exception()
        if exc is not None:
            self.ask("pending", exc, addr)
        elif self.pending and self.args.profile:  # held back: by PRIMASK, or by what runs
            self.held[addr, "masked" if self.uc.reg_read(UC_ARM_REG_PRIMASK) & 1 else "outranked"] += 1

    def enter(self, exc, back):
        """Take exception exc, coming back to `back`: push the frame on the stack in use, and go through the vector."""
        uc = self.uc
        psp = bool(uc.reg_read(UC_ARM_REG_CONTROL) & 2) and not self.active
        sp = uc.reg_read(UC_ARM_REG_PSP if psp else UC_ARM_REG_MSP)
        xpsr = uc.reg_read(UC_ARM_REG_XPSR) & ~0x1FF
        if sp & 4:
            sp, xpsr = sp - 4, xpsr | 0x200
        sp -= 32
        frame = [uc.reg_read(r) for r in FRAME] + [back & ~1, xpsr | 0x01000000]
        uc.mem_write(sp, struct.pack("<8I", *[w & 0xFFFFFFFF for w in frame]))
        uc.reg_write(UC_ARM_REG_CONTROL, uc.reg_read(UC_ARM_REG_CONTROL) & ~2)
        uc.reg_write(UC_ARM_REG_PSP if psp else UC_ARM_REG_MSP, sp)
        uc.reg_write(UC_ARM_REG_SP, uc.reg_read(UC_ARM_REG_MSP))
        self.active.append(exc)
        self.entries[exc] += 1
        uc.reg_write(UC_ARM_REG_LR, 0xFFFFFFF1 if len(self.active) > 1 else 0xFFFFFFFD if psp else 0xFFFFFFF9)
        vector = struct.unpack("<I", uc.mem_read(4 * exc, 4))[0]
        if self.entries[exc] <= LOGGED or exc - 16 in self.args.show_irq:
            self.log(f"exc   {exc} at {back & ~1:06x} -> {vector & ~1:06x}{self.when()}")
        return vector

    def leave(self, excret):
        """Return from the exception taken last: pop the frame off the stack EXC_RETURN names."""
        uc = self.uc
        self.active.pop()
        for word in list(self.cleared):  # the ones the returning handler wrote are let go
            by_depth = {d: ones for d, ones in self.cleared[word].items() if d <= len(self.active)}
            if by_depth:
                self.cleared[word] = by_depth
            else:
                del self.cleared[word]
        psp = bool(excret & 4)
        if psp:
            uc.reg_write(UC_ARM_REG_CONTROL, uc.reg_read(UC_ARM_REG_CONTROL) | 2)
        sp = uc.reg_read(UC_ARM_REG_PSP if psp else UC_ARM_REG_MSP)
        frame = struct.unpack("<8I", uc.mem_read(sp, 32))
        for r, w in zip(FRAME, frame):
            uc.reg_write(r, w)
        sp += 32 + (4 if frame[7] & 0x200 else 0)
        uc.reg_write(UC_ARM_REG_PSP if psp else UC_ARM_REG_MSP, sp)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_XPSR, frame[7] & ~0x1FF)
        return frame[6] | 1

    def run(self):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_MSP, self.sp)
        uc.reg_write(UC_ARM_REG_SP, self.sp)
        at, left = self.reset, self.args.max
        self.deadline = end = time.monotonic() + self.args.seconds
        while True:
            if time.monotonic() > end:
                return f"ran its {self.args.seconds:g} s, at {at & ~1:06x}"
            self.asked.clear()
            before = left
            try:
                uc.emu_start(at | 1, 0xFFFFFFFF, count=left)
            except UcError as e:
                return f"fault at {self.pc():06x}: {e}, lr {uc.reg_read(UC_ARM_REG_LR):#x}"
            # A run stopped by a hook has used an unknown share of its count; the count ends the run only when it
            # ran out unasked, so --max bounds the instructions of each stretch between exceptions, not their sum.
            if not self.asked and self.uc.mem_read(self.pc() - 2, 2) == b"\x30\xbf":
                # Unicorn ends the run at a wfi: the core waits for an interrupt, which only the chip can raise.
                at = self.pc()
                why = self.wait_for_interrupt(at)
                if why:
                    return why
                continue
            if not self.asked:
                return f"stopped after {before} instructions at {self.pc():06x}"
            kind, n, where = self.asked[0]
            if kind == "until":
                return f"reached {where:06x}"
            if kind == "time":
                return f"ran its {self.args.seconds:g} s, at {where:06x}"
            if kind == "refused":
                return f"the bridge refused a write, of {n:08x} or one queued before it, at {where:06x}; see the log"
            if kind == "refused-read":
                return f"the bridge refused a write queued before the read of {n:08x} at {where:06x}; see the log"
            if kind == "unready":
                return f"the core not ready after {n} accesses, at {where:06x}"
            if kind == "stall":
                return f"no answer to the HCI command {n:04x} in {HCI_WAIT} accesses, at {where:06x}"
            if kind == "done":
                return f"every HCI command answered, the last {n:04x}, at {where:06x}"
            if kind == "silent":
                return f"the core went silent after an access to {n:08x} at {where:06x}"
            if kind == "intr" and n == EXCP_SWI:  # the pc is past the svc already
                at = self.enter(SVC, where)
            elif kind == "intr" and n == EXCP_EXCEPTION_EXIT:  # a branch to EXC_RETURN in handler mode
                at = self.leave(where | 1)
            elif kind == "return":
                at = self.leave(n | 1)
            elif kind == "pending":
                self.pending.discard(n)
                at = self.enter(n, where)
            else:
                return f"exception {n} at {where:06x}, not taken"


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--log", help="the access log, a line each (default: none)")
    p.add_argument("--max", type=lambda s: int(s, 0), default=200_000_000,
                   help="instructions between two exceptions before the run ends")
    p.add_argument("--reg", action="append", default=[], help="ADDR=VALUE: what a read of that register gives")
    p.add_argument("--auto", action="store_true", help="answer polls with guesses, see TRIES")
    p.add_argument("--until", type=lambda s: int(s, 0), help="end the run when it reaches this address")
    p.add_argument("--board", action="store_true", help="the chip's own registers, through the Wi-Fi system's bridge")
    p.add_argument("--idle", type=float, default=10.0, help="seconds a wfi waits for an interrupt before the run ends")
    p.add_argument("--seconds", type=float, default=600.0, help="seconds the whole run may take")
    p.add_argument("--read-rom", action="store_true",
                   help=f"with --board, read the ROM into {ROM_IMAGE} before the patch remaps it, and end")
    p.add_argument("--ram", help="where to write the core's RAM, 0x200000 to 0x220000, as the run left it")
    p.add_argument("--kick", action="append", default=[],
                   help="ADDR=VALUE: a backplane write the host makes with the patch, with --board")
    p.add_argument("--hci", action="append", default=[],
                   help="OPCODE[:PARAMETERS]: an HCI command in hex, sent once the core is ready, with --board")
    p.add_argument("--w1c", action="append", default=[], type=lambda s: int(s, 0) & ~3,
                   help="ADDR: a status cleared by writing ones, whose reads leave those ones out until the handler"
                        " that wrote them returns, with --board")
    p.add_argument("--local", action="append", default=[], type=lambda s: int(s, 0) & ~3,
                   help="ADDR: a register kept in the emulator, never the chip's, as without --board, with --board")
    p.add_argument("--clear", action="append", default=[],
                   help="ADDR=BITS: bits a write to that register leaves clear on the chip, with --board")
    p.add_argument("--record", help="FILE: every access forwarded to the chip, a line each, r|w ADDR SIZE VALUE PC N, "
                   "and each HCI command and event, a line of # each")
    p.add_argument("--profile", action="store_true", help="count the blocks run once the first HCI command is sent")
    p.add_argument("--trace", type=int, default=0, help="N: log the first N blocks run once the patch is written")
    p.add_argument("--poke", action="append", default=[],
                   help="ADDR=VALUE: a word of the core's memory written once the core is ready, with --board")
    p.add_argument("--watch", action="append", default=[], type=lambda s: int(s, 0),
                   help="ADDR: log who reads and writes that word of memory")
    p.add_argument("--show", action="append", default=[], type=lambda s: int(s, 0) & ~3,
                   help="ADDR: log every access to that register, not only the first at each instruction")
    p.add_argument("--show-irq", action="append", default=[], type=lambda s: int(s, 0),
                   help="N: log every time line N is raised and taken")
    args = p.parse_args()
    args.reg = {int(k, 0) & ~3: int(v, 0) for k, v in (r.split("=") for r in args.reg)}
    args.hci = [(int(c.split(":")[0], 16), bytes.fromhex(c.split(":")[1] if ":" in c else "")) for c in args.hci]
    args.clear = {int(k, 0) & ~3: int(v, 0) for k, v in (r.split("=") for r in args.clear)}
    args.poke = [(int(k, 0), int(v, 0)) for k, v in (r.split("=") for r in args.poke)]
    args.kick = [(int(k, 0), int(v, 0)) for k, v in (r.split("=") for r in args.kick)]

    out = open(args.log, "w") if args.log else None
    log = (lambda line: print(line, file=out, flush=True)) if out else (lambda line: None)
    # A termination ends the run as an interrupt does, so the board is let go and halts.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    board = None
    if args.board:
        board = Bridge(log=lambda line: log(f"board {line}"))
        if args.read_rom:
            words = []
            try:
                for at in range(0, ROM_TOP, 4):  # each read twice: while the core runs, a read gives the one before
                    board.read(BOARD_WINDOW + at)
                    words.append(board.read(BOARD_WINDOW + at))
            finally:
                board.close()
            with open(ROM_IMAGE, "wb") as f:
                f.write(struct.pack(f"<{len(words)}I", *words))
            print(f"cyw43-rehost: the ROM is in {ROM_IMAGE}")
            return 0
    core = None
    try:
        if board:
            park(board)
            board.sniff(BOARD_WINDOW + SNIFFED, BOARD_WINDOW + SPARE)  # every answer brings the lines the core saw
            print("cyw43-rehost: the core's processor is parked")
        core = Core(args, log, board)
        why = core.run()
    except KeyboardInterrupt:
        why = "interrupted"
    finally:
        if board:
            try:
                print(f"cyw43-rehost: the host's view of the core, BT_CTRL {host_read(board, BT_CTRL):#010x}")
                ticks = read(board, SNIFFED + 8)
                time.sleep(0.1)
                later = read(board, SNIFFED + 8)
                print(f"cyw43-rehost: the parked processor's ticks {ticks}, {later - ticks} in 0.1 s")
            except Exception as e:  # the board gone or hung: it is still let go
                print(f"cyw43-rehost: BT_CTRL not read: {e}")
            board.close()
    if board:
        print(f"cyw43-rehost: {board.asks} asks of the board")
    if core is None:  # interrupted while it parked
        print(f"cyw43-rehost: {why}")
        return 1
    if core.record:
        core.record.close()
    if args.ram:
        open(args.ram, "wb").write(bytes(core.uc.mem_read(RAM, IMAGE_TOP - RAM)))
    print(f"cyw43-rehost: {why}")
    print("register windows, accesses:")
    for base, n in sorted(core.windows.items()):
        print(f"  {base:08x} {n}")
    for (at, word), k in sorted(core.answers.items()):
        print(f"poll {word:08x} at {at:06x}: answered {k} times")
    for exc, k in sorted(core.irqs.items()):
        print(f"irq {exc - 16}: raised by the chip {k} times")
    print(f"lines enabled {core.enabled:#x}, raised and not taken {sorted(e - 16 for e in core.raised)}")
    pending = sorted((e, core.priority(e)) for e in core.pending)
    active = [(e, core.priority(e)) for e in core.active]
    print(f"pending {pending}, active {active}, PRIMASK {core.uc.reg_read(UC_ARM_REG_PRIMASK)}")
    for exc, k in sorted(core.entries.items()):
        print(f"exception {exc}{f' (irq {exc - 16})' if exc >= 16 else ''}: taken {k} times")
    print(f"wfi: {core.waits} waits")
    print(f"reads in the same instant as the one before: {core.instants}")
    for kind, data in core.events:
        print(f"hci event, type {kind}: {data.hex()}")
    for (addr, why), k in core.held.most_common(20):
        print(f"held {why} at {addr:06x}: {k} times")
    for addr, k in core.profile.most_common(60):
        print(f"block {addr:06x}: {k} times")
    for addr, k in sorted(SILENT.items()):
        print(f"no answer {addr:06x}: {k} times")
    for addr, k in sorted(REREAD.items()):
        print(f"read again {addr:06x}: {k} times")
    for (kind, at, pc), k in sorted(core.watched.items()):
        print(f"watch {kind} {at:06x} at {pc:06x}: {k} times")
    if out:
        out.close()


if __name__ == "__main__":
    sys.exit(main())
