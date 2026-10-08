#!/usr/bin/env python3
"""Read a trace log of user/wifi/esp32c6/: the snapshot list, the compare of two runs against the traced one, and
the library and function each device access belongs to.

  snapshot LOG              the aligned whole-word addresses the run read, as the snap= lines a later run reads back
  compare DRY1 DRY2 TRACED  the traced run's final state against two dry runs', nonzero on a divergence
  attrib [--only NAME] LOG  the library and function each access is in, see below

The root task of root.c owns what this parses, see its trace_line and trace_dump. The channel loses the odd line on
a long stream, so a log is not always whole; each subcommand prints what it found of that, and wrongs its own answer
only where the loss is beside what it reports.

Usage: tools/mac-trace.py {snapshot,compare,attrib} ... ; each subcommand has its own --help.
"""

import argparse
import bisect
import collections
import glob
import hashlib
import re
import subprocess
import sys

# The ESP32-C6's code memory, as a pc falls in it: the ROM at 0x40000000, the image's RAM code above that,
# and its flash-mapped code at 0x42000000.
IROM_BASE = 0x42000000
IRAM_BASE = 0x40800000
IRAM_TOP = 0x41000000

# A record line: "[seq] pc 0x... tN W4 0xaddr = 0xvalue"; see root.c's trace_line, which owns this format.
RECORD = re.compile(r"^\s*[\d.]+\s+\[\d+\]\s+pc\s+(0x[0-9a-fA-F]+)\s+t\d+\s+([WR])(\d)\s+(0x[0-9a-fA-F]+)")
# A cycle's header: "[seq] cycle xT of L:", the L records after it printed once for T turns; see root.c's trace_out_cycle.
CYCLE = re.compile(r"\[\d+\]\s+cycle x(\d+) of (\d+):")
# The stream's own count of the accesses it served: "[seq] trace over: N accesses".
OVER = re.compile(r"trace over:\s*(\d+)\s+accesses")
# A line that carries a sequence number but is no record: the stream's own markers, see root.c's trace_dump.
MARKER = re.compile(r"\[\d+\]\s+(?:trace over:|snapshot of\b)")
# The sequence number at the front of a stream line, whichever it is.
SEQ = re.compile(r"\[(\d+)\]")
# A snapshot line: "snap 0xaddr = 0xvalue", and its block's header, "snapshot of N addresses"; see root.c's trace_dump.
SNAP = re.compile(r"snap (0x[0-9a-fA-F]+) = (0x[0-9a-fA-F]+)")
SNAPHDR = re.compile(r"snapshot of (\d+) addresses")
# The line tools/esp32c6-run.py writes before it boots, naming what it flashed and its hash.
FLASHED = re.compile(r"# flashed 0x[0-9a-fA-F]+ sha256 ([0-9a-f]{64})")
# A ROM linker script: "name = 0x...", possibly inside PROVIDE(...), comments left to strip.
PLACED = re.compile(r"^\s*(?:PROVIDE\s*\(\s*)?([A-Za-z_][A-Za-z0-9_.$]*)\s*=\s*0x[0-9a-fA-F]+", re.M)


def scan(path):
    """The accesses a log holds, and where it is not whole.

    Returns (records, damaged). A record is (seq, pc, op, width, address, turns): turns is what a cycle printed it
    once for, one outside a cycle. A damaged entry is (seq, why) for a line with a sequence number that is no
    record, and for a sequence number missing from the run. A damaged line's own access is lost with it.
    """
    out = []
    damaged = []
    cycle = None  # [turns, records of the turn still to come]
    numbers = []
    with open(path, errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            s = SEQ.search(line)
            if not s:
                continue
            seq = int(s.group(1))
            numbers.append(seq)
            c = CYCLE.search(line)
            r = RECORD.match(line)
            if c:
                cycle = [int(c.group(1)), int(c.group(2))]
            elif r:
                turns = 1
                if cycle and cycle[1] > 0:
                    turns = cycle[0]
                    cycle[1] -= 1
                out.append((seq, int(r.group(1), 16), r.group(2), int(r.group(3)), int(r.group(4), 16), turns))
            elif not MARKER.search(line):
                damaged.append((seq, "line %d cut: %s" % (lineno, line.strip()[:50])))
    if numbers:
        for seq in sorted(set(range(numbers[0], numbers[-1] + 1)) - set(numbers)):
            damaged.append((seq, "missing"))
    return out, damaged


def served(path):
    """The accesses the stream's own "trace over" line reports, or None if the log has none."""
    with open(path, errors="replace") as f:
        for line in f:
            m = OVER.search(line)
            if m:
                return int(m.group(1))
    return None


def snapshots(path):
    """The snapshot a run's log holds, address -> value; the block's own count has to match its lines'."""
    out = {}
    want = None
    with open(path, errors="replace") as f:
        for line in f:
            m = SNAP.search(line)
            if m:
                out[int(m.group(1), 16)] = int(m.group(2), 16)
                continue
            h = SNAPHDR.search(line)
            if h:
                want = int(h.group(1))
    if want is None:
        sys.exit("mac-trace: %s holds no snapshot; was it run with snap= lines?" % path)
    if len(out) != want:
        sys.exit("mac-trace: %s says %d snapshot addresses but holds %d" % (path, want, len(out)))
    return out


def symbolizer(elf, addrs):
    """The function each address falls in, from an ELF, through llvm-symbolizer."""
    if not addrs:
        return []
    out = subprocess.run(["llvm-symbolizer", "--obj=" + elf, "--functions=linkage", "--no-inlines", *map(hex, addrs)],
                         capture_output=True, text=True).stdout
    return [block.split("\n")[0] for block in out.strip().split("\n\n")]


def defined(path, external=True):
    """The symbols a file defines, from llvm-nm; its locals too when external is false."""
    flags = ["--defined-only", "--extern-only"] if external else ["--defined-only"]
    out = subprocess.run(["llvm-nm", *flags, "--format=just-symbols", path], capture_output=True, text=True).stdout
    return {s.split()[-1] for s in out.splitlines() if s.strip() and not s.endswith(":")}


def placed(path):
    """The symbols a ROM linker script places, its comments stripped."""
    text = re.sub(r"/\*.*?\*/", "", open(path).read(), flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return set(PLACED.findall(text))


def ranges(addresses):
    """The addresses sorted, gathered into (lo, hi) where four apart stands together."""
    out = []
    for a in sorted(addresses):
        if out and a == out[-1][1] + 4:
            out[-1][1] = a
        else:
            out.append([a, a])
    return out


def snapshot(args):
    """The snap= lines a trace log's reads ask a later run to read back."""
    acc, damaged = scan(args.log)
    if damaged:
        print("mac-trace: %s is short in %d places" % (args.log, len(damaged)), file=sys.stderr)
    seen = set()
    out = []
    skipped = 0
    for seq, pc, op, width, address, turns in acc:
        if op != "R":
            continue
        if width == 4 and address % 4 == 0:
            if address not in seen:
                seen.add(address)
                out.append(address)
        else:
            skipped += 1
    if not out:
        sys.exit("mac-trace: no aligned whole-word reads in %s" % args.log)
    lines = ranges(out)
    for lo, hi in lines:
        print("snap=%x" % lo if lo == hi else "snap=%x-%x" % (lo, hi))
    print("mac-trace: %d addresses read, in %d lines; %d reads left out" % (len(out), len(lines), skipped),
          file=sys.stderr)


def compare(args):
    """The words the two dry runs differ in, then the traced run's divergences from them; the count of those."""
    dry1 = snapshots(args.dry1)
    dry2 = snapshots(args.dry2)
    traced = snapshots(args.traced)
    if set(dry1) != set(dry2) or set(dry1) != set(traced):
        sys.exit("mac-trace: the three runs' snapshots name different addresses")
    volatile = sorted(a for a in dry1 if dry1[a] != dry2[a])
    divergent = sorted(a for a in dry1 if a not in volatile and traced[a] != dry1[a])
    print("mac-trace: %d addresses; %d volatile between the dry runs; %d divergences"
          % (len(dry1), len(volatile), len(divergent)))
    for a in volatile:
        print("  volatile  0x%08x  dry 0x%08x / 0x%08x" % (a, dry1[a], dry2[a]))
    for a in divergent:
        print("  divergent 0x%08x  dry 0x%08x / traced 0x%08x" % (a, dry1[a], traced[a]))
    return 1 if divergent else 0


def attrib(args):
    """The library and function each access is in, from the ELF's names and the libraries' symbols.

    It refuses only when the log is short beside the library --only asks for, whose part is then not whole; with
    no --only its counts are lower bounds over the whole log, and every memory-mapped access is named.
    """
    if args.image:
        want = hashlib.sha256(open(args.image, "rb").read()).hexdigest()
        with open(args.log, errors="replace") as f:
            if want not in {m.group(1) for m in FLASHED.finditer(f.read())}:
                sys.exit("mac-trace: %s is not the image the log flashed" % args.image)

    owner = {}
    romlib = {}
    names = []
    for spec in args.lib:
        name, rest = spec.split("=", 1)
        archive, _, script = rest.partition(":")
        names.append(name)
        for s in defined(archive):
            owner.setdefault(s, name)
        if script:
            for s in placed(script):
                romlib.setdefault(s, name)
    for pattern in args.own:
        for obj in glob.glob(pattern, recursive=True):
            for s in defined(obj, external=False):
                owner[s] = "own"

    acc, damaged = scan(args.log)
    pcs = sorted({r[1] for r in acc})
    image = [p for p in pcs if p >= IROM_BASE or IRAM_BASE <= p < IRAM_TOP]
    rom = [p for p in pcs if p not in image]
    function = {}
    for p, n in zip(image, symbolizer(args.elf, image)):
        function[p] = n
    for p, n in zip(rom, symbolizer(args.rom, rom)):
        function[p] = n

    def library(pc):
        if pc in function and function[pc] in owner:
            return owner[function[pc]]
        if pc < IROM_BASE:
            return romlib.get(function.get(pc), "rom")
        return "unknown"

    by_records = collections.Counter()
    by_accesses = collections.Counter()
    by_function = collections.Counter()
    known = []  # (seq, library), in stream order, to name what a damaged sequence number sits beside
    for seq, pc, op, width, address, turns in acc:
        lib = library(pc)
        known.append((seq, lib))
        by_records[lib] += 1
        by_accesses[lib] += turns
        by_function[(lib, function.get(pc, "?"))] += turns

    beside = collections.Counter()
    if damaged:
        seqs = [s for s, _ in known]
        print("the log is short in %d places:" % len(damaged))
        for seq, why in damaged[:30]:
            i = bisect.bisect_left(seqs, seq)
            near = [known[j][1] for j in (i - 1, i) if 0 <= j < len(known)]
            for l in set(near):
                beside[l] += 1
            print("  seq %d %s (beside %s)" % (seq, why, ", ".join(near) if near else "nothing"))
        if len(damaged) > 30:
            print("  ... and %d more" % (len(damaged) - 30))
        if args.only and beside.get(args.only):
            sys.exit("mac-trace: %s is short beside %s, so its part is not whole" % (args.log, args.only))
    print("the log: %d records, %d accesses%s" % (sum(by_records.values()), sum(by_accesses.values()),
                                                   ", lower bounds, the log is short" if damaged else ""))
    for lib, n in by_records.most_common():
        print("  %-9s %6d records  %6d accesses" % (lib, n, by_accesses[lib]))
    over = served(args.log)
    if not damaged and over is not None and over != sum(by_accesses.values()):
        sys.exit("mac-trace: the records' turns sum to %d, but the stream says %d accesses"
                 % (sum(by_accesses.values()), over))

    shown = [args.only] if args.only else [n for n in names + ["own", "rom", "unknown"] if n in by_records]
    for lib in shown:
        print("\n%s:" % lib)
        for (l, fn), n in by_function.most_common():
            if l == lib:
                print("  %-46s %d" % (fn, n))
        if args.only:
            print("  ordered writes:")
            for seq, pc, op, width, address, turns in acc:
                if library(pc) == lib and op == "W":
                    print("    %-46s w%d 0x%08x" % (function.get(pc, "?"), width, address))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("snapshot", help="a trace log's reads, as the snap= lines a later run reads back")
    p.add_argument("log")
    p.set_defaults(run=snapshot)

    p = sub.add_parser("compare", help="the traced run's final state against two dry runs'")
    p.add_argument("dry1")
    p.add_argument("dry2")
    p.add_argument("traced")
    p.set_defaults(run=compare)

    p = sub.add_parser("attrib", help="the library and function each access is in")
    p.add_argument("--elf", required=True, help="the driver's ELF, for the image's code")
    p.add_argument("--rom", required=True, help="the ROM's ELF, for the ROM's code")
    p.add_argument("--lib", action="append", default=[], metavar="NAME=ARCHIVE[:ROM.ld]",
                   help="a library, by the archive that defines it and the ROM script that places the rest")
    p.add_argument("--own", action="append", default=[], metavar="OBJ",
                   help="the driver's own objects, by path or glob; the owner not any library's")
    p.add_argument("--image", metavar="FILE", help="refuse unless the log flashed this file, by its sha256")
    p.add_argument("--only", metavar="NAME", help="hold this library alone, and refuse only if it is the damaged one")
    p.add_argument("log")
    p.set_defaults(run=attrib)

    args = parser.parse_args()
    sys.exit(args.run(args))


if __name__ == "__main__":
    main()
